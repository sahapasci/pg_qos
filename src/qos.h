/*
 * qos.h - PostgreSQL Quality of Service (QoS) Extension Header
 *
 * This header file contains the main definitions, constants, and function
 * prototypes for the PostgreSQL QoS extension, providing a unified interface
 * for quality of service management in PostgreSQL databases.
 *
 * Author:  M.Atif Ceylan
 * Company: AppstoniA OÜ
 * Created: October 02, 2025
 * Version: 1.0
 * License: See LICENSE file in the project root
 *
 * Copyright (c) 2025 AppstoniA OÜ
 * All rights reserved.
 */

#ifndef QOS_H
#define QOS_H

#include "postgres.h"
#include "fmgr.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "nodes/nodes.h"

/*
 * Rate limit kinds - index into the shared rate-limiter entries.
 * Keep in sync with qos_rate_kind_name() in hooks_rate.c.
 */
typedef enum QoSRateKind
{
    QOS_RATE_TX = 0,
    QOS_RATE_SELECT,
    QOS_RATE_UPDATE,
    QOS_RATE_DELETE,
    QOS_RATE_INSERT,
    QOS_RATE_NKINDS
} QoSRateKind;

/* Bounds for qos.max_*_rate_window (milliseconds) */
#define QOS_RATE_WINDOW_MIN_MS      100
#define QOS_RATE_WINDOW_MAX_MS      (24 * 60 * 60 * 1000)   /* 1 day */
#define QOS_RATE_WINDOW_DEFAULT_MS  1000

/* QoS Limits Structure */
typedef struct QoSLimits
{
    int64   work_mem_limit;        /* Max work_mem in bytes (-1 = no limit) */
    int     cpu_core_limit;        /* Max CPU cores (-1 = no limit) */
    int     max_concurrent_tx;     /* Max concurrent transactions (-1 = no limit) */

    /* Statement-specific concurrent execution limits */
    int     max_concurrent_select; /* Max concurrent SELECT statements (-1 = no limit) */
    int     max_concurrent_update; /* Max concurrent UPDATE statements (-1 = no limit) */
    int     max_concurrent_delete; /* Max concurrent DELETE statements (-1 = no limit) */
    int     max_concurrent_insert; /* Max concurrent INSERT statements (-1 = no limit) */
    int     work_mem_error_level;  /* QoS work_mem violation severity (-1 = unset) */

    /*
     * Time-windowed (rate) limits.  Each entry is a (count, window) pair:
     * "at most <count> operations per <window> milliseconds".  A count of -1
     * disables the limit; the window defaults to QOS_RATE_WINDOW_DEFAULT_MS
     * when only the count is configured.  Indexed by QoSRateKind so the
     * enforcement code can stay table-driven.
     */
    int     max_rate[QOS_RATE_NKINDS];             /* -1 = no limit */
    int     max_rate_window_ms[QOS_RATE_NKINDS];   /* -1 = unset */
} QoSLimits;

typedef enum QoSWorkMemErrorLevel
{
    QOS_WORK_MEM_ERROR_WARNING = 0,
    QOS_WORK_MEM_ERROR_ERROR = 1
} QoSWorkMemErrorLevel;

/* QoS Statistics */
typedef struct QoSStats
{
    uint64  total_queries;
    uint64  throttled_queries;
    uint64  rejected_queries;
    uint64  work_mem_violations;
    uint64  cpu_violations;
    uint64  concurrent_tx_violations;
    uint64  concurrent_select_violations;
    uint64  concurrent_update_violations;
    uint64  concurrent_delete_violations;
    uint64  concurrent_insert_violations;
    uint64  rate_violations[QOS_RATE_NKINDS];
} QoSStats;

/*
 * Token-bucket state for one (database, role, kind) triple.
 *
 * Refill is lazy: nothing runs periodically, each check adds
 * elapsed * (count / window) tokens before deciding.  That makes the cost of
 * a check independent of the window length.
 *
 * Padded to a cache line because every backend sharing a (db, role) writes
 * the same entry on every statement; without padding neighbouring entries
 * would false-share.
 */
typedef struct QoSRateEntry
{
    Oid     database_oid;       /* InvalidOid = free slot */
    Oid     role_oid;
    int     kind;               /* QoSRateKind */
    double  tokens;             /* Currently available tokens */
    int64   last_refill_us;     /* MONOTONIC microseconds, not wall clock */
    uint64  rejected;
} QoSRateEntry;

typedef union QoSRateSlot
{
    QoSRateEntry entry;
    char         pad[PG_CACHE_LINE_SIZE];
} QoSRateSlot;

#define QOS_MAX_RATE_ENTRIES    512
#define QOS_RATE_LOCK_STRIPES   8

/* CPU Affinity Tracking Entry */
#define MAX_CORES_PER_ENTRY 64
typedef struct QoSAffinityEntry
{
    Oid     database_oid;
    Oid     role_oid;
    int     num_cores;                      /* Number of cores assigned */
    int     assigned_cores[MAX_CORES_PER_ENTRY]; /* Array of assigned core IDs */
} QoSAffinityEntry;

#define MAX_AFFINITY_ENTRIES 128

/* Backend Status Entry for Concurrency Tracking */
typedef struct QoSBackendStatus
{
    pid_t   pid;            /* Process ID (0 if slot unused) */
    Oid     role_oid;       /* User OID */
    Oid     database_oid;   /* Database OID */
    CmdType cmd_type;       /* Current command type (CMD_UNKNOWN if none) */
    bool    in_transaction; /* Is in transaction? */
} QoSBackendStatus;

/* Shared State */
typedef struct QoSSharedState
{
    LWLock     *lock;
    QoSStats    stats;
    int         settings_epoch;     /* Bumped on ALTER ROLE/DB SET qos.* to notify sessions */
    int         next_cpu_core;      /* Round-robin counter for CPU core assignment (protected by lock) */
    int         max_backends;       /* MaxBackends value at startup */
    QoSAffinityEntry affinity_entries[MAX_AFFINITY_ENTRIES]; /* Track which db+role combinations have affinity set */

    /*
     * Rate limiter state.  Protected by rate_locks[slot % QOS_RATE_LOCK_STRIPES],
     * NOT by the ->lock above: rate checks must not contend with the
     * O(max_backends) backend_status scan done for concurrency limits.
     */
    LWLockPadded *rate_locks;
    QoSRateSlot   rate_slots[QOS_MAX_RATE_ENTRIES];

    /*
     * Per-backend status array for robust concurrency tracking.
     * Indexed by BackendId (0-based).
     * Must be last member for flexible array sizing.
     */
    QoSBackendStatus backend_status[FLEXIBLE_ARRAY_MEMBER];
} QoSSharedState;

/* Global variables */
extern QoSSharedState *qos_shared_state;
extern bool qos_enabled;

/* Shared hint text listing every accepted qos.* parameter name */
extern const char *qos_valid_param_hint;

/* exported functions */
extern void _PG_init(void);
extern void _PG_fini(void);

/* helper C functions callable from SQL */
extern Datum qos_version(PG_FUNCTION_ARGS);
extern Datum qos_get_stats(PG_FUNCTION_ARGS);
extern Datum qos_reset_stats(PG_FUNCTION_ARGS);

/* Function declarations */
extern QoSLimits qos_get_role_limits(Oid roleId);
extern QoSLimits qos_get_database_limits(Oid dbId);
extern QoSLimits qos_get_role_db_limits(Oid roleId, Oid dbId);
extern int64 qos_parse_memory_unit(const char *str);
extern bool qos_is_valid_qos_param_name(const char *name);
extern bool qos_apply_qos_param_value(QoSLimits *limits, const char *name,
                                      const char *value, bool strict);
extern void qos_limits_init_unset(QoSLimits *limits);

/* rate limiting (hooks_rate.c) */
extern const char *qos_rate_kind_name(int kind);
extern bool qos_rate_check(int kind, int count, int window_ms,
                           int *retry_after_ms);
/* cache/epoch notifications */
extern void qos_notify_settings_change(void);

#endif /* QOS_H */