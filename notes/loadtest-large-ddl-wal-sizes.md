# WAL written to master's system catalog by large DDL

Measured 2026-10-01 with `PgLocalCatalogDdlWalSizeTest.DISABLED_DdlWalSize`
(`pg_local_catalog_load-test.cc`), release build, dev-server-sanketh-4. Cluster:
1 master, 1 tserver with its local catalog copy off, so nothing polls master
while the statements run. Log: `loadtest-results/host4-16vcpu/ddl_wal.log.gz`.
Plan and load results: [loadtest-plan.md](loadtest-plan.md),
[loadtest-summary.md](loadtest-summary.md).

Per scenario, the test reads before and after the statements (plus 2 s after
the last one, so master's post-commit DDL verification writes are counted):

- WAL ops: committed op index of master's system catalog tablet.
- WAL bytes: `log_bytes_logged` of that tablet (bytes appended to its WAL).
- Response bytes: one change stream polls from the start position until it is
  caught up; the serialized size of all responses, i.e. what one tserver
  receives to catch up on the scenario.
- Catalog version: increments of the database's `pg_yb_catalog_version` row.

Tables have two columns (`a INT, b TEXT`). Each scenario runs once.

| DDL | WAL ops | WAL bytes | Response bytes (one tserver) | Records | Catalog version | DDL time |
|---|---|---|---|---|---|---|
| idle 2 s (control) | 0 | 0 | 23 | 0 | +0 | — |
| `CREATE TABLE t (a int, b text)` | 49 | 15,804 | 13,232 | 50 | +1 | 165 ms |
| `ALTER TABLE t ADD COLUMN c int` | 19 | 7,539 | 6,936 | 20 | +1 | 138 ms |
| `CREATE INDEX ON t (b)` (empty table) | 74 | 31,694 | 29,621 | 75 | +4 | 20.2 s (see note 3) |
| `DROP TABLE t` | 66 | 21,346 | 18,184 | 68 | +1 | 176 ms |
| 100 x `CREATE TABLE`, one transaction each | 4,803 | 1,553,964 | 1,311,504 | 4,903 | +100 | 9.3 s |
| `ANALYZE` (whole database: 100 tables x 100 rows, plus system catalogs) | 1,954 | 1,620,742 | 1,516,907 | 1,954 | +145 | 6.1 s |
| `BEGIN; ANALYZE; COMMIT` (same database) | 838 | 1,592,121 | 1,537,142 | 838 | +1 | 2.9 s |
| `DROP SCHEMA s CASCADE` (100 tables) | 4,453 | 2,012,427 | 1,823,297 | 4,553 | +1 | 5.0 s |
| `ALTER TABLE p ADD COLUMN c int` (partitioned, 100 partitions) | 1,173 | 723,535 | 669,847 | 1,274 | +1 | 5.7 s |
| `BEGIN; 20 x CREATE TABLE; COMMIT` | 837 | 297,708 | 235,580 | 857 | +1 | 1.5 s |
| `BEGIN; 20 x ALTER TABLE ADD COLUMN; COMMIT` | 241 | 125,069 | 114,002 | 261 | +1 | 1.0 s |
| `BEGIN; 20 mixed (5 CREATE TABLE, 5 ADD COLUMN, 5 CREATE INDEX, 5 DROP TABLE); COMMIT` | 578 | 229,745 | 194,629 | 598 | +1 | 1.3 s |
| `CREATE DATABASE d1` (from template1) | 41 | 7,892,640 | 11,600,015 | 41,831 | +1 | 0.75 s |
| `DROP DATABASE d1` | 135 | 153,201 | 171,741 | 353 | +1 | 0.45 s |

## Findings

0. `CREATE DATABASE` is the largest DDL measured: 7.9 MB of WAL in only 41
   ops, because the new database's catalog rows are copied from the template
   in a few very large write batches. Its response is larger than its WAL:
   11.6 MB in 41,831 records (one record per copied row, each with its own
   key copy and transaction state). With master's 4 MB batch limit a tserver
   needs at least 3 polls to catch up on it, and it does not fit in one
   response. `DROP DATABASE` writes 153 KB. Measured by
   `DISABLED_DdlWalSizeDatabase` (`host4-16vcpu/ddl_wal_db.log.gz`).
1. `ANALYZE` on a whole database commits each table in its own transaction
   and bumps the database's catalog version once per analyzed table: +145 for
   100 user tables, so about 45 system catalog tables were analyzed as well.
   Wrapped in `BEGIN; ... COMMIT` it bumps the version once and writes fewer
   WAL ops (838 against 1,954, so fewer separate commits), but the same bytes
   (1.59 MB against 1.62 MB): the bytes are the `pg_statistic` and `pg_class`
   rows, which are the same either way. For tservers the difference is 145
   invalidations against one; the WAL to ship is the same.
2. Largest single statements, by WAL bytes:
   - `DROP SCHEMA ... CASCADE` with 100 tables: 2.0 MB, about 20 KB per table
     (a single `DROP TABLE` writes 21 KB).
   - `ANALYZE` of 100 small tables: 1.6 MB, about 16 KB per table.
   - `ALTER TABLE ... ADD COLUMN` on 100 partitions: 0.72 MB, 7.2 KB per
     partition; the estimate "single table x 100" (7.5 KB x 100 = 0.75 MB) is
     within 4%.
3. `CREATE INDEX` took 20.2 s because the tserver retried
   `WaitForYsqlBackendsCatalogVersion` ("Recently became leader: cannot yet
   serve") on a freshly started master; its WAL numbers are unaffected. It
   bumps the version 4 times (one per index permission step).
4. 20-statement migration batches write 0.13-0.30 MB and bump the version
   once. Per statement inside a batch: `CREATE TABLE` about 15 KB (same as
   standalone), `ADD COLUMN` about 6 KB.
5. Response bytes are 84-97% of WAL bytes for every DDL except
   `CREATE DATABASE` (147%), so the WAL size is a good proxy for what every
   tserver receives. Every scenario except `CREATE DATABASE` fits in one batch
   (master sends at most 4 MB per response, `consensus_max_batch_size_bytes`);
   all fit in the 128 MB log cache.

## What it means for master load (estimate, not measured)

The FixedLag runs measured about 16 ms of master CPU per poll for a 626 KB
response from the log cache at saturation (10 ms when not saturated). If cost
grows linearly with response size, one tserver catching up on a 2 MB
`DROP SCHEMA` costs master about 30-50 ms of CPU, and 200 tservers catching up
on it about 6-10 CPU-seconds, spread over the polls that follow the DDL. On
the same assumption, `CREATE DATABASE` (11.6 MB of responses) costs about
0.2-0.3 CPU-seconds per tserver, about 40-60 CPU-seconds for 200 tservers.

## Bytes a tserver reads from master on its first connection to a database

Measured 2026-10-01 on released code, not the local-copy branch:
`~/yugabyte-2025.2.3.0/bin/yugabyted` (2025.2.3.0-b149), one node at
127.0.0.71, default settings (catalog preload and tserver response cache on),
data in the session scratchpad. Bytes are deltas of master's per-method
counters `service_response_bytes_*` / `service_request_bytes_*` /
`handler_latency_*` (calls) around each step; master serves catalog reads
through `TabletServerService.Read`. Each step is `ysqlsh -d <db> -c "SELECT 1"`
(a new connection), followed by 2 s.

| Step | Master response bytes (all methods) | `Read` response bytes | `Read` calls | Other notable |
|---|---|---|---|---|
| `CREATE DATABASE d1` (from an existing connection) | 10 KB | 5 KB | 17 | — |
| 1st connection to `d1` | 4.06 MB | 3.66 MB | 11 | `GetTableLocations` 0.38 MB / 48 calls; `GetTableSchema` 17 KB / 32 calls |
| 2nd connection to `d1` (2 s later) | 3.81 MB | 3.69 MB | 102 | `ListTables` 90 KB; `GetTableSchema` 25 KB / 43 calls |
| 3rd, 4th connection to `d1` | 3.5-3.7 KB | 2.8 KB | 3 | — |
| connection to `yugabyte` (existing database) | 4.0 KB | 2.8 KB | 3 | — |
| idle 5 s (control) | 1.6 KB | 0 | 0 | heartbeats, lease |
| tserver restarted (killed, restarted by yugabyted), 1st connection to `d1` | 3.68 MB | 3.66 MB | 11 | `GetTableSchema` 17 KB / 32 calls |
| same, 2nd connection to `d1` | 3.5 KB | 2.8 KB | 3 | — |
| same, 1st connection to `yugabyte` | 3.68 MB | 3.66 MB | 11 | `GetTableSchema` 17 KB / 32 calls |
| same, 2nd connection to `yugabyte` | 3.5 KB | 2.8 KB | 3 | — |

Findings:

- The first connection to a database on a tserver pulls about 3.66 MB of
  catalog rows from master in 11 large `Read` calls (the catalog preload),
  whether the database is new or the tserver just restarted, and whether or
  not the database's relcache init file is on disk. Later connections on the
  same tserver cost about 3 KB (served by the tserver's response cache).
- Right after `CREATE DATABASE` the 2nd connection, 2 s after the 1st, pulled
  3.69 MB again (102 smaller `Read` calls) even though the 1st had written
  `d1`'s init files (04:30:18.44, during the 1st connection); from the 3rd
  connection on it was cheap. Cause not investigated.
- Compared with the WAL of `CREATE DATABASE` (7.9 MB WAL, 11.6 MB of change
  stream responses to catch up on it): the preload for one database is
  3.66 MB, about 0.46x the WAL and 0.32x the change-stream bytes — the same
  order, smaller rather than larger. Per tserver, a new database costs about
  3.7 MB of preload on its first connection, 7.4 MB in total if the 2nd
  connection repeats it as above; 200 tservers each connecting once pull
  about 0.73 GB from master.
