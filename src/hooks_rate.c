/*
 * hooks_rate.c - Time-windowed (rate) limit enforcement
 *
 * Implements the qos.max_*_rate limits ("<count>/<window>", e.g. "100/1s"):
 * "at most N operations per W milliseconds" for transactions and for each
 * statement type, counted per (database, role) across all backends.
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
#include "port/atomics.h"
#include "storage/lwlock.h"

/*
 * Backend-local memo of our token bucket indexes, so a check does not rescan
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
 * Initialise a freshly claimed bucket.
 *
 * A new bucket starts FULL, the standard token bucket convention: an idle
 * system has its whole burst allowance available.  Starting empty would
 * reject the very first statement after a limit is configured and make every
 * fresh bucket behave as an outage.
 */
static void
qos_rate_init_entry(void *entry, void *arg)
{
    QoSRateEntry *e = (QoSRateEntry *) entry;
    int initial_tokens = *(int *) arg;

    e->tokens = (double) initial_tokens;
    e->last_refill_us = qos_rate_now_us();
    e->rejected = 0;
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

    slot = qos_find_or_create_slot(qos_shared_state->rate_slots,
                                   sizeof(QoSRateSlot),
                                   QOS_MAX_RATE_ENTRIES,
                                   MyDatabaseId, GetUserId(), kind,
                                   qos_shared_state->rate_locks,
                                   QOS_RATE_LOCK_STRIPES,
                                   qos_rate_init_entry, &count,
                                   &rate_slot_cache, "rate limiter");
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
