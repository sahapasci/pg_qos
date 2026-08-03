/*
 * qos_stats.c - QoS statistics: collection and SQL-facing views
 *
 * Two halves:
 *
 *   Collection - cumulative counters per (database, role), kept in shared
 *   memory and bumped with plain atomics.  A counter needs no consistency
 *   with its neighbours, so no lock is taken on the hot path.
 *
 *   Reporting  - set-returning functions over the shared state (live backend
 *   activity, rate limit windows, CPU affinity, cumulative counters) plus a
 *   Prometheus exposition-format renderer.  These return OIDs; name
 *   resolution happens in the SQL views, following the pg_stat_statements
 *   userid/dbid convention.
 *
 * Counters live only in shared memory and reset when the postmaster
 * restarts.  Prometheus tolerates counter resets (rate() handles them), and
 * this avoids the file format compatibility burden pg_stat_statements carries.
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
#include "fmgr.h"
#include "funcapi.h"
#include "access/htup_details.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_database.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "portability/instr_time.h"
#include "storage/lwlock.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/tuplestore.h"

/*
 * InitMaterializedSRF() is the PG16+ spelling of what PG15 called
 * SetSingleFuncCall().  The extension targets PG15-PG18.
 */
#if PG_VERSION_NUM < 160000
#define InitMaterializedSRF(fcinfo, flags) SetSingleFuncCall((fcinfo), (flags))
#endif

PG_FUNCTION_INFO_V1(qos_reset_stats);
PG_FUNCTION_INFO_V1(qos_reset_stats_for);
PG_FUNCTION_INFO_V1(qos_stat);
PG_FUNCTION_INFO_V1(qos_stat_activity);
PG_FUNCTION_INFO_V1(qos_stat_rate);
PG_FUNCTION_INFO_V1(qos_stat_cpu);
PG_FUNCTION_INFO_V1(qos_prometheus_metrics);

/* Backend-local memo of our own stats slot */
static QoSSlotCache stat_slot_cache = QOS_SLOT_CACHE_INIT;

static void qos_stat_reset_entry(QoSRoleDbStats *e);

/*
 * Lowercase kind label for metrics and view columns.
 *
 * qos_rate_kind_name() returns the uppercase SQL spelling used in error
 * messages ("SELECT"); metric labels want the lowercase machine-readable form.
 */
static const char *
qos_kind_label(int kind)
{
    switch (kind)
    {
        case QOS_RATE_TX:     return "tx";
        case QOS_RATE_SELECT: return "select";
        case QOS_RATE_UPDATE: return "update";
        case QOS_RATE_DELETE: return "delete";
        case QOS_RATE_INSERT: return "insert";
        default:              return "unknown";
    }
}


/* ------------------------------------------------------------------------
 * Collection
 * ------------------------------------------------------------------------ */

/*
 * Return this backend's (database, role) counter block, creating it on first
 * use.  Returns NULL when statistics cannot be recorded - callers must treat
 * that as "skip counting", never as an error: losing a counter is not a
 * reason to fail a user's query.
 */
QoSRoleDbStats *
qos_stat_entry(void)
{
    int slot;

    if (!qos_shared_state)
        return NULL;

    slot = qos_find_or_create_slot(qos_shared_state->stat_slots,
                                   sizeof(QoSStatSlot),
                                   QOS_MAX_STAT_ENTRIES,
                                   MyDatabaseId, GetUserId(), -1,
                                   qos_shared_state->stat_locks,
                                   QOS_STAT_LOCK_STRIPES,
                                   NULL, NULL,
                                   &stat_slot_cache, "statistics");
    if (slot < 0)
        return NULL;

    return &qos_shared_state->stat_slots[slot].entry;
}

/*
 * Count one governed statement.
 */
void
qos_stat_count_statement(void)
{
    QoSRoleDbStats *e = qos_stat_entry();

    if (e)
        pg_atomic_fetch_add_u64(&e->total_statements, 1);
}

/*
 * Count one rejection: bumps both the specific reason and the total.
 */
void
qos_stat_count_rejection(QoSRejectCategory category, int kind)
{
    QoSRoleDbStats *e = qos_stat_entry();

    if (!e)
        return;

    switch (category)
    {
        case QOS_REJECT_CONCURRENT:
            if (kind >= 0 && kind < QOS_RATE_NKINDS)
                pg_atomic_fetch_add_u64(&e->concurrent_violations[kind], 1);
            break;
        case QOS_REJECT_RATE:
            if (kind >= 0 && kind < QOS_RATE_NKINDS)
                pg_atomic_fetch_add_u64(&e->rate_violations[kind], 1);
            break;
        case QOS_REJECT_WORK_MEM:
            pg_atomic_fetch_add_u64(&e->work_mem_violations, 1);
            break;
    }

    pg_atomic_fetch_add_u64(&e->rejected_total, 1);
}

static void
qos_stat_reset_entry(QoSRoleDbStats *e)
{
    int k;

    pg_atomic_write_u64(&e->total_statements, 0);
    pg_atomic_write_u64(&e->rejected_total, 0);
    pg_atomic_write_u64(&e->work_mem_violations, 0);

    for (k = 0; k < QOS_RATE_NKINDS; k++)
    {
        pg_atomic_write_u64(&e->concurrent_violations[k], 0);
        pg_atomic_write_u64(&e->rate_violations[k], 0);
    }
}

/*
 * qos_reset_stats() - zero every counter.
 *
 * Slots are not released, only zeroed: another backend may hold a memoised
 * index into one, and freeing it would let a later claim reuse the slot for a
 * different key underneath that backend.
 */
Datum
qos_reset_stats(PG_FUNCTION_ARGS)
{
    int i;

    if (!qos_shared_state)
        PG_RETURN_VOID();

    for (i = 0; i < QOS_MAX_STAT_ENTRIES; i++)
    {
        QoSRoleDbStats *e = &qos_shared_state->stat_slots[i].entry;

        if (e->database_oid != InvalidOid)
            qos_stat_reset_entry(e);
    }

    /* Rejection counters kept alongside the rate limit windows */
    for (i = 0; i < QOS_MAX_RATE_ENTRIES; i++)
    {
        QoSRateEntry *r = &qos_shared_state->rate_slots[i].entry;

        if (r->database_oid != InvalidOid)
            r->rejected = 0;
    }

    PG_RETURN_VOID();
}

/*
 * qos_reset_stats(datname, rolname) - zero the counters for one pair.
 *
 * Names rather than OIDs, to match what the views report.  An unknown name is
 * an error: silently resetting nothing would look like success.
 */
Datum
qos_reset_stats_for(PG_FUNCTION_ARGS)
{
    char *db_name = text_to_cstring(PG_GETARG_TEXT_PP(0));
    char *role_name = text_to_cstring(PG_GETARG_TEXT_PP(1));
    Oid target_db;
    Oid target_role;
    int i;

    target_db = get_database_oid(db_name, false);
    target_role = get_role_oid(role_name, false);

    if (!qos_shared_state)
        PG_RETURN_VOID();

    for (i = 0; i < QOS_MAX_STAT_ENTRIES; i++)
    {
        QoSRoleDbStats *e = &qos_shared_state->stat_slots[i].entry;

        if (e->database_oid == target_db && e->role_oid == target_role)
            qos_stat_reset_entry(e);
    }

    for (i = 0; i < QOS_MAX_RATE_ENTRIES; i++)
    {
        QoSRateEntry *r = &qos_shared_state->rate_slots[i].entry;

        if (r->database_oid == target_db && r->role_oid == target_role)
            r->rejected = 0;
    }

    PG_RETURN_VOID();
}


/* ------------------------------------------------------------------------
 * Reporting helpers
 * ------------------------------------------------------------------------ */

/*
 * Look up a database or role name.  Both catalogs are shared, so this
 * resolves names for every database in the cluster, not just the current one.
 * Dropped objects fall back to the numeric OID rather than vanishing from the
 * output - a metric that silently disappears is worse than one labelled by OID.
 */
static char *
qos_lookup_name(Oid oid, bool is_database)
{
    char *name;

    if (!OidIsValid(oid))
        return psprintf("%u", oid);

    name = is_database ? get_database_name(oid) : GetUserNameFromId(oid, true);

    if (name == NULL)
        return psprintf("%u", oid);

    return name;
}

/*
 * Current window state for a rate limit entry.
 *
 * The stored counter is only rolled over when a check runs, so an idle entry
 * would read as used up even though its window has long expired.  Apply the
 * same rollover qos_rate_check() would - without writing to shared memory -
 * so the reported numbers are what a statement arriving now would meet.
 */
static void
qos_stat_window_state(const QoSRateEntry *e, int window_ms,
                      uint32 *used, int *reset_ms)
{
    instr_time now;
    int64 now_us;
    int64 window_us;
    int64 elapsed;
    int64 remaining_us;

    if (window_ms <= 0)
    {
        *used = e->used;
        *reset_ms = 0;
        return;
    }

    INSTR_TIME_SET_CURRENT(now);
    now_us = (int64) INSTR_TIME_GET_MICROSEC(now);

    window_us = (int64) window_ms * 1000;
    elapsed = now_us - e->window_start_us;

    if (elapsed < 0 || elapsed >= window_us)
    {
        /* Window has expired: the next statement starts a fresh one */
        *used = 0;
        *reset_ms = window_ms;
        return;
    }

    remaining_us = window_us - elapsed;

    *used = e->used;
    *reset_ms = (int) ((remaining_us + 999) / 1000);
}

static const char *
qos_cmd_type_name(CmdType cmd)
{
    switch (cmd)
    {
        case CMD_SELECT: return "SELECT";
        case CMD_UPDATE: return "UPDATE";
        case CMD_INSERT: return "INSERT";
        case CMD_DELETE: return "DELETE";
        default:         return NULL;   /* idle / not a governed command */
    }
}


/* ------------------------------------------------------------------------
 * Set-returning functions
 * ------------------------------------------------------------------------ */

#define QOS_STAT_NCOLS 15

Datum
qos_stat(PG_FUNCTION_ARGS)
{
    ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    int i;

    InitMaterializedSRF(fcinfo, 0);

    if (!qos_shared_state)
        return (Datum) 0;

    for (i = 0; i < QOS_MAX_STAT_ENTRIES; i++)
    {
        QoSRoleDbStats *e = &qos_shared_state->stat_slots[i].entry;
        Datum values[QOS_STAT_NCOLS];
        bool nulls[QOS_STAT_NCOLS] = {0};
        int c = 0;
        int k;

        if (e->database_oid == InvalidOid)
            continue;

        pg_read_barrier();

        values[c++] = CStringGetTextDatum(qos_lookup_name(e->database_oid, true));
        values[c++] = CStringGetTextDatum(qos_lookup_name(e->role_oid, false));
        values[c++] = Int64GetDatum((int64) pg_atomic_read_u64(&e->total_statements));
        values[c++] = Int64GetDatum((int64) pg_atomic_read_u64(&e->rejected_total));

        for (k = 0; k < QOS_RATE_NKINDS; k++)
            values[c++] = Int64GetDatum((int64) pg_atomic_read_u64(&e->concurrent_violations[k]));
        for (k = 0; k < QOS_RATE_NKINDS; k++)
            values[c++] = Int64GetDatum((int64) pg_atomic_read_u64(&e->rate_violations[k]));

        values[c++] = Int64GetDatum((int64) pg_atomic_read_u64(&e->work_mem_violations));

        Assert(c == QOS_STAT_NCOLS);
        tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
    }

    return (Datum) 0;
}

Datum
qos_stat_activity(PG_FUNCTION_ARGS)
{
    ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    int i;

    InitMaterializedSRF(fcinfo, 0);

    if (!qos_shared_state)
        return (Datum) 0;

    LWLockAcquire(qos_shared_state->lock, LW_SHARED);

    for (i = 0; i < qos_shared_state->max_backends; i++)
    {
        QoSBackendStatus *b = &qos_shared_state->backend_status[i];
        Datum values[5];
        bool nulls[5] = {0};
        const char *cmd;

        if (b->pid == 0)
            continue;

        cmd = qos_cmd_type_name(b->cmd_type);

        values[0] = Int32GetDatum((int32) b->pid);
        values[1] = CStringGetTextDatum(qos_lookup_name(b->database_oid, true));
        values[2] = CStringGetTextDatum(qos_lookup_name(b->role_oid, false));
        if (cmd)
            values[3] = CStringGetTextDatum(cmd);
        else
            nulls[3] = true;
        values[4] = BoolGetDatum(b->in_transaction);

        tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
    }

    LWLockRelease(qos_shared_state->lock);

    return (Datum) 0;
}

Datum
qos_stat_rate(PG_FUNCTION_ARGS)
{
    ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    int i;

    InitMaterializedSRF(fcinfo, 0);

    if (!qos_shared_state)
        return (Datum) 0;

    for (i = 0; i < QOS_MAX_RATE_ENTRIES; i++)
    {
        QoSRateEntry *e = &qos_shared_state->rate_slots[i].entry;
        Datum values[7];
        bool nulls[7] = {0};
        QoSLimits limits;
        int count;
        int window_ms;
        uint32 used;
        int reset_ms;
        LWLock *lock;
        QoSRateEntry snapshot;

        if (e->database_oid == InvalidOid)
            continue;

        pg_read_barrier();

        if (e->kind < 0 || e->kind >= QOS_RATE_NKINDS)
            continue;

        /*
         * Take a consistent snapshot of used and window_start_us: they are
         * two fields updated together under the entry's lock, and rolling over
         * from a mismatched pair would report a nonsense level.
         */
        lock = &qos_shared_state->rate_locks[i % QOS_RATE_LOCK_STRIPES].lock;
        LWLockAcquire(lock, LW_SHARED);
        snapshot = *e;
        LWLockRelease(lock);

        qos_compute_effective_limits(snapshot.role_oid, snapshot.database_oid, &limits);
        count = limits.max_rate[snapshot.kind];
        window_ms = limits.max_rate_window_ms[snapshot.kind];
        if (count > 0 && window_ms <= 0)
            window_ms = QOS_RATE_WINDOW_DEFAULT_MS;

        values[0] = CStringGetTextDatum(qos_lookup_name(snapshot.database_oid, true));
        values[1] = CStringGetTextDatum(qos_lookup_name(snapshot.role_oid, false));
        values[2] = CStringGetTextDatum(qos_kind_label(snapshot.kind));

        if (count > 0)
        {
            qos_stat_window_state(&snapshot, window_ms, &used, &reset_ms);

            values[3] = Int32GetDatum(count);
            values[4] = Int32GetDatum(window_ms);
            values[6] = Int32GetDatum(reset_ms);
        }
        else
        {
            /*
             * The entry exists but the limit has since been removed: without a
             * window there is nothing to roll over against, so report the
             * stored counter as-is and leave the window columns NULL.
             */
            used = snapshot.used;

            nulls[3] = true;
            nulls[4] = true;
            nulls[6] = true;
        }

        values[5] = Int32GetDatum((int32) used);

        tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
    }

    return (Datum) 0;
}

Datum
qos_stat_cpu(PG_FUNCTION_ARGS)
{
    ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
    int i;

    InitMaterializedSRF(fcinfo, 0);

    if (!qos_shared_state)
        return (Datum) 0;

    LWLockAcquire(qos_shared_state->lock, LW_SHARED);

    for (i = 0; i < MAX_AFFINITY_ENTRIES; i++)
    {
        QoSAffinityEntry *a = &qos_shared_state->affinity_entries[i];
        Datum values[3];
        bool nulls[3] = {0};
        Datum *core_datums;
        int j;
        int ncores;

        if (a->database_oid == InvalidOid)
            continue;

        ncores = a->num_cores;
        if (ncores > MAX_CORES_PER_ENTRY)
            ncores = MAX_CORES_PER_ENTRY;

        values[0] = CStringGetTextDatum(qos_lookup_name(a->database_oid, true));
        values[1] = CStringGetTextDatum(qos_lookup_name(a->role_oid, false));

        core_datums = (Datum *) palloc(sizeof(Datum) * Max(ncores, 1));
        for (j = 0; j < ncores; j++)
            core_datums[j] = Int32GetDatum(a->assigned_cores[j]);

        values[2] = PointerGetDatum(construct_array(core_datums, ncores, INT4OID,
                                                    sizeof(int32), true, TYPALIGN_INT));

        tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
        pfree(core_datums);
    }

    LWLockRelease(qos_shared_state->lock);

    return (Datum) 0;
}


/* ------------------------------------------------------------------------
 * Prometheus exposition format
 * ------------------------------------------------------------------------ */

/*
 * Escape a label value.
 *
 * Prometheus requires backslash, double quote and newline to be escaped in a
 * label value.  Role and database names are user-controlled and may contain
 * all three (CREATE ROLE "a""b" is legal SQL), and an unescaped quote does
 * not corrupt one metric - it makes the whole scrape unparseable.
 */
static void
qos_append_escaped_label(StringInfo buf, const char *value)
{
    const char *p;

    for (p = value; *p != '\0'; p++)
    {
        switch (*p)
        {
            case '\\': appendStringInfoString(buf, "\\\\"); break;
            case '"':  appendStringInfoString(buf, "\\\""); break;
            case '\n': appendStringInfoString(buf, "\\n"); break;
            default:   appendStringInfoChar(buf, *p); break;
        }
    }
}

static void
qos_append_labelled_metric(StringInfo buf, const char *metric,
                           const char *datname, const char *rolname,
                           const char *extra_label, const char *extra_value,
                           const char *value)
{
    appendStringInfoString(buf, metric);
    appendStringInfoString(buf, "{datname=\"");
    qos_append_escaped_label(buf, datname);
    appendStringInfoString(buf, "\",rolname=\"");
    qos_append_escaped_label(buf, rolname);
    appendStringInfoChar(buf, '"');

    if (extra_label)
    {
        appendStringInfo(buf, ",%s=\"", extra_label);
        qos_append_escaped_label(buf, extra_value);
        appendStringInfoChar(buf, '"');
    }

    appendStringInfo(buf, "} %s\n", value);
}

Datum
qos_prometheus_metrics(PG_FUNCTION_ARGS)
{
    StringInfoData buf;
    int i;
    int k;

    initStringInfo(&buf);

    if (!qos_shared_state)
        PG_RETURN_TEXT_P(cstring_to_text_with_len(buf.data, buf.len));

    /* --- cumulative counters --- */

    appendStringInfoString(&buf,
        "# HELP qos_statements_total Statements processed under QoS governance\n"
        "# TYPE qos_statements_total counter\n");

    for (i = 0; i < QOS_MAX_STAT_ENTRIES; i++)
    {
        QoSRoleDbStats *e = &qos_shared_state->stat_slots[i].entry;
        char *datname;
        char *rolname;

        if (e->database_oid == InvalidOid)
            continue;
        pg_read_barrier();

        datname = qos_lookup_name(e->database_oid, true);
        rolname = qos_lookup_name(e->role_oid, false);

        qos_append_labelled_metric(&buf, "qos_statements_total", datname, rolname,
                                   NULL, NULL,
                                   psprintf(UINT64_FORMAT,
                                            pg_atomic_read_u64(&e->total_statements)));
    }

    appendStringInfoString(&buf,
        "# HELP qos_rejected_total Operations rejected by a QoS limit\n"
        "# TYPE qos_rejected_total counter\n");

    for (i = 0; i < QOS_MAX_STAT_ENTRIES; i++)
    {
        QoSRoleDbStats *e = &qos_shared_state->stat_slots[i].entry;
        char *datname;
        char *rolname;

        if (e->database_oid == InvalidOid)
            continue;
        pg_read_barrier();

        datname = qos_lookup_name(e->database_oid, true);
        rolname = qos_lookup_name(e->role_oid, false);

        for (k = 0; k < QOS_RATE_NKINDS; k++)
        {
            qos_append_labelled_metric(&buf, "qos_rejected_total", datname, rolname,
                                       "reason",
                                       psprintf("concurrent_%s", qos_kind_label(k)),
                                       psprintf(UINT64_FORMAT,
                                                pg_atomic_read_u64(&e->concurrent_violations[k])));
            qos_append_labelled_metric(&buf, "qos_rejected_total", datname, rolname,
                                       "reason",
                                       psprintf("rate_%s", qos_kind_label(k)),
                                       psprintf(UINT64_FORMAT,
                                                pg_atomic_read_u64(&e->rate_violations[k])));
        }

        qos_append_labelled_metric(&buf, "qos_rejected_total", datname, rolname,
                                   "reason", "work_mem",
                                   psprintf(UINT64_FORMAT,
                                            pg_atomic_read_u64(&e->work_mem_violations)));
    }

    /* --- live rate limit windows --- */

    appendStringInfoString(&buf,
        "# HELP qos_rate_used Operations counted in the current window\n"
        "# TYPE qos_rate_used gauge\n"
        "# HELP qos_rate_limit Configured operations per window\n"
        "# TYPE qos_rate_limit gauge\n"
        "# HELP qos_rate_window_reset_seconds Time left until the window resets\n"
        "# TYPE qos_rate_window_reset_seconds gauge\n");

    for (i = 0; i < QOS_MAX_RATE_ENTRIES; i++)
    {
        QoSRateEntry *e = &qos_shared_state->rate_slots[i].entry;
        QoSRateEntry snapshot;
        QoSLimits limits;
        LWLock *lock;
        char *datname;
        char *rolname;
        int count;
        int window_ms;
        uint32 used;
        int reset_ms;

        if (e->database_oid == InvalidOid)
            continue;
        pg_read_barrier();

        if (e->kind < 0 || e->kind >= QOS_RATE_NKINDS)
            continue;

        lock = &qos_shared_state->rate_locks[i % QOS_RATE_LOCK_STRIPES].lock;
        LWLockAcquire(lock, LW_SHARED);
        snapshot = *e;
        LWLockRelease(lock);

        qos_compute_effective_limits(snapshot.role_oid, snapshot.database_oid, &limits);
        count = limits.max_rate[snapshot.kind];
        window_ms = limits.max_rate_window_ms[snapshot.kind];
        if (count > 0 && window_ms <= 0)
            window_ms = QOS_RATE_WINDOW_DEFAULT_MS;

        if (count <= 0)
            continue;       /* limit removed; nothing meaningful to export */

        datname = qos_lookup_name(snapshot.database_oid, true);
        rolname = qos_lookup_name(snapshot.role_oid, false);

        qos_stat_window_state(&snapshot, window_ms, &used, &reset_ms);

        qos_append_labelled_metric(&buf, "qos_rate_used", datname, rolname,
                                   "kind", qos_kind_label(snapshot.kind),
                                   psprintf("%u", used));
        qos_append_labelled_metric(&buf, "qos_rate_limit", datname, rolname,
                                   "kind", qos_kind_label(snapshot.kind),
                                   psprintf("%d", count));
        qos_append_labelled_metric(&buf, "qos_rate_window_reset_seconds", datname, rolname,
                                   "kind", qos_kind_label(snapshot.kind),
                                   psprintf("%.3f", (double) reset_ms / 1000.0));
    }

    /* --- live activity --- */

    appendStringInfoString(&buf,
        "# HELP qos_active_backends Backends currently running a governed operation\n"
        "# TYPE qos_active_backends gauge\n");

    {
        /*
         * Aggregate per (database, role, cmd_type) in one pass over the
         * backend array, reusing the stats slots as the key space so the
         * label sets match the counter metrics above.
         */
        int *counts;
        Oid *dbs;
        Oid *roles;
        int nkeys = 0;
        int capacity = QOS_MAX_STAT_ENTRIES;

        counts = (int *) palloc0(sizeof(int) * capacity * QOS_RATE_NKINDS);
        dbs = (Oid *) palloc(sizeof(Oid) * capacity);
        roles = (Oid *) palloc(sizeof(Oid) * capacity);

        LWLockAcquire(qos_shared_state->lock, LW_SHARED);

        for (i = 0; i < qos_shared_state->max_backends; i++)
        {
            QoSBackendStatus *b = &qos_shared_state->backend_status[i];
            int kind;
            int key;

            if (b->pid == 0)
                continue;

            switch (b->cmd_type)
            {
                case CMD_SELECT: kind = QOS_RATE_SELECT; break;
                case CMD_UPDATE: kind = QOS_RATE_UPDATE; break;
                case CMD_DELETE: kind = QOS_RATE_DELETE; break;
                case CMD_INSERT: kind = QOS_RATE_INSERT; break;
                default: continue;
            }

            for (key = 0; key < nkeys; key++)
            {
                if (dbs[key] == b->database_oid && roles[key] == b->role_oid)
                    break;
            }

            if (key == nkeys)
            {
                if (nkeys >= capacity)
                    continue;
                dbs[nkeys] = b->database_oid;
                roles[nkeys] = b->role_oid;
                nkeys++;
            }

            counts[key * QOS_RATE_NKINDS + kind]++;
        }

        LWLockRelease(qos_shared_state->lock);

        for (i = 0; i < nkeys; i++)
        {
            char *datname = qos_lookup_name(dbs[i], true);
            char *rolname = qos_lookup_name(roles[i], false);

            for (k = 0; k < QOS_RATE_NKINDS; k++)
            {
                if (k == QOS_RATE_TX)
                    continue;   /* backend_status tracks statements, not tx here */

                qos_append_labelled_metric(&buf, "qos_active_backends", datname, rolname,
                                           "cmd_type", qos_kind_label(k),
                                           psprintf("%d",
                                                    counts[i * QOS_RATE_NKINDS + k]));
            }
        }

        pfree(counts);
        pfree(dbs);
        pfree(roles);
    }

    PG_RETURN_TEXT_P(cstring_to_text_with_len(buf.data, buf.len));
}
