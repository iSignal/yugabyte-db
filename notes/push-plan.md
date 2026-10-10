# Master push of the system catalog WAL: design and implementation plan

Date: 2026-10-07. Implements [post-perf-design.md](post-perf-design.md) §3 on
the code described in [implementation2.md](implementation2.md). Companions:
[plan2.md](plan2.md) (the design this amends, §4.1, §4.2, §4.4, §7.1),
[loadtest-summary.md](loadtest-summary.md) (the measurements it answers),
[todos.md](todos.md) (T1, T2, T16, T21 are absorbed here). Code pointers are
to the worktree `repo6`, branch `docdb_local_catalog`, uncommitted fixes 1
and 2 included.

## 1. What changes, in one table

| Requested item | What this plan does | Section |
|---|---|---|
| 1. Master keeps the tservers' positions and a bounded record-format cache; pushes in pages; only to tservers that need a page; every 5 min by default; reads the WAL for positions below the cache | Target map on master; page cache of serialized records; push cycle that certifies S once, builds each page once, sends the same bytes to every target below the page; pages built from the WAL for stragglers and sent only to them; timer cycle every `local_catalog_push_interval_ms` (300 000) | 2 |
| 2. Lock release pushes first, waits for every lease holder's acknowledgement, then releases | The release's RPC carries the pages a target needs when they fit (the common case: the DDL's own records); a target that needs more is pushed in pages and its release handler waits at its gate; the gate, not master, is what orders "copy holds S" before "locks released", exactly as today | 2.5, 6.2 |
| 3. A tserver that regains its lease catches up to now + skew before PG starts | The lease manager sends a floor request carrying the bound `MaxGlobalNow()` and defers the postmaster restart until the copy serves at that bound, with a timeout after which PG starts and reads master as today | 3.3 |
| 4. A wave of rejoining tservers holds DDLs back | They do not: a copy that is not serving has no backends to protect, so its release handler acknowledges without waiting (today's rule), and the serving flip is conditioned on A[db] reaching every version published meanwhile (T2). Delaying the lease is rejected. | 4 |
| 5. A tserver with a floor asks master for a push instead of polling | `RequestSysCatalogPush{uuid, applied op id, floor}`; master records it and runs a cycle at once whose S covers the floor; requests within a short window share one cycle | 2.6, 3.2 |

Nobody pulls. `GetSysCatalogChanges` stays only behind
`enable_local_catalog_push=false` for A/B runs and for the load harness until
the harness has push receivers (8.6); it is removed afterwards.

## 2. Master side

All of it lives in one new master component, `SysCatalogPusher`
(`src/yb/master/sys_catalog_pusher.{h,cc}`), owned by `Master` next to
`sys_catalog_change_service_` (`master.cc:338`), started when the feature
is on. It owns: the target map (2.1), the page cache (2.2), one scheduler
thread that runs push cycles (2.3), the in-flight accounting (2.4), and the
RPC handlers for floor requests (2.6). The record-building functions of
`xcluster_producer.cc` (`PopulateWriteRecord`, `PopulateTransactionRecord`,
`PopulateChangeMetadataRecord`, lines 100-270) are reused unchanged; the
loop that calls them (`GetChangesForXCluster`, `:291-490`) is not, because
it builds one response per caller.

### 2.1 Target map

```
struct Target {
  std::string uuid;
  OpId     acked;              // highest WAL position the copy has applied, as last told by this
                               // tserver (push acknowledgement or heartbeat), index-compared
  MonoTime acked_at;
  HybridTime floor;            // highest floor any request from this tserver asked for and no
                               // push has satisfied yet; invalid when none
  HybridTime last_safe_time;   // C the tserver reported in its last acknowledgement
  bool     in_flight;          // one push RPC at a time per target, so pages arrive in order
  int      consecutive_failures;
  MonoTime next_attempt;       // backoff after a failure
};
std::map<std::string, Target> targets_;  // keyed by tserver uuid
```

Entries are created from three sources and never persisted:


- A heartbeat that carries `local_catalog_applied_op_id` (field 25 of
  `TSHeartbeatRequestPB`, read today in `ts_descriptor.cc:207-210`) updates
  `acked` if higher. The heartbeat is the only source for a tserver that has
  never been pushed to, such as every tserver right after a master leader
  change.
- A push acknowledgement updates `acked` and `last_safe_time`. This is the
  primary source: it is fresher than the heartbeat and it is what lets the
  scheduler send the next page to that target at once.
- A floor request (2.6) updates `acked` and sets `floor`.

Membership: a cycle pushes only to tservers in
`ObjectLockInfoManager::GetAllTSDescriptorsWithALiveLease()`
(`object_lock_info_manager.cc:848`, the same set a release targets). A
target with no live lease keeps its entry, so that its position is known
when it rejoins, and is dropped after
`sys_catalog_change_stream_idle_timeout_sec` without any update, as streams
are dropped today (`sys_catalog_change_service.cc:63-70`).

### 2.2 Page cache

The record cache is a deque of pages; each page is the serialized records
of a contiguous range of WAL operations, built once and shared by every
RPC that carries it.

```
struct Page {
  int64_t first_index;      // first WAL index this page covers
  OpId    end;              // last WAL operation covered (term and index), the checkpoint a
                            // receiver advances to after applying the page
  RefCntBuffer records;     // concatenated serialized entries of
                            // `repeated CDCRecordPB records = 4` of GetChangesResponsePB
  size_t  bytes;
  HybridTime safe_time;     // set only on a page sealed at a cycle's certified position (2.3)
  bool    ends_at_restore;  // sealed at a RESTORE_SYS_CATALOG operation: no record for it, and
                            // a receiver below it must re-seed
};
std::deque<Page> pages_;    // ascending, contiguous: pages_[i+1].first_index == pages_[i].end.index + 1
int64_t cached_through_;    // pages_.back().end.index, 0 when empty
```

Rules:

- A page covers every operation in its range, including ones that produce
  no record (a transaction's CREATED/COMMITTED status records, master's
  no-ops). The receiver advances its position to `end` after applying the
  page; this is the same rule the pull path uses with `checkpoint`
  (`local_catalog_poller.cc` `PublishAppliedBatch`).
- Concatenating serialized `records` entries is a valid serialization of
  the repeated field, so a response for a page is assembled by appending the
  page's buffer to a fixed header; no `CDCRecordPB` is built, serialized or
  destroyed per target. Per-target cost is one memcpy of the page into the
  request's `bytes` field (about 0.1 ms per MB, against the 16-25 ms per MB
  measured for building a response).
- A page is sealed when its size reaches `local_catalog_push_page_bytes`
  (1 MB), after a CHANGE_METADATA record (the producer's rule that it is the
  last record of a batch, `xcluster_producer.cc:412-416`), at a restore
  operation, and at a cycle's certified position (2.3). The last of these is
  what attaches a safe time to a page.
- Memory: the deque is bounded by `local_catalog_push_cache_mb` (64), the
  oldest page is dropped first, and the cache is charged to a mem tracker
  under master's, as the change service's responses are today. The log
  cache keeps the system catalog operations too (fix 2,
  `PeerMessageQueue::SetEvictLogCacheOnlyForMemory`); a range below the page
  cache is rebuilt from it, or from disk below that.
- A page is built from `LogCache::ReadOps(after, to, max_bytes)` on the
  system catalog tablet's consensus, with `to` set so that a page never
  reaches beyond the position the cycle certified.
- Nothing from a page is reused across a master leader term: a new leader
  starts with an empty cache, and a term change in the system catalog
  tablet's consensus empties it, because an entry past the old leader's
  committed index may have been replaced (plan2 §11.2 item 1).

### 2.3 Push cycle

One cycle does the work the pull path does once per stream, once:

```
cycle(force_s, floors):
  term := system catalog consensus leader term; on change: clear pages_ and every target's acked
  if force_s or any floor is set:
      for the highest floor f: clock.Update(f)            -- as wait_for_safe_time does today
  leader_safe_time := tablet's leader safe time
  ResolveIntents(leader_safe_time)                           -- once, not once per target
  S := leader_safe_time
  I_S := majority-replicated index at this moment            -- ReadReplicatedMessagesForXCluster's
                                                              -- majority_replicated_index
  build pages for (cached_through_, I_S]: ReadOps, Populate* records, serialize, seal;
      the page sealed at I_S gets safe_time := S (an empty page if nothing was written)
  enqueue every live-lease target t with t.acked.index < I_S
  return (S, I_S)                                            -- a release continues from here (2.5)

drain (scheduler thread, after the cycle returned, under the bounds of 2.4):
  for the next target t with a free slot:
      if t.acked.index + 1 >= pages_.front().first_index:
          next(t) := the first page with end.index > t.acked.index
      else:
          next(t) := a transient page built now from the WAL for (t.acked, min(t.acked + page, pages_.front().first_index));
                     built once and shared by every target whose next page is the same range
                     (joiners from one partition sit at close positions); dropped when no target needs it
          if the WAL no longer holds t.acked + 1: send reseed_required instead
      send next(t); on acknowledgement, t.acked := reported position; re-enqueue t if still below I_S
```

The cycle itself never reads below the cache: the transient pages of a
target far behind are built on the scheduler thread when that target's
slot comes up, so a release that triggered the cycle has already sent its
RPCs by then.

What S means is unchanged from plan2 §4.1: every transaction that touched
the system catalog and committed at or below S has its apply record at or
before I_S, because `ResolveIntents` ran before I_S was captured and S is
below master's clock from then on. A receiver that has applied every page
through the one carrying S may publish C := S. The pair (S, I_S) stays
valid forever, so the page keeps its safe time for a tserver that receives
it in a later cycle.

The 250 ms throttle of the per-stream path
(`xcluster_consistent_wal_safe_time_frequency_ms`) becomes a cycle
throttle: with catalog transactions open, cycles that are neither forced by
a release nor requested by a floor run at most every
`local_catalog_push_min_cycle_interval_ms` (50). Cycles requested within that
interval share one cycle. Status round trips to coordinators are therefore
at most 20 per second for the whole cluster, against 4 × N per second today
(10-22 k/s at N=2000-4000 measured).

### 2.4 In-flight bounds and scheduling

- One push RPC in flight per target, so pages reach a copy in order and a
  gap can only come from a lost or reordered push, which the receiver
  detects (3.1).
- At most `local_catalog_push_max_in_flight_rpcs` (64) RPCs and
  `local_catalog_push_max_in_flight_bytes` (64 MB) outstanding across all
  targets. The queue is drained in waves: an acknowledgement frees its
  slot and bytes, the scheduler sends that target's next page if it has
  one, else the next waiting target's first page.
- Order within the queue: targets with a floor set first (a request is
  waiting on them), then targets by ascending `acked` (the furthest behind
  first, so that the shared transient pages are consumed and dropped
  early), release-driven targets interleaved by arrival.
- Failure: a push that fails (RPC error, timeout, receiver error other than
  "gap") backs off per target (100 ms doubling to 5 s) and counts
  `consecutive_failures`; the target keeps its entry and is retried on
  later cycles. A receiver that answers "gap, I am at X" resets `acked` to
  X and the next page is chosen from there at once.
- The scheduler never blocks on an RPC: sends are asynchronous through a
  `rpc::Rpcs` with a `TabletServerServiceProxy` per target (the proxy cache
  master already keeps for `RetrySpecificTSRpcTask`), callbacks re-enter
  the scheduler under its mutex.

### 2.5 Release integration

Today `UpdateAllTServers<ReleaseObjectLockRequestPB>::PrepareLocalCatalogPushes`
(`object_lock_info_manager.cc:1772-1900`) fetches one batch from the lowest
fresh reported position, slices it per target, caps a slice at 1 MB and
skips stale or far-behind targets. It is replaced by:

```
LaunchRpcs:
  ts_descriptors_ := live-lease tservers
  cycle := pusher.RunCycleAndWait(force_s = true)       -- S after the DDL's commit; blocks the
                                                        -- release thread as GetChangesForRelease
                                                        -- does today (metric local_catalog_release_push_fetch_us)
  for each target t:
      needed := cycle.PagesNeeded(t)                    -- from t.acked, which the previous release's
                                                        -- acknowledgement or a heartbeat set
      if needed is within the cache and total bytes(needed) <= local_catalog_release_inline_max_bytes (4 MB):
          local_catalog_pushes_[t] := header + needed    -- one memcpy per page per target
      else:
          pusher.Schedule(t)                             -- paged pushes from the drain of 2.3; the
                                                        -- release's gate waits if the copy is serving,
                                                        -- acknowledges at once if it is not (4)
  LaunchRpcsFrom(0)
```

`PagesNeeded` only looks up the cache; a target below the cache (a
rejoining tserver) is scheduled and its pages are built by the scheduler
thread later, so the release thread never reads the WAL below the head
pages the cycle built.

The release RPC's payload field stays `ReleaseObjectLockRequestPB.local_catalog_changes`
(field 16, never persisted). The receiver's `ApplyPushedBatch` path stays,
fed through the apply queue (3.1). Two differences from today: the payload
is a sequence of pages instead of one sliced batch, and no target is left
without records because its report is stale or it lags: a target whose
position master does not know (new leader, no heartbeat yet) is scheduled
for a push from a fixed look-back (`local_catalog_push_unknown_position_lookback_ops`,
default 1000, the receiver drops the overlap) and, if that does not cover
it, the receiver's gate wait sends a floor request (3.2) that tells master
the position.

The release's deadline semantics are unchanged: the tserver's gate waits up
to the release deadline, a timeout fails the RPC and master retries
(`UpdateTServer::HandleResponse`, `:2088-2120`); the DDL's client waits
`tserver_yb_client_default_timeout_ms` and then is told success while master
completes the release asynchronously (`ts_local_lock_manager.cc:983-988`),
which plan2 §11.2 item 2 and [post-perf-design.md](post-perf-design.md) §4
accept.

### 2.6 Floor requests

New master RPC on `SysCatalogChangeService`:

```
message RequestSysCatalogPushRequestPB {
  optional bytes   requestor_uuid = 1;
  optional OpIdPB  applied_op_id  = 2;   // the copy's position, so master needs no heartbeat
  optional fixed64 floor          = 3;   // hybrid time the copy must be complete up to
}
message RequestSysCatalogPushResponsePB {
  optional CDCErrorPB error = 1;
}
```

The handler updates the target's `acked` and `floor` (keeping the higher of
the existing and the requested floor) and wakes the scheduler. The
scheduler runs a cycle whose S is at or above the highest pending floor
(clock update, then `ResolveIntents`), subject to the coalescing interval
of 2.3. The floor is cleared when the target acknowledges a page whose
safe time is at or above it. A request whose floor the last cycle already
covers (floor ≤ pages_.back().safe_time) needs no new S: the scheduler sends
the pages the target lacks, the last of which carries that safe time.

A tserver sends at most one floor request at a time (3.2), so a storm of
1000 tservers each waiting on a floor costs master 1000 small RPCs and one
cycle, not 1000 cycles.

### 2.7 Timer cycle

Every `local_catalog_push_interval_ms` (300 000) the scheduler runs a cycle
with `force_s = false` if any of: the system catalog WAL head is above
`cached_through_`, any live-lease target's `acked` is below the head, any
floor is pending. Otherwise the tick does nothing and costs nothing. This
cycle carries master's own catalog writes, temporary-relation DDL (whose
writer's own-write floor requests a cycle sooner, 3.2), and non-DDL catalog
writes, and it advances C on an idle cluster so that fresh catalog snapshots
do not read at an old time indefinitely.

Why 5 minutes is acceptable: C is the read time of a fresh catalog snapshot
(plan2 §5.3 rule 4) and reads below C are consistent and gated by versions,
so a C up to 5 minutes old is correct. A read that carries its own time h
above C (an explicit `yb_read_time` or `ysql_dump --read-time`, a catalog
snapshot opened on master and still in use, a user catalog scan at the
transaction's read time under plan2 §14 step 2) no longer goes to master:
h is at or below now, so the tserver sets floor h, requests a push, waits
up to `local_catalog_serve_wait_timeout_ms`, and serves at h (plan2 §5.3
rule 5 and todos T13 become this one rule). The timer's only visible cost
is therefore one floor round trip for the first such read after a quiet
period. Every number in 6 scales with the number of non-empty cycles, not
with the interval.

### 2.8 Leader change, restore, WAL retention

- Master leader change: the new leader's pusher starts with an empty cache
  and an empty target map. Targets fill from heartbeats within one
  heartbeat interval (1 s) and from floor requests at once. A release in
  that window pushes from the look-back position of 2.5. No tserver pulls.
- Restore of the system catalog: page building stops at the
  RESTORE_SYS_CATALOG operation (today's `update_on_snapshot_op_func`,
  `sys_catalog_change_service.cc:140-148`); the page is sealed with
  `ends_at_restore`; a target whose next needed page is that one is sent
  `reseed_required` with no records, as the pull answers today.
- WAL retention: unchanged (plan2 §7.3). A target whose `acked + 1` is
  below the retained WAL is sent `reseed_required`; today's
  `CHECKPOINT_TOO_OLD` answer becomes that field. Copies still do not
  anchor master's WAL.

## 3. Tserver side

### 3.1 Push handler and apply queue

New tserver RPC on `TabletServerService` (next to `ReleaseObjectLocks`,
`tserver_service.proto:155`):

```
message PushSysCatalogChangesRequestPB {
  optional bytes  target_uuid  = 1;   // refused if it is not this tserver's
  optional OpIdPB page_from    = 2;   // the position the page follows: page.first_index - 1
  optional OpIdPB page_end     = 3;   // the page's end checkpoint
  optional bytes  records      = 4;   // the page's serialized records
  optional fixed64 safe_time   = 5;   // set only on a page sealed at a certified position
  optional bool   reseed_required = 6;
  optional int64  master_term  = 7;
}
message PushSysCatalogChangesResponsePB {
  optional TabletServerErrorPB error = 1;
  optional OpIdPB  applied_op_id = 2;   // after this page, or the position a gap was found at
  optional fixed64 safe_time     = 3;   // C after this page
  optional bool    gap           = 4;   // page_from.index > applied.index: master re-sends from applied
}
```

`LocalCatalogPoller` becomes `LocalCatalogApplier`: the same thread, the
same `EnsureLocalTablet`, `ApplyRecords`, `PublishAppliedBatch`,
`PersistCheckpoint`, re-seed handling, but driven by a queue of pages
instead of a timer:

- The push handler and the release handler both enqueue a page and wait,
  with their RPC deadline, for the applier to report the outcome (today's
  `PendingPush` handoff, `local_catalog_poller.cc:475-540`, generalized from
  one slot to a queue ordered by `page_from`).
- The applier takes the lowest page; if `page_from.index > applied.index`
  it answers `gap` with its position and drops that page; if
  `page_end.index <= applied.index` it answers success without applying
  (the overlap case of a release page racing a push); otherwise it drops
  the records at or below `applied` (as `DoApplyPushedBatch` does), applies
  the rest, advances the position to `page_end`, and if the page carries a
  safe time publishes C and A[db] as `PublishAppliedBatch` does.
- A page carrying `reseed_required` triggers `RequestReseed` as today.
- The periodic `FetchAndApplyOnce` is kept only for
  `enable_local_catalog_push=false`.

### 3.2 Waits become floor requests

Every place that calls `RequestImmediatePoll(target)` today calls
`RequestPush(floor)` instead:

| Caller | Floor | Today |
|---|---|---|
| Release handler gate wait (`ts_local_lock_manager.cc:577-590`) | the release's `catalog_versions_read_time` (or, after T1, the version: see below) | `WaitForSafeTime` → `RequestImmediatePoll(target)` |
| Heartbeat-held version report (`heartbeater.cc:560-573`) | the report's read time | `HoldsCatalogStateAt` → immediate poll |
| Own-write check after a non-version DDL (`pg_client_session.cc:3573-3646` step 3) | the session's `own_catalog_write_floor_` | `WaitForSafeTime` |
| Version check before a local read (step 2) | `clock()->Now()` | `WaitForAppliedVersion` |
| Lease regain (3.3) | `MaxGlobalNow()` at the grant | `OnLeaseGained` → immediate poll |
| Session wait after a master fallback (todos T13, planned) | the master read's time | — |
| A read carrying its own time h above C (plan2 §5.3 rule 5; §2.7 here) | h | routed to master |

`LocalCatalogReplica::RequestPush(floor)` keeps the highest pending floor
(today's `wait_target_` CAS, `local_catalog_poller.cc:125-132`), sends one
`RequestSysCatalogPush` at a time, and re-sends it at most every
`local_catalog_floor_request_interval_ms` (1000) while the floor is not
reached, so that a lost request or a master leader change is repaired. The
waits themselves (`WaitForSafeTime`, `WaitForAppliedVersion`, their
deadlines and the fall-back to master on timeout) are unchanged.

A version floor: with T1 the release handler waits on A[db] ≥ V, not on a
time. The request then carries the version in place of a time
(`optional uint32 db_oid = 4; optional uint64 version = 5;`), and master's
cycle is forced (a new S after the DDL's commit certifies the version's
rows). T1 is independent of this plan and can land before or after it.

### 3.3 Lease regain: catch up before PG starts

Today (`ysql_lease_manager.cc:145-190`): on a new lease epoch the refresher
thread sets the epoch, calls `OnLeaseGained(MaxGlobalNow())` (the bound B),
resets the lock manager, and calls `server_.RestartPG()`; backends then
read master until C ≥ B flips the copy to serving.

Change: the postmaster restart is deferred until the copy serves at B, with
a bound.

```
refresher thread (new epoch e):                    applier thread
  lease_is_live_ = true, epoch = e
  OnLeaseGained(B = MaxGlobalNow())
      state := kDisabledLease, floor := B
      RequestPush(B) ──────────────────────────► master: cycle with S ≥ B, pages for this target
  schedule DeferredPgRestart(e) on a pool; return   ◄──── pages ... page with S ≥ B
                                                     C := S ≥ B, A[db] read at C
DeferredPgRestart(e):                                OnSafeTimePublished: serving condition (T2):
  WaitForServing(deadline = now + local_catalog_serve_wait_timeout_ms)                  C ≥ B and A[db] ≥ every version published
  if the epoch is still e: RestartPG()                   while not serving → kServing
```

- The wait runs off the refresher thread, because that thread must keep
  sending refreshes every `ysql_lease_refresher_interval_ms` (1000) or the
  lease (TTL 30 s) expires again.
- The hold is bounded by `local_catalog_serve_wait_timeout_ms` (the one
  fall-back knob, default 300 000; `0` starts PG at once, `-1` holds until
  the copy serves): if the copy has not reached B by then (a long
  catch-up, a re-seed, master unreachable), PG starts and reads master
  while the copy is not serving, which is today's behavior.
- The serving condition is todos T2 in full: while not serving,
  `TabletServer::SetYsqlDBCatalogVersions*` records the highest version
  published per database; `OnSafeTimePublished` enters serving only when
  C ≥ B and `applied_version_[db]` is at or above that record for every
  database; `OnLeaseGained` in state `kServing` with `safe_time() < B`
  moves to `kDisabledLease`. With PG not running there are no backends in
  the window, and with PG running after the timeout the backends read
  master, so the flip is safe in both cases.

Why every DDL after B reaches this tserver through the release path: master
set `live_lease` for epoch e before answering the refresh
(`RefreshYsqlLease`, `object_lock_info_manager.cc:1393-1430`), the tserver
read B after receiving that answer, and a release computes its targets
(`GetAllTSDescriptorsWithALiveLease`) after its DDL committed. A DDL that
committed after B therefore computed its targets after `live_lease` was
set, and this tserver is among them. A DDL that committed at or before B
and did not target this tserver is covered by C ≥ B (plan2 §7.1).

### 3.4 What is removed on the tserver

With `enable_local_catalog_push` on: the poll timer, the poll RPC and
`wait_for_safe_time`, `IntervalToNextPoll`'s poll interval, and the
`local_catalog_poll_*` flags. The applied op id stays in the heartbeat
(2.1). The re-seed, bootstrap, checkpoint and apply code is untouched.

## 4. The joiner wave

Scenario: a partition heals, K tservers get new lease epochs within a few
seconds, each needs minutes of WAL. DDLs keep running elsewhere.

What happens with this plan, per DDL:

```
master                              joiner J (not serving, PG not started)      serving tserver T
DDL commits; cycle; S_r
release(V, pages) ──────────────────► handler: copy not serving → no gate,     ──► handler: apply page,
                                        record V as published-while-not-serving,    gate passes, publish V,
                                        ack at once                                 ack
◄── acks from all ───────────────────
client told success
                                      pushes keep arriving from the cycle queue;
                                      J flips to serving when C ≥ B and A[db] ≥ V
```

- The DDL is not held: J acknowledges without waiting because it has
  nothing to protect; the versions it was told are folded into its serving
  condition. This is today's rule (`ts_local_lock_manager.cc:577`, skipped
  when `!IsServing()`) made safe by T2.
- Master's resources are held by the in-flight bounds (2.4): K joiners
  consume at most 64 RPCs and 64 MB at a time, so the release's small pages
  to the serving tservers are delayed by at most one wave. The queue order
  of 2.4 puts floor-bearing targets first, which the joiners are; to keep
  release pages ahead of bulk catch-up, release-driven pages take a
  reserved share of the in-flight budget (`local_catalog_push_release_reserved_rpcs`,
  16).
- The wave costs master one cycle per release (as today) plus the bulk
  bytes once per page per joiner; no per-joiner CPU beyond memcpy.

Rejected: delaying the lease grant until the copy is current. The lease
decides when a tserver's locks are released and its sessions killed, and a
grant that waits on a WAL transfer couples availability to catch-up time.

Optional later: a joiner whose needed range exceeds
`local_catalog_push_reseed_over_bytes` re-seeds from master instead of
replaying (a snapshot transfer is bounded on master too and is faster than
hours of WAL), or from a peer tserver's copy (post-perf-design §3 item 5).
Not in this plan.


## 5. Correctness notes

### 5.1 Ordering and gaps

A copy's position moves only by applying a page whose `page_from` is at or
below its position and whose `page_end` is above it. Pages from two
sources (a release's inline payload and a scheduled push) go through one
queue ordered by `page_from`, so the second of two overlapping pages is
filtered to nothing, and a page that starts above the position is answered
`gap` and master re-sends from the acknowledged position. Lost pushes are
repaired by the next cycle (the target's `acked` did not move) and by the
tserver's periodic floor re-send while a wait is open.

### 5.2 Safe time

Unchanged contract (plan2 §4.1): (S, I_S) is certified by `ResolveIntents`
before I_S is captured. A page sealed at I_S carries S; a receiver
publishes C := S only after applying that page, which means every page
before it too (ordering, 5.1). Pages sealed at the byte cap carry no S. A
cycle with no new WAL produces an empty page at I_S with S, which is the
"empty response with a safe time" of the pull path.

### 5.3 Term changes

Positions are compared by index (today's rule,
`local_catalog_poller.cc` `PublishAppliedBatch`). A master leader change
clears the page cache, and a page carries `master_term`; a receiver whose
copy applied entries of a higher term than the sender's (a stale leader)
refuses the page. Entries a new leader truncates are todos T3(a) and not
addressed here.

## 6. Cost after the change

Per the measurements in [loadtest-summary.md](loadtest-summary.md) and
[load_user_scenario_summary.md](load_user_scenario_summary.md):

| Item | Today (pull) | With push |
|---|---|---|
| Idle master CPU, 1000 tservers | 0.93 cores (10 polls/s each at 80-93 µs) | 0: no RPC when nothing changed |
| Master CPU per DDL at N targets | N × build+serialize+destroy of the DDL's records (97 % of a lagged poll) | one build per page + N memcpy; the release's own RPC fan-out as today |
| `CREATE DATABASE` to 1000 tservers (11.6 MB each) | 290 CPU-s + 11.6 GB | about 0.3 CPU-s of page building + 11.6 GB NIC, bounded to 64 MB in flight |
| Coordinator status RPCs with DDLs open | 4 × N per second (10-22 k/s at N=2000-4000) | at most 20/s cluster-wide |
| Joiner catch-up of 26 MB (2000 DDLs) × 100 joiners | 65 CPU-s on master | pages built once (0.65 CPU-s), 2.6 GB NIC in waves |

The NIC is the remaining limit (post-perf-design §4); the one-level
distribution tree is not in this plan.

## 7. Flags

| Flag | Owner | Default | Meaning |
|---|---|---|---|
| `enable_local_catalog_push` | master, tserver | true when `enable_local_tserver_catalog` | push on; off keeps the pull path for A/B |
| `local_catalog_push_interval_ms` | master | 300 000 | timer cycle |
| `local_catalog_push_min_cycle_interval_ms` | master | 50 | coalescing window for requested cycles while catalog transactions are open |
| `local_catalog_push_page_bytes` | master | 1 MB | page size |
| `local_catalog_push_cache_mb` | master | 64 | page cache bound |
| `local_catalog_push_max_in_flight_rpcs` / `_bytes` | master | 64 / 64 MB | fan-out bounds |
| `local_catalog_push_release_reserved_rpcs` | master | 16 | in-flight slots release pages may always take |
| `local_catalog_release_inline_max_bytes` | master | 4 MB | pages carried inside the release RPC |
| `local_catalog_push_unknown_position_lookback_ops` | master | 1000 | push start for a target with no known position |
| `local_catalog_push_rpc_timeout_ms` | master | 30 000 | per push |
| `local_catalog_floor_request_interval_ms` | tserver | 1000 | re-send while a wait is open |
| `local_catalog_serve_wait_timeout_ms` | tserver, runtime | 300 000 (today 5 000) | the one fall-back knob: how long a read wait, an explicit-read-time wait, or the PG start hold after a new lease waits for the copy before master is used; `0` at once, `-1` never |

Removed when push is on: `enable_local_catalog_release_push`,
`local_catalog_release_push_max_bytes`, `_max_lag_ops`,
`_report_max_age_ms`, `local_catalog_poll_interval_ms`,
`local_catalog_poll_rpc_timeout_ms`.

## 8. Implementation steps

Each step builds and passes `pg_local_catalog-test` (release) before the
next; the load harness runs at steps 1, 4 and 6.

1. **Page cache behind the pull RPC (T21 alone).** `SysCatalogPusher` with
   the cache and the cycle of 2.3 but no RPC sends; `GetSysCatalogChanges`
   answers from the cache: the pages above the caller's position,
   concatenated into `changes.records`, with the checkpoint and safe time
   of the last page; a caller below the cache gets transient pages. The
   per-stream `StreamMetadata` goes away. Measure FixedLag N=100: master CPU
   per poll should fall from 15.8 ms to under 1 ms. Tests: unit test of
   page sealing, S attachment, slicing and eviction (`sys_catalog_pusher-test.cc`);
   the pull tests of `pg_local_catalog-test` unchanged.
2. **Tserver push receiver.** `PushSysCatalogChanges` RPC, the apply queue
   of 3.1, `RequestPush` with the floor request RPC of 3.2 replacing
   `RequestImmediatePoll` (the poll timer still runs). Tests: a unit test of
   the queue's gap/overlap/order rules; a cluster test that pushes a page
   through the RPC by hand and checks C and the position.
3. **Master scheduler.** Target map, push sends with the bounds of 2.4,
   floor request handler, timer cycle. Pull still on. Tests: pushes reach
   all tservers after a DDL with the poll timer paused
   (`TEST_local_catalog_pause_poller`); a straggler is paged; a floor
   request moves C past the floor within one cycle; in-flight bound
   honored (metric `local_catalog_push_in_flight_rpcs` max).
4. **Release integration** (2.5) and the pull timer off by default. Replace
   `PrepareLocalCatalogPushes`; port the release-push tests
   (`ReleaseCarriesTheDdlsOwnRecords`, `PushWithAGapIsRefusedAndPulled` →
   re-pushed, `OverTheCapMasterCarriesNothing` → paged,
   `StaleReportGetsNoRecords` → look-back push,
   `NewMasterLeaderCarriesNothingUntilReportsArrive`, `ConcurrentDdlsDoNotStormRefusals`,
   `AStragglerDoesNotDisableThePushForOthers`). Measure RealTserversOnly:
   DDL p50 and gate waits against the 277-295 ms / 6 ms of fix 1.
5. **Lease regain** (3.3) with T2's serving condition. Tests: the rejoin
   test of plan2 §12 (a tserver partitioned past its lease, a DDL meanwhile,
   reconnect; its first query sees the DDL and reaches master zero times);
   the login test of todos T2; PG starts within the timeout when master is
   unreachable.
6. **Load harness for push.** The harness runs N push receivers (fake
   targets: one RPC service instance per uuid behind a few ports, one
   messenger per core) registered with master through a test flag
   (`TEST_local_catalog_push_extra_targets`), acknowledging with a fixed or
   advancing position. Scenarios: Steady (DDL releases fan out to N),
   FixedLag (N targets stuck at one position: measures fan-out bytes and
   memcpy cost per cycle), JoinerWave (K targets start far behind at once:
   measures wave time under the bounds), Idle. Report master CPU per cycle,
   bytes, in-flight occupancy, cycle latency, status RPCs on coordinators.
7. **Remove the pull path**: `GetSysCatalogChanges`, `FetchAndApplyOnce`,
   the poll flags, the load harness's pull streams.
8. **Notes**: plan2 §4.1, §4.2, §4.4, §7.1 rewritten for push;
   implementation2 §2, §5, §7; todos T1/T2/T16/T21 closed or re-pointed;
   post-perf-design §4 updated with the measurements of step 6.

Order rationale: step 1 is the measured bottleneck and is safe on its own;
steps 2-3 add push beside pull so that every intermediate state has a
working path; step 4 is where the DDL latency and the correctness of the
release change; step 5 is independent of 1-4 except for the floor request.

## 9. Tests

Fixture names refer to `pg_local_catalog-test.cc` (`PgLocalCatalogTest`,
three tservers, feature on; `PgLocalCatalogPushTest` becomes the default
once push is on). Counters named here are the existing ones
(`local_catalog_reads_served`, `reads_to_master_*`, `pushes_received/applied/refused`,
`gate_wait_us`) plus the new master counters of the pusher
(`local_catalog_push_cycles`, `push_rpcs_sent`, `push_bytes`,
`push_pages_built`, `push_transient_pages_built`, `push_in_flight_rpcs` max,
`floor_requests_received`, `release_inline_pages`, `release_inline_skipped`)
and the tserver's `local_catalog_floor_requests_sent`. "Reads reach master"
is `reads_to_master_*` on the tserver and the master's catalog read count
the existing tests already sample.

### 9.1 Unit tests, master (`sys_catalog_pusher-test.cc`)

| Test | Asserts |
|---|---|
| `PagesSealAtCapMetadataRestoreAndCycleEnd` | a page ends at `local_catalog_push_page_bytes`, after a CHANGE_METADATA record, at a RESTORE_SYS_CATALOG operation (`ends_at_restore`), and at the cycle's I_S; only the last carries S |
| `PageBytesEqualProducerRecords` | for the same WAL range, the page's concatenated bytes parse into the same `records` the per-call producer (`GetChangesForXCluster`) returns, record for record |
| `EmptyCycleCarriesSafeTime` | no new WAL: the cycle appends an empty page at I_S with S |
| `EvictionByBytesOnly` | with the cache bound exceeded the oldest page goes, regardless of any target's position; a target below the cache gets a transient page built from the WAL; the transient page is shared by two targets at close positions and dropped after both acknowledged |
| `BelowRetentionIsReseed` | a target whose `acked + 1` is below the retained WAL gets `reseed_required` and no page |
| `FloorRequestsCoalesce` | 100 floor requests inside `local_catalog_push_min_cycle_interval_ms` produce one cycle whose S covers the highest floor; a floor above the last S after the interval forces a new cycle; a floor the last S already covers produces no new S |
| `InFlightBoundsHold` | 1000 fake targets all behind: at no moment more than `max_in_flight_rpcs` RPCs or `max_in_flight_bytes` outstanding; one RPC per target at a time; release-driven pages still get their reserved slots while the bulk is draining |
| `GapAnswerRewinds` | a `gap` acknowledgement at X moves `acked` to X and the next page starts there |
| `TermChangeClearsCache` | a new leader term empties the cache and every `acked`; the next cycle rebuilds from the look-back |

### 9.2 Unit tests, tserver apply queue (`local_catalog_applier-test.cc`)

| Test | Asserts |
|---|---|
| `QueueOrdersByPageFrom` | pages enqueued out of order are applied in `page_from` order |
| `GapIsAnsweredNotApplied` | `page_from.index > applied.index` returns `gap` with the position and applies nothing |
| `OverlapIsFiltered` | a page ending at or below `applied` is acknowledged without a write; a page straddling `applied` applies only the records above it |
| `SafeTimeOnlyFromLastPage` | C moves only after a page carrying S, and A[db] is read at that C |
| `ApplyOutlivesHandler` | the handler's deadline passes mid-apply; the page still completes, the position advances, the re-sent page is filtered as overlap (port of `ApplyOutlivingItsCallerDoesNotCrash`) |

### 9.3 Cluster tests: lock release and paging

| Test | Scenario | Asserts |
|---|---|---|
| `ReleaseCarriesOnePageInline` | `CREATE TABLE` on tserver 0 | tservers 1 and 2 each receive one inline page holding the DDL's records; `push_rpcs_sent` unchanged; `floor_requests_sent` unchanged; `gate_wait_us` below 10 ms; the table is usable on tserver 2 with no read reaching master |
| `CreateDatabaseIsPagedNotInline` | `CREATE DATABASE` (11.6 MB) on tserver 0 | `release_inline_skipped` = 2; each other tserver receives at least 10 push pages before its gate passes; the statement completes synchronously (no "falling back to async release" warning on tserver 0, master's release task finished before the client returned); a connection to the new database on tserver 2 immediately after preloads from the copy with zero master catalog reads; peak `push_in_flight_bytes` at or below the bound |
| `DropSchemaCascadeIsInlineMultiPage` | `DROP SCHEMA ... CASCADE` with 100 tables (1.8 MB) | 2 inline pages per target (1 MB pages, 4 MB cap); one release RPC per target |
| `SmallInlineCapPagesEveryDdl` | `local_catalog_release_inline_max_bytes=16384`, 20 `CREATE TABLE` | every release schedules pushes instead of inlining; every DDL completes synchronously; DDL p50 within 2x of the inline fixture's (the cost of one extra RPC) |
| `StragglerIsPushedNotPolled` | applier paused on tserver 2 (`TEST_local_catalog_pause_applier`), 10 DDLs on tserver 0, resume | tserver 1 completes every gate inline; tserver 2's releases are acknowledged only after resume; after resume tserver 2 reaches the head by push with zero pull RPCs and zero floor requests beyond the ones its gates sent |
| `LostPushIsRepairedNextCycle` | `TEST_local_catalog_drop_pushed_batch` on tserver 1 for one release | its `acked` does not move on master; the gate's floor request triggers a cycle that re-sends the page; the gate passes within one cycle |
| `GapFromStaleReportIsRepushed` | `TEST_local_catalog_report_applied_op_id_ahead=20` on tserver 1 (port of the existing test) | the inline page is answered `gap`; master re-pushes from the real position; the gate passes without a pull |
| `ConcurrentDdlsShareCycles` | 4 sessions × 50 DDLs across 3 tservers | `push_cycles` at most the number of releases; coordinator `GetTransactionStatus` RPC rate at most 20/s; every DDL synchronous; the three copies converge (`ConcurrentDdlsFromTwoTserversConverge` assertions) |
| `NewMasterLeaderPushesFromLookback` | 3 masters, step down, `CREATE TABLE` within 500 ms | the new leader's targets have no position; the release schedules look-back pushes; the DDL completes; no re-seed; `push_transient_pages_built` ≥ 1 |

### 9.4 Cluster tests: lease rejoin

Common setup: three tservers; tserver 2 ("J") stops refreshing its lease
(`TEST_tserver_enable_ysql_lease_refresh=false`) until master expires it
(`master_ysql_operation_lease_ttl_ms` lowered to 5 s for the fixture);
DDLs run on tserver 0 while J has no lease; J's refresh is re-enabled.

| Test | While J has no lease | Asserts after J rejoins |
|---|---|---|
| `RejoinCatchesUpFromCache` | `CREATE TABLE t; CREATE INDEX i ON t(a)` | J's floor request carries B and its position; the first cycle after it pushes the missing pages; J's postmaster is restarted only after the copy serves at B; `reads_to_master_*` on J stay at their pre-partition values; the first query on J uses index `i` (EXPLAIN) and reaches master zero times |
| `RejoinBelowCacheReadsWal` | `local_catalog_push_cache_mb=1`, 200 `CREATE TABLE` | `push_transient_pages_built` ≥ 1 for J; same serving and zero-master-read assertions |
| `RejoinBelowRetentionReseeds` | `log_min_seconds_to_retain=5`, DDLs, wait past retention, force a log roll | J receives `reseed_required`, re-seeds, serves after the first pushed page with S ≥ B; PG restarted once |
| `RejoinPgStartHoldTimesOut` | master's push to J blocked (`TEST_local_catalog_push_block_target=J`), `local_catalog_serve_wait_timeout_ms=3000` | PG on J starts after 3 s and its reads reach master; unblock; J flips to serving and the next statement reads the copy |
| `RejoinPgStartHoldNever` | same, `local_catalog_serve_wait_timeout_ms=-1`, unblock after 5 s | no connection is accepted on J for 5 s; after the unblock PG starts and the first query reads the copy |
| `RejoinWaveDoesNotHoldDdl` | tservers 1 and 2 both lose and regain their lease together; a DDL loop on tserver 0 runs throughout | DDL p50 during the rejoin within 1.5x of the loop's baseline; both copies flip within one cycle after their catch-up; the loop's DDLs are visible on both with zero master reads |
| `ReleaseDuringCatchupGatesTheFlip` | after J's lease is back, J's applier is paused before B is reached; `ALTER ROLE u NOLOGIN` on tserver 0; applier resumed | J's release handler acknowledged without waiting (DDL completed synchronously); J did not flip while `A[db] <` the published version; after the flip a connection as `u` on J fails (login reads the copy, not a stale preload); `reads_to_master_*` unchanged |
| `RejoinWithNoDdlFlipsOnEmptyCycle` | nothing | J's floor request yields an empty page with S ≥ B; J flips; PG restarted; no pull |

### 9.5 Cluster tests: floors, timer, read times, fallback knob

| Test | Scenario | Asserts |
|---|---|---|
| `TempTableFloorRequestsOneCycle` | `CREATE TEMP TABLE` then a `SELECT` on it in the same session | one `floor_requests_sent`, one cycle; the `SELECT` is served locally after the wait; no poll |
| `ExplicitReadTimeAboveCIsServedAfterFloor` | quiet cluster (C old), `SET yb_read_time` to now, catalog query | one floor request, `reads_served` increments, `reads_to_master_*` do not |
| `MasterSnapshotTimeIsFloorForNextLocalRead` | a statement forced to master (in a DDL block) then a plain statement | the plain statement waits for C ≥ the master read's time (todos T13) and is served locally |
| `TimerCycleAdvancesIdleC` | `local_catalog_push_interval_ms=2000`, no DDL | C on every tserver advances at least every 2 s; with no WAL change at all, `push_rpcs_sent` grows by at most one empty-page push per target per tick |
| `IdleClusterSendsNoPolls` | 30 s, no DDL, `local_catalog_push_interval_ms` default | zero `GetSysCatalogChanges` calls on master; the tservers' poll counters stay at 0 |
| `FallbackKnobZeroUsesMasterAtOnce` | `local_catalog_serve_wait_timeout_ms=0`, applier paused, DDL elsewhere, read on the paused tserver | the read goes to master without waiting; resume; the next read is local |
| `FallbackKnobNeverHoldsTheStatement` | `-1`, applier paused for 3 s | the statement blocks for the 3 s and then completes from the copy; `reads_to_master_*` unchanged |
| `StalledApplyTimesOutAndRecovers` | `TEST_local_catalog_pushed_batch_apply_delay_ms` above `local_catalog_push_rpc_timeout_ms` for one page | master retries that target with backoff; the page applies once; the re-sent page is filtered as overlap; other targets unaffected |
| `RestoreIsReseedByPush` | PITR restore of the system catalog (port of `dfdc66137d2`) | every tserver receives `reseed_required` by push, re-seeds, serves |

### 9.6 Regression

- The 48 tests of `pg_local_catalog-test` pass with push on; the nine
  release-push tests are replaced by 9.3.
- `pg_catalog_version-test.cc` tests pinned to the feature off stay off.
- Java `PgBackendsTest.CachedVersion` with `local_catalog_push_interval_ms`
  large (the heartbeat moving-target case of todos T1, observed 2026-09-29)
  passes: the version report is satisfied by the release's own page, not by
  the timer.

### 9.7 Load harness (step 6)

Scenarios and the numbers to report are in §8 step 6. Each is compared
against the pull-path row of [loadtest-summary.md](loadtest-summary.md)
with the same N on the same host: Steady N=200 and N=1000 (master CPU per
DDL, cycle latency, coordinator status RPCs/s), FixedLag N=100 (master CPU
per MB sent: expected under 1 ms, against 16-25 ms), JoinerWave K=100 at
26 MB each (wave time against the in-flight bound; master CPU), Idle
N=8000 (expected 0 polls, master CPU at its DDL-free baseline), and
CreateDatabaseFanout N=100 (one `CREATE DATABASE`: bytes, longest
acknowledgement, synchronous completion).

## 10. Benchmark re-run

Session `2be87e07-ac83-4ee5-a28f-aaf65e14b512` (`local-catalog-cache-perf`,
tracker file
`~/ysql/notes/sessions-dev-server-sanketh-3/2026-09-09-local-catalog-cache-perf.md`,
worktree repo6) ran the measurements this plan answers. Re-prompt it once
the implementation is complete and 9.1-9.6 pass: run 9.7 and the original
`loadtest-plan.md` scenarios on the same host as
[loadtest-summary.md](loadtest-summary.md), then update
[loadtest-summary.md](loadtest-summary.md),
[load_user_scenario_summary.md](load_user_scenario_summary.md) and
[post-perf-design.md](post-perf-design.md) §4 with the push numbers.

## 11. Implementation status (2026-10-07)

Steps 1-7 are in repo6, uncommitted; details, deviations and test results are in
[worklog2.md](worklog2.md) "Master push (push-plan.md), 2026-10-07", the code map in
[implementation2.md](implementation2.md) §2, §5, §7.

| Step | State |
|---|---|
| 1-5, 7 | coded; release: `sys_catalog_pusher-test` 10/10, `pg_local_catalog-test` 69/69 (one disabled; after the shutdown fix in worklog2), `pg_backends-test` 27/27, PITR restore tests of `yb-admin-snapshot-schedule-test` 6/6 |
| §9 tests | written, with the exceptions listed in worklog2; §9.2 as RPC-level tests against a real tserver; `TimerCycleAdvancesIdleC` replaced by `TimerCycleCarriesWritesOutsideDdl` |
| 6 | load harness push scenarios coded and smoke-tested in fastdebug (20 receivers); measurements are §10's |
| 8 | plan2, implementation2, todos (T1, T2, T10, T13, T14, T16, T21, T33, T34, T36), post-perf-design status, worklog2. post-perf-design §4 waits for the §10 numbers |
| §10 | session `0ef85aa9` ("local tserver copy master load test 16 core") asked to run §9.7 and the loadtest-plan scenarios, 2026-10-07 |
| §9.7 results | run by `0ef85aa9`; two harness defects and one pusher defect (positions reported before a leader's first cycle were dropped) fixed; see worklog2 "§9.7 load results from session 0ef85aa9" |

§2.3 deviation: a cycle does not move master's clock to the floor (`clock.Update(f)`); successive
lease-bound floors (`MaxGlobalNow()`) ratcheted the hybrid clock until a tserver crashed on the
skew check. Master re-runs cycles until its own clock passes the floor.

Flags differ from §7: `enable_local_catalog_push` does not exist (step 7 removed the pull
path it switched to); `local_catalog_poll_rpc_timeout_ms` became
`local_catalog_master_rpc_timeout_ms` (floor requests and fetching the copy).
