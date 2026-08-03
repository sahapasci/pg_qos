-- qos--1.1.sql
-- PostgreSQL QoS Resource Governor Extension
-- SQL install script for the QoS extension

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION qos" to load this file. \quit

-- Function: qos_version()
-- Returns the version of the QoS extension
CREATE FUNCTION qos_version() 
RETURNS text
LANGUAGE C IMMUTABLE STRICT
AS '$libdir/qos', 'qos_version';

-- Function: qos_reset_stats()
-- Resets QoS statistics
CREATE FUNCTION qos_reset_stats()
RETURNS void
LANGUAGE C STRICT
AS '$libdir/qos', 'qos_reset_stats';

-- View: qos_rsettings
-- Shows current QoS settings for all roles and databases using pg_db_role_setting
CREATE VIEW qos_settings AS
SELECT 
    COALESCE(r.rolname, 'database-wide') as rolname,
    COALESCE(d.datname, 'cluster-wide') as datname,
    cfg as setting
FROM pg_db_role_setting s
LEFT JOIN pg_roles r ON r.oid = s.setrole
LEFT JOIN pg_database d ON d.oid = s.setdatabase,
LATERAL unnest(s.setconfig) cfg
WHERE cfg LIKE 'qos.%';

-- Extension metadata
COMMENT ON EXTENSION qos IS 'PostgreSQL Quality of Service (QoS) Resource Governor';
COMMENT ON FUNCTION qos_version() IS 'Returns QoS extension version';
COMMENT ON FUNCTION qos_reset_stats() IS 'Resets QoS statistics counters';

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
-- Live rate limit windows
--
-- used and window_reset_ms describe the window a statement arriving now would
-- meet: rollover is lazy, so an expired window is reported as already reset.
-- rate_limit, window_ms and window_reset_ms are NULL when an entry outlives
-- the setting that created it.
-- ---------------------------------------------------------------------------
CREATE FUNCTION qos_stat_rate(
    OUT datname             text,
    OUT rolname             text,
    OUT kind                text,
    OUT rate_limit          int,
    OUT window_ms           int,
    OUT used                int,
    OUT window_reset_ms     int
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
COMMENT ON VIEW qos_stat_rate IS 'Live rate limit windows';
COMMENT ON VIEW qos_stat_cpu IS 'CPU cores assigned per database and role';
