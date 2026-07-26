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
- Enforce per-role and per-database limits via `ALTER ROLE/DATABASE SET qos.*`
- Fast, reliable cache invalidation across sessions (no reconnect) using a shared epoch mechanism

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
is a pair of settings — a count and a window:

| Count | Window | Meaning |
| --- | --- | --- |
| `qos.max_tx_rate` | `qos.max_tx_rate_window` | transactions per window |
| `qos.max_select_rate` | `qos.max_select_rate_window` | SELECTs per window |
| `qos.max_update_rate` | `qos.max_update_rate_window` | UPDATEs per window |
| `qos.max_delete_rate` | `qos.max_delete_rate_window` | DELETEs per window |
| `qos.max_insert_rate` | `qos.max_insert_rate_window` | INSERTs per window |

- `-1` (the default) disables the limit, so behaviour is unchanged until you
  set one.
- Windows accept `ms`, `s` and `min` suffixes; a bare number means
  milliseconds. The minimum is **100 ms**, the maximum 1 day.
- Setting a count without a window uses a default window of `1s`. Setting a
  window without a count does nothing.
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
ALTER ROLE app_user SET qos.max_tx_rate = '100';
ALTER ROLE app_user SET qos.max_tx_rate_window = '1s';
ALTER ROLE app_user SET qos.max_select_rate = '10';
ALTER ROLE app_user SET qos.max_select_rate_window = '500ms';
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
  - Each `(database, role, operation)` gets a token bucket in shared memory, refilled lazily: a check adds `elapsed * (count / window)` tokens and then consumes one.
  - Nothing runs periodically, so the CPU cost of a check does not depend on the window length — a 100 ms window costs the same as a one hour window. The check itself is O(1) and measurably cheaper than the concurrency scan that already runs per statement.
  - A fresh bucket starts full, so an idle system has its whole burst allowance available.
  - Timing uses a monotonic clock, so an NTP step backwards cannot stall a limit.
  - Fails open: if the shared slot table (512 entries) is exhausted, a warning is logged and traffic is allowed rather than blocked.


## Observability and Logging

Increase verbosity temporarily to trace QoS activity:

```sql
SET client_min_messages = 'debug1';
```

You’ll see messages when cache is refreshed, CPU workers are adjusted, or limits are enforced.

## Upgrading to 1.1

Version 1.1 adds the rate limits and grows the extension's shared memory
state, so `ALTER EXTENSION` alone is not enough — the new module must be
loaded by a restarted postmaster:

```bash
make && sudo make install
pg_ctl restart          # required: shared memory layout changed
```

```sql
ALTER EXTENSION qos UPDATE TO '1.1';
```

Existing `qos.*` settings are unaffected and no rate limit is active until you
set one, so the upgrade is behaviour-neutral by default.

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
  - `hooks_rate.c`: time-windowed (rate) limits — token buckets in shared memory
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

