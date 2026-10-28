# Tserver-local catalog copy with object locking off

Date: 2026-10-17. Status: plan, not coded. Code pointers are against branch `docdb_local_catalog`
at `5886f97e64e` (worktree `repo6`). This plan builds on
[simplified_with_object_locks.md](simplified_with_object_locks.md) (the floors, the release wait,
the session floor F_s, the serving state) and on [todos.md](todos.md) T44 (the release floor).
"Legacy mode" below means `enable_object_locking_for_table_locks` off, so
`YBCIsLegacyModeForCatalogOps()` is true (`ybc_pggate.cc:2084-2086`).

## 1. Goal and guarantee

Serve catalog reads from the copy when object locking is off.

Before object locking, YB gives no guarantee about when a catalog change reaches a tserver.
Heartbeats deliver catalog versions on a best-effort basis, and two retries (section 5) let a
statement catch up when it meets a newer version. This plan keeps that behavior, with one change:

| | Before object locking, no copy | Legacy mode with the copy |
|---|---|---|
| A completed DDL on other tservers | the version arrives with a heartbeat | The DDL's client is told success only after every lease holder has run the release: its copy holds the DDL and the versions are published. The exception is the async fallback after `tserver_yb_client_default_timeout_ms`. |
| Cached catalog entries of existing backends | refreshed when the version reaches the tserver, or after a version-mismatch error | the same |
| Catalog cache misses | read master's latest state | read the copy at C. After a completed DDL, C on every lease holder includes it. |
| A new connection's catalog version | read from master | read from the copy at C, with the catalog rows (as under object locking, plan2 §13) |

## 2. Prerequisites

- **YSQL lease on.** Without object locking, the lease is controlled by
  `enable_ysql_operation_lease` (default true; `IsYsqlLeaseEnabled`, `ysql_operation_lease.cc:27-30`).
  The release targets, the lease bound B and the PG start hold all depend on it.
- **Invalidation messages on.** The release floor of T44 is the write time of the versions' rows
  in `pg_yb_invalidation_messages`. With messages off there are no such rows, and the floor falls
  back to master's read time, which moves on every retry.
- **Gating code to change.** The new prerequisite is: object locking on, or the YSQL lease and
  invalidation messages on. These places check object locking today:

  | Gate | Effect with object locking off |
  |---|---|
  | `LocalCatalogPrerequisitesMet()` (`tablet_server.cc:935-946`) and the pusher in `master.cc:339` | no copy, no pusher |
  | `IsObjectLockingEnabled()` (`pg_client_session.cc:4863-4867`) | `ReleaseObjectLocksIfNecessary` returns OK at once (`:4774-4776`), so no release is sent |
  | `StartTSLocalLockManagerUnlocked` (`ysql_lease_manager.cc:337-341`) | no `TSLocalLockManager` |
  | `ReleaseWithRetriesGlobal` (`ts_local_lock_manager.cc:964-975`) | returns at once on an empty `weak_ptr`; the resend loop lives only as long as the lock manager |
  | `TabletServiceImpl::ReleaseObjectLocks` (`tablet_service.cc:3982-3987`) | answers success without publishing |

  The catalog part of the release handler (`ts_local_lock_manager.cc:577-623`) lives inside the
  lock manager and has to move out of it, together with the resend loop.

  Master needs no gate change: `ObjectLockInfoManager` is created unconditionally
  (`catalog_manager.cc:1105`), the release path does not read the flag, and
  `GetAllTSDescriptorsWithALiveLease` depends only on lease refreshes
  (`object_lock_info_manager.cc:861-871`). A release request must carry the DDL tserver's
  `session_host_uuid` and lease epoch. Master fills them from its own map only when they are
  missing, and that map is filled only by lock acquisition (`MaybePopulateHostInfo`,
  `object_lock_info_manager.cc:1224-1231`, `:1283-1304`).

## 3. Version propagation: the release without locks

1. **DDL commit.** After a DDL transaction that incremented a catalog version commits, its
   tserver sends `ReleaseObjectLocksGlobal` with no locks to master and waits for it, as the
   synchronous release does under object locking. On timeout it falls back to async in the same
   way (`ReleaseWithRetriesGlobal`, `ts_local_lock_manager.cc:983-988`). In legacy mode the
   increment takes no object lock (`yb_catalog_version.c:497-498` takes it only outside legacy
   mode), so the trigger is the increment itself. The place is the DDL commit path:
   `YBCommitTransactionContainingDDL` (`pg_yb_utils.c`) → `FinishTransaction` in DDL mode →
   `DdlAtomicityFinishTransaction`, where `ReleaseObjectLocksIfNecessary` runs under object
   locking (`pg_client_session.cc:3271-3296`).
2. **Master** fans the release out to every tserver with a live lease, with the version map, the
   invalidation messages, the floor (T44) and the pages, as under object locking (gates in §2).
3. **Tserver release handler without a lock manager.** In legacy mode it runs only the catalog
   part:
   - **On arrival:** update the breaking versions that `CatalogVersionChecker` reads.
   - **After the wait for the floor:** publish the versions and messages to shared memory.

   Today both happen in one call, `SetYsqlDBCatalogVersionsUnlocked` (`tablet_server.cc:1640-1660`,
   `:1808`), after the wait (`ts_local_lock_manager.cc:580-596`). In that order, a stale DML stays
   accepted on the tablet's tserver for as long as that tserver's copy lags. The checker does not
   depend on the copy; only the shared-memory publish does. A rejected DML retries through §5.3,
   which waits for the copy.

   When the copy does not serve, the handler publishes at once and raises F_serve.
4. **Publish the copy's versions after every apply.** After the applier applies a page that
   carries a safe time and publishes C, it reads at C, from the copy:
   - the `pg_yb_catalog_version` rows (`current_version` and `last_breaking_version` for every
     database);
   - the `pg_yb_invalidation_messages` rows of the versions above the ones already published.

   It then publishes them to shared memory and to the checker's map, through the same call the
   release uses (`SetYsqlDBCatalogVersionsWithInvalMessages`). Rules:
   - **Only forward.** A version is published only when it is above the published one. C only
     grows, so the copy's versions only grow. A release publishes only after C has reached its
     floor, so the copy's versions are at or above any version a release published. Use
     `ignore_catalog_version_staleness_check`, as releases do, so that an equal or older entry is
     ignored rather than treated as a stale master report.
   - **Missing messages.** A version whose messages row is missing, or has NULL messages, is
     published without messages. Backends then do a full catalog refresh, as they do today when
     messages are missing.
   - **Dropped databases.** A database missing from the copy's version table at C was dropped,
     and its entry is removed, as a full report from master does.
   - **Cost.** One read of the version table, and of the new messages rows, per applied page that
     carries a safe time. `LocalCatalogApplier::ReadLocalCatalogVersions`
     (`local_catalog_applier.cc:812`) already reads the versions. It is kept in legacy mode and
     extended to `last_breaking_version` and the messages.

   Why: a version reaches shared memory and the checker only through a release, a restore message
   or a heartbeat. The periodic pushes bring a missed DDL's rows into the copy but, without this
   step, publish no version. A tserver that missed a release would then keep the old version until
   the next version-incrementing DDL anywhere in the cluster, because each release carries the
   full map. Until then:
   - its backends keep their cached entries for that DDL;
   - its tablets accept DML carrying the old version.

   With this step, the push that brings the rows also publishes the version.
   - Master pushes the DDL's pages to every live target that is behind, after the release's push
     cycle, whether or not the release RPC to that target succeeded (`SysCatalogPusher::Drain`).
     So the repair takes about one push round trip, faster than the heartbeat repair before object
     locking.
   - Heartbeat version reports are then not needed while the copy serves. They stay ignored, as in
     simplified_with_object_locks.md §4.3.

   Lost releases in legacy mode: master has no lock records for the DDL, so when the DDL's tserver
   dies before its release completes, nothing re-sends that release. This step covers that case
   too. Not checked: what the DDL verifier's `UnlockObject` (`object_lock_info_manager.cc:1308`)
   does for a transaction with no host info.

   Under object locking this step is also safe: publishing a version before the DDL's release
   reaches this tserver only applies invalidations early, and the held lock still keeps backends
   off the DDL's objects. simplified_with_object_locks.md does not need it there.
5. **PG's own shared-memory write after a DDL** (`YbCheckNewSharedCatalogVersionOptimization`,
   `pg_yb_utils.c:3437`) does nothing while the feature is on, as in simplified_with_object_locks.md
   §4.4. The synchronous release has already published the same versions on the DDL's tserver.

**Behavior changes for legacy users.** All of these are performance costs, not blockers: each
is a bounded wait on a state the copy reaches, never a wrong read (reviewed 2026-10-17).

- **DDL latency depends on the slowest lease holder's copy.** Legacy DDL never waited for other
  tservers. With item 1, every version-incrementing DDL waits until every lease holder's copy has
  reached the floor. The wait lasts up to `tserver_yb_client_default_timeout_ms` (60 s,
  `ts_local_lock_manager.cc:985-990`); after that the release continues asynchronously and the DDL
  returns success. Until T44 is fixed, a copy slower than one attempt fails every attempt.
  - Option: wait synchronously only for the DDL's own tserver, and release to the other tservers
    asynchronously. That keeps today's DDL latency. The cost: on other tservers, a catalog miss
    right after the DDL returns may not see it for one push.
- **After the async fallback, the DDL's own backend waits again.**
  `YbWaitForSharedCatalogVersionToCatchup` (`pg_yb_utils.c:3436`, `:3477`; connection manager)
  polls for up to max(60 s, 10 heartbeats) without triggering anything (`:1185-1194`). On a
  lagging tserver a DDL can therefore take 120 s. Change: skip that wait when the release fell
  back to async, because the guarantee it serves has already been given up.
- **Temp-relation DDL pays one push round trip on the session's next catalog read.** Under
  object locking the temp relation's global `AccessExclusiveLock` release already moves C past
  the commit (simplified_with_object_locks.md §7 item 6). In legacy mode no lock is taken and no
  version is incremented, so only the commit part of F_s covers the writer, and its next fresh
  catalog read waits for a floor request. `DISCARD ALL` that drops temp objects is the same case
  (connection pools; open item in simplified_with_object_locks.md §7 item 2).
- **PG start after a restart or lease regain waits for the copy** (C ≥ B, no timeout), as under
  object locking. The wait is one push round trip in steady state; after a long outage it is
  the catch-up or a re-seed.
- **A release that a target refused is retried by master** per target until acknowledged or the
  target loses its lease (simplified_with_object_locks.md §7 item 9); in legacy mode master holds
  no lock records for the DDL, so item 4 above is what repairs a release whose DDL tserver died.

Waits that today expect a heartbeat to raise the shared-memory version, and what satisfies them:

| Wait | Code | Satisfied by |
|---|---|---|
| After the backend's own DDL: optimization skipped; global breaking DDL; connection manager | `YbWaitForSharedCatalogVersionToCatchup` at `pg_yb_utils.c:3061`, `:3435`, `:3475` | the DDL's own synchronous release, which carries the full version map; after an async fallback, the publish after the apply of the DDL's pages (item 4) |
| The retry refresh | `postgres.c:4776` | the publish after the apply that moved C to the retry's floor (item 4, section 5.3) |
| A new connection revalidating the relcache init file | `relcache.c:6572` | the publish after every apply (item 4, section 6) |
| Index backfill waiting for the tserver's version | `TabletServiceImpl::WaitForYsqlBackendsCatalogVersion`, `tablet_service.cc:2475-2493` | the release of the version-incrementing index state change, or the publish after the apply of its pages (item 4). Its `heartbeater()->TriggerASAP()` no longer delivers versions; replace it with a floor request at the tserver's clock reading. |

## 4. Catalog read time

### 4.1 How the legacy catalog read time works

- **Which reads use it.** Every catalog read that is not in DDL mode or under
  `yb_non_ddl_txn_for_sys_tables_allowed` goes to the legacy catalog session
  (`ShouldHandleTransactionally`, `pg_session.cc:192-221`; `GetRequiredSessionType`, `:225-238`).
  That covers catalog cache misses and executor scans of catalog tables.
- **One read time per backend.** pggate keeps one `catalog_read_time_` per backend
  (`pg_session.h:285`).
  - When it is empty, the next legacy catalog read asks the tserver to pick a time, with the
    uncertainty window clamped (`pg_session.cc:950-960`).
  - The tserver returns the time it picked as `catalog_read_time`, and pggate stores it
    (`pg_perform_future.cc:75`).
  - Every later legacy catalog read sends that time as an explicit read time (`pg_session.cc:950-951`).
- **Resets** (`PgSession::ResetCatalogReadPoint`, `pg_session.cc:1118`):

  | When | Code |
  |---|---|
  | at the start of every frontend message (Query, Parse, Bind, Execute) | `YbInvalidateCatalogSnapshot`, `postgres.c:7037-7044` → `snapmgr.c:2829-2832` |
  | after a DDL commit: the separate (autonomous) DDL transaction and the transactional DDL block | `pg_yb_utils.c:3399`, `:3408` |
  | on a cache refresh from invalidation messages | `inval.c:931-932` |
  | in the error path before the version read | `postgres.c:4979` |

  The read time therefore lives for one message at most. A miss does not pick a new time; the
  first legacy catalog read after a reset does.
- **Snapshot too old.** It can happen only when one message runs longer than the system
  catalog's history retention (`timestamp_syscatalog_history_retention_interval_sec`, 4 h): a long
  statement, or a function or `DO` block whose SPI statements all run inside one message. Today a
  catalog miss at that time fails on master the same way. The copy uses the same cutoff
  (`LocalCatalogAllowedHistoryCutoff`), and a read below it goes to master.

### 4.2 The copy's rules on the legacy read time

| Copy rule (object locking) | Legacy equivalent | Status |
|---|---|---|
| A fresh catalog snapshot gets C | The first legacy read after a reset arrives without a time and with the clamp flag; the tserver picks C (`SetFreshCatalogSnapshotReadTime`) and returns it as `catalog_read_time` | works today; the startup preload uses this path (implementation2 §9) |
| Later reads under the snapshot read at the same time | Later reads carry that C as an explicit time h; the rule "h ≤ C: serve at h" always holds, because C only grows | works |
| Wait for the session floor F_s before a fresh read | The legacy fresh read goes through the same `LocalCatalogCompleteTime` | works, if F_s is per backend for all session kinds (open item 1) |
| A snapshot that started on master stays on master | When the copy does not serve, the first read gets a clamped clock reading t from master; later reads arrive with h = t > C and wait for a push or go to master | same rule |
| DDL reads go to master | transactional session, DDL mode | same |
| `SERIALIZABLE` catalog reads go to master | Not needed in legacy mode: catalog reads in a serializable transaction use the legacy catalog session, which carries no transaction (`ShouldHandleTransactionally` returns false, `pg_session.cc:217-221`) | served by the copy |

**Autonomous DDL.** The separate DDL transaction commits through `HandleCommit` in DDL mode, which
sets the commit part of F_s (`pg_client_session.cc:4734-4740`). pggate then resets the catalog read
time (`pg_yb_utils.c:3408`), so the next statement's fresh read waits for the floor. The user
transaction that issued the DDL keeps its own data read point, as before object locking.

## 5. The two retries that read master's version

### 5.1 Today

- **Breaking-version check.**
  - `CatalogVersionChecker` (`service_util.h:343-406`) runs on every user-table read
    (`read_query.cc:408-411`) and write (`tablet_service.cc:2681-2691`). It is skipped when object
    locking is on (`service_util.h:358-360`).
  - It compares the operation's catalog version with the last breaking version in the tablet's
    tserver's in-memory map, and fails only when the operation's version is below it ("The catalog
    snapshot used for this transaction has been invalidated").
  - Most DDLs start as breaking (`pg_yb_utils.c:3660`). `CREATE`, `GRANT` and `ALTER TABLE` are
    marked non-breaking; `ALTER TABLE` relies on the schema-version check instead. `REVOKE` is
    breaking.
- **Schema-version mismatch.** The tablet rejects the operation with "schema version mismatch for
  table <id>". PG drops that table's pggate cache entry (`YBTableSchemaVersionMismatchError`,
  `postgres.c:4904`; `:5009`).
- **Both errors go through `YBPrepareCacheRefreshIfNeeded`.**
  - In legacy mode it reads the version with `YbGetMasterCatalogVersion` (`postgres.c:4977-4995`).
  - If that differs from the backend's version, it refreshes before the retry, with
    `is_retry = true` (`:5060`).
  - The refresh waits up to max(60 s, 10 heartbeats) for shared memory to reach that version
    (`YbWaitForSharedCatalogVersionToCatchup`, `postgres.c:4776`; `pg_yb_utils.c:1173-1222`). It
    only polls; it does not trigger a heartbeat.
  - If shared memory gets there, the backend applies the invalidation messages. Otherwise it does a
    full refresh, which adopts master's version (`relcache.c:3185`) while shared memory stays
    behind.

### 5.2 The problem with the copy

The version read is an ordinary catalog read on the legacy catalog session, so the copy answers it
at C. Today master answers it at its latest time, so the read sees every DDL committed before it.
With the copy it sees only DDLs up to C. When the copy is behind the version that caused the
rejection, the read returns the old version, and no refresh runs.

For a schema-version mismatch, the retry then has a fresh pggate table schema (fetched from master)
and a stale `pg_attribute` in the relcache. That is the combination recorded as the root cause of
GH#33765 in `~/ysql/notes/default-col-jepsen-bug/bug.md` ("Root cause"): a write that is short of
columns, written into a packed row. Today the combination already occurs, but only while the DDL's
PG transaction has changed the DocDB schema version and has not yet committed its catalog version
increment (bug.md, line 114). Without 5.3, the copy would extend it to DDLs that have committed
but whose rows the copy does not hold yet.

5.3 restores today's behavior. It does not close the window in which the ALTER has changed the
tablet's schema version and its catalog version increment is not committed yet: no floor taken at
the rejection contains a commit that has not happened. That window exists without the copy and
needs a fix of its own.

### 5.3 Fix: the version read on the retry path waits for the copy to catch up

```
backend B on T2                              T1 (tablet's tserver)               master
DML with version v ──────────────────────►  map: breaking version vb > v
                                              reject (MISMATCHED_SCHEMA)
◄──────────── error (carries no hybrid time)
error path: version read marked "latest"
  T2: floor := MaxGlobalNow() ≥ commit(vb)
      floor request ──────────────────────────────────────────────────────►  cycles until S ≥ floor
      wait until C reaches the floor; serve the read at C   → version ≥ vb
      (the apply that moved C also published vb and its messages to shared memory, §3 item 4)
refresh: shared memory already holds vb; apply messages, retry
```

- **Why the local clock alone is not enough.** The error response does not carry the rejecting
  tserver's hybrid time:
  - both rejection paths answer through `SetupErrorAndRespond(resp->mutable_error(), ...)`
    (`tablet_service.cc:2751`, `read_query.cc:141`), which writes only the error
    (`service_util.cc:118-122`);
  - `propagated_hybrid_time` is set only on the success paths;
  - the batcher applies `propagated_hybrid_time` only when the status is OK (`batcher.cc:827-828`,
    `:894-899`).

  T2's clock reading after the error is therefore bounded relative to commit(vb) only by the
  physical clock skew, and may be below it.
- **The floor.** `MaxGlobalNow()` on T2, taken at the version read: T2's clock plus
  `max_clock_skew_usec` (500 ms by default, `physical_time.cc:38`). No hybrid time anywhere in the
  cluster exceeds it, so it is at or above commit(vb), which happened before the rejection. The
  lease bound B uses the same bound (`ysql_lease_manager.cc:186`).
- **Cost.** Master does not move its clock for a floor (plan2 §4.1), so the push reaches a floor
  ahead of master's clock only when master's clock passes it: up to about `max_clock_skew_usec`
  per retry. This path runs only after a version-mismatch error. Before the copy, the same path
  read master and could then wait for a heartbeat.
- **Marking the read.** Add one flag to the version read in the two error paths (`postgres.c:4980`,
  and `:4763` when `is_retry`). The read arrives as a fresh legacy read, because the catalog read
  time was reset just before (`postgres.c:4979`). The tserver waits until C reaches the floor, then
  serves.
- **Alternatives considered:**
  - *Breaking-version errors carry vb in a structured field, and PG waits until shared memory
    reaches vb.* That needs no clock wait: shared memory reaches vb only after the gate saw the
    copy hold it. It does not help schema-version errors, which carry no version. Today the
    breaking version appears only in the error text, and not at all under
    `TEST_hide_details_for_pg_regress` (`service_util.h:386-394`).
  - *Set `propagated_hybrid_time` on error responses and apply it in the batcher on error.* This
    also needs the rejecting tserver's clock to be at or above commit(vb), which has not been
    checked, and it changes error handling for every read and write.
- **Shared memory.** The refresh still waits for shared memory to reach the version. The apply
  that moved C to the floor also published the copy's versions and messages (§3 item 4), so
  shared memory already holds vb when the version read returns. The refresh applies the messages
  without a further wait, whether or not the release of vb has reached this tserver.

## 6. New connections

A new connection takes its catalog version from the copy at C, in the same read as its catalog rows
(decided, as under object locking: plan2 §13). The relcache init file stays, as in plan2 §13.

When the new backend's version is above the init file's stored version (`relcache.c:9036-9041`),
`YbTryRevalidateRelcacheFile` waits for shared memory to reach the backend's version
(`relcache.c:6572`). Because every apply publishes the copy's versions (§3 item 4), shared memory
trails the copy only between an apply and its publish, so the wait is short.

## 7. Unchanged

- Reads in DDL mode go to master.
- Temp-relation DDL: the commit part of F_s.
- Restart and lease regain: PG is held until C ≥ B.
- PITR: the restore message of T43.
- Major version upgrade: no special case (T25).
- Parallel workers: each reads the copy at its own C, as on the object-locking path.
- The breaking-version check on a tablet's tserver uses that tserver's map. It is updated when a
  release arrives (§3 item 3) and after every apply (§3 item 4), at least as fast as heartbeats
  updated it before object locking.
- `yb_fail_catalog_write_on_catalog_version_mismatch` (default false) reads master's authoritative
  version (`tablet_service.cc:2681-2687`) and does not touch the copy.

Out of scope: the response cache, sys-table prefetching and the startup preload, and
`yb_make_next_ddl_statement_nonincrementing`.

## 8. Open items

1. Whether F_s (the commit floor and the master-read floor) is one per backend, shared by the plain
   and the legacy catalog session kinds. Today the floor fields are members of
   `PgClientSessionImpl` (`pg_client_session.cc:5029-5043`).
2. Master side: closed, no gate (§2). Remaining: the release request carries `session_host_uuid`
   and the lease epoch, and the DDL verifier's release path for a transaction with no host info
   (§3 item 4).
3. Where exactly the DDL commit path sends the release in legacy mode, for both the autonomous DDL
   transaction and the transactional DDL block, and who owns the resend loop without a lock
   manager.
4. The gate changes listed in §2.
5. Master-side release retries (simplified_with_object_locks.md open item 9). With the publish
   after every apply (§3 item 4), a refused release no longer leaves a tserver stale for long, so
   this stays a latency item.
6. Choice for legacy DDL latency (§3, behavior changes): wait for every lease holder, or only for
   the DDL's own tserver.
7. Closed: the message-queue cutoff (`cutoff_catalog_version`) is advanced by the garbage
   collection pass over the local backends' versions (`tablet_server.cc:2097-2112`,
   `:2138-2216`), not from where the messages came. The per-database queue entry is created by
   any full map (`:1832-1834`), which the publish after every apply (§3 item 4) provides
   (simplified_with_object_locks.md open item 7).

## 9. Tests

| Test | Checks |
|---|---|
| DDL on one tserver, a catalog miss on another right after the DDL returns | the miss sees the DDL; the release, not a heartbeat, published the version |
| `ALTER TABLE ... ADD COLUMN` on one tserver while another tserver's copy is paused (`TEST_local_catalog_pause_applier`), then a DML on the altered table from that tserver | the retry's version read waits for the copy; the relcache is refreshed before the retry; no write with a stale `pg_attribute` |
| `REVOKE` on one tserver, then a DML from another tserver whose copy is paused | the breaking-version retry succeeds once the copy catches up |
| Release forced async (`TEST_make_global_lock_release_async`) | other backends on that tserver get the new version once the copy applies the DDL's pages (§3 item 4), before the release arrives; the DDL's session reads its own change |
| A release that never reaches one tserver (the release RPC to it fails; the pushes succeed) | the tserver publishes the version from its copy after the next apply; its backends invalidate and its tablets reject DML carrying the old breaking version |
| `CREATE INDEX` with backfill | `WaitForYsqlBackendsCatalogVersion` completes without heartbeat versions |
| Autonomous DDL followed by a statement in the same session | the statement's catalog read waits for the commit floor |
| A `SERIALIZABLE` transaction's catalog misses | served by the copy |
| The `pg_local_catalog-test` suites, run with object locking off | same results as with object locking on |
