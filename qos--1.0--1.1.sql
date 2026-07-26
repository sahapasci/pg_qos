-- qos--1.0--1.1.sql
-- Upgrade from QoS extension 1.0 to 1.1
--
-- 1.1 adds time-windowed (rate) limits: qos.max_*_rate and
-- qos.max_*_rate_window.  These are enforced entirely in the C module and
-- configured through pg_db_role_setting, so the SQL surface is unchanged.
--
-- NOTE: 1.1 grows the shared memory state, so a PostgreSQL restart is
-- required for the new limits to take effect - ALTER EXTENSION alone is not
-- sufficient.

\echo Use "ALTER EXTENSION qos UPDATE TO '1.1'" to load this file. \quit

COMMENT ON EXTENSION qos IS 'PostgreSQL Quality of Service (QoS) Resource Governor';
