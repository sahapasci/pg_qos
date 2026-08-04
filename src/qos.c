/*
 * qos.c - PostgreSQL Quality of Service (QoS) Extension Main Module
 *
 * This file contains the main implementation of the PostgreSQL QoS extension,
 * including initialization, configuration management, and the extension's
 * primary functions for quality of service control.
 *
 * Author:  M.Atif Ceylan
 * Company: AppstoniA OÜ
 * Created: October 02, 2025
 * Version: 1.1
 * License: See LICENSE file in the project root
 *
 * Copyright (c) 2025 AppstoniA OÜ
 * All rights reserved.
 */

#include "postgres.h"
#include "fmgr.h"
#include "qos.h"
#include "hooks.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "storage/ipc.h"
#include "miscadmin.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_database.h"
#include "catalog/pg_db_role_setting.h"
#include "utils/syscache.h"
#include "access/xact.h"
#include "access/transam.h"
#include "utils/array.h"
#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
#include "catalog/indexing.h"
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>

PG_MODULE_MAGIC;

/* Global state for QoS tracking */
QoSSharedState *qos_shared_state = NULL;

/* GUC variables */
bool qos_enabled = true;

/* Hook save variables */
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static shmem_request_hook_type prev_shmem_request_hook = NULL;

/* Forward declarations */
static void qos_shmem_request(void);
static void qos_shmem_startup(void);
static void parse_role_configs(ArrayType *configs, QoSLimits *limits);

const char *qos_valid_param_hint =
    "Valid parameters: qos.work_mem_limit, qos.cpu_core_limit, "
    "qos.max_concurrent_tx, qos.max_concurrent_select, "
    "qos.max_concurrent_update, qos.max_concurrent_delete, "
    "qos.max_concurrent_insert, qos.work_mem_error_level, "
    "qos.max_tx_rate, qos.max_select_rate, qos.max_update_rate, "
    "qos.max_delete_rate, qos.max_insert_rate";

/*
 * Parameter names for the time-windowed (rate) limits, indexed by QoSRateKind.
 * Each limit is configured as a single "<count>/<window>" value, e.g. "100/1s".
 */
static const char *qos_rate_params[QOS_RATE_NKINDS] = {
    "qos.max_tx_rate",
    "qos.max_select_rate",
    "qos.max_update_rate",
    "qos.max_delete_rate",
    "qos.max_insert_rate"
};

static char *qos_trim_whitespace(char *str);
static bool qos_lookup_rate_param(const char *name, int *kind);
static bool qos_parse_duration_value(const char *value_str, int *out_ms,
                                     const char *param_name,
                                     const char *display_value, bool strict);
static bool qos_parse_rate_value(const char *value_str, int *out_count,
                                 int *out_window_ms,
                                 const char *param_name, bool strict);
static bool qos_parse_int32_value(const char *value_str, int *out,
                                  int min_value, int max_value,
                                  bool allow_negative_one,
                                  const char *param_name, bool strict);
static bool qos_parse_memory_value(const char *value_str, int64 *out,
                                   const char *param_name, bool strict);
static bool qos_parse_work_mem_error_level(const char *value_str,
                                           const char *param_name, bool strict);
static bool qos_is_valid_qos_param_name_internal(const char *name);

bool qos_is_valid_qos_param_name(const char *name);
bool qos_apply_qos_param_value(QoSLimits *limits, const char *name,
                               const char *value, bool strict);

PG_FUNCTION_INFO_V1(qos_version);
PG_FUNCTION_INFO_V1(qos_get_stats);

Datum
qos_version(PG_FUNCTION_ARGS)
{
    PG_RETURN_TEXT_P(cstring_to_text("PostgreSQL QoS Resource Governor 1.1"));
}

/*
 * Deprecated as of 1.1; the qos_stat* views replace it.
 *
 * The SQL function is dropped by qos--1.0--1.1.sql, but this symbol MUST stay
 * in the module: the released qos--1.0.sql binds to it, and removing it makes
 * that script fail to load against this binary - breaking CREATE EXTENSION
 * for a pinned 1.0, and any restore path that installs one.
 */
Datum
qos_get_stats(PG_FUNCTION_ARGS)
{
    PG_RETURN_TEXT_P(cstring_to_text("deprecated: query the qos_stat view instead"));
}

/*
 * Request shared memory space for QoS tracking
 */
static void
qos_shmem_request(void)
{
    Size size;

    if (prev_shmem_request_hook)
        prev_shmem_request_hook();

    /* Calculate size needed for shared state + per-backend status array */
    size = sizeof(QoSSharedState);
    size = add_size(size, mul_size(MaxBackends, sizeof(QoSBackendStatus)));
    
    RequestAddinShmemSpace(MAXALIGN(size));
    RequestNamedLWLockTranche("qos", 1);

    /*
     * Separate tranche for the rate limiter.  Sharing the "qos" lock would
     * serialise every rate check behind the O(max_backends) backend_status
     * scan performed for the concurrency limits.
     */
    RequestNamedLWLockTranche("qos_rate", QOS_RATE_LOCK_STRIPES);

    /* Serialises statistics slot creation only; counters use atomics */
    RequestNamedLWLockTranche("qos_stat", QOS_STAT_LOCK_STRIPES);
}

/*
 * Initialize shared memory
 */
static void
qos_shmem_startup(void)
{
    bool found;
    Size size;

    if (prev_shmem_startup_hook)
        prev_shmem_startup_hook();

    /* Calculate size needed for shared state + per-backend status array */
    size = sizeof(QoSSharedState);
    size = add_size(size, mul_size(MaxBackends, sizeof(QoSBackendStatus)));

    LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
    
    qos_shared_state = ShmemInitStruct("qos_shared_state",
                                        size,
                                        &found);
    
    if (!found)
    {
        int i;
        
        /* Initialize shared state */
        memset(qos_shared_state, 0, size);
        qos_shared_state->lock = &(GetNamedLWLockTranche("qos")->lock);
        qos_shared_state->rate_locks = GetNamedLWLockTranche("qos_rate");
        qos_shared_state->stat_locks = GetNamedLWLockTranche("qos_stat");
        qos_shared_state->settings_epoch = 0;
        qos_shared_state->next_cpu_core = 0;
        qos_shared_state->max_backends = MaxBackends;
        
        /* Initialize affinity tracking array */
        for (i = 0; i < MAX_AFFINITY_ENTRIES; i++)
        {
            qos_shared_state->affinity_entries[i].database_oid = InvalidOid;
            qos_shared_state->affinity_entries[i].role_oid = InvalidOid;
            qos_shared_state->affinity_entries[i].num_cores = 0;
        }
        
        /* Initialize rate limiter slots (InvalidOid marks a free slot) */
        for (i = 0; i < QOS_MAX_RATE_ENTRIES; i++)
        {
            qos_shared_state->rate_slots[i].entry.database_oid = InvalidOid;
            qos_shared_state->rate_slots[i].entry.role_oid = InvalidOid;
            qos_shared_state->rate_slots[i].entry.kind = -1;
        }

        /*
         * Initialize statistics slots.  Every atomic is initialised here,
         * once, rather than when a slot is claimed: pg_atomic_init_u64 is not
         * safe against concurrent access, and doing it at claim time would
         * race with readers scanning the array.
         */
        for (i = 0; i < QOS_MAX_STAT_ENTRIES; i++)
        {
            QoSRoleDbStats *e = &qos_shared_state->stat_slots[i].entry;
            int k;

            e->database_oid = InvalidOid;
            e->role_oid = InvalidOid;
            pg_atomic_init_u64(&e->total_statements, 0);
            pg_atomic_init_u64(&e->rejected_total, 0);
            pg_atomic_init_u64(&e->work_mem_violations, 0);
            for (k = 0; k < QOS_RATE_NKINDS; k++)
            {
                pg_atomic_init_u64(&e->concurrent_violations[k], 0);
                pg_atomic_init_u64(&e->rate_violations[k], 0);
            }
        }

        /* Initialize backend status array */
        for (i = 0; i < MaxBackends; i++)
        {
            qos_shared_state->backend_status[i].pid = 0;
            qos_shared_state->backend_status[i].role_oid = InvalidOid;
            qos_shared_state->backend_status[i].database_oid = InvalidOid;
            qos_shared_state->backend_status[i].cmd_type = CMD_UNKNOWN;
            qos_shared_state->backend_status[i].in_transaction = false;
        }
    }
    
    LWLockRelease(AddinShmemInitLock);
}

/*
 * Parse configuration array and extract QoS limits
 */
static void
parse_role_configs(ArrayType *configs, QoSLimits *limits)
{
    int nelems;
    Datum *elems;
    bool *nulls;
    int i;
    
    if (!configs)
        return;
        
    nelems = ArrayGetNItems(ARR_NDIM(configs), ARR_DIMS(configs));
    deconstruct_array(configs, TEXTOID, -1, false, TYPALIGN_INT,
                    &elems, &nulls, &nelems);
    
    for (i = 0; i < nelems; i++)
    {
        char *config_str;
        char *name;
        char *value;

        if (nulls[i])
            continue;

        config_str = TextDatumGetCString(elems[i]);

        /* Parse "name=value" format */
        name = config_str;
        value = strchr(config_str, '=');
        if (value)
        {
            *value = '\0';
            value++;

            name = qos_trim_whitespace(name);
            value = qos_trim_whitespace(value);

            if (pg_strncasecmp(name, "qos.", 4) == 0)
                (void) qos_apply_qos_param_value(limits, name, value, false);
        }
        else if (pg_strncasecmp(config_str, "qos.", 4) == 0)
        {
            elog(DEBUG1, "qos: invalid parameter format \"%s\" (expected name=value)",
                 config_str);
        }

        pfree(config_str);
    }

    pfree(elems);
    pfree(nulls);
}

/*
 * Reset every limit to "unset".
 *
 * Kept in one place so that adding a field to QoSLimits cannot silently leave
 * one of the three catalog lookups (role / database / role+database)
 * initialising garbage.
 */
void
qos_limits_init_unset(QoSLimits *limits)
{
    int i;

    if (limits == NULL)
        return;

    limits->work_mem_limit = -1;
    limits->cpu_core_limit = -1;
    limits->max_concurrent_tx = -1;
    limits->max_concurrent_select = -1;
    limits->max_concurrent_update = -1;
    limits->max_concurrent_delete = -1;
    limits->max_concurrent_insert = -1;
    limits->work_mem_error_level = -1;

    for (i = 0; i < QOS_RATE_NKINDS; i++)
    {
        limits->max_rate[i] = -1;
        limits->max_rate_window_ms[i] = -1;
    }
}

/*
 * Map a qos.max_*_rate parameter name to its kind.
 * Returns false if the name is not a rate parameter.
 */
static bool
qos_lookup_rate_param(const char *name, int *kind)
{
    int i;

    if (name == NULL)
        return false;

    for (i = 0; i < QOS_RATE_NKINDS; i++)
    {
        if (strcmp(name, qos_rate_params[i]) == 0)
        {
            *kind = i;
            return true;
        }
    }

    return false;
}

/*
 * Parse the window half of a rate value: an integer with an optional
 * ms/s/min suffix.  A bare number is interpreted as milliseconds.
 *
 * The lower bound exists to stop nonsensical configurations, not for
 * performance reasons: window rollover is lazy, so a 100ms window costs
 * exactly as much CPU per check as a 1 hour window.
 *
 * display_value is the full "<count>/<window>" the user wrote; error messages
 * quote that rather than the isolated window half, which on its own would look
 * like something the user never typed.
 */
static bool
qos_parse_duration_value(const char *value_str, int *out_ms,
                         const char *param_name, const char *display_value,
                         bool strict)
{
    char *endptr;
    long base;
    int64 multiplier = 1;
    int64 result;
    const char *suffix;

    if (value_str == NULL || *value_str == '\0')
        goto invalid;

    errno = 0;
    base = strtol(value_str, &endptr, 10);
    if (endptr == value_str || errno == ERANGE)
        goto invalid;

    suffix = endptr;
    while (*suffix != '\0' && isspace((unsigned char) *suffix))
        suffix++;

    if (*suffix != '\0')
    {
        if (pg_strcasecmp(suffix, "ms") == 0)
            multiplier = 1;
        else if (pg_strcasecmp(suffix, "s") == 0 || pg_strcasecmp(suffix, "sec") == 0)
            multiplier = 1000;
        else if (pg_strcasecmp(suffix, "min") == 0)
            multiplier = 60 * 1000;
        else
            goto invalid;
    }

    if (base < 0)
        goto invalid;

    result = (int64) base * multiplier;

    if (result < QOS_RATE_WINDOW_MIN_MS || result > QOS_RATE_WINDOW_MAX_MS)
    {
        if (strict)
            ereport(ERROR,
                    (errmsg("qos: invalid value for %s: \"%s\"", param_name, display_value),
                     errdetail("Window must be between %d ms and %d ms.",
                               QOS_RATE_WINDOW_MIN_MS, QOS_RATE_WINDOW_MAX_MS)));
        else
            elog(DEBUG1, "qos: out-of-range value for %s: \"%s\" (ignored)",
                 param_name, display_value);
        return false;
    }

    if (out_ms)
        *out_ms = (int) result;
    return true;

invalid:
    if (strict)
        ereport(ERROR,
                (errmsg("qos: invalid value for %s: \"%s\"", param_name, display_value),
                 errdetail("Expected a duration with optional unit (ms, s, min) "
                           "after the slash.")));
    else
        elog(DEBUG1, "qos: invalid value for %s: \"%s\" (ignored)", param_name,
             display_value);
    return false;
}

/*
 * Parse a rate limit: "<count>/<window>", e.g. "100/1s" or "10/500ms".
 * "-1" disables the limit.
 *
 * The slash is mandatory: a bare count would leave the window implicit, and an
 * implicit window is exactly what made the old two-parameter form easy to
 * misconfigure.
 */
static bool
qos_parse_rate_value(const char *value_str, int *out_count, int *out_window_ms,
                     const char *param_name, bool strict)
{
    char *copy;
    char *count_part;
    char *window_part;
    char *slash;
    int count = -1;
    int window_ms = -1;

    if (value_str == NULL || *value_str == '\0')
        goto invalid;

    copy = pstrdup(value_str);
    count_part = qos_trim_whitespace(copy);

    /* The one value that carries no window */
    if (strcmp(count_part, "-1") == 0)
    {
        pfree(copy);
        if (out_count)
            *out_count = -1;
        if (out_window_ms)
            *out_window_ms = -1;
        return true;
    }

    slash = strchr(count_part, '/');
    if (slash == NULL)
    {
        pfree(copy);
        goto invalid;
    }

    *slash = '\0';
    window_part = qos_trim_whitespace(slash + 1);
    count_part = qos_trim_whitespace(count_part);

    if (!qos_parse_int32_value(count_part, &count, 1, INT_MAX, false,
                               param_name, false))
    {
        pfree(copy);
        goto invalid;
    }

    if (!qos_parse_duration_value(window_part, &window_ms, param_name,
                                  value_str, strict))
    {
        pfree(copy);
        return false;
    }

    pfree(copy);

    if (out_count)
        *out_count = count;
    if (out_window_ms)
        *out_window_ms = window_ms;
    return true;

invalid:
    if (strict)
        ereport(ERROR,
                (errmsg("qos: invalid value for %s: \"%s\"", param_name, value_str),
                 errdetail("Expected \"<count>/<window>\", for example \"100/1s\", "
                           "or -1 to disable.")));
    else
        elog(DEBUG1, "qos: invalid value for %s: \"%s\" (ignored)", param_name,
             value_str);
    return false;
}

static char *
qos_trim_whitespace(char *str)
{
    char *end;

    if (str == NULL)
        return NULL;

    while (*str != '\0' && isspace((unsigned char) *str))
        str++;

    if (*str == '\0')
        return str;

    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char) *end))
    {
        *end = '\0';
        end--;
    }

    return str;
}

static bool
qos_parse_int32_value(const char *value_str, int *out,
                      int min_value, int max_value,
                      bool allow_negative_one,
                      const char *param_name, bool strict)
{
    char *endptr;
    long value;

    if (value_str == NULL || *value_str == '\0')
        goto invalid;

    errno = 0;
    value = strtol(value_str, &endptr, 10);
    if (endptr == value_str || *endptr != '\0' || errno == ERANGE)
        goto invalid;

    if (allow_negative_one && value == -1)
    {
        if (out)
            *out = -1;
        return true;
    }

    if (value < min_value || value > max_value)
        goto invalid;

    if (out)
        *out = (int) value;
    return true;

invalid:
    if (strict)
        ereport(ERROR,
                (errmsg("qos: invalid value for %s: \"%s\"", param_name, value_str)));
    else
        elog(DEBUG1, "qos: invalid value for %s: \"%s\" (ignored)", param_name, value_str);
    return false;
}

static bool
qos_parse_memory_value(const char *value_str, int64 *out,
                       const char *param_name, bool strict)
{
    char *endptr;
    long long base;
    int64 multiplier = 1;
    const char *suffix;

    if (value_str == NULL || *value_str == '\0')
        goto invalid;

    errno = 0;
    base = strtoll(value_str, &endptr, 10);
    if (endptr == value_str || errno == ERANGE)
        goto invalid;

    suffix = endptr;
    while (*suffix != '\0' && isspace((unsigned char) *suffix))
        suffix++;

    if (*suffix != '\0')
    {
        if (pg_strcasecmp(suffix, "kb") == 0 || pg_strcasecmp(suffix, "k") == 0)
            multiplier = 1024L;
        else if (pg_strcasecmp(suffix, "mb") == 0 || pg_strcasecmp(suffix, "m") == 0)
            multiplier = 1024L * 1024L;
        else if (pg_strcasecmp(suffix, "gb") == 0 || pg_strcasecmp(suffix, "g") == 0)
            multiplier = 1024L * 1024L * 1024L;
        else
            goto invalid;
    }

    if (base == -1 && *suffix != '\0')
        goto invalid;

    if (base < -1)
        goto invalid;

    if (base > 0 && multiplier > 1)
    {
        if ((int64) base > (INT64_MAX / multiplier))
            goto invalid;
    }

    if (out)
        *out = (int64) base * multiplier;
    return true;

invalid:
    if (strict)
        ereport(ERROR,
                (errmsg("qos: invalid value for %s: \"%s\"", param_name, value_str),
                 errdetail("Expected a number with optional unit (kB, MB, GB) or -1.")));
    else
        elog(DEBUG1, "qos: invalid value for %s: \"%s\" (ignored)", param_name, value_str);
    return false;
}

static bool
qos_parse_work_mem_error_level(const char *value_str,
                               const char *param_name, bool strict)
{
    if (value_str == NULL || *value_str == '\0')
        goto invalid;

    if (pg_strcasecmp(value_str, "warning") == 0 || pg_strcasecmp(value_str, "error") == 0)
        return true;

invalid:
    if (strict)
        ereport(ERROR,
                (errmsg("qos: invalid value for %s: \"%s\"", param_name, value_str),
                 errdetail("Expected \"warning\" or \"error\".")));
    else
        elog(DEBUG1, "qos: invalid value for %s: \"%s\" (ignored)", param_name, value_str);
    return false;
}

static bool
qos_is_valid_qos_param_name_internal(const char *name)
{
    int kind;

    if (name == NULL)
        return false;

    if (qos_lookup_rate_param(name, &kind))
        return true;

    if (strcmp(name, "qos.work_mem_limit") == 0)
        return true;
    if (strcmp(name, "qos.cpu_core_limit") == 0)
        return true;
    if (strcmp(name, "qos.max_concurrent_tx") == 0)
        return true;
    if (strcmp(name, "qos.max_concurrent_select") == 0)
        return true;
    if (strcmp(name, "qos.max_concurrent_update") == 0)
        return true;
    if (strcmp(name, "qos.max_concurrent_delete") == 0)
        return true;
    if (strcmp(name, "qos.max_concurrent_insert") == 0)
        return true;
    if (strcmp(name, "qos.enabled") == 0)
        return true;
    if (strcmp(name, "qos.work_mem_error_level") == 0)
        return true;

    return false;
}

bool
qos_is_valid_qos_param_name(const char *name)
{
    return qos_is_valid_qos_param_name_internal(name);
}

bool
qos_apply_qos_param_value(QoSLimits *limits, const char *name,
                          const char *value, bool strict)
{
    int parsed_int = -1;
    int64 parsed_mem = -1;
    char *value_copy = NULL;
    char *trimmed_value = NULL;
    int rate_kind = 0;

    if (name == NULL)
        return false;

    if (pg_strncasecmp(name, "qos.", 4) != 0)
        return false;

    if (!qos_is_valid_qos_param_name_internal(name))
    {
        if (strict)
            ereport(ERROR,
                    (errmsg("qos: invalid parameter name \"%s\"", name),
                     errhint("%s", qos_valid_param_hint)));
        else
            elog(DEBUG1, "qos: invalid parameter name \"%s\" (ignored)", name);
        return false;
    }

    if (strcmp(name, "qos.enabled") == 0)
        return true;

    if (value == NULL)
    {
        if (strict)
            ereport(ERROR,
                    (errmsg("qos: missing value for parameter \"%s\"", name)));
        else
            elog(DEBUG1, "qos: missing value for parameter \"%s\" (ignored)", name);
        return false;
    }

    value_copy = pstrdup(value);
    trimmed_value = qos_trim_whitespace(value_copy);

    /* Time-windowed (rate) limits: a single "<count>/<window>" value */
    if (qos_lookup_rate_param(name, &rate_kind))
    {
        int parsed_window = -1;

        if (!qos_parse_rate_value(trimmed_value, &parsed_int, &parsed_window,
                                  name, strict))
        {
            pfree(value_copy);
            return false;
        }
        if (limits)
        {
            limits->max_rate[rate_kind] = parsed_int;
            limits->max_rate_window_ms[rate_kind] = parsed_window;
        }
        pfree(value_copy);
        return true;
    }

    if (strcmp(name, "qos.work_mem_limit") == 0)
    {
        if (!qos_parse_memory_value(trimmed_value, &parsed_mem, name, strict))
        {
            pfree(value_copy);
            return false;
        }
        if (limits)
            limits->work_mem_limit = parsed_mem;
        pfree(value_copy);
        return true;
    }

    if (strcmp(name, "qos.cpu_core_limit") == 0)
    {
        if (!qos_parse_int32_value(trimmed_value, &parsed_int, 0, INT_MAX, true, name, strict))
        {
            pfree(value_copy);
            return false;
        }
        if (limits)
            limits->cpu_core_limit = parsed_int;
        pfree(value_copy);
        return true;
    }

    if (strcmp(name, "qos.max_concurrent_tx") == 0)
    {
        if (!qos_parse_int32_value(trimmed_value, &parsed_int, 0, INT_MAX, true, name, strict))
        {
            pfree(value_copy);
            return false;
        }
        if (limits)
            limits->max_concurrent_tx = parsed_int;
        pfree(value_copy);
        return true;
    }

    if (strcmp(name, "qos.max_concurrent_select") == 0)
    {
        if (!qos_parse_int32_value(trimmed_value, &parsed_int, 0, INT_MAX, true, name, strict))
        {
            pfree(value_copy);
            return false;
        }
        if (limits)
            limits->max_concurrent_select = parsed_int;
        pfree(value_copy);
        return true;
    }

    if (strcmp(name, "qos.max_concurrent_update") == 0)
    {
        if (!qos_parse_int32_value(trimmed_value, &parsed_int, 0, INT_MAX, true, name, strict))
        {
            pfree(value_copy);
            return false;
        }
        if (limits)
            limits->max_concurrent_update = parsed_int;
        pfree(value_copy);
        return true;
    }

    if (strcmp(name, "qos.max_concurrent_delete") == 0)
    {
        if (!qos_parse_int32_value(trimmed_value, &parsed_int, 0, INT_MAX, true, name, strict))
        {
            pfree(value_copy);
            return false;
        }
        if (limits)
            limits->max_concurrent_delete = parsed_int;
        pfree(value_copy);
        return true;
    }

    if (strcmp(name, "qos.max_concurrent_insert") == 0)
    {
        if (!qos_parse_int32_value(trimmed_value, &parsed_int, 0, INT_MAX, true, name, strict))
        {
            pfree(value_copy);
            return false;
        }
        if (limits)
            limits->max_concurrent_insert = parsed_int;
        pfree(value_copy);
        return true;
    }

    if (strcmp(name, "qos.work_mem_error_level") == 0)
    {
        if (!qos_parse_work_mem_error_level(trimmed_value, name, strict))
        {
            pfree(value_copy);
            return false;
        }
        if (limits)
            limits->work_mem_error_level = (pg_strcasecmp(trimmed_value, "error") == 0)
                                             ? QOS_WORK_MEM_ERROR_ERROR
                                             : QOS_WORK_MEM_ERROR_WARNING;
        pfree(value_copy);
        return true;
    }

    if (value_copy)
        pfree(value_copy);
    return false;
}

/*
 * Get QoS limits for current role using pg_db_role_setting
 */
QoSLimits
qos_get_role_limits(Oid roleId)
{
    QoSLimits limits;
    Relation pg_db_role_setting_rel;
    ScanKeyData scankey[2];
    SysScanDesc scan;
    HeapTuple tuple;
    
    /* Set defaults */
    qos_limits_init_unset(&limits);
    
    /* Open pg_db_role_setting catalog */
    pg_db_role_setting_rel = table_open(DbRoleSettingRelationId, AccessShareLock);
    
    /* Scan for this role's settings (setdatabase = 0 means all databases) */
    ScanKeyInit(&scankey[0],
                Anum_pg_db_role_setting_setdatabase,
                BTEqualStrategyNumber, F_OIDEQ,
                ObjectIdGetDatum(InvalidOid));
    ScanKeyInit(&scankey[1],
                Anum_pg_db_role_setting_setrole,
                BTEqualStrategyNumber, F_OIDEQ,
                ObjectIdGetDatum(roleId));
    
    scan = systable_beginscan(pg_db_role_setting_rel, DbRoleSettingDatidRolidIndexId,
                              true, NULL, 2, scankey);
    
    tuple = systable_getnext(scan);
    if (HeapTupleIsValid(tuple))
    {
        bool isnull;
        Datum configDatum;
        
        configDatum = heap_getattr(tuple, Anum_pg_db_role_setting_setconfig,
                                  RelationGetDescr(pg_db_role_setting_rel), &isnull);
        
        if (!isnull)
        {
            ArrayType *configs = DatumGetArrayTypeP(configDatum);
            parse_role_configs(configs, &limits);
            
            /* Free detoasted copy if it was created */
            if ((Pointer) configs != DatumGetPointer(configDatum))
                pfree(configs);
        }
    }
    
    systable_endscan(scan);
    table_close(pg_db_role_setting_rel, AccessShareLock);
    
    return limits;
}

/*
 * Get QoS limits for current database using pg_db_role_setting
 */
QoSLimits
qos_get_database_limits(Oid dbId)
{
    QoSLimits limits;
    Relation pg_db_role_setting_rel;
    ScanKeyData scankey[2];
    SysScanDesc scan;
    HeapTuple tuple;
    
    /* Set defaults */
    qos_limits_init_unset(&limits);
    
    /* Open pg_db_role_setting catalog */
    pg_db_role_setting_rel = table_open(DbRoleSettingRelationId, AccessShareLock);
    
    /* Scan for this database's settings (setrole = 0 means all roles) */
    ScanKeyInit(&scankey[0],
                Anum_pg_db_role_setting_setdatabase,
                BTEqualStrategyNumber, F_OIDEQ,
                ObjectIdGetDatum(dbId));
    ScanKeyInit(&scankey[1],
                Anum_pg_db_role_setting_setrole,
                BTEqualStrategyNumber, F_OIDEQ,
                ObjectIdGetDatum(InvalidOid));
    
    scan = systable_beginscan(pg_db_role_setting_rel, DbRoleSettingDatidRolidIndexId,
                              true, NULL, 2, scankey);
    
    tuple = systable_getnext(scan);
    if (HeapTupleIsValid(tuple))
    {
        bool isnull;
        Datum configDatum;
        
        configDatum = heap_getattr(tuple, Anum_pg_db_role_setting_setconfig,
                                  RelationGetDescr(pg_db_role_setting_rel), &isnull);
        
        if (!isnull)
        {
            ArrayType *configs = DatumGetArrayTypeP(configDatum);
            parse_role_configs(configs, &limits);
            
            /* Free detoasted copy if it was created */
            if ((Pointer) configs != DatumGetPointer(configDatum))
                pfree(configs);
        }
    }
    
    systable_endscan(scan);
    table_close(pg_db_role_setting_rel, AccessShareLock);
    
    return limits;
}

/*
 * Get QoS limits for a specific role+database combination using pg_db_role_setting
 * This handles the ALTER ROLE x IN DATABASE y SET qos.* case
 */
QoSLimits
qos_get_role_db_limits(Oid roleId, Oid dbId)
{
    QoSLimits limits;
    Relation pg_db_role_setting_rel;
    ScanKeyData scankey[2];
    SysScanDesc scan;
    HeapTuple tuple;
    
    /* Set defaults */
    qos_limits_init_unset(&limits);
    
    /* Skip if either OID is invalid */
    if (!OidIsValid(roleId) || !OidIsValid(dbId))
        return limits;
    
    /* Open pg_db_role_setting catalog */
    pg_db_role_setting_rel = table_open(DbRoleSettingRelationId, AccessShareLock);
    
    /* Scan for this specific role+database combination */
    ScanKeyInit(&scankey[0],
                Anum_pg_db_role_setting_setdatabase,
                BTEqualStrategyNumber, F_OIDEQ,
                ObjectIdGetDatum(dbId));
    ScanKeyInit(&scankey[1],
                Anum_pg_db_role_setting_setrole,
                BTEqualStrategyNumber, F_OIDEQ,
                ObjectIdGetDatum(roleId));
    
    scan = systable_beginscan(pg_db_role_setting_rel, DbRoleSettingDatidRolidIndexId,
                              true, NULL, 2, scankey);
    
    tuple = systable_getnext(scan);
    if (HeapTupleIsValid(tuple))
    {
        bool isnull;
        Datum configDatum;
        
        configDatum = heap_getattr(tuple, Anum_pg_db_role_setting_setconfig,
                                  RelationGetDescr(pg_db_role_setting_rel), &isnull);
        
        if (!isnull)
        {
            ArrayType *configs = DatumGetArrayTypeP(configDatum);
            parse_role_configs(configs, &limits);
            
            /* Free detoasted copy if it was created */
            if ((Pointer) configs != DatumGetPointer(configDatum))
                pfree(configs);
        }
    }
    
    systable_endscan(scan);
    table_close(pg_db_role_setting_rel, AccessShareLock);
    
    return limits;
}

/*
 * Parse memory unit string (e.g., "64MB", "1GB")
 * Public function - can be used by other modules
 */
int64
qos_parse_memory_unit(const char *str)
{
    char *endptr;
    int64 value = strtol(str, &endptr, 10);
    
    if (*endptr != '\0')
    {
        if (strcasecmp(endptr, "kb") == 0 || strcasecmp(endptr, "k") == 0)
            value *= 1024L;
        else if (strcasecmp(endptr, "mb") == 0 || strcasecmp(endptr, "m") == 0)
            value *= 1024L * 1024L;
        else if (strcasecmp(endptr, "gb") == 0 || strcasecmp(endptr, "g") == 0)
            value *= 1024L * 1024L * 1024L;
    }
    
    return value;
}

/* Module load/unload */
void _PG_init(void)
{
    if (!process_shared_preload_libraries_in_progress)
    {
        ereport(ERROR,
                (errmsg("qos must be loaded via shared_preload_libraries")));
        return;
    }

    /* Define GUC variables */
    DefineCustomBoolVariable("qos.enabled",
                            "Enable QoS resource governor",
                            NULL,
                            &qos_enabled,
                            true,
                            PGC_SIGHUP,
                            0,
                            NULL, NULL, NULL);

    /* Register shmem hooks */
    prev_shmem_request_hook = shmem_request_hook;
    shmem_request_hook = qos_shmem_request;
    
    prev_shmem_startup_hook = shmem_startup_hook;
    shmem_startup_hook = qos_shmem_startup;

    /* Register execution hooks */
    qos_register_hooks();

    elog(INFO, "PostgreSQL QoS Resource Governor loaded");
}

void _PG_fini(void)
{
    /* Unregister hooks */
    qos_unregister_hooks();
    
    elog(INFO, "PostgreSQL QoS Resource Governor unloaded");
}