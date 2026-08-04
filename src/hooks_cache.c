/*
 * hooks_cache.c - QoS Cache Management
 *
 * This file implements the caching mechanism for QoS limits,
 * including invalidation callbacks and cache refresh logic.
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
#include "utils/inval.h"
#include "utils/syscache.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_database.h"
#include "catalog/pg_db_role_setting.h"
#include "access/xact.h"

/*
 * Cached QoS limits - invalidated via syscache callback.
 * Initialised to "all unset" by qos_init_cache(); until then limits_cached is
 * false so qos_get_cached_limits() always refreshes first.
 */
static QoSLimits cached_limits;
static Oid cached_user_id = InvalidOid;
static Oid cached_db_id = InvalidOid;
static bool limits_cached = false;
static int last_seen_epoch = -1; /* session-local view of shared settings epoch */

/*
 * Invalidation callback for pg_db_role_setting changes
 * This is called automatically when ALTER ROLE/DATABASE changes settings
 */
static void
qos_invalidate_cache_callback(Datum arg, int cacheid, uint32 hashvalue)
{
    /* Invalidate cache when database or role settings change */
    if (cacheid == DATABASEOID || cacheid == AUTHOID)
    {
        limits_cached = false;
        elog(DEBUG1, "qos: cache invalidated via syscache (cacheid=%d)", cacheid);
    }
}

/*
 * Relcache invalidation callback for pg_db_role_setting changes
 * Captures ALTER ROLE/DATABASE SET updates
 */
static void
qos_relcache_callback(Datum arg, Oid relid)
{
    /* Invalidate cache on any relcache event; pg_db_role_setting may pass InvalidOid */
    limits_cached = false;
    elog(DEBUG1, "qos: cache invalidated via relcache (relid=%u)", relid);
}

/*
 * Initialize cache system and register callbacks
 */
void
qos_init_cache(void)
{
    qos_limits_init_unset(&cached_limits);

    /* Register syscache invalidation callbacks for role and database changes */
    CacheRegisterSyscacheCallback(DATABASEOID, qos_invalidate_cache_callback, (Datum) 0);
    CacheRegisterSyscacheCallback(AUTHOID, qos_invalidate_cache_callback, (Datum) 0);

    /* Register relcache callback for pg_db_role_setting changes (ALTER ... SET) */
    CacheRegisterRelcacheCallback(qos_relcache_callback, (Datum) 0);
}

/*
 * Invalidate cache (public interface)
 */
void
qos_invalidate_cache(void)
{
    limits_cached = false;
}

/*
 * Notify that QoS settings changed (called from utility hook)
 * Bumps shared epoch so all sessions detect the change promptly
 */
void
qos_notify_settings_change(void)
{
    if (!qos_shared_state)
        return;

    LWLockAcquire(qos_shared_state->lock, LW_EXCLUSIVE);
    qos_shared_state->settings_epoch++;
    LWLockRelease(qos_shared_state->lock);

    elog(DEBUG1, "qos: settings_epoch bumped to %d", qos_shared_state->settings_epoch);
}

/*
 * Merge two rate limits, keeping the more restrictive (count, window) PAIR.
 *
 * Rate limits must not be merged field-by-field the way the scalar limits are:
 * taking min(count) and min(window) independently produces a limit that exists
 * in neither source.  For example role=10/1s (10/sec) and db=100/500ms
 * (200/sec) would yield 10/500ms = 20/sec, which is *less* restrictive than
 * the 10/sec the role asked for.
 *
 * Compare normalised rates without dividing: a is stricter than b iff
 * a_count/a_win < b_count/b_win  <=>  a_count * b_win < b_count * a_win.
 * A window of -1 means "unset" and is treated as the default.
 */
static void
qos_pick_min_rate(int *out_count, int *out_window,
                  int a_count, int a_window,
                  int b_count, int b_window)
{
    if (a_window <= 0)
        a_window = QOS_RATE_WINDOW_DEFAULT_MS;
    if (b_window <= 0)
        b_window = QOS_RATE_WINDOW_DEFAULT_MS;

    if (a_count < 0 && b_count < 0)
    {
        *out_count = -1;
        *out_window = -1;
        return;
    }

    if (a_count < 0)
    {
        *out_count = b_count;
        *out_window = b_window;
        return;
    }

    if (b_count < 0)
    {
        *out_count = a_count;
        *out_window = a_window;
        return;
    }

    if ((int64) a_count * (int64) b_window <= (int64) b_count * (int64) a_window)
    {
        *out_count = a_count;
        *out_window = a_window;
    }
    else
    {
        *out_count = b_count;
        *out_window = b_window;
    }
}

/*
 * Resolve the effective limits for an arbitrary (role, database) pair.
 *
 * Callable for any pair, not just the current session: the statistics views
 * use it to report the limit alongside each rate limit window, and it works across
 * databases because pg_db_role_setting is a shared catalog.
 */
void
qos_compute_effective_limits(Oid roleId, Oid dbId, QoSLimits *out)
{
    QoSLimits role_limits;
    QoSLimits db_limits;
    QoSLimits role_db_limits;
    int k;

    qos_limits_init_unset(out);

    /* Query catalogs for all 3 possible setting scopes */
    role_limits = qos_get_role_limits(roleId);
    db_limits = qos_get_database_limits(dbId);
    role_db_limits = qos_get_role_db_limits(roleId, dbId);

    /*
     * Calculate limits (most restrictive wins).
     * Three sources:
     *   role_limits    = ALTER ROLE x SET qos.*           (db=0, role=X)
     *   db_limits      = ALTER DATABASE y SET qos.*       (db=Y, role=0)
     *   role_db_limits = ALTER ROLE x IN DATABASE y SET   (db=Y, role=X)
     * Take the minimum of all that are set (>= 0).
     */
    #define PICK_MIN(a, b) \
        ((a) >= 0 && (b) >= 0 ? Min((a), (b)) : ((a) >= 0 ? (a) : (b)))

    #define CALC_LIMIT(field) \
        do { \
            int64 _tmp = PICK_MIN(role_limits.field, db_limits.field); \
            out->field = PICK_MIN(_tmp, role_db_limits.field); \
        } while (0)

    CALC_LIMIT(work_mem_limit);
    CALC_LIMIT(cpu_core_limit);
    CALC_LIMIT(max_concurrent_tx);
    CALC_LIMIT(max_concurrent_select);
    CALC_LIMIT(max_concurrent_update);
    CALC_LIMIT(max_concurrent_delete);
    CALC_LIMIT(max_concurrent_insert);
    CALC_LIMIT(work_mem_error_level);

    #undef CALC_LIMIT
    #undef PICK_MIN

    /* Rate limits merge as (count, window) pairs - see qos_pick_min_rate */
    for (k = 0; k < QOS_RATE_NKINDS; k++)
    {
        int c, w;

        qos_pick_min_rate(&c, &w,
                          role_limits.max_rate[k],
                          role_limits.max_rate_window_ms[k],
                          db_limits.max_rate[k],
                          db_limits.max_rate_window_ms[k]);
        qos_pick_min_rate(&c, &w, c, w,
                          role_db_limits.max_rate[k],
                          role_db_limits.max_rate_window_ms[k]);

        out->max_rate[k] = c;
        out->max_rate_window_ms[k] = w;
    }
}

/*
 * Initialize or refresh cached limits for current session
 * Cache is automatically invalidated via syscache callback when configs change
 */
static void
qos_refresh_cached_limits(void)
{
    Oid current_user_id;
    Oid current_db_id;

    current_user_id = GetUserId();
    current_db_id = MyDatabaseId;

    /* If shared settings epoch changed, force invalidate */
    if (qos_shared_state && last_seen_epoch != qos_shared_state->settings_epoch)
    {
        elog(DEBUG1, "qos: settings_epoch changed %d -> %d, invalidating cache",
             last_seen_epoch, qos_shared_state->settings_epoch);
        limits_cached = false;
        last_seen_epoch = qos_shared_state->settings_epoch;
    }

    /* If cache still valid for same user/db, return */
    if (limits_cached && cached_user_id == current_user_id && cached_db_id == current_db_id)
        return;

    qos_compute_effective_limits(current_user_id, current_db_id, &cached_limits);

    /*
     * Note: rate limit counters in shared memory are intentionally NOT reset
     * here.  They are keyed by (db, role, kind), not by the limit value, and
     * qos_rate_check() compares `used` against whatever count is in force at
     * the time - so lowering a limit takes effect on the very next check
     * without losing the shared state other backends are using.
     */

    /* Update cache metadata */
    cached_user_id = current_user_id;
    cached_db_id = current_db_id;
    limits_cached = true;
    
    elog(DEBUG1, "qos: effective limits - work_mem=%ld cpu=%d tx=%d sel=%d upd=%d del=%d ins=%d errlvl=%d (user=%u db=%u)",
        cached_limits.work_mem_limit, cached_limits.cpu_core_limit,
        cached_limits.max_concurrent_tx, cached_limits.max_concurrent_select,
        cached_limits.max_concurrent_update, cached_limits.max_concurrent_delete,
        cached_limits.max_concurrent_insert, cached_limits.work_mem_error_level,
        cached_user_id, cached_db_id);

    elog(DEBUG1, "qos: effective rate limits - tx=%d/%dms sel=%d/%dms upd=%d/%dms del=%d/%dms ins=%d/%dms",
        cached_limits.max_rate[QOS_RATE_TX], cached_limits.max_rate_window_ms[QOS_RATE_TX],
        cached_limits.max_rate[QOS_RATE_SELECT], cached_limits.max_rate_window_ms[QOS_RATE_SELECT],
        cached_limits.max_rate[QOS_RATE_UPDATE], cached_limits.max_rate_window_ms[QOS_RATE_UPDATE],
        cached_limits.max_rate[QOS_RATE_DELETE], cached_limits.max_rate_window_ms[QOS_RATE_DELETE],
        cached_limits.max_rate[QOS_RATE_INSERT], cached_limits.max_rate_window_ms[QOS_RATE_INSERT]);
}

/*
 * Get cached effective limits (refreshes if needed)
 */
QoSLimits
qos_get_cached_limits(void)
{
    qos_refresh_cached_limits();
    return cached_limits;
}
