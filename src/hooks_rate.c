/*
 * hooks_rate.c - Time-windowed (rate) limit enforcement
 *
 * Implements the qos.max_*_rate / qos.max_*_rate_window limits: "at most N
 * operations per W milliseconds" for transactions and for each statement
 * type, counted per (database, role) across all backends.
 *
 * The algorithm is a token bucket with lazy refill.  Nothing runs
 * periodically - each check adds elapsed * (count / window) tokens before
 * deciding - so the CPU cost of a check is independent of the window length:
 * a 100ms window costs exactly as much as a one hour window.
 *
 * Author:  M.Atif Ceylan
 * Company: AppstoniA OÜ
 * Created: October 28, 2025
 * Version: 1.1
 * License: See LICENSE file in the project root
 *
 * Copyright (c) 2025 AppstoniA OÜ
 * All rights reserved.
 */

#include "postgres.h"
#include "qos.h"
#include "hooks_internal.h"
#include "miscadmin.h"
#include "portability/instr_time.h"
#include "storage/lwlock.h"

/*
 * Backend-local slot cache.
 *
 * Without this every check would scan QOS_MAX_RATE_ENTRIES to find its
 * bucket.  Same pattern as qos_backend_slot in hooks.c: remember the index,
 * re-validate it cheaply, and rebuild it when the session's identity or the
 * shared settings epoch changes.
 */
static int  cached_rate_slot[QOS_RATE_NKINDS] = {-1, -1, -1, -1, -1};
static Oid  cached_slot_db = InvalidOid;
static Oid  cached_slot_role = InvalidOid;

/* Warn once per backend when the slot table is exhausted */
static bool rate_table_full_warned = false;

static int64 qos_rate_now_us(void);
static int qos_rate_find_slot(Oid database_oid, Oid role_oid, int kind,
                              int initial_tokens);
static void qos_rate_reset_slot_cache(void);

/*
 * Human-readable name for a rate kind, used in error messages.
 */
const char *
qos_rate_kind_name(int kind)
{
    switch (kind)
    {
        case QOS_RATE_TX:     return "transaction";
        case QOS_RATE_SELECT: return "SELECT";
        case QOS_RATE_UPDATE: return "UPDATE";
        case QOS_RATE_DELETE: return "DELETE";
        case QOS_RATE_INSERT: return "INSERT";
        default:              return "unknown";
    }
}

/*
 * Drop the cached slot indexes, forcing the next check to re-resolve them.
 */
static void
qos_rate_reset_slot_cache(void)
{
    int i;

    for (i = 0; i < QOS_RATE_NKINDS; i++)
        cached_rate_slot[i] = -1;

    cached_slot_db = InvalidOid;
    cached_slot_role = InvalidOid;
}

/*
 * Monotonic clock in microseconds.
 *
 * Deliberately not GetCurrentTimestamp(): that is wall-clock time, and an NTP
 * step backwards would make elapsed negative and stall every limit until the
 * clock caught up.  instr_time uses CLOCK_MONOTONIC where available.
 */
static int64
qos_rate_now_us(void)
{
    instr_time now;

    INSTR_TIME_SET_CURRENT(now);
    return (int64) INSTR_TIME_GET_MICROSEC(now);
}

/*
 * Locate the slot for (database_oid, role_oid, kind), allocating one if this
 * is the first check for that triple.  Returns -1 if the table is full.
 *
 * A newly created bucket starts FULL (initial_tokens), the standard token
 * bucket convention: an idle system has its whole burst allowance available.
 * Starting empty would reject the very first statement after a limit is
 * configured and make every fresh bucket behave as an outage.
 *
 * Caller must NOT hold a rate lock; this function takes them itself.
 */
static int
qos_rate_find_slot(Oid database_oid, Oid role_oid, int kind, int initial_tokens)
{
    int i;

    /* Fast path: cached index still points at our bucket.  Slots are never
     * released once claimed, so a cached index stays valid for the life of
     * the backend as long as the session identity is unchanged. */
    if (cached_slot_db == database_oid && cached_slot_role == role_oid &&
        cached_rate_slot[kind] >= 0)
        return cached_rate_slot[kind];

    /* Session identity changed - the whole cache is stale */
    if (cached_slot_db != database_oid || cached_slot_role != role_oid)
    {
        qos_rate_reset_slot_cache();
        cached_slot_db = database_oid;
        cached_slot_role = role_oid;
    }

    for (;;)
    {
        int free_slot = -1;
        LWLock *lock;
        QoSRateEntry *e;
        bool claimed = false;

        /*
         * Scan for an existing entry.  Reads are done without a lock: the key
         * fields are only ever written once, under a lock, while the slot is
         * being claimed, and database_oid is written last.  A racing reader
         * can therefore only fail to see a new entry, never see a half-built
         * one - and that case is caught by the re-check under the lock below.
         */
        for (i = 0; i < QOS_MAX_RATE_ENTRIES; i++)
        {
            e = &qos_shared_state->rate_slots[i].entry;

            if (e->database_oid == database_oid && e->role_oid == role_oid &&
                e->kind == kind)
            {
                cached_rate_slot[kind] = i;
                return i;
            }

            if (free_slot < 0 && e->database_oid == InvalidOid)
                free_slot = i;
        }

        if (free_slot < 0)
        {
            if (!rate_table_full_warned)
            {
                ereport(WARNING,
                        (errmsg("qos: rate limiter slot table is full (%d entries)",
                                QOS_MAX_RATE_ENTRIES),
                         errdetail("Rate limits are not enforced for db=%u role=%u.",
                                   database_oid, role_oid)));
                rate_table_full_warned = true;
            }
            return -1;
        }

        /* Claim the free slot, re-checking under the lock */
        lock = &qos_shared_state->rate_locks[free_slot % QOS_RATE_LOCK_STRIPES].lock;
        e = &qos_shared_state->rate_slots[free_slot].entry;

        LWLockAcquire(lock, LW_EXCLUSIVE);

        if (e->database_oid == InvalidOid)
        {
            e->tokens = (double) initial_tokens;
            e->last_refill_us = qos_rate_now_us();
            e->rejected = 0;
            e->kind = kind;
            e->role_oid = role_oid;
            /* Written last: this is what publishes the slot to readers */
            e->database_oid = database_oid;
            claimed = true;
        }
        else if (e->database_oid == database_oid && e->role_oid == role_oid &&
                 e->kind == kind)
        {
            claimed = true;     /* another backend built the same entry */
        }

        LWLockRelease(lock);

        if (claimed)
        {
            cached_rate_slot[kind] = free_slot;
            return free_slot;
        }

        /*
         * Lost the race to a different key.  Rescan from the start rather
         * than resuming past this slot: a concurrent backend may have
         * published our key in a slot we already walked, and creating a
         * duplicate entry would permanently split the bucket.
         */
    }
}

/*
 * Consume one token for `kind` under the limit `count` per `window_ms`.
 *
 * Returns true when the operation is allowed (a token was consumed), false
 * when the limit is exhausted.  On rejection *retry_after_ms, if supplied, is
 * set to how long the caller would have to wait for one token.
 *
 * Fails open: if shared state or a slot is unavailable the operation is
 * allowed, so a full slot table can never wedge a production workload.
 */
bool
qos_rate_check(int kind, int count, int window_ms, int *retry_after_ms)
{
    int slot;
    LWLock *lock;
    QoSRateEntry *e;
    int64 now;
    int64 elapsed;
    double tokens_per_us;
    bool allowed;

    if (retry_after_ms)
        *retry_after_ms = 0;

    if (!qos_shared_state || count <= 0 || window_ms <= 0)
        return true;

    if (kind < 0 || kind >= QOS_RATE_NKINDS)
        return true;

    slot = qos_rate_find_slot(MyDatabaseId, GetUserId(), kind, count);
    if (slot < 0)
        return true;    /* fail open */

    e = &qos_shared_state->rate_slots[slot].entry;
    lock = &qos_shared_state->rate_locks[slot % QOS_RATE_LOCK_STRIPES].lock;

    tokens_per_us = (double) count / ((double) window_ms * 1000.0);

    LWLockAcquire(lock, LW_EXCLUSIVE);

    now = qos_rate_now_us();
    elapsed = now - e->last_refill_us;

    /* Lazy refill.  elapsed < 0 should be impossible with a monotonic clock,
     * but guard anyway rather than hand out tokens for negative time. */
    if (elapsed > 0)
        e->tokens += (double) elapsed * tokens_per_us;
    e->last_refill_us = now;

    /* Burst ceiling: a bucket never banks more than one window's worth */
    if (e->tokens > (double) count)
        e->tokens = (double) count;

    if (e->tokens >= 1.0)
    {
        e->tokens -= 1.0;
        allowed = true;
    }
    else
    {
        double missing = 1.0 - e->tokens;

        e->rejected++;
        allowed = false;

        if (retry_after_ms)
        {
            double wait_ms = missing / tokens_per_us / 1000.0;

            *retry_after_ms = (wait_ms < 1.0) ? 1 : (int) (wait_ms + 0.5);
        }
    }

    LWLockRelease(lock);

    return allowed;
}
