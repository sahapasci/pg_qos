/*
 * hooks_rate.c - Time-windowed (rate) limit enforcement
 *
 * Implements the qos.max_*_rate limits ("<count>/<window>", e.g. "100/1s"):
 * "at most N operations per W milliseconds" for transactions and for each
 * statement type, counted per (database, role) across all backends.
 *
 * The algorithm is a fixed window counter: time is cut into <window>-long
 * slices and each slice allows <count> operations, so "3/50s" means three
 * operations in every 50 second slice and the whole allowance returns at once
 * when the slice ends.
 *
 * The known trade-off is the boundary effect: <count> operations at the end of
 * one slice and <count> more at the start of the next put 2x <count> through in
 * a short span.  A sliding window would avoid that but needs one timestamp per
 * operation in shared memory, which does not fit a fixed-size slot table.
 *
 * Rollover is lazy.  Nothing runs periodically - each check compares now
 * against the slice it holds - so the CPU cost of a check is independent of
 * the window length: a 100ms window costs exactly as much as a one hour one.
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
#include "port/atomics.h"
#include "storage/lwlock.h"

/*
 * Backend-local memo of our counter indexes, so a check does not rescan
 * the slot array.  Managed by qos_find_or_create_slot().
 */
static QoSSlotCache rate_slot_cache = QOS_SLOT_CACHE_INIT;

static int64 qos_rate_now_us(void);
static void qos_rate_init_entry(void *entry, void *arg);

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
 * Initialise a freshly claimed counter.
 *
 * The first window starts now with the full allowance unspent: an idle system
 * has all of it available.  Starting the counter part-used would reject the
 * very first statement after a limit is configured and make every fresh entry
 * behave as an outage.
 */
static void
qos_rate_init_entry(void *entry, void *arg)
{
    QoSRateEntry *e = (QoSRateEntry *) entry;

    (void) arg;

    e->window_start_us = qos_rate_now_us();
    e->used = 0;
    e->rejected = 0;
}

/*
 * Count one operation of `kind` against the limit `count` per `window_ms`.
 *
 * Returns true when the operation is allowed (the counter was incremented),
 * false when the current window is used up.  On rejection *retry_after_ms, if
 * supplied, is set to how long is left until the window resets.
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
    int64 window_us;
    bool allowed;

    if (retry_after_ms)
        *retry_after_ms = 0;

    if (!qos_shared_state || count <= 0 || window_ms <= 0)
        return true;

    if (kind < 0 || kind >= QOS_RATE_NKINDS)
        return true;

    slot = qos_find_or_create_slot(qos_shared_state->rate_slots,
                                   sizeof(QoSRateSlot),
                                   QOS_MAX_RATE_ENTRIES,
                                   MyDatabaseId, GetUserId(), kind,
                                   qos_shared_state->rate_locks,
                                   QOS_RATE_LOCK_STRIPES,
                                   qos_rate_init_entry, NULL,
                                   &rate_slot_cache, "rate limiter");
    if (slot < 0)
        return true;    /* fail open */

    e = &qos_shared_state->rate_slots[slot].entry;
    lock = &qos_shared_state->rate_locks[slot % QOS_RATE_LOCK_STRIPES].lock;

    window_us = (int64) window_ms * 1000;

    LWLockAcquire(lock, LW_EXCLUSIVE);

    now = qos_rate_now_us();
    elapsed = now - e->window_start_us;

    if (elapsed < 0)
    {
        /* Impossible with a monotonic clock, but restart rather than let a
         * window stretch to the far future if one ever does go backwards. */
        e->window_start_us = now;
        e->used = 0;
    }
    else if (elapsed >= window_us)
    {
        /*
         * Advance by whole windows instead of snapping the start to now: that
         * keeps the slices on the grid laid down by the first operation, so a
         * steady stream of traffic cannot drag the boundary forward and turn
         * the window into a rolling one.
         */
        e->window_start_us += (elapsed / window_us) * window_us;
        e->used = 0;
    }

    if (e->used < (uint32) count)
    {
        e->used++;
        allowed = true;
    }
    else
    {
        e->rejected++;
        allowed = false;

        if (retry_after_ms)
        {
            int64 remaining_us = e->window_start_us + window_us - now;

            /* Round up: reporting the truncated value would send the client
             * back a hair too early, into the same exhausted window. */
            *retry_after_ms = (remaining_us <= 1000)
                ? 1 : (int) ((remaining_us + 999) / 1000);
        }
    }

    LWLockRelease(lock);

    return allowed;
}
