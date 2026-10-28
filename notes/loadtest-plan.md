# Master load test for the system catalog change service — plan

Design under test: [plan2.md](plan2.md) §4.1 (master poll service), §11.2
item 4. Run history and numbers: [loadtest-worklog.md](loadtest-worklog.md).
Final results table: [loadtest-summary.md](loadtest-summary.md). Open items:
[loadtest-todo.md](loadtest-todo.md). Code: `~/worktrees/repo6`, branch
`docdb_local_catalog`, uncommitted (patch: `loadtest-results/repo6-current.patch`).

## 1. Goal

Measure how many tserver polls master's system catalog change service can
serve, what each poll costs master in CPU, how long a poll takes on master,
and how large the responses are, when many tservers poll at once. Find the
limit (master CPU or something else) without starting that many tservers.

## 2. Why a load client stands in for tservers

Master keeps all per-caller state under the request's `requestor_uuid`
(`sys_catalog_change_service.cc`, `GetOrCreateStream`). A client that polls
with N distinct uuids therefore makes master do what N tservers' pollers make
it do: N stream states, N safe-time computations (each one resolves the open
catalog transactions against their coordinators), N WAL reads from each
stream's own position, and N responses built, serialized and freed. The client
does not apply records; that work happens on each tserver and costs master
nothing.

## 3. Harness

Test `src/yb/yql/pgwrapper/pg_local_catalog_load-test.cc`
(`pgwrapper_pg_local_catalog_load-test`), all tests disabled by default. Run in
release only (fastdebug overstated a lagged poll's CPU about 14x):

```
YB_EXTRA_GTEST_FLAGS="--gtest_also_run_disabled_tests --local_catalog_load_streams=N ..." \
  ./yb_build.sh release --clang21 --cxx-test pgwrapper_pg_local_catalog_load-test \
  --gtest_filter '*FixedLag*'
```

Cluster: `ExternalMiniCluster`, 1 master, object locking, concurrent DDL and
the local catalog copy on, `log_min_seconds_to_retain=7200`, replication factor
min(3, tservers).

Load client (in the test process):

- N streams, each with its own uuid, each polling every 100 ms; at most one
  poll in flight per stream; ticks aligned (default) or spread over the
  interval (`--local_catalog_load_spread_ticks`).
- 8 client messengers (`--local_catalog_load_client_messengers`), one reactor
  and one connection to master each; stream i uses messenger i mod 8 and is
  sent by driver thread i mod 8. One messenger or one driver thread saturates
  a core long before master does.
- Streams that do not advance receive responses into `google::protobuf::Empty`
  (no parse); streams that advance parse each response and move to its
  checkpoint.

Phases: 10 s baseline without the client (DDL load and real pollers only),
then a 30 s load phase with the client (`--local_catalog_load_baseline_sec`,
`--local_catalog_load_duration_sec`). Every number is reported per phase.

## 4. Scenarios

| Test | Real tservers | Their copies | DDL during the phases | Client streams | Stream position |
|---|---|---|---|---|---|
| `FixedLag` | 1 | off | none; 50 `CREATE TABLE lag_<i> (a INT)` run once before the phases (2297 WAL ops) | N | fixed at the position before the 50 DDLs; every poll re-reads the same range |
| `Steady` | 1 | off | 4 sessions looping `CREATE TABLE`, `ALTER TABLE ADD COLUMN`, `DROP TABLE` | N | master's current position; advances with every response |
| `SteadyNoDdl` | 1 | off | none | N | master's current position; advances; every poll returns no records |
| `RealTserversOnly` | `--local_catalog_load_real_tservers` | on | as `Steady` | 0 | real pollers only, including the lock-release gate waits |

Variant: `--local_catalog_load_extra_master_flags=--log_cache_size_limit_mb=0`
(no log cache: every poll reads the WAL). A 1 MB cache is not a useful
variant: it holds the whole test WAL (0.84 MB).

## 5. Measurements

| What | Source |
|---|---|
| Polls/s | master handler count for `GetSysCatalogChanges` |
| Master CPU, total and by thread pool | `/proc/<master pid>/stat`, `/proc/<pid>/task/*/stat` |
| Master CPU per poll | master CPU / polls/s (includes master's other work, e.g. DDL; Steady baseline is about 1.47 cores) |
| Master response time | master handler latency for `GetSysCatalogChanges` (mean, p99, max) |
| Response size | master counter `service_response_bytes_..._GetSysCatalogChanges` / polls |
| Queue | per-service `rpcs_in_queue_*` sampled every 10 ms; `rpcs_queue_overflow` |
| Log cache | `log_cache_num_ops`, `log_cache_size`, `log_cache_disk_reads` of the system catalog tablet |
| Collateral | lease and heartbeat handler p99; `GetTransactionStatus` RPCs/s on tservers |
| Client | per-poll latency (p50, p99, p99.9, max), errors by kind, test-process CPU |
| Real tservers | polls/s, gate waits (count, mean), reads that waited |
| Host | busy CPUs, CPU outside the test process tree; `HOST UNDER LOAD` / `HOST SATURATED` flags |
| Where CPU goes | `perf record -g -p <master pid>` 8-10 s in the load phase (`pgrep -n -x yb-master`); `pstack` for blocked threads; `top -H` for a saturated single thread |

## 6. Code changes made under this plan (uncommitted in repo6)

| Change | Files | Status |
|---|---|---|
| Fix 1: poll carries the waiter's target (`wait_for_safe_time`); master forces S for it; replica wakes waiters only on progress | `cdc_service.proto`, `sys_catalog_change_service.cc`, `local_catalog_poller.{h,cc}`, `local_catalog_replica.{h,cc}` | kept; pg_local_catalog-test 48/48 |
| Fix 2: system catalog log cache evicts only for memory | `consensus_queue.{h,cc}`, `raft_consensus.{h,cc}`, `sys_catalog.cc` | kept; pg_local_catalog-test 48/48 |
| Fix 3: one safe-time computation shared by all streams | `sys_catalog_change_service.{h,cc}` | removed on request; `loadtest-results/fix3-shared-safe-time.patch` |
| Idle-stream pruning at most every 10 s | `sys_catalog_change_service.{h,cc}` | kept; no measurable effect |
| Change service on master's normal-priority pool, queue `master_svc_queue_length` | `master.cc` | kept; not regression-tested |
| Log cache entry caches its serialized size | `log_cache.{h,cc}` | no measurable effect; suggested revert; not regression-tested |

## 7. Scenarios from the original request not covered

1. Case 2 at scale: "a DDL just completed and all tservers poll until their
   local time passes the DDL's commit time (release push off)". The client
   streams poll on a fixed tick and never wait on a gate, so the burst of
   forced-S polls from 200 tservers right after one DDL was not simulated. It
   was measured only with 3 real tservers (`RealTserversOnly`), where it found
   the waiter spin (fix 1). A faithful version needs client streams that, on a
   DDL commit, poll back-to-back with `wait_for_safe_time` until their S
   passes the commit.
2. Case 1 as a real catch-up: `FixedLag` re-polls the same range forever,
   which is an upper bound. One catch-up per tserver after a 1-hour gap
   (advance once, then current) was not run, nor a lag beyond one batch (4 MB)
   or beyond the 128 MB log cache.
3. Calibration: N real tservers against N client streams (planned at N=10)
   was not run; the per-poll cost of real pollers versus client streams is
   unverified.
4. Network: all runs are single-host loopback; no latency (tc netem), no
   bandwidth limit (FixedLag sends 600 MB/s).
5. Three masters (follower replication, leader changes) and a master host
   without the client and the tserver on it.
6. Push instead of pull (`enable_local_catalog_release_push`) under load.
7. The N=3000 Steady stall (spread ticks) is not diagnosed (worklog §4.6).
