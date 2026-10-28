# Simplified freshness design for the tserver-local catalog copy under object locking

Date: 2026-10-17. Status: proposal, not coded. Code pointers are against branch
`docdb_local_catalog` at `5886f97e64e` (worktree `repo6`). Scope: object locking, concurrent
(transactional) DDL and invalidation messages on, per-database catalog versions, a copy on
every tserver that holds a YSQL lease, and the tserver response cache off (sys-table prefetching
is removed, plan2 §13; a cache hit is answered before the copy is consulted,
`pg_client_session.cc:3398-3405`, and would bypass F_s).

This document replaces these parts of [plan2.md](plan2.md):

- §4.3 (freshness guarantee): the gate compares hybrid times only; no applied catalog version is
  read from the copy or compared.
- §5.3 rules 2 and 3 (the version wait and the own-write wait): replaced by one session floor.
- §7.1, the serving condition: replaced by one tserver floor.
- §10, major version upgrade: no special case (todos T25).
- §11.1 item 6 (a session sees a change and then loses it): handled by the session floor.

It changes these todos: T1 and T14 are no longer needed; T12 disappears; T13 changes shape; T25
is decided (no special case).

## 1. Summary

A backend reads the copy at a hybrid time only when the copy is known to hold every catalog
change that this backend could have been told about. Every such fact is a hybrid-time floor:

- the bound that a lock release or a restore message carries, which the tserver waits for before
  it publishes the versions in that message;
- the lease bound, which the tserver waits for before PG starts after a new lease;
- the session floor, which a session waits for after its own catalog writes or after a catalog
  read that master answered.

Nothing on the tserver reads or compares the catalog versions held in the copy. Catalog versions
reach shared memory only from master: in lock releases, in restore messages, and in heartbeats
while the copy does not serve.

There is no flag that chooses between the copy and master. Routine cases wait for the copy.
Exceptional cases (corruption, apply errors, I/O errors) send catalog reads to master until the
copy serves again, and a backend that read master stays on master for its lifetime.

## 2. Facts the design rests on

Verified in the code at the commit above.

1. **Every catalog version increment ends with a lock release to every lease holder.** In
   non-legacy mode, each PG-side increment takes the exclusive object lock on
   `pg_yb_catalog_version` (`yb_catalog_version.c:498`, `:748`, `:854`). That lock is global,
   so master sends the release to every tserver with a live lease
   (`GetAllTSDescriptorsWithALiveLease`). Today a release is retried only by the DDL's tserver
   (open item 9); the design requires that every release is retried until each such tserver
   acknowledges it or loses its lease.
2. **The release carries the full version map and the invalidation messages.** Master reads the
   versions after the commit (`PopulateDbCatalogVersionCache`, `object_lock_info_manager.cc:1133`)
   and sends the read time as `catalog_versions_read_time`. Every version in the map committed at
   or before that time.
3. **The release handler publishes the versions before it releases the local locks**
   (`ts_local_lock_manager.cc:577-623`). A backend that takes a lock conflicting with the DDL's
   lock always sees the new version first.
4. **Every top-level DDL increments the version by default.** `yb_always_increment_catalog_version_on_ddl`
   defaults to true (`guc.c:4073`), and `YbGetDdlMode` starts from it (`pg_yb_utils.c:3655-3659`).
   The exceptions do not need an increment for other sessions:
   - DDL on temp relations only (`pg_yb_utils.c:3923-3928`, `:3953-3958`, `:4025-4045`,
     `:4143-4148`, `:4188-4194`, `:4219-4224`);
   - `DISCARD` (`pg_yb_utils.c:3982-3990`);
   - the internal drops of `REFRESH MATERIALIZED VIEW`, and DDL subcommands, whose top-level
     statement increments.
5. **Catalog writes outside DDL are disabled while the copy is on.** Login profiles, large
   objects, and writes under `yb_non_ddl_txn_for_sys_tables_allowed` outside upgrade and initdb
   are disabled (todos T4, T5, T6). YSQL upgrade migrations commit as DDL
   (`pg_yb_utils.c:4340-4355`).
6. **Master writes catalog versions without a lock release in one routine case: a PITR restore.**
   Clone may be a second case if it restores the system catalog (todos T40). Major version
   upgrade writes versions only while it builds the new catalog, which no running backend reads
   (todos T25).
7. **Catalog reads cannot get read restarts.** pggate clamps the uncertainty window of every
   catalog-snapshot read (`ClampCatalogReadTime`, `pg_txn_manager.cc:776-798`), and the copy
   serves at `ReadHybridTime::SingleTime(h)`.
8. **PG takes catalog snapshots separately from transaction snapshots, in every isolation level.**
   A fresh catalog snapshot is taken after every new transaction snapshot, after every applied
   invalidation message, and for every scan of a catalog without a syscache
   (`snapmgr.c:390-392`, `:438`, `:546-557`; `inval.c:704`, `:852`). PG puts no predicate locks
   on system catalogs (`predicate.c:498-503`), so `SERIALIZABLE` needs nothing more from catalog
   reads than other isolation levels.

## 3. Floors

| Floor | Value | Where it applies |
|---|---|---|
| Release bound | `catalog_versions_read_time` of the release: the time master read the version map it sends | one release on one tserver |
| Restore bound | The same field, carried by the restore message of todos T43 | one restore on one tserver |
| Lease bound B | The tserver's clock reading (`MaxGlobalNow`) when a new lease epoch is observed, including the first epoch of a new process (`ysql_lease_manager.cc:186`) | the tserver |
| Tserver floor F_serve | The maximum of B and of the bound of every release, restore message or heartbeat report published while the copy does not serve. Set to `HybridTime::kMax` on lease loss. | the tserver |
| Session floor F_s | The clock reading taken when master's answer to any catalog read of this session arrives; and the clock reading taken after this session commits a transaction that wrote catalog rows | one PG session |

Notes on F_s:

- The master answer has moved this tserver's clock past master's read time, so a clock reading
  taken when the answer arrives is at or above everything that read saw. The current code instead
  sets a flag (`catalog_read_from_master_`, `pg_client_session.cc:3759`) and takes the clock
  reading at the next read (`:3639-3641`), which waits longer than needed.
- Catalog reads in DDL mode raise F_s too. Without that, a DDL transaction block that reads
  master and then rolls back would let the next statement read the copy below what the block saw.
- The commit part is set only when the transaction wrote catalog rows. pggate tracks this in
  `has_catalog_write_ops_in_ddl_mode_` (`pg_session.cc:1296`). Today `HandleCommit` sets the
  floor for every successful DDL-mode commit (`pg_client_session.cc:4734-4740`), so a connection
  pool that runs `DISCARD ALL` on each reset would pay one push round trip per reset.
  `HandleCommit` does not receive that flag: the commit request's DDL-mode payload is
  `has_docdb_schema_changes`, `silently_altered_db` and `use_regular_transaction_block` only
  (`pg_client.proto:568-569`, `pg_txn_manager.cc:529-539`), so it needs a request field.
  `HandleCommit` runs only when the DDL session has a live transaction (`:4642-4648`); check
  whether `DISCARD ALL` with no temp objects starts one at all.
- The commit part should be the commit hybrid time from the commit response, not a clock
  reading. Today it is `clock()->Now()` taken in `HandleCommit` before the release runs
  (`pg_client_session.cc:4731-4741`); the release's S is master's leader safe time and master
  does not advance its clock from the release request, so a tserver clock ahead of master's by
  more than the tserver-to-master latency leaves C below F_s after the release and the DDL
  session's next read pays a push round trip.
- F_s is cleared once C reaches it, so later reads pay nothing.
- A read at an explicit time below the copy's retention goes to master at that time h. Since
  h < C, it does not raise F_s.

## 4. Rules

### 4.1 Lock release

The release is driven from master: master knows each copy's position, so master decides what each
target gets and when, and no tserver ever blocks an RPC thread waiting for its copy.

```
master, on a release (one persisted request: versions, messages, bound, the cycle's end position E)
  1. one push cycle: certify S after the DDL's commit; build the new WAL into pages; E := cache end
  2. per lease holder, pages it lacks = from its acknowledged position to E
       all in the cache and ≤ local_catalog_release_inline_max_bytes (4 MB)
                               → release RPC carrying the pages
       otherwise               → target joins the release's pending set; the pusher is told
                                 (release priority); no RPC yet
  3. the pusher pushes pending targets first, under its in-flight bounds, as it does today
  4. a push answer or a release answer shows a pending target at position ≥ E
                               → release RPC with no pages to that target
  5. a target that answers "behind" goes back to the pending set
  6. complete when every target has acknowledged or lost its lease

release handler on a tserver (never waits)
  copy does not serve:   publish at once; F_serve := max(F_serve, release bound)
                         queue the pages; release local locks; acknowledge
  copy serves:           apply the pages in the release (if any)
                         C ≥ release bound  → publish versions + invalidation messages;
                                              release local locks; acknowledge
                         C < release bound  → answer "behind" (position, C); locks stay held
```

The gate check (C ≥ release bound) is the only thing the copy adds to DDL version propagation.
The release's cycle certifies an S at or above the bound, so a target that got its pages inline,
or reached E by push, passes the check at once. The check stays as the safety net; it is never a
wait.

What this replaces (code at `5886f97e64e`):

| Today | Here |
|---|---|
| The handler blocks in `WaitForSafeTime` (`ts_local_lock_manager.cc:587`) for up to the release deadline, one RPC thread per waiting release, and sends its own floor request | no wait on the tserver; master pushes pending targets because it put them in the set |
| A target that cannot catch up within the deadline fails the whole release (`object_lock_info_manager.cc:1812-1826`); the DDL's tserver resends with a new version read and a new cycle, so the bound moves on every attempt (todos T44) | the release to a target is sent when master sees it caught up; one version read, one bound, one E for the whole release; the DDL tserver's resend covers only a lost RPC |
| Retries per target as a separate mechanism (the earlier open item 9) | the pending set is the retry |
| Catch-up judged by a hybrid-time comparison on the tserver | judged by position on master (acked ≥ E), which it tracks anyway; the handler's time check passes at once |

What stays:

- A copy that is not serving acknowledges at once without pages, so a joiner wave does not hold
  DDLs back; the handler decides that, master needs no serving flag.
- The synchronous wait of the DDL's tserver and the fallback to async after
  `tserver_yb_client_default_timeout_ms` (60 s); after the fallback the release keeps completing
  on master in the background.
- The 4 MB inline cap, now purely "what rides in the first RPC"; everything above it is the
  pending set. The cap exists because the release RPCs go to every lease holder at once, outside
  the pusher's in-flight bounds.

What it needs:

- Master keeps the release's version map, bound and E with the persisted in-progress release
  until completion. Today the persisted copy strips the catalog fields and a relaunch re-reads
  them (`ReleaseRequestToPersist`, `object_lock_info_manager.cc:653-679`); a new leader must
  continue the same release with the same bound, so the fields are persisted (or the bound is
  made attempt-independent, todos T44 option 2, and E recomputed by a new cycle).
- A "behind" answer (position, C) as a distinct response, not an error.
- The pending set is driven from push answers (`OnPushDone`) as well as release answers
  (`OnReleaseDone`).
- A failed version read on master fails the release before any RPC goes out, so it is retried;
  every request of a lease-expiry cleanup batch carries the map (today only the first does,
  `:1426`).

Interleaving: a newer DDL's release can carry the pages that bring a pending target to E. The
older release then goes out with no pages; its handler finds C past its bound, publishes its
older map, which the tserver ignores as not newer, and unlocks.

Pages: sealed at `local_catalog_push_page_bytes` (1 MB), at a restore of the system catalog (a
page of its own) and at the position the cycle certified; only the last page of a cycle carries
S. A push RPC carries one page (small consecutive ones up to 1 MB), one in flight per tserver.
The seal after a schema-change record (`sys_catalog_pusher.cc:407-409`, the xCluster producer's
`ends_batch` for `CHANGE_METADATA_OP`) is dropped: the applier handles such a record in place, in
WAL order, and nothing reads `ends_with_metadata` (todos T46).

A release completes on a tserver only after it has published the versions, and the release is
the only path that delivers a version to a serving copy; that is what lets heartbeat version
reports be ignored (4.3).

### 4.2 Restore message (todos T43)

The restore page is sent at once to every lease holder and carries the database oid, the restore
time, the write time, the new version map and its bound. It follows the release rule of 4.1:
apply in place, wait until C ≥ the bound, publish, acknowledge. Master reports RESTORED only after
every lease holder has acknowledged or lost its lease.

### 4.3 Heartbeat version reports

- Copy serves: version data in a heartbeat response is ignored. Master may stop sending it to
  such tservers. Every version change that matters reaches the tserver in a release or a restore
  message (facts 1 and 6), and a heartbeat could not publish a version earlier than the release
  does, because both have to wait for the same pages.
- Copy does not serve: the report is published at once, and its bound raises F_serve. This is
  the catch-up path after a lease regain and during exceptional states.

The held heartbeat report (`held_version_report_` in `heartbeater.cc`) and todos T1 and T14 are
no longer needed.

### 4.4 PG's own shared-memory write after a DDL

`YbCheckNewSharedCatalogVersionOptimization` (`pg_yb_utils.c:3437`) writes the DDL's new version
into shared memory through `TabletServer::SetTserverCatalogMessageList` (`tablet_server.cc:1383`)
without any check on the copy. While the feature is on, this write does nothing.

- The release already published the same versions and messages on this tserver after its wait
  (4.1).
- If the synchronous release fell back to async (`ts_local_lock_manager.cc:983-988`), other
  backends keep the old version until the release arrives. That is the release fan-out window
  accepted in todos A1.
- The DDL's own session is covered by the commit part of F_s.
- `YbWaitForSharedCatalogVersionToCatchup` (`pg_yb_utils.c:1140-1226`; callers `:3435` for a
  global breaking DDL, `:3475` for the connection manager, `:3120` on `TryAgain`) polls shared
  memory until it reaches the DDL's version, for up to max(60 s, 10 × heartbeat interval), then
  warns and continues. Today the backend's own write satisfies it at once; with the write a
  no-op it is satisfied by the release, so after an async fallback the DDL backend waits there
  until the release lands.

### 4.5 Serving state

- The copy serves when C ≥ F_serve.
- It leaves serving on:
  - lease loss (F_serve := kMax; backends are killed, as without the copy);
  - a local fault: N consecutive apply failures, or a corruption-class error while applying or
    reading (todos T11, T15). Withdrawal is triggered by local errors only. A copy that lags
    because master does not push or cannot certify an S stays serving (6.3).
  - a re-seed.
- After a local fault the copy re-seeds automatically and serves again once C ≥ F_serve.

### 4.6 Read rule

Evaluated by the tserver for each catalog read request:

1. **Always master, by construction:**
   - reads in DDL mode;
   - reads under `yb_non_ddl_txn_for_sys_tables_allowed` (upgrade, initdb);
   - non-MVCC and historic snapshots;
   - explicit read times below the copy's retention.

   Reads in DDL mode raise F_s.
2. **The session is on master for its lifetime (5.3): master.**
3. **The copy does not serve: master.** The session is then marked as on master for its lifetime
   (5.3).
4. **C < F_s:** ask master for a push up to F_s and wait, bounded by the request's deadline.
5. **A fresh catalog snapshot** gets C, and every later read under that snapshot reads at the
   same time.
6. **A snapshot that already has a time h:** h ≤ C is served at h. When h > C, the tserver asks
   for a push up to h and waits, bounded by the request's deadline, then serves at h. The copy
   never serves at a time other than h.

## 5. When the copy cannot be used

### 5.1 Routine cases: wait for the copy

| Case | Wait |
|---|---|
| Restart, including the restart that turns the feature on (first bootstrap) | PG's start is held until C ≥ B. A first bootstrap requires a restart, and the restart waits until the copy is seeded. |
| Lease loss and regain | Backends are killed on loss; PG's start is held until C ≥ B. |
| DDL | Commit part of F_s; the version-incrementing release has normally moved C past it already. |
| Temp-relation DDL, `DISCARD` that dropped temp objects | Nothing beyond the release: the DDL holds a global `AccessExclusiveLock` on the temp relation (open item 6), so its synchronous release has already moved C past its commit. The commit part of F_s matters only after an async fallback. |
| User catalog reads in a transaction block or a mixed statement (todos T8 step 1, which sends them to master at the transaction's read time) | Master part of F_s: the next local read waits. Step 2 (todos T19) serves them from the copy when the transaction's read time is at or below C, with no floor. |
| `SERIALIZABLE` | Today its catalog reads go to master, which raises F_s. After the change in 5.4 they are served at C like other isolation levels. |
| Explicit read time above C (`yb_read_time`, `ysql_dump --read-time`, the logical replication walsender, `yb_decode.c:606-608`) | Push up to h, then serve at h. |
| PITR in-place apply (todos T43) | The copy stops serving only for its flush and reopen; sessions wait. If this window sent reads to master, every backend with a catalog miss in it would stay on master for its lifetime. |
| Major version upgrade | No special case (todos T25): the catalog that running backends read is frozen during the upgrade, and backends switch catalogs only by restarting. |

The PG start hold has no timeout, and read waits are bounded by the request's deadline. The flag
`local_catalog_serve_wait_timeout_ms` goes away.

### 5.2 Exceptional cases: master until the copy serves again

Corruption, apply errors and I/O errors on the copy (4.5). While the copy re-seeds, catalog reads
go to master.

### 5.3 Switching back after an exceptional case

A backend that read master cannot move to the copy freely. Its catalog cache and its last
statement reflect master at a time t1, and the copy, once it serves again, may be at C < t1:

```
session S on T2                                      DDL D on T1
copy not serving: catalog miss reads master at t1
  sees table t2 (D committed at c < t1; D's
  release not yet processed on T2)
copy serves again at C, with C < c
next statement: catalog miss reads the copy at C → t2 missing
D's release reaches T2: wait C ≥ bound, publish, unlock → t2 visible again
```

Rule: a session whose catalog read went to master because the copy did not serve stays on master
until the backend exits. A backend that did no catalog read during the outage keeps using the
copy: its last local snapshot was at the old C, which is below the new C. Backends started after
the copy serves again use the copy.

Parallel workers need no special case. A worker is a new backend and reads the copy at its own
C, independent of its leader, as today. Every object the plan uses is locked by the leader. Every
DDL on such an object reached this tserver either through the release wait (C passed its bound)
or while the copy did not serve (its bound is in F_serve), so the copy at C contains it.

Option recorded for a later decision: switch a session back at its next fresh catalog snapshot
once C ≥ F_s, without waiting, with a background floor request. A session whose catalog misses
arrive faster than one push round trip keeps raising F_s and can stay on master.

### 5.4 `SERIALIZABLE`

Today the copy serves no catalog read of a serializable transaction (worklog2 "Serializable
isolation no longer serves catalog reads from the copy", commit 146f355944c).

- **Why the copy is excluded today.** The catalog snapshot and the transaction share one
  `ConsistentReadPoint` on the tserver, multiplexed by read-point serial number
  (`SetupPlainSessionReadTime`). A serializable transaction must carry no read time. Putting C on
  that read point made the transaction's next write carry a read time, which `async_rpc.cc:496`
  rejects. The fix sends no read time and no clamp for catalog reads in a serializable
  transaction (`pg_txn_manager.cc:856-860`). Each such catalog read then reads master at the
  tablet's latest time per operation, so a YB serializable transaction's catalog reads are not one
  consistent catalog snapshot, unlike PG's.
- **Change.** Keep a catalog serial's time outside the transaction's read point, and let pggate
  send the catalog-snapshot flag and the clamp for serializable transactions. The copy already
  executes its reads with empty transaction metadata (`ExecuteLocalCatalogReads`).
- **Not checked:** whether catalog reads in a serializable transaction write read intents on
  master's system catalog today.

### 5.5 Cases that do not change the read path

- Master cannot certify an S, cannot send a release, or changes leader. Master retries the
  release. Backends keep reading the copy at the old C, and the DDL's locks stay held on the
  tserver that has not processed the release.
- Sessions with an open F_s still wait in these cases: the DDL's own session after its commit, a
  session after a temp DDL, a session after a user catalog read in a transaction block, and a read
  at an explicit time above C. If certification stalls, these sessions wait until the request
  deadline and then fail. The DDL's client has usually been told success by then, because the
  synchronous release falls back to async after `tserver_yb_client_default_timeout_ms`.

## 6. What is removed

| Item | Location |
|---|---|
| The version wait on the read path | `WaitForAppliedVersion`, `LocalCatalogCompleteTime` (`pg_client_session.cc:3583-3605`) |
| Reading and storing the copy's catalog versions | `LocalCatalogApplier::ReadLocalCatalogVersions` (`local_catalog_applier.cc:812`), `applied_version_` |
| The per-database serving condition | `published_while_not_serving_`, `NotePublishedVersions`, the version loop in `OnSafeTimePublished` (`local_catalog_replica.cc:513-551`); replaced by F_serve |
| The backend's version on catalog requests | `ReadTimeOptionsPB` fields 10 and 11, `YbGetLocalCatalogVersion`, `PgSession::SetBackendCatalogVersion`, `backend_catalog_version_` |
| The version skip of the own-write wait | `own_catalog_write_floor_version_` (`pg_client_session.cc:3619-3622`); todos T12 |
| The master-read flag | `catalog_read_from_master_`; replaced by the master part of F_s |
| The held heartbeat report | `held_version_report_`, `LocalCatalogCopyHoldsHeartbeatVersions`; todos T1, T14 |
| The timeout-to-master branches of the read waits | `pg_client_session.cc:3600-3604`, `:3630-3634`, `:3646-3650` |
| The wait timeout flag | `local_catalog_serve_wait_timeout_ms` |
| The major version upgrade guard | `SetMajorVersionUpgradeInProgress`, the upgrade term in `IsServing()` (`local_catalog_replica.cc:216-228`); todos T25 |

What stays: the copy's safe time C, the lock-release wait, the lease bound, the PG start hold,
pages carried in the release, floor requests, and the explicit-read-time rule.

## 7. Open items

1. **Restore message (todos T43).** Carry the version map and a bound, apply in place, and wait
   for acknowledgements before RESTORED. Without it, a PITR restore is visible to new connections
   only after the next heartbeat (pitr-handling.md §1.1).
2. **Commit part of F_s only for catalog-writing commits.** Add a request field carrying
   `has_catalog_write_ops_in_ddl_mode_` to the commit request and read it in `HandleCommit`
   (3).
3. **Withdrawal on local faults (todos T11, T15).** Define N and the corruption-class errors; a
   lagging copy is not withdrawn.
4. **`SERIALIZABLE` (5.4).**
5. **The release bound (todos T44).** Today it is master's read time of the version map, so it
   is later than the newest commit in the map, and every retry that master processes again reads
   a later time; a tserver slower than one attempt then never completes the release. A release
   that changed no version still makes every tserver wait, which costs master a push cycle per
   release. Preferred fix: the bound is the newest commit time among the versions in the map,
   taken from the write time of their rows in `pg_yb_invalidation_messages` (inserted once by the
   DDL's transaction, never updated). It does not change between attempts while the map is
   unchanged. Three writers leave a current version without a messages row (database creation,
   PITR restore, major upgrade); T44 adds NULL-message rows there.
6. **Temp-relation DDL takes a global object lock.** Checked 2026-10-17: `heap_create_with_catalog`
   takes `AccessExclusiveLock` on every new relation, temp included (`heap.c:1503-1508`);
   `LockAcquireExtended` routes it to `YBCAcquireObjectLock` with no temp exclusion
   (`lock.c:982-999`); `IsTableLockTypeGlobal` makes `ACCESS_EXCLUSIVE` a global lock
   (`pg_client_session.cc:1487-1503`). Its release reaches master with `populate_db_catalog_info`
   (proto default true), carries the full map and a read time, and every tserver waits for C.
   So every temp-relation DDL already costs master a version read, a push cycle and a cluster
   fan-out, independent of this design, and the commit part of F_s costs nothing for it.
7. **Message queue cutoff.** Checked 2026-10-17: `cutoff_catalog_version` is advanced by the
   garbage collection pass over the local backends' versions
   (`MaybeClearInvalidationMessageQueueUnlocked`, `tablet_server.cc:2097-2112`;
   `DoGarbageCollectionOfInvalidationMessages`, `:2138-2216`), not from heartbeat data. What a
   full map (heartbeat or release) controls is the existence of the per-database queue entry
   (`:1832-1834`): `SetTserverCatalogMessageList` answers `TryAgain` until some map contains
   the database (`:1410-1420`). The `CREATE DATABASE` release carries the full map, so a new
   database gets its entry with the release. No issue.
8. **Clone (todos T40).** Check whether a clone restores the system catalog; if it does, it needs
   the restore message of 4.2.
9. **Master-driven release completion (4.1).** Today a target that answers the release with an
   error (its wait for the bound timed out, `ts_local_lock_manager.cc:593-596`) fails the whole
   release on master: `HandleResponse` retries only `ShutdownInProgress` and `INVALID_YSQL_LEASE`
   (`object_lock_info_manager.cc:1921-1949`), `CheckForDone` fails on the first non-OK target
   (`:1812-1826`), the persisted request is relaunched only when a leader loads the catalog
   (`RelaunchInProgressRequests`, `catalog_loaders.cc:596-600`), and the DDL's tserver resends
   with a new version read and push cycle (`ReleaseWithRetriesGlobalNow`,
   `ts_local_lock_manager.cc:82-131`; todos T44) for as long as it lives. Two more releases
   complete without publishing versions: a release whose version read failed on master only
   warns and goes out without a map (`object_lock_info_manager.cc:1192`), and in a lease-expiry
   cleanup batch only the first request carries the map (`:1426`). Change: the pending set of
   4.1 (the handler never waits; master sends the release to a target when its position reaches
   the release's E; "behind" answers return the target to the set), with the release's map, bound
   and E kept with the persisted request; a failed version read fails the release; every request
   of a lease-expiry batch carries the map. The DDL tserver's resend then covers only lost RPCs.

## 8. Tests

| Test | Checks |
|---|---|
| Existing `pg_local_catalog-test` freshness group (`CrossTserverDdlVisibleImmediately`, `OwnTserverDdlServedLocally`, `GateHoldsDdlWhileApplierIsPaused`, `TemporaryRelation`, `ConcurrentDdlsFromTwoTserversConverge`) | unchanged results with the version machinery removed |
| `MasterSnapshotTimeIsFloorForNextLocalRead` | the master part of F_s |
| New: DDL transaction block that reads master and rolls back | the next statement reads the copy only at or above the block's master reads |
| New: `DISCARD ALL` on a backend with no temp objects | no floor request |
| New: sync release falls back to async on the DDL's tserver (`TEST_make_global_lock_release_async`) | other backends on that tserver do not see the new version before the release is processed; the DDL's session reads its own change |
| New: apply errors injected on one tserver (`TEST_local_catalog_fail_push` variant for apply) | the copy leaves serving, reads go to master, sessions that read master stay there, new backends use the copy after the re-seed |
| New: heartbeat version report while the copy serves | ignored; versions arrive with the release |
| `PgLocalCatalogRejoinTest` | PG starts only after C ≥ B; the hold has no timeout |
| `ysql_major_upgrade_*-test.cc` with the copy on | the copy serves during `MONITORING` and after finalize |
| `yb-admin-snapshot-schedule-test` PITR tests | no stale read after RESTORED, once the restore message exists |
