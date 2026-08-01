-- qos--1.0--1.1.sql
-- Upgrade from QoS extension 1.0 to 1.1
--
-- 1.1 adds:
--   * time-windowed (rate) limits: qos.max_*_rate, configured as
--     '<count>/<window>' (e.g. '100/1s') and enforced in the C module through
--     pg_db_role_setting, so they need no SQL objects of their own
--   * observability: per-(database, role) counters, live activity views and
--     a Prometheus renderer
--
-- NOTE: 1.1 grows the shared memory state, so a PostgreSQL restart is
-- required - ALTER EXTENSION alone is not sufficient.

\echo Use "ALTER EXTENSION qos UPDATE TO '1.1'" to load this file. \quit

-- qos_get_stats() was a stub that always returned 'not implemented yet'.
-- The views below replace it.  The C symbol stays in the module so that
-- qos--1.0.sql keeps loading against this binary.
DROP FUNCTION IF EXISTS qos_get_stats();

-- ---------------------------------------------------------------------------
-- Cumulative counters per (database, role)
--
-- Counters live in shared memory only and reset when the postmaster restarts.
-- A database or role dropped since the counters were recorded is reported by
-- its OID rather than dropped from the output.
-- ---------------------------------------------------------------------------
CREATE FUNCTION qos_stat(
    OUT datname                     text,
    OUT rolname                     text,
    OUT total_statements            bigint,
    OUT rejected_total              bigint,
    OUT concurrent_tx_violations    bigint,
    OUT concurrent_select_violations bigint,
    OUT concurrent_update_violations bigint,
    OUT concurrent_delete_violations bigint,
    OUT concurrent_insert_violations bigint,
    OUT rate_tx_violations          bigint,
    OUT rate_select_violations      bigint,
    OUT rate_update_violations      bigint,
    OUT rate_delete_violations      bigint,
    OUT rate_insert_violations      bigint,
    OUT work_mem_violations         bigint
)
RETURNS SETOF record
LANGUAGE C STRICT VOLATILE PARALLEL SAFE
AS '$libdir/qos', 'qos_stat';

CREATE VIEW qos_stat AS SELECT * FROM qos_stat();

-- ---------------------------------------------------------------------------
-- Live backend activity
-- ---------------------------------------------------------------------------
CREATE FUNCTION qos_stat_activity(
    OUT pid             int,
    OUT datname         text,
    OUT rolname         text,
    OUT cmd_type        text,
    OUT in_transaction  boolean
)
RETURNS SETOF record
LANGUAGE C STRICT VOLATILE PARALLEL SAFE
AS '$libdir/qos', 'qos_stat_activity';

CREATE VIEW qos_stat_activity AS SELECT * FROM qos_stat_activity();

-- ---------------------------------------------------------------------------
-- Live token buckets
--
-- tokens_available is the level a statement arriving now would find: the
-- stored value is refreshed lazily, so it is refilled before being reported.
-- rate_limit is NULL when a bucket outlives the setting that created it.
-- ---------------------------------------------------------------------------
CREATE FUNCTION qos_stat_rate(
    OUT datname             text,
    OUT rolname             text,
    OUT kind                text,
    OUT rate_limit          int,
    OUT window_ms           int,
    OUT tokens_available    float8
)
RETURNS SETOF record
LANGUAGE C STRICT VOLATILE PARALLEL SAFE
AS '$libdir/qos', 'qos_stat_rate';

CREATE VIEW qos_stat_rate AS SELECT * FROM qos_stat_rate();

-- ---------------------------------------------------------------------------
-- CPU affinity assignments (Linux only; empty elsewhere)
-- ---------------------------------------------------------------------------
CREATE FUNCTION qos_stat_cpu(
    OUT datname         text,
    OUT rolname         text,
    OUT assigned_cores  int[]
)
RETURNS SETOF record
LANGUAGE C STRICT VOLATILE PARALLEL SAFE
AS '$libdir/qos', 'qos_stat_cpu';

CREATE VIEW qos_stat_cpu AS SELECT * FROM qos_stat_cpu();

-- ---------------------------------------------------------------------------
-- Prometheus exposition format
-- ---------------------------------------------------------------------------
CREATE FUNCTION qos_prometheus_metrics()
RETURNS text
LANGUAGE C STRICT VOLATILE PARALLEL SAFE
AS '$libdir/qos', 'qos_prometheus_metrics';

-- ---------------------------------------------------------------------------
-- Reset
-- ---------------------------------------------------------------------------
CREATE FUNCTION qos_reset_stats(datname text, rolname text)
RETURNS void
LANGUAGE C STRICT VOLATILE
AS '$libdir/qos', 'qos_reset_stats_for';

-- ---------------------------------------------------------------------------
-- Access control
--
-- These expose activity and identities from every database in the cluster,
-- so they follow the convention used by the pg_stat_* views: readable by
-- pg_monitor, not by PUBLIC.
-- ---------------------------------------------------------------------------
REVOKE ALL ON FUNCTION qos_stat() FROM PUBLIC;
REVOKE ALL ON FUNCTION qos_stat_activity() FROM PUBLIC;
REVOKE ALL ON FUNCTION qos_stat_rate() FROM PUBLIC;
REVOKE ALL ON FUNCTION qos_stat_cpu() FROM PUBLIC;
REVOKE ALL ON FUNCTION qos_prometheus_metrics() FROM PUBLIC;
REVOKE ALL ON FUNCTION qos_reset_stats() FROM PUBLIC;
REVOKE ALL ON FUNCTION qos_reset_stats(text, text) FROM PUBLIC;

GRANT EXECUTE ON FUNCTION qos_stat() TO pg_monitor;
GRANT EXECUTE ON FUNCTION qos_stat_activity() TO pg_monitor;
GRANT EXECUTE ON FUNCTION qos_stat_rate() TO pg_monitor;
GRANT EXECUTE ON FUNCTION qos_stat_cpu() TO pg_monitor;
GRANT EXECUTE ON FUNCTION qos_prometheus_metrics() TO pg_monitor;

GRANT SELECT ON qos_stat TO pg_monitor;
GRANT SELECT ON qos_stat_activity TO pg_monitor;
GRANT SELECT ON qos_stat_rate TO pg_monitor;
GRANT SELECT ON qos_stat_cpu TO pg_monitor;

COMMENT ON FUNCTION qos_prometheus_metrics() IS 'QoS metrics in Prometheus exposition format';
COMMENT ON VIEW qos_stat IS 'Cumulative QoS counters per database and role (reset on restart)';
COMMENT ON VIEW qos_stat_activity IS 'Backends currently tracked by QoS';
COMMENT ON VIEW qos_stat_rate IS 'Live rate limit token buckets';
COMMENT ON VIEW qos_stat_cpu IS 'CPU cores assigned per database and role';
