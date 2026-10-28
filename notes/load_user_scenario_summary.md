# Master load for large DDL, per 100 and per 1000 tservers

Date: 2026-10-01. Scope: the tserver-local catalog copy with fixes 1 and 2 in
(waiters wake on progress; master's log cache keeps system catalog ops), fix 3
out, release push off (the default), so every tserver pulls every DDL from
master. The section "With master push" (2026-10-07) gives the same rows for
the push implementation that replaced pull. Derivations and sources are at
the bottom.

Cost rate: **25 ms of master CPU per MB sent to one tserver**, WAL range in
master's log cache. About 97% of that is building one response protobuf per
tserver (FixedLag perf profile, 16 vCPUs, [loadtest-summary.md](loadtest-summary.md)):

- converting the cached WAL ops into `CDCRecordPB` records: 34% (key decode
  and re-encode, copying key and value bytes);
- serializing the response: 36% (computing its size about 15%);
- destroying the response: 27%.

Reading the log cache costs 1.7%, locks 0.3%. Deserialization happens on the
receiving tserver, not on master. Every tserver at the same position gets an
identical response, so the work is the same response rebuilt once per
tserver.

Assumptions for the table:

- All tservers poll the same DDL at the same time.
- Master has 32 vCPUs, and CPU per MB stays at the measured 16-vCPU rate
  (scaling above 16 vCPUs is not measured).
- Master's NIC is 10 Gbit (1.25 GB/s).
- Longest response = the time until the last tserver has the DDL: the larger
  of (master CPU / 32) and (data / 1.25 GB/s). At this host size the two are
  almost equal (S x 0.078 s and S x 0.08 s per 100 tservers, S in MB).

| DDL | Size S each tserver receives | Per 100 tservers: data / master CPU | Per 100: longest response (CPU-bound / NIC-bound) | Per 1000 tservers: data / master CPU / longest response |
|---|---|---|---|---|
| Catch-up of 50 `CREATE TABLE t (a INT)` | 0.63 MB (2297 WAL ops) | 63 MB / 1.6 CPU-s | 49 ms / **50 ms** | 0.63 GB / 16 CPU-s / **0.5 s** |
| Catch-up of 2000 DDLs at about 13 KB each | about 26 MB | 2.6 GB / 65 CPU-s | 2.0 s / **2.1 s** | 26 GB / 650 CPU-s / **21 s** |
| `DROP SCHEMA s CASCADE`, 100 tables | 1.82 MB (18 KB per table) | 182 MB / 4.6 CPU-s | 142 ms / **146 ms** | 1.8 GB / 46 CPU-s / **1.5 s** |
| `ALTER TABLE ADD COLUMN`, 100 partitions | 0.67 MB (7 KB per partition) | 67 MB / 1.7 CPU-s | 52 ms / **54 ms** | 0.67 GB / 17 CPU-s / **0.54 s** |
| `CREATE DATABASE` (from template1) | 11.6 MB (41,831 records; at least 3 polls, 4 MB batches) | 1.16 GB / 29 CPU-s | 0.91 s / **0.93 s** | 11.6 GB / 290 CPU-s / **9.3 s** |
| Migration batch, 20 statements in one transaction | 0.11-0.24 MB | 24 MB / 0.6 CPU-s | 19 ms / **19 ms** | 0.24 GB / 6 CPU-s / **0.19 s** |

### With master push (measured 2026-10-07)

Master pushes each WAL range to every tserver behind it (push-plan.md),
bounded to 48 RPCs and about 50 MB in flight. Measured on the same 16-vCPU
host (release, [loadtest-summary.md](loadtest-summary.md) "Push"):

- Catch-up and large DDLs: **about 1-1.5 ms of master CPU per MB sent to one
  tserver** (pages are built once and the same bytes go to every target),
  against 25 ms/MB for pull.
- Many small DDLs: about **75 µs of master CPU per push RPC**, and the pusher
  sends about one push RPC per DDL per tserver.

The bytes are the same as for pull, so the NIC-bound time does not change; the
CPU-bound time becomes small next to it.

| DDL | Size S | Push per 100 tservers: master CPU (pull) | Push per 1000 tservers: master CPU (pull) | Push, time until the last tserver has it |
|---|---|---|---|---|
| Catch-up of 50 `CREATE TABLE t (a INT)` | 0.63 MB (each receiver got 0.87 MB, see note) | **0.45 CPU-s measured** (1.6) | **1.3 CPU-s measured** (16) | 0.63 s measured at 100; 1.2 s at 1000 (loopback); NIC-bound 0.5 s at 1000 |
| Catch-up of 2000 DDLs | about 26 MB (26.6 MB measured) | **4.0 CPU-s measured** (65) | **26 CPU-s measured** (650) | 8.4 s measured at 100; 13.6 s at 1000 (loopback); NIC-bound 21 s at 1000 |
| `DROP SCHEMA s CASCADE`, 100 tables | 1.82 MB | 0.3 CPU-s (4.6) | 2.7 CPU-s (46) | NIC-bound 1.5 s at 1000 |
| `ALTER TABLE ADD COLUMN`, 100 partitions | 0.67 MB | 0.1 CPU-s (1.7) | 1.0 CPU-s (17) | NIC-bound 0.54 s at 1000 |
| `CREATE DATABASE` | 11.5 MB (pushed record bytes) | **1.8-2.2 CPU-s measured**, including the statement (29) | **9.2-10.5 CPU-s measured**, including the statement (290) | all 100 at 0.49 s and all 1000 at 4.66 s after the statement (loopback, about 2.5 GB/s); NIC-bound 9.3 s at 1000 |
| Migration batch, 20 statements in one transaction | 0.11-0.24 MB | 0.04 CPU-s (0.6) | 0.4 CPU-s (6) | NIC-bound 0.19 s at 1000 |

- "Measured" rows are `PushJoinerWave` (K receivers start behind and are
  pushed to the head) and `CreateDatabaseFanout`; the others are S x 1.5 ms/MB
  x the number of tservers.
- Note on 0.87 MB: a receiver whose position master does not know yet is
  pushed from the start of master's page cache, which here began about
  250 KB before the 50 DDLs.
- Loopback times are with all receivers in one process on the master's
  host; they show master's own rate (up to about 2 GB/s), not a NIC.
- Not measured: DDL completion while it waits for 1000 real tservers. The
  receivers in these runs hold no lease, so no DDL waits for them.

Not included in the table:

- Poll queue overflow. With the default queue of 50 calls, most of 1000
  simultaneous polls are rejected and retried with backoff, which lengthens
  the tail. The 16-vCPU runs used a 1000-entry queue and still rejected up to
  1.7% of polls at N=8000.
- Lease RPC latency while master is saturated (lease RPCs share master's
  RPC thread pools with the polls; not measured under saturation).
- Larger objects: sizes grow with columns, indexes and constraints per table;
  every size here is for two-column tables.

## Rationale: how each number is derived

### Sources

| Source | What it provides |
|---|---|
| [loadtest-large-ddl-wal-sizes.md](loadtest-large-ddl-wal-sizes.md) | Response bytes one tserver receives per DDL: `PgLocalCatalogDdlWalSizeTest.DISABLED_DdlWalSize` and `DISABLED_DdlWalSizeDatabase`, release, dev-server-sanketh-4 (16 vCPUs), 1 master, tables `(a INT, b TEXT)`, one run per scenario. One change stream polls from the position before the statements until caught up; the serialized size of all responses is the size S. |
| [loadtest-summary.md](loadtest-summary.md) | Master CPU per poll at saturation: FixedLag, log cache, N=100, release, dev-server-sanketh-4: 958 polls/s, 15.2 master cores, 15.8 ms CPU per poll, 626,231 bytes per response. |
| [loadtest-summary.md](loadtest-summary.md) "Push" | Push rows: `PushJoinerWave` K=100 and K=1000 with 50 and 2000 lag DDLs (master CPU 0.45, 1.29, 3.99 and 25.9 CPU-s; 1.0-1.5 ms per MB sent at the larger sizes), `CreateDatabaseFanout` N=100 and N=1000 (2.17 CPU-s for 1.185 GB; 10.5 CPU-s for 11.85 GB), `PushSteady` (about 75 µs of master CPU per push RPC). Release, dev-server-sanketh-4, 2026-10-07; logs in `loadtest-results/host4-push/`. |
| [loadtest-worklog.md](loadtest-worklog.md) §3 | The same FixedLag workload on the 4-vCPU host, release, master not saturated: 9.8-10.5 ms per 0.62 MB poll, 0 log cache disk reads (`host3-4core/cache_fixedlag10{,b}.log.gz`). |

### Cost rate

- 25 ms/MB = 15.8 ms per poll / 0.626 MB per poll (loadtest-summary.md,
  FixedLag with log cache, N=100, master saturated at 15.2 of 16 vCPUs).
- The 4-vCPU host measured 16 ms/MB (10 ms / 0.62 MB) with master at about 1
  core. The saturated 16-vCPU figure is used because it is what master pays
  per vCPU when it is busy, which is the case these scenarios describe.
- The rate is measured on `CREATE TABLE` WAL only. Most of the cost is per
  record (key decode and re-encode, building one record per row,
  serialization, destruction: about 97% of master CPU in FixedLag), so
  DDLs with many small records, such as `CREATE DATABASE`, may cost more per
  MB. Not measured.

### Sizes S

| Row | Derivation |
|---|---|
| 50 `CREATE TABLE t (a INT)` | Measured: FixedLag response, 626,231 bytes (loadtest-summary.md), 2297 WAL ops ([loadtest-worklog.md](loadtest-worklog.md) §1 and §3). |
| 2000 DDLs | Extrapolated: 100 separate `CREATE TABLE` = 1,311,504 response bytes (loadtest-large-ddl-wal-sizes.md), 13.1 KB per DDL, x 2000 = 26 MB. The mix matters: per DDL, `ALTER TABLE ADD COLUMN` is 6.9 KB, `DROP TABLE` 18.2 KB, `CREATE INDEX` 29.6 KB, so 2000 DDLs range from about 14 MB to 60 MB. 26 MB fits in master's 128 MB log cache; whether a 24 h range stays in the cache depends on master's other system catalog WAL per hour, which is not measured. |
| `DROP SCHEMA`, 100 tables | Measured: 1,823,297 response bytes. Per table 18.2 KB; a single `DROP TABLE` is 18,184 bytes, so N tables scale linearly (500 tables, about 9.1 MB). |
| `ADD COLUMN`, 100 partitions | Measured: 669,847 response bytes. The other session found the WAL within 4% of 100 x a single-table `ADD COLUMN`. |
| `CREATE DATABASE` | Measured: 11,600,015 response bytes in 41,831 records (`DISABLED_DdlWalSizeDatabase`). Above the 4 MB batch limit (`consensus_max_batch_size_bytes`), so one tserver needs at least 3 polls. |
| Migration batch | Measured: 20 x `ALTER TABLE ADD COLUMN` in one transaction = 114,002 bytes; 20 x `CREATE TABLE` = 235,580 bytes; a 20-statement mix = 194,629 bytes. |

### Columns

- Data per 100 tservers = 100 x S.
- Master CPU per 100 tservers = 100 x S x 25 ms.
- CPU-bound time = master CPU / 32 vCPUs.
- NIC-bound time = data / 1.25 GB/s.
- Per 1000 tservers = per 100 x 10, for data, CPU and both times.
