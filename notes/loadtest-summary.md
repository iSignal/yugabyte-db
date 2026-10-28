# Master load test — summary

Release build, 1 master, host dev-server-sanketh-4: 16 vCPUs (8 cores x 2
hyperthreads), idle apart from the test. Fixes 1 and 2 in, fix 3 out, change
service on master's normal-priority pool. Plan: [loadtest-plan.md](loadtest-plan.md);
runs: [loadtest-worklog.md](loadtest-worklog.md).

FixedLag = every stream catches up 50 DDLs' worth of WAL (2297 ops) again on
every poll. Steady = streams at master's current position, 4 DDL sessions
running. Steady-NoDDL = streams at master's current position, no DDL at all,
so every poll returns no records (spread ticks).

| Scenario | Polls/sec | Mean master response time | Mean master CPU/poll | Cores used (master) | Mean response size |
|---|---|---|---|---|---|
| FixedLag, log cache, N=100 | 958 | 41.5 ms | 15.8 ms | 15.2 | 626,231 B |
| FixedLag, no cache, N=100 | 374 | 251.9 ms | 37.6 ms | 14.1 | 618,637 B |
| Steady, log cache, N=500 | 4,996 | 1.4 ms | 1.10 ms (0.81 ms excluding DDL baseline) | 5.5 | 46,553 B |
| Steady, log cache, N=1000 | 9,903 | 3.1 ms | 0.68 ms (0.53 ms excluding DDL baseline) | 6.8 | 33,202 B |
| Steady, log cache, N=2000 | 19,792 | 3.1 ms | 0.38 ms (0.31 ms excluding DDL baseline) | 7.6 | 19,098 B |
| Steady-NoDDL, N=1000 | 9,986 | 1.0 ms (p99 66 µs) | 93 µs | 0.93 | 40 B |
| Steady-NoDDL, N=2000 | 19,859 | 0.8 ms (p99 60 µs) | 80 µs | 1.6 | 40 B |
| Steady-NoDDL, N=4000 | 39,447 | 0.5 ms (p99 49 µs) | 81 µs | 3.2 | 40 B |
| Steady-NoDDL, N=8000 | 78,465 | 0.4 ms (p99 50 µs) | 70 µs | 5.5 | 40 B |

- Mean master response time = master handler mean. CPU/poll = master CPU /
  polls/s; Steady's DDL baseline is 1.47 cores.
- No cache = `log_cache_size_limit_mb=0`.
- Steady-NoDDL: the handler mean is above its p99 because of rare long
  outliers (max 0.3-0.7 s); 11,203 (N=4000) and 40,118 (N=8000) polls were
  rejected with the 1000-entry queue full, at most 1.7% of polls. Master is
  not saturated at N=8000 (host 9.0/16 busy, client 2.7 cores); with no
  catalog transaction open every poll computes a new safe time and resolves
  nothing (0 transaction status RPCs).
- FixedLag is master-CPU-bound in both rows; Steady meets 10 x N polls/s at
  these N (39,084 polls/s at N=4000 with spread ticks, host 14.5/16 busy,
  master 8.2 cores).

## Where the overhead is

1. FixedLag, log cache: about 97% of master CPU produces one identical
   626 KB protobuf response per stream: building records from the cached WAL
   ops 34% (key decode, re-encode, copying key and value bytes), serialization
   36% (size computation about 15%), destruction 27%. Log cache read 1.7%,
   locks 0.3%.
2. FixedLag, no cache: the same, plus converting the WAL's on-disk format —
   2.4x the CPU per poll (38 ms against 16 ms): WAL entry decode and message
   parse 41% of master CPU, a reflection walk for response memory accounting
   (`SpaceUsedLong`, disk path only) 11%. Disk I/O 0.8% (`pread`); the WAL is
   in the page cache.
3. Steady, log cache: the same kind of work on 19-33 KB responses: poll
   handler 69-74% of master CPU — building records 37-39% (key decode and
   re-encode about 15%), serialization about 11%, destruction 11-12%. RPC
   network I/O 9-11%, master's DDL work 7-14%; ResolveIntents, log cache read
   and locks at most 3% each. ResolveIntents' cost is load elsewhere:
   10-22k `GetTransactionStatus` RPCs/s on the coordinators at N=2000-4000.

## Reducing it

Building, serializing and freeing the same records once per stream is the
cost. Options: serialize each WAL op's records once and assemble responses by
copying bytes (per-op cache), or build one batch per 50-100 ms cycle from the
earliest near-head position to the latest S and serve (or push) the same
serialized bytes to every tserver, with laggers on the per-stream path. Fix 3
(shared safe time) cuts coordinator RPCs and blocked threads, not CPU.

## Push (push-plan.md §9.7), 2026-10-07

Release build of repo6's uncommitted master-push implementation (the pull path
removed), same host (dev-server-sanketh-4, 16 vCPUs), 1 master, 1 real
tserver with its copy off unless stated. N push receivers in the test process
register with master (`TEST_local_catalog_push_extra_targets`), acknowledge
what they receive, and discard the records. Source snapshot and logs:
`loadtest-results/host4-push/` (`source-repo6.patch`, `pl-run-*.log.gz`,
`run-*.vmstat`). Host busy during the runs (vmstat): 0.6-6.7 of 16 vCPUs, no
other work on the host.

The receivers have no YSQL lease, so they are not lock-release targets: a DDL
does not wait for them, and DDL latency below shows only master's CPU
contention, not the gate.

### Steady: DDLs fan out to N receivers

4 DDL sessions (CREATE TABLE, ADD COLUMN, DROP TABLE loop), 30 s.

| | Push N=1 (baseline) | Push N=200 | Push N=1000 | Pull N=1000 (above) |
|---|---|---|---|---|
| master cores | 1.52 | 2.40 | 4.33-4.43 | 6.8 |
| master cores above the N=1 baseline | — | 0.88 | 2.81-2.91 | 5.33 (above its 1.47 DDL baseline) |
| DDLs in 30 s | 1149 | 1119 | 972-1014 | — |
| bytes sent | 16 MB | 2.98 GB (98.6 MB/s) | 12.6-13.2 GB (417-434 MB/s) | 327 MB/s |
| per DDL per receiver | — | 13.3 KB, 1.8 push RPCs | 13.0 KB, 1.13 push RPCs | 10 polls/s per stream |
| push RPCs/s | 76 | 13.4k | 36-38k | 9.9k polls/s |
| added master CPU per push RPC | — | 66 µs | 74-79 µs | — |
| added master CPU per DDL per receiver | — | 119 µs | 84-91 µs | — |
| push cycles | 2297, mean 646 µs | 2238, mean 723 µs | 1937-2023, mean 846-928 µs | — |
| pages built (each once) | 2136 | 2086 | 1780-1896 | — |
| in flight max | 2 RPCs | 48 RPCs, 11.9 MB | 48 RPCs, 11.9 MB | — |
| coordinator status RPCs/s | 149 | 149 | 135-137 | — |
| DDL p50 / p99 | 99 / 158 ms | 102 / 165 ms | 114-118 / 182-198 ms | — |
| push failures, gaps | 0, 0 | 0, 0 | 0, 0 | — |

Where master's CPU goes at N=1000 (perf, 15 s, `perf-steady1000-children.txt`):
the pusher's scheduler loop 19.9% (issuing the async push RPC 9.0%, target
hash-map lookups 2.7%, `PrepareSend` 2.6%, `PushCall` allocation 2.0%,
`OnPushDone` 1.4%); RPC reactors sending the pushes 14.6%
(`ProcessOutboundQueue`); reactors reading responses 13.7%, of which handling
push responses 7.0%; memcpy 4.6%; the rest is master's own DDL work
(DDL service, invalidation-message reads, system catalog writes, object
unlocks, about 20%). Per-target serialization, which dominated pull, is gone;
what remains is the fixed cost of one push RPC per DDL per target.

### Catch-up: K receivers start behind and are pushed to the head

`PushJoinerWave`: K receivers start at the position recorded before the lag
DDLs; no DDL runs during the phase. "Pull estimate" = K x the bytes x the
pull rate measured above (25 ms/MB from the log cache, 61 ms/MB without).

| | K=100, 50 DDLs | K=1000, 50 DDLs | K=100, 2000 DDLs, 1 MB page cache | K=1000, 2000 DDLs, 1 MB page cache |
|---|---|---|---|---|
| bytes per receiver | 873 KB (*) | 873 KB (*) | 26.6 MB | 26.6 MB |
| bytes sent | 87 MB | 0.87 GB | 2.66 GB | 26.6 GB |
| push RPCs | 100 | 1000 | 2600 | 26,000 |
| pages built from the WAL (transient) | 0 | 0 | 72 | 24 |
| master CPU | 0.45 CPU-s | 1.29 CPU-s | 3.99 CPU-s | 25.9 CPU-s |
| master CPU per MB sent | 5.2 ms | 1.5 ms | 1.5 ms | 1.0 ms |
| pull estimate | 1.6 CPU-s | 16 CPU-s | 65 CPU-s (from disk 160) | 650 CPU-s (from disk 1600) |
| caught up, p50 / max | 609 / 633 ms | 0.88 / 1.21 s | 8.35 / 8.44 s | 12.3 / 13.6 s |
| in flight max | 48 RPCs, 42 MB | 48 RPCs, 42 MB | 48 RPCs, 50 MB | 48 RPCs, 50 MB |
| gaps | 0 | 0 | 100 (one per receiver) | 1000 (one per receiver) |

(*) A receiver whose position master does not know yet is pushed from the
start of master's page cache, which here begins about 250 KB before the 50
DDLs (0.62 MB), so each receiver got 873 KB.

Times are on loopback with receivers in one process; a real NIC adds
bytes / bandwidth (26.6 GB at 10 Gbit is about 21 s).

### Idle, CREATE DATABASE, calibration

| Scenario | Result |
|---|---|
| `PushIdle`, N=8000, no DDL, 30 s | 0 push cycles in steady state. Master 0.16 cores for the whole phase, which includes one initial push to each new receiver: 8001 RPCs, 1.98 GB (247 KB each, the look-back window master pushes to a receiver whose position it does not know). Pull at N=8000 cost 5.5 cores continuously. |
| `CreateDatabaseFanout`, N=100 and N=1000 | one `CREATE DATABASE` (1.27 s / 1.34 s). N=100: 1.185 GB pushed (11.85 MB per receiver), 1001 RPCs, master 2.17 CPU-s for the whole 33 s phase including the statement (pull estimate 29 CPU-s). N=1000: 11.85 GB, 10,001 RPCs (10 per receiver, about 1.2 MB each), master 10.5 CPU-s for the 63 s phase, 0.89 ms per MB (pull estimate 290 CPU-s). In flight max 48 RPCs / 70 MB in both. Every receiver received the full 11.85 MB within the phase. Rerun after the harness and pusher fixes (`host4-push/rerun-2/`): N=100 master 1.76 CPU-s (1.49 ms/MB), N=1000 master 9.16 CPU-s (0.77 ms/MB), same bytes and RPCs; that rerun still read the head after requesting the cycle, so the head included writes no cycle carried (N=100 0/100 caught up). Rerun 3 (`host4-push/rerun-3/`, head read and clock started when the statement returns): N=100 100/100 caught up, p50 499 ms, max 500 ms, master 2.13 CPU-s; N=1000 1000/1000 caught up, p50 4.74 s, max 4.74 s (11.85 GB, about 2.5 GB/s on loopback), master 9.55 CPU-s (0.81 ms/MB). Rerun 3 shared host4 with another session's release build (up to 7.7 of 16 vCPUs busy). Rerun 4 on a quiet host (`host4-push/rerun-4/`, no other work at start or end, 0.7-0.8 of 16 vCPUs busy): N=100 100/100 caught up, p50 487 ms, max 488 ms, master 1.92 CPU-s (1.6 ms/MB); N=1000 1000/1000, p50 4.65 s, max 4.66 s, master 9.66 CPU-s (0.82 ms/MB). A run with master's push logging (`--vmodule=sys_catalog_pusher=2`) shows where the time goes: the statement returned at 20:14:41.09, the cycle certified at 41.147, and the pushes ran from 41.148 to 41.627 (480 ms for 1.18 GB, about 2.5 GB/s), in 9 page ranges, each sent to all 100 receivers before the next; so the caught-up time is transfer time, and all receivers finish within a few ms of each other. Synchronous completion is not measured (the receivers hold no lease; known limitation). |
| `RealTserversOnly`, 3 real tservers with copies on, 4 DDL sessions | master 1.97 cores (baseline phase 1.65); 1004 DDLs in 30 s, p50 122 ms, p99 193 ms; per tserver about 3 floor requests/s and 66 gate waits/s with a mean wait under 1 µs (the copy already holds the version when the gate checks). Flagged `HOST UNDER LOAD` (0.34 cores outside the test; limit 0.25). |
| `PushFixedLag`, N=100 | Not comparable to pull FixedLag. A receiver that never advances makes every push a failure ("pages did not move the copy", `sys_catalog_pusher.cc:967-995`) and master backs off exponentially: 1100 pushes, 1000 failures, 1100 gaps, 0 caught up, 0.16 cores. Intended (push implementation, 2026-10-07): the receiver accepts each push's first page without moving, so the second page lies above its position and it answers "gap"; the 50 lag DDLs are inside master's page cache, so no transient pages are built. |
| `DdlWalSize`, `DdlWalSizeDatabase` | WAL ops and WAL bytes per DDL match the 2026-10-01 measurements within 2%. Rerun after the harness fix (the receiver reports its position before it is added) and the pusher fix (a new leader's first cycle no longer drops target positions): `record_bytes`, the serialized records above the start position one receiver gets, is 82-96% of the WAL bytes for every DDL: `CREATE TABLE` 13,044 of 15,761; 100 x `CREATE TABLE` 1,297,206 of 1,546,849; `DROP SCHEMA` 100 tables 1,810,225 of 2,003,930; `ALTER TABLE ADD COLUMN` 100 partitions 664,869 of 719,317; whole-database `ANALYZE` 1,515,180 of 1,618,536. `CREATE DATABASE` 11,476,039 of 7,892,754 (145%), `DROP DATABASE` 169,336 of 152,931. Idle control: 0 bytes, 1 page. These match the pull response bytes of 2026-10-01 within 2%. |

### What it means

- Steady fan-out costs about 75 µs of master CPU per push RPC, and the
  pusher sends about one RPC per DDL per target: 1000 targets at about 33
  DDLs/s cost about 2.9 cores, against 5.3 cores for pull at the same N.
  Batching several DDLs into one push per target would cut it in proportion.
- Catch-up costs about 1-1.5 ms of master CPU per MB sent, 15-25x less than
  pull from the log cache and 40-60x less than pull without it, and the
  in-flight bound (48 RPCs, about 50 MB) holds master's memory flat:
  1000 tservers x 26.6 MB took 26 CPU-s and 13.6 s on loopback.
- An idle cluster costs nothing after the first push to each target.

### Catch-up CPU, profiled (2026-10-19)

repo6 at `86cc3c09297` (push implementation committed), release, host4 idle
(no other work at start or end). `PushJoinerWave`, receivers start 2000 DDLs
behind (about 25.8 MB each), 30 s phase. During the catch-up the runner took
master's user and kernel CPU and page faults from `/proc`, per-thread CPU every
second (`pidstat`), and a `sudo perf -g` profile with kernel stacks, in a 16 s
window that opens when master's push byte counter starts rising. Artifacts:
`loadtest-results/host4-push/rerun-6/`.

| | K=1000, 64 MB cache, unprofiled | K=1000, 64 MB cache | K=1000, 1 MB cache | K=100, 64 MB cache |
|---|---|---|---|---|
| bytes sent | 25.8 GB | 25.8 GB | 26.6 GB | 2.58 GB |
| pages per receiver | 4550 | 4544 | 189 (built from the WAL, 24 builds) | 4540 |
| master CPU, 30 s phase | 21.5 CPU-s | 22.4 CPU-s | 20.5 CPU-s | 3.2 CPU-s |
| per MB sent | 0.83 ms | 0.87 ms | 0.77 ms | 1.25 ms |
| all caught up, p50 / max | 14.0 / 14.2 s | 14.5 / 14.7 s | 11.8 / 12.1 s | 1.93 / 1.95 s |
| master user / kernel CPU in the window | — | 15.3 / 6.4 s (29% kernel) | 12.6 / 7.3 s | 2.0 / 0.7 s |
| minor page faults in the window | — | 3,245 | 171,629 | 6,240 |

Where master's CPU goes, K=1000 with the 64 MB cache (10,300 samples, share of
all of master's samples):

| Thread | Work | Share |
|---|---|---|
| pusher (one thread, 62.9% of samples) | copy each page's bytes into the target's push request (`PrepareSend`, `set_records`) | 16.9% |
| pusher | copy the request again while serializing it into the RPC buffer | 16.7% |
| pusher | CRC32c over the serialized request | 7.9% |
| pusher | memory allocator | 7.2% |
| pusher | other: page and op-id protobuf headers, scheduler lookups | 14.3% |
| RPC reactors (33.3% of samples) | kernel copy into the socket and TCP send | 24.1% |
| RPC reactors | other | 8.6% |

Findings:

- The pusher thread is the bottleneck: it ran at 98-100% of one core every
  second from the first push to the end of the catch-up (13-14 s), while the
  reactors used about half a core in total. Catch-up runs at the speed of
  that one thread: about 1.8 GB/s at K=1000.
- The pusher copies every byte twice per target (into the request, then into
  the RPC buffer) and checksums it once; that is about 41% of master CPU and
  about two thirds of the pusher thread. The page bytes are the same for every
  target.
- Page faults are not a factor (3,245 in 16 s). The memory allocator is 7%.
- Larger pages are cheaper: with the 1 MB cache master built 189 large pages
  per receiver instead of 4,544 cached small ones, and the catch-up was 19%
  faster with 9% less CPU, so part of the cost is per page.
- About 29% of master CPU is kernel time on the reactors (socket send); on
  loopback the kernel also does the receive side, so a real network would
  shift part of it off master.

What would remove the bottleneck: build each page's serialized bytes and CRC
once, and send them to every target as a shared buffer (for example an RPC
sidecar that references the page), so the pusher thread no longer copies or
checksums per target; or spread pushes over several threads.
