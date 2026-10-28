# Load test and poll-cost work items

Status: [ ] open, [~] running, [x] done. Plan and results: loadtest-plan.md.
Worktree: repo6, granted by session "local tserver docdb catalog impl Sep 15"
from 05:43 for about 1 hour; ask again before 06:40 if more time is needed;
message it when done.

## A. Real tservers poll master far above their 100 ms interval

- [x] A1 Diagnosis runs, release, RealTserversOnly, 3 tservers, 4 DDL sessions:
      E1 default; E2 master xcluster_consistent_wal_safe_time_frequency_ms=0
      (no safe-time throttle); E3 no DDL. Logs /tmp/spin_e{1,2,3}_*.log.
      E1 result: 860-1257 polls/s per tserver; a gate wait is open 69-87% of
      the time (mean 87-109 ms); about 1250 polls per second of gate wait.
      E2 (throttle off): 32-36 polls/s per tserver, gate wait mean 6 ms, DDL
      p50 299 ms (E1: 1.00 s). E3 (no DDL): 9.9 polls/s per tserver, no waits.
- [x] A2 Cause: while a DDL keeps a catalog transaction open, master computes
      a new S for a stream at most once per 250 ms; the replica wakes every
      waiter after every poll (local_catalog_replica.cc:287) and each waiter
      requests another immediate poll (:324, :346), so a tserver with an open
      wait polls back-to-back until the throttle lets S through.
- [x] A3 Fix (uncommitted in repo6: cdc_service.proto field 3 wait_for_safe_time; sys_catalog_change_service.cc; local_catalog_poller.{h,cc}; local_catalog_replica.{h,cc}). Candidate: the poll request carries the waiter's target time;
      master updates its clock from it and computes S for that request
      without the 250 ms throttle; the replica wakes waiters only when C or
      an applied version moved. Requests without the field (the load client)
      keep today's path.
- [x] A4 Verify the fix. E1 with the fix: 31-35 polls/s per tserver (was
      860-1257), gate wait mean 6.1-6.4 ms (was 87-109 ms), DDL p50 295 ms
      (was 1.00 s), 409 DDLs in 30 s (was 120). pg_local_catalog-test
      (release) with A3 + B4 + pruning: 48/48 passed (/tmp/regress_local_catalog.log).

- [ ] A5 DDL latency with copies on is p50 1.00 s, p99 1.03 s (E1), against
      135-169 ms with the copy off (Steady). A fixed 1 s wait is suspected
      (heartbeat interval?). Find it. With the A3 fix the p50 is 295 ms; why it
      was exactly 1.0 s before is not identified (low priority).

## B. Master CPU per poll, and whether master can serve 100+ streams

- [x] B0 Release numbers at N=10 (FixedLag response 0.62 MB, 2347 records per poll): Steady about 1.9 ms CPU per poll;
      FixedLag (2297 ops behind) about 33 ms CPU per poll.
- [x] B1 Steady, release, N=50 and N=100: does master keep up (polls/s =
      10 x N, latency, queue)? Yes at N=100: 999 polls/s, master 1.02 -> 1.59
      cores, handler mean 7.1 ms p99 46 ms, client p50 24 ms p99 61 ms,
      queue max 44, no overflow, no errors. N=50: 499 polls/s, 0.95 -> 1.33
      cores. Added CPU per stream falls with N (0.019, 0.0075, 0.0057 cores at
      N=10, 50, 100); records per poll also fell (93, 76, 68). N=200: 1998
      polls/s, master 0.98 -> 1.77 cores, handler mean 9.7 ms p99 67 ms,
      client p50 33 ms p99 99 ms, queue max 99, no overflow, no errors.
      All B1 numbers are with the old eviction (cache empty); redo after B4.
- [x] B2 (two attempts failed: master's argv[0] is plain "yb-master", so a
      path pattern never matches; now pgrep -n -x yb-master) perf profile of master during Steady and during FixedLag: WAL read
      and decode, record build, ResolveIntents, response size accounting,
      serialization, locks.
- [x] B5 Remaining cost at N=200 (perf, Steady, after B4), share of master
      CPU samples: poll handler 41%, of which record building (key decode and
      re-encode, allocation) about 12%, response serialization 6.8%, response
      destruction 4.5%, log cache read 5.6%, ResolveIntents 1.5%,
      GetOrCreateStream 1.3% (pruning scan on every call). Every stream
      repeats record building and serialization for the same WAL ops; the
      structural fix is todos T21 (build each batch once, serve all streams).
      Simple fix applied: prune idle streams at most every 10 s. Test
      artifact: the 10 ms queue sampler costs master 4.3% (metrics JSON).
      Transaction status RPCs grow with N (587/s at N=100, 1185/s at N=200):
      each stream resolves open transactions once per 250 ms.
- [x] B5a FixedLag perf after B4 (10 ms CPU per poll): building records
      about 42%, serialization 17.9%, destruction 17.6%, log cache read 13.5%,
      ResolveIntents and stream lookup about 0%. About 78% is per-stream
      duplication of an identical response. Structural fix = T21 (not simple;
      design in loadtest-plan.md §10.3).
- [x] B6 Shared safe time across streams (written in
      sys_catalog_change_service.{h,cc}, not built yet): a stream due for a
      new S adopts the most recent (S, WAL position) pair any stream computed
      in the same leader term within the 250 ms window; a request with a wait
      target adopts it only when S reaches the target; the release path
      always computes. Steady N=200 result: txn status RPCs/s 110 (was
      1185-1202; DDL-only baseline 40), poll handler mean 1.0 ms p99 24 ms
      (was 5.8 / 42), polls with S 59% (was 33%), client p50 20 ms p99 61 ms,
      master CPU unchanged (+0.39 cores), queue max 136. RealTserversOnly (3
      tservers): gate wait mean 5.7 ms, DDL p50 277 ms, 433 DDLs in 30 s.
      pg_local_catalog-test (release) on the final code (A3 + B4 + pruning +
      B6): 48/48 passed (/tmp/regress_local_catalog_final.log). Verify with Steady N=200 (txn status RPCs/s, master CPU) and
      RealTserversOnly (gate waits), then rerun pg_local_catalog-test.
- [x] B3 Not needed: perf was conclusive (B5, B5a). If perf is not conclusive: per-stage thread-CPU timers in the change
      service (resolve intents, WAL read, record build, total), exported as
      metrics and read by the test.
- [x] B4 (implemented: PeerMessageQueue::SetEvictLogCacheOnlyForMemory, set
      by SysCatalogTable::OpenTablet when the feature is on; the leader and
      follower skip EvictThroughOp(all-replicated index); size and
      server-wide memory eviction unchanged. Runs FixedLag N=10, Steady
      N=100, N=200 with perf done.) Result: FixedLag N=10 cache holds 2902
      ops (0.83 MB), 0 disk reads; master 0.98 cores (was 2.98-3.05), CPU per
      poll 9.8 ms (was 32-33), 99.9 polls/s. Steady N=100 +0.20 cores (was
      +0.57); N=200 +0.31 cores (was +0.79), client p50 23 ms p99 70 ms.
      Test the log-cache hypothesis: the system catalog stream never
      reports its position (UpdateCDCConsumerOpId), so master's log cache
      holds none of its ops and every poll re-reads the WAL from disk.
      Experiment: pin the cache for the slowest stream; measure CPU per poll.

## C. Notes and hand-off

- [x] C1 Release numbers and the findings of A and B into loadtest-plan.md §9.
- [x] C2 Message the other session that repo6 is free.

## D. Open after 2026-09-30

- [ ] D1 T21: build each WAL range's response once and serve every stream from
      it (serialized bytes), the remaining bottleneck (about 78% of a lagged
      poll). Design sketch in loadtest-plan.md §10.3.
- [ ] D2 A5: why DDL latency was exactly 1.0 s before fix 1.
- [ ] D3 Review and land fixes 1-2 (patch: loadtest-results/fixes-1-2-and-loadtest.patch,
      matches repo6). Fix 3 was removed from repo6 on request (2026-09-30);
      it is kept as loadtest-results/fix3-shared-safe-time.patch. Add unit tests: waiter wakes only on
      progress; wait_for_safe_time forces S; sys catalog log cache keeps ops
      after replication; shared safe time adopted across streams.
- [ ] D4 Measure on a larger, unshared host and with 3 masters; add network
      delay (tc netem) for the lagged case (0.62 MB per poll).
- [ ] D5 Test harness: write the CSV to a stable path; the 10 ms queue
      sampler costs master about 4% (metrics JSON), sample less often or
      subtract.
