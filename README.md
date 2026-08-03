# PostgreSQL QoS Resource Governor

PostgreSQL extension that provides Quality of Service (QoS) style resource governance for sessions and queries.

## Purpose of the extension

To provide isolation in environments where multiple databases—especially in vertical setups—run within the same cluster, 
ensuring that the resource usage of one database does not affect the others. The goal is to avoid unnecessary resource dedication 
and to prevent complicating maintenance/administration processes by eliminating the need to separate databases into new instances to in order to achieve this isolation.

## Capabilities

- Limit CPU usage by binding the backend to N CPU cores (Linux only); planner integration ensures parallel workers stay within that cap
- Track and cap concurrent transactions and statements (SELECT/UPDATE/DELETE/INSERT)
- Limit work_mem per session
- Cap the *rate* of transactions and statements ("100 per second") per database and role
- Enforce per-role and per-database limits via `ALTER ROLE/DATABASE SET qos.*`
- Fast, reliable cache invalidation across sessions (no reconnect) using a shared epoch mechanism
- Report live activity and per-(database, role) counters through SQL views and a Prometheus endpoint

## Requirements

- PostgreSQL 15 or newer (officially supported)
- Build toolchain and server headers to install from source code (`pg_config` must be available)
- Linux for CPU limiting; on other platforms, only parallel worker limiting is applied

## Installation From Package

### Debian 13 Package
```bash
# Install
sudo dpkg -i postgresql-<version>-qos_1.0.0-1_debian13_amd64.deb
sudo apt-get install -f
```

### Ubuntu 24.04 Package
```bash
# Install
sudo dpkg -i postgresql-<version>-qos_1.0.0-1-ubuntu24_amd64.deb
sudo apt-get install -f
```

### RHEL/AlmaLinux/Centos 10 (PGDG) Package
```bash
# Install
sudo rpm -i postgresql<version>-qos-1.0.0-1.el10.x86_64.rpm
```

## Installation From Source

### Debian/Ubuntu packages:

- Install the server development package that matches your PostgreSQL version:
  - `postgresql-server-dev-15` (for PostgreSQL 15)
  - `postgresql-server-dev-16` (for PostgreSQL 16)
  - `postgresql-server-dev-17` (for PostgreSQL 17)
  - `postgresql-server-dev-18` (for PostgreSQL 18)

### Example (Ubuntu/Debian):

```bash
sudo apt update
# Choose the version you run (15/16/17/18)
sudo apt install postgresql-server-dev-18 build-essential
```

### RHEL/AlmaLinux/Centos/Rocky packages:

- Install the server development package that matches your PostgreSQL version:
  - `postgresql15-devel` (for PostgreSQL 15)
  - `postgresql16-devel` (for PostgreSQL 16)
  - `postgresql17-devel` (for PostgreSQL 17)
  - `postgresql18-devel` (for PostgreSQL 18)

### Example (RHEL/AlmaLinux/Centos/Rocky):

```bash
# Setup pgdg repository
sudo dnf install -y https://download.postgresql.org/pub/repos/yum/reporpms/EL-10-x86_64/pgdg-redhat-repo-latest.noarch.rpm
# Choose the version you run (15/16/17/18)
sudo dnf install -y gcc make autoconf libtool automake postgresql18-devel redhat-rpm-config
```

### Build and Install

1. Build

```bash
make
```

2. Install

```bash
sudo make install
```

### Notes:

- Ensure the correct `postgresql-server-dev-<version>` for Debian/Ubuntu or `postgresql<version>-devel` for RHEL/AlmaLinux/Centos/Rocky is installed so `pg_config` points to your intended server version.
- If you have multiple PostgreSQL versions installed, you can point the build to a specific one by exporting PG_CONFIG:

```bash
# Debian/Ubuntu
export PG_CONFIG=/usr/lib/postgresql/<version>/bin/pg_config
make clean
make
sudo make install

# RHEL/AlmaLinux/Centos/Rocky
export PG_CONFIG=/usr/pgsql-<version>/bin/pg_config
make clean
make
sudo make install
```

## Configure

1. Enable the extension (server restart required due to hooks and shared memory)

Edit `postgresql.conf`:

```conf
shared_preload_libraries = 'qos'
```

Restart PostgreSQL, then in each database where you want QoS active:

```sql
CREATE EXTENSION qos;
```

## Configuration: qos.* settings

Configure limits per role and/or per database using standard GUC storage in `pg_db_role_setting`:

- `qos.work_mem_limit` (bytes) — max effective work_mem per session, e.g. `64MB`, `1GB`
- `qos.cpu_core_limit` (integer) — max CPU cores
- `qos.max_concurrent_tx` (integer) — max concurrent transactions
- `qos.max_concurrent_select` (integer) — max concurrent SELECT statement
- `qos.max_concurrent_update` (integer) — max concurrent UPDATE statement
- `qos.max_concurrent_delete` (integer) — max concurrent DELETE statement
- `qos.max_concurrent_insert` (integer) — max concurrent INSERT statement

### Rate limits (time-windowed)

The `max_concurrent_*` settings above cap how many operations run *at the same
time*; they do not cap how *fast* operations arrive. A single backend can run
50,000 SELECTs per second while never exceeding `max_concurrent_select = 1`.

The rate limits below cap throughput: "at most N operations per W
milliseconds", counted per `(database, role)` across all backends. Each limit
is a single setting written as `'<count>/<window>'`:

| Setting | Meaning |
| --- | --- |
| `qos.max_tx_rate` | transactions per window |
| `qos.max_select_rate` | SELECTs per window |
| `qos.max_update_rate` | UPDATEs per window |
| `qos.max_delete_rate` | DELETEs per window |
| `qos.max_insert_rate` | INSERTs per window |

- `-1` (the default) disables the limit, so behaviour is unchanged until you
  set one.
- The slash is required: `'100/1s'`, `'10/500ms'`, `'5/1min'`. A bare `'100'`
  is rejected — only `-1` may omit the window.
- Windows accept `ms`, `s` and `min` suffixes; a bare number means
  milliseconds. The minimum is **100 ms**, the maximum 1 day.
- The count must be at least 1.
- Exceeding a limit raises `ERROR` with SQLSTATE `54000`
  (`program_limit_exceeded`), the same as the concurrency limits. The hint
  reports roughly how long to wait before retrying.

Prefer a larger count over a shorter window when the two express the same
average rate: `100/1s` and `10/100ms` both allow ~100/sec, but the smaller
count rejects far more legitimate traffic to natural jitter. Aim for a count
of at least ~20.

Examples:

```sql
-- Per-role limits
ALTER ROLE app_user SET qos.work_mem_limit = '32MB';
ALTER ROLE app_user SET qos.cpu_core_limit = '2';
ALTER ROLE app_user SET qos.max_concurrent_select = '100';

-- Per-database limits (for all roles)
ALTER DATABASE appdb SET qos.max_concurrent_tx = '200';

-- Per-role limits for a specific database
ALTER ROLE app_user IN database appdb SET qos.work_mem_limit = '4MB';
ALTER ROLE app_user IN database appdb SET qos.max_concurrent_update = '10';

-- Rate limits: at most 100 transactions per second, 10 SELECTs per 500 ms
ALTER ROLE app_user SET qos.max_tx_rate = '100/1s';
ALTER ROLE app_user SET qos.max_select_rate = '10/500ms';
```

Effective limits are the most restrictive combination of role-level and database-level settings.
Rate limits are merged as whole `(count, window)` pairs by comparing normalised
rates, so `10/1s` (10/sec) wins over `100/500ms` (200/sec).

## How it works

- Work_mem enforcement
  - Intercepts `SET work_mem` and rejects values above `qos.work_mem_limit`.

- CPU limiting
  - On Linux, QoS binds the backend to the N CPU cores (CPU affinity) to cap total CPU usage.
  - The planner hook ensures `Gather`/`Gather Merge` parallel workers do not exceed the allowed cores so parallelism respects the cap.
  - On non-Linux platforms, only the planner effect applies.

- Concurrency limits
  - Executor hooks track active transactions and statements per command type; caps are enforced against configured maxima.

- Rate limits
  - Each `(database, role, operation)` gets a fixed window counter in shared memory: time is cut into `window`-long slices and each slice allows `count` operations. The whole allowance returns at once when a slice ends, so `'3/50s'` means three operations in every 50 second slice — not one every 16.7 seconds.
  - Rollover is lazy: a check compares the current time against the slice it holds and zeroes the counter if it has expired. Nothing runs periodically, so the CPU cost of a check does not depend on the window length — a 100 ms window costs the same as a one hour window. The check itself is O(1) and measurably cheaper than the concurrency scan that already runs per statement.
  - Known trade-off — the boundary effect: `count` operations at the end of one slice and `count` more at the start of the next put 2x `count` through in a short span. Avoiding it needs a sliding window, which would cost one timestamp per operation in shared memory.
  - A fresh counter starts unspent, so an idle system has its whole allowance available.
  - Timing uses a monotonic clock, so an NTP step backwards cannot stall a limit.
  - Fails open: if the shared slot table (512 entries) is exhausted, a warning is logged and traffic is allowed rather than blocked.


## Observability

Four views expose what QoS is doing. All of them, and the functions behind
them, are readable by `pg_monitor` and not by `PUBLIC` — they report activity
and identities from every database in the cluster:

```sql
GRANT pg_monitor TO metrics_user;
```

The shared state is cluster-wide, so querying from any single database
returns rows for every database. `CREATE EXTENSION qos` in one database is
enough for monitoring.

| View | Shows |
| --- | --- |
| `qos_stat` | cumulative counters per database and role |
| `qos_stat_activity` | backends currently running a governed operation |
| `qos_stat_rate` | live rate limit windows, with the configured limit |
| `qos_stat_cpu` | CPU cores assigned per database and role (Linux) |

```sql
-- Who is being rejected, and how often relative to their traffic?
SELECT datname, rolname, total_statements, rejected_total,
       round(100.0 * rejected_total / nullif(total_statements, 0), 2) AS reject_pct
FROM qos_stat
ORDER BY rejected_total DESC;

-- How close is each rate limit to exhaustion right now?
SELECT datname, rolname, kind, used, rate_limit, window_reset_ms
FROM qos_stat_rate
WHERE rate_limit IS NOT NULL;

-- What is running under QoS at this instant?
SELECT pid, datname, rolname, cmd_type, in_transaction FROM qos_stat_activity;
```

`qos_stat` breaks rejections down by cause: `concurrent_*_violations`,
`rate_*_violations` and `work_mem_violations`, plus `rejected_total`.

Counters live in shared memory only and **reset when the postmaster
restarts** — there is no on-disk state to keep compatible across upgrades.
Prometheus handles counter resets natively (`rate()` accounts for them).

Reset manually with `SELECT qos_reset_stats();` for everything, or
`SELECT qos_reset_stats('appdb', 'app_user');` for one database and role.

`used` and `window_reset_ms` are rolled over before they are reported, so they
describe the window a statement arriving now would actually meet — an expired
window reads as already reset, not as the stale value left by the last check.

A database or role dropped since its counters were recorded is reported by its
OID in the `datname`/`rolname` column, so the row stays identifiable instead of
turning into a nameless `NULL`.

### Prometheus

`qos_prometheus_metrics()` renders everything in exposition format:

```sql
SELECT qos_prometheus_metrics();
```

```text
# HELP qos_statements_total Statements processed under QoS governance
# TYPE qos_statements_total counter
qos_statements_total{datname="appdb",rolname="app_user"} 154823
# HELP qos_rejected_total Operations rejected by a QoS limit
# TYPE qos_rejected_total counter
qos_rejected_total{datname="appdb",rolname="app_user",reason="rate_select"} 340
# TYPE qos_rate_used gauge
qos_rate_used{datname="appdb",rolname="app_user",kind="select"} 3
# TYPE qos_rate_window_reset_seconds gauge
qos_rate_window_reset_seconds{datname="appdb",rolname="app_user",kind="select"} 12.480
```

Label values are escaped, so role and database names containing quotes or
backslashes cannot break the scrape.

For `postgres_exporter`, either scrape that function or read the views
directly via a custom query file:

```yaml
# queries.yaml
qos_stat:
  query: "SELECT datname, rolname, total_statements, rejected_total,
                 rate_select_violations, concurrent_select_violations
          FROM qos_stat"
  metrics:
    - datname:                      {usage: "LABEL"}
    - rolname:                      {usage: "LABEL"}
    - total_statements:             {usage: "COUNTER"}
    - rejected_total:               {usage: "COUNTER"}
    - rate_select_violations:       {usage: "COUNTER"}
    - concurrent_select_violations: {usage: "COUNTER"}

qos_stat_rate:
  query: "SELECT datname, rolname, kind, rate_limit, used, window_reset_ms
          FROM qos_stat_rate WHERE rate_limit IS NOT NULL"
  metrics:
    - datname:         {usage: "LABEL"}
    - rolname:         {usage: "LABEL"}
    - kind:            {usage: "LABEL"}
    - rate_limit:      {usage: "GAUGE"}
    - used:            {usage: "GAUGE"}
    - window_reset_ms: {usage: "GAUGE"}
```

### Logging

Increase verbosity temporarily to trace QoS activity:

```sql
SET client_min_messages = 'debug1';
```

You’ll see messages when cache is refreshed, CPU workers are adjusted, or limits are enforced.

## Upgrading to 1.1

1.1 adds the rate limits and the observability views, and grows the shared
memory state — so `ALTER EXTENSION` alone is not enough, the new module must
be loaded by a restarted postmaster:

```bash
make && sudo make install
pg_ctl restart          # required: shared memory layout changed
```

```sql
ALTER EXTENSION qos UPDATE TO '1.1';
```

What changes:

- New `qos.max_*_rate` settings (`'<count>/<window>'`). Existing `qos.*`
  settings are untouched and no rate limit is active until you set one, so
  enforcement is unchanged by the upgrade itself.
- New `qos_stat*` views and `qos_prometheus_metrics()`. Counters are collected
  from the moment the new module loads and reset on every restart.
- `qos_get_stats()` is dropped — it was a stub that always returned
  `not implemented yet`; the views replace it.

## Limitations

- CPU limiting is only available on Linux; other platforms only enforce parallel worker limits via the planner.
- Requires `shared_preload_libraries` and a server restart to activate.
- Official support targets PostgreSQL 15 and newer.

## Uninstall

```sql
DROP EXTENSION IF EXISTS qos;
```

Remove from `shared_preload_libraries` and restart the server.

## Development

- Build with PGXS (Makefile provided)
- Targets PG15–PG18 server APIs
- Modular codebase:
  - `hooks.c`: hook registration and coordination
  - `hooks_cache.c`: session cache + shared epoch invalidation
  - `hooks_resource.c`: CPU/memory enforcement + planner hook
  - `hooks_statement.c`: statement-level concurrency tracking
  - `hooks_transaction.c`: transaction-level concurrency tracking
  - `hooks_rate.c`: time-windowed (rate) limits — fixed window counters in shared memory
  - `qos_stats.c`: statistics collection and the SQL/Prometheus views
  - `qos_shmem_slot.c`: shared-memory slot lookup used by the rate limiter and the statistics
  - `qos.c`/`qos.h`: shared memory, catalog reads, helpers

## License

PostgreSQL extension that provides Quality of Service (QoS) style resource governance for sessions and queries.

Copyright (C) 2025  AppstoniA

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.

