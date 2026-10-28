# Master load test — worklog

Plan: [loadtest-plan.md](loadtest-plan.md). Final table:
[loadtest-summary.md](loadtest-summary.md). All result files are under
`loadtest-results/` in this directory: `host3-4core/` (dev-server-sanketh-3,
4 vCPUs, shared with about 68 idle Claude Code sessions: every phase there is
flagged `HOST UNDER LOAD`) and `host4-16vcpu/` (dev-server-sanketh-4, 16 vCPUs =
8 cores x 2 hyperthreads, idle). Logs are gzipped full test logs; `*.perf.gz`
are `perf record -g` files of the master; `*.top` are `top -H`; `*.pstack1`
are stack dumps; `*.sh` are the drivers. The pre-rewrite plan with every
intermediate table is `loadtest-results/loadtest-plan-before-rewrite-2026-10-01.md`.

"CPU/poll" = master CPU / polls/s, including master's other work.

## 1. 2026-09-30, host3 (4 vCPUs), fastdebug — harness bring-up

| Run | Files | Result |
|---|---|---|
| Steady N=10, 3 real tservers with copies, smoke | `host3-4core/loadtest_build1.log.gz` | harness works; master 0.93 cores, 1079 polls/s (real pollers far above their 100 ms interval: the waiter spin, §2) |
| Steady and FixedLag N=10, 1 tserver copy off | `host3-4core/loadtest_run3.log.gz` | FixedLag: 2297 ops behind, 1.23 MB per poll, 0.46 s CPU/poll; log cache holds 0 ops |
| same, log cache 1 MB | `host3-4core/loadtest_run4_logcache1mb.log.gz` | same within noise |

Fastdebug numbers are not usable for cost (about 14x release for a lagged
poll).

## 2. 2026-09-30, host3, release — fix 1 (waiter spin)

| Run | Files | Result |
|---|---|---|
| Steady + FixedLag N=10 | `host3-4core/loadtest_run5_release.log.gz` | Steady 1.9 ms CPU/poll; FixedLag 33 ms CPU/poll, 0.62 MB, 93 polls/s, 3.05 cores |
| E1 RealTserversOnly, 3 tservers, 4 DDL sessions | `host3-4core/spin_e1_default.log.gz` | 860-1257 polls/s per tserver (target 10), gate wait 87-109 ms, DDL p50 1.00 s |
| E2 throttle off (`xcluster_consistent_wal_safe_time_frequency_ms=0`) | `host3-4core/spin_e2_no_throttle.log.gz` | 32-36 polls/s per tserver, gate wait 6 ms, DDL p50 299 ms |
| E3 no DDL | `host3-4core/spin_e3_no_ddl.log.gz` | 9.9 polls/s per tserver |
| E1 with fix 1 | `host3-4core/spin_e1_fixed.log.gz` | 31-35 polls/s per tserver, gate wait 6.1-6.4 ms, DDL p50 295 ms, 409 DDLs/30 s (was 120) |
| pg_local_catalog-test, fixes 1-2 | `host3-4core/regress_local_catalog.log.gz` | 48/48 passed |

Cause: while a DDL keeps a catalog transaction open, master computes a new S
for a stream at most every 250 ms; the replica woke every waiter after every
poll and each waiter requested another immediate poll.

## 3. 2026-09-30, host3, release — fix 2 (log cache), fix 3 (shared S)

| Run | Files | Result |
|---|---|---|
| Steady N=50/100/200, before fix 2 | `host3-4core/scale_steady{50,100,200}.log.gz` | master +0.38/+0.57/+0.79 cores; 10 x N polls/s; log cache 0 ops |
| FixedLag N=10, fix 2 | `host3-4core/cache_fixedlag10{,b}.log.gz`, `perf_cache_fixedlag10b.data` | cache 2902 ops, 0 disk reads; 0.98-1.05 cores; 9.8-10.5 ms CPU/poll (was 33) |
| Steady N=100/200, fix 2 | `host3-4core/cache_steady{100,200,200b}.log.gz`, `perf_cache_steady200*.data` | N=200 +0.31-0.38 cores (was +0.79); handler 5.8 ms; txn status RPCs 1185-1202/s |
| Steady N=200, fix 3 | `host3-4core/b6_steady200.log.gz` | txn status RPCs 110/s; handler 1.0 ms; CPU unchanged |
| RealTserversOnly 3 tservers, fix 3 | `host3-4core/b6_real3.log.gz` | gate wait 5.7 ms, DDL p50 277 ms |
| pg_local_catalog-test, fixes 1-3 | `host3-4core/regress_local_catalog_final.log.gz` | 48/48 passed |

Fix 3 was then removed from repo6 (`fix3-shared-safe-time.patch`).

## 4. 2026-09-30/10-01, host4 (16 vCPUs), release — scaling

### 4.1 FixedLag, original client (one messenger, parse)

`host4-16vcpu/fl_{50,100,150,200}.log.gz`: 500/506/498/492 polls/s, master
5.5-6.4 cores, host 7-8 of 16 busy. `fl_diag.pstack1`: 58 of 65 handler
threads in `LogCache::ReadOps`'s spin lock; perf (`fl_diag.log.gz`): lock held
0.38 cores — not the limit. Caching each entry's serialized size
(`fl2_*.log.gz`) changed nothing. `fl_client.top`: the client's single
messenger reactor at 87% of a core — the limit.

### 4.2 FixedLag, 8 client messengers, no parse, log cache

`host4-16vcpu/fl3_{100,150,200,300}.log.gz`: 958/949/948/944 polls/s, master
15.1-15.2 cores, client 0.41-0.45 cores, host 15.8-15.9/16, 15.8-16.0 ms
CPU/poll, 626 KB responses, handler mean 41-126 ms. Profile
`fl_diag3.perf.gz` (N=100): serialization 36.3%, record building 33.7%,
destruction 27.1%, tcmalloc 4.8%, spin locks 0.3%.

### 4.3 FixedLag, no log cache (`log_cache_size_limit_mb=0`)

`host4-16vcpu/fld_{50,100,200}.log.gz`: N=50 346 polls/s 40.1 ms CPU/poll
13.9 cores; N=100 374 polls/s 37.6 ms 14.1 cores; N=200 responses cut to
190 KB (not comparable). Profile `fl_diag4.perf.gz`: WAL read 40.6% (entry
decode 28.5%, parse 7.1%, CRC 0.8%, pread 0.8%), `SpaceUsedLong` 10.9%,
serialization 11.5%, destruction 8.3%.

### 4.4 Steady, service on the high-priority pool

Runs at N=500/1000/2000 (logs overwritten by §4.5; numbers here). N=500 4,997
polls/s, 5.4 cores. N=1000 2,062 polls/s. N=2000 deadlock: `st_diag.pstack1`
shows 1024/1024 Master-high-pri workers in `ResolveIntents` waiting on a
`GetTransactionStatus` future whose callback runs on the same pool
(`transaction_rpc.cc:174-176`); RefreshYsqlLease timed out after 15 s, the
tserver's lease expired, its PG sessions were killed.

### 4.5 Steady, service on the normal pool (queue 1000)

First sweep (one client driver thread): N=500 4,996 polls/s (`st_500.log.gz`);
N=1000-3000 940-2,124 polls/s with 30 s timeouts. `st_client.top`: the single
driver thread at 98% of a core. After 8 driver threads
(`st_{1000,2000,3000}.log.gz`): 9,886 / 19,845 / 29,562 polls/s, master
6.9 / 7.6 / 8.0 cores, client 3.4 / 4.1 / 4.3 cores, host 12.4-14.2/16, 5,966
queue overflows at N=3000 (aligned ticks). Spread ticks
(`st4_{3000,4000}.log.gz`): N=4000 39,084 polls/s, master 8.2 cores, host
14.5/16, 12,719 overflows, 21,780 txn status RPCs/s, DDLs 221/30 s.

### 4.6 Open: Steady N=3000 spread-tick stall

`host4-16vcpu/st4_3000.log.gz`: handler mean 29 s, queue at 1000, 35 polls/s,
3 DDLs of 30 s each, for the whole phase; the N=4000 run right after was
normal. Not diagnosed; suspected, unverified: the normal pool full of polls
blocked in ResolveIntents on a committed-but-not-applied catalog transaction
whose apply needs the same pool.

### 4.7 Steady profile

`host4-16vcpu/stp_{1000,2000}.{log,perf}.gz`: 9,903 / 19,792 polls/s, 6.76 /
7.59 cores, handler mean 3.1 ms, 33.2 / 19.1 KB. Poll handler 69.3 / 73.8% of
master CPU: record building about 37 / 39% (key decode 8.5%, re-encode about
7%), serialization 10.8%, destruction 11-12%, log cache read 2.5-2.9%,
ResolveIntents 1.6-2.5%; reactors 9-11%; master DDL work 7-14%; locks 0.6%.

### 4.8 Steady-NoDDL (no DDL, every poll empty, spread ticks)

`host4-16vcpu/snd_{1000,2000,4000,8000}.log.gz`: 9,986 / 19,859 / 39,447 /
78,465 polls/s, master 0.93 / 1.58 / 3.19 / 5.52 cores, 93 / 80 / 81 / 70 µs
CPU per poll, handler p99 49-66 µs (mean 0.4-1.0 ms from rare outliers up to
0.7 s), 40 B responses, client 0.55-2.7 cores, host up to 9.0/16; queue
overflows 11,203 (N=4000) and 40,118 (N=8000); 0 transaction status RPCs.
Master not saturated at N=8000.

## 5. 2026-10-01, host4 — WAL size of large DDL

`PgLocalCatalogDdlWalSizeTest.DISABLED_DdlWalSize`,
`host4-16vcpu/ddl_wal.log.gz`; results in
[loadtest-large-ddl-wal-sizes.md](loadtest-large-ddl-wal-sizes.md). Largest:
`DROP SCHEMA ... CASCADE` with 100 tables 2.0 MB; `ANALYZE` of 100 tables
1.6 MB (+145 catalog versions; +1 inside `BEGIN ... COMMIT`); `ADD COLUMN` on
100 partitions 0.72 MB; 20-statement migration batches 0.13-0.30 MB.
`CREATE DATABASE d1` (`DISABLED_DdlWalSizeDatabase`,
`host4-16vcpu/ddl_wal_db.log.gz`): 41 ops, 7.9 MB WAL, 11.6 MB / 41,831
records of responses; `DROP DATABASE d1` 135 ops, 153 KB.
