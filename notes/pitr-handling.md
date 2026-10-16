# PITR handling for the tserver-local catalog copy

Status: plan, not coded (2026-10-10). Todo: [T43](todos.md#243-t43-pitr-restore-reaches-the-copies-in-place).
Code pointers are against branch `docdb_local_catalog` at `5886f97e64e`
(worktree `repo6`).

## 1. Problem

A point-in-time restore (PITR) of a YSQL database rewrites master's system
catalog. Today the copy learns of the restore only from the next push after
it, and then re-seeds by remote bootstrap. That has two costs.

### 1.1 New connections read the catalog from before the restore

Master marks the restoration complete without waiting for the copies. A
connection opened after `yb-admin` reports the restore complete, but before its
tserver's copy has received the restore page, reads the copy as it was before
the restore.

Observed locally in release, `yb-admin-snapshot-schedule-test`
`PgsqlSequencePartialCleanupAfterRestore/NotColocated_PITR`, which fails in 10 of
15 runs with the copy on and passed the one run with it off.
Timeline of run `/tmp/s2_loop_2.log`, tserver ts-2, database 16384:

```
master                                tserver ts-2                                 test client
------                                ------------                                 -----------
16.931                                copy's C = 16.931, applied version 3
16.976  restore starts; the restore
        op raises 16384 to version 4
~18.07  restore complete -> RESTORED
                                      shared memory still holds version 3 ------>  new connection (session 7):
                                                                                   backend at version 3
                                      version wait: copy 3 >= backend 3, no wait
                                      reads served by the copy at 16.931 ------->  sequence from before the
                                                                                   restore is still there:
                                                                                   "Unable to find relation for
                                                                                   sequence", then CREATE
                                                                                   SEQUENCE: "already exists"
18.42   heartbeat carries version 4 ----> gate holds it, asks for a push; the
                                      restore page arrives; re-seed starts
```

Each check passes for a reason:

- The version wait (`pg_client_session.cc:3583-3605`) compares the copy's
  applied version with the version the backend reports. A new backend takes
  its version from shared memory. A restore has no object lock release, so
  the new version reaches shared memory only with the next heartbeat. Until
  then the backend reports the old version and the copy holds it.
- The heartbeat gate (`heartbeater.cc:604-620`) holds the new version until
  the copy holds master's read time. That works only after the heartbeat has
  arrived; before it, nothing on the tserver says a restore happened.
- Without the copy, the same new backend reads master at the latest time and
  sees the restored rows, whatever version shared memory holds. The copy
  makes this window possible.

The statement in plan2 §6.2 ("no safe time can pass the restore before that
answer is seen") holds here: C = 16.931 is below the restore. It is not
enough, because a copy whose C is below the restore still serves reads
after the restore has completed.

### 1.2 Every restore re-seeds every copy

The restore page tells every copy to re-seed. A re-seed replaces the whole
copy by remote bootstrap from the master leader, on every tserver at the same
time, and every database is read from master until it ends. A restore of one
database therefore costs a fetch of the whole system catalog per tserver
(T39) and takes local serving away from all databases.

## 2. What a PITR does today

Verified in code:

- Master submits a `RESTORE_SYS_CATALOG` raft op
  (`master_snapshot_coordinator.cc:1867-1886`). It carries the snapshot id,
  the restore time (`snapshot_hybrid_time`) and the restoration id; master
  derives the database oid from the snapshot.
- Every master peer, leader and followers, applies that op by computing and
  writing the restore itself (`CatalogManager::RestoreSysCatalog`,
  `catalog_manager_ext.cc:3218`). The restored rows are written into RocksDB
  directly and are not WAL records, so no push page can carry them
  (`sys_catalog_pusher.cc:320`).
- Fast PITR (`enable_fast_pitr`, default true,
  `RestoreSysCatalogFastPitr`, `catalog_manager_ext.cc:3161`) does three
  things:
  1. **Catalog rows of the database.** It flushes, then sets a hybrid-time
     filter for the database and the restore time on every existing SST file
     (`RocksDBPatcher::SetHybridTimeFilter`, `docdb_rocksdb_util.cc:924`),
     then reopens RocksDB. Every key of that database's catalog tables with a
     hybrid time above the restore time, in a file that existed at the
     restore, is hidden. No catalog row is rewritten. Shared catalogs and
     other databases are untouched.
  2. **Catalog version.** It writes the database's `pg_yb_catalog_version`
     row with `current_version + 1` at the op's write time
     (`PgCatalogRestorePatch::Finish`, `restore_sys_catalog_state.cc:196-210`).
     Only the current version changes, not the breaking version, and no
     invalidation messages are written for the new version.
  3. **Master's own entries** (tables, tablets, namespaces). It diffs them
     against the snapshot, which it restores into a temporary RocksDB first
     (`RestoreSnapshotToTmpRocksDb`).
- Master keeps the catalog history the filter needs. With any snapshot
  schedule, the retention bound for all catalog tables is the current time
  minus the longest schedule retention
  (`AllowedHistoryCutoffProvider`, `master_snapshot_coordinator.cc:928-951`).
  This is not per database: one schedule keeps history for every database's
  catalog rows.

From the Slack thread on DB-21996 (Yamen), not verified in code here:

- Each tablet's `RESTORE_ON_TABLET` sends AbortTransaction to the status
  tablets it participates in. At `FINISH_RESTORE`, master sends
  AbortTransaction to all status tablets. Transactions open across the
  restore are therefore aborted.
- "We also bump the pitr_counter, which forces all tservers to renew their
  leases and thus kill all the pg backends." At this branch's base, the PITR
  counter appears only in `LeaderEpoch` for master's own catalog mutations,
  and no code connecting it to YSQL leases was found. In the local runs above,
  no tserver got a new lease epoch in the 1.5 s between restore completion and
  test end. Open item 1 in §5.

Existing backends: they are not killed by the restore in the runs observed.
A backend that sees the new version, which has no invalidation messages, is
expected to reset its whole catalog cache (not verified in PG code, open item
2). The reset reads the catalog, so it must read the state after the restore.

## 3. Plan

Three changes, and a fallback.

```
master leader                                    each lease holder's copy
-------------                                    ------------------------
apply RESTORE_SYS_CATALOG (as today)
send restore page {db_oid, restore_at,  ------>  stop serving
  write_time, new_version}                       flush; set filter(db_oid, restore_at)
                                                 on the copy's SST files; reopen
                                                 write pg_yb_catalog_version row
                                                 C passes the restore op; serve again
                                       <------   ack
all lease holders acked
(or their lease expired)
set_complete_time -> RESTORED
```

### 3.1 The copy applies the restore in place

- The restore page carries the database oid, the restore time, the op's
  write time and the new catalog version. The current page has no records and
  only `reseed_required`.
- The copy repeats steps 1 and 2 of §2 on its own RocksDB, in master's order:
  flush, set the filter, reopen, write the version row. The filter is defined
  by database and time, not by file layout, so it applies to the copy's files
  as it does to master's.
- The copy stops serving for the flush and reopen. Reads go to master during
  that time, for all databases. Every other database keeps its copy contents.
- Step 3 of §2, master's own entries, is not repeated. The copy does not serve
  them. Whether later WAL records for those entries apply correctly on top of
  the copy's old entries is open item 3.

### 3.2 The copy keeps master's catalog history

The filter needs the versions at the restore time to still exist. Master keeps
them (§2); the copy keeps only its own tablet retention today (no code gives
it master's bound; not verified, open item 4). If the copy has compacted past
the restore time, the filter hides the newer version and no older version is
left, so the row disappears.

- Master sends its current catalog-table retention bound with push pages (or
  in the heartbeat response). The copy uses the older of that bound and its own
  default retention as its history cutoff.
- Remote bootstrap copies master's SST files, so a copy seeded or re-seeded
  while a schedule exists already has the retained history.
- When a schedule is created, the copy's own default window
  (`timestamp_history_retention_interval_sec`, 900 s by default) covers the time
  until master's bound reaches it.
- Cost: each copy keeps the same catalog history as master's catalog tablet,
  for every database, for the longest schedule retention. Without schedules
  nothing changes.

### 3.3 Master waits for the copies before reporting the restore complete

- In `FinishRestoration` (`master_snapshot_coordinator.cc:1965`), before
  `restoration->set_complete_time(...)`, master has the pusher send the restore
  page at once to every tserver with a live YSQL lease.
- The code from `set_complete_time` on moves into a callback that runs when
  every target has acknowledged. `FinishRestoration` runs under `mutex_` and
  cannot block.
- A target that does not answer is retried until its lease expires, as the
  object lock release is.
- A tserver whose lease has lapsed is not waited for. Its copy does not serve,
  and on its new lease epoch it asks for a push up to a bound past the restore.
- This is the release-gate rule applied to a restore: the client is told the
  operation is done only after every lease holder's copy holds it. No lock is
  needed to keep new connections out. A connection opened after RESTORED reads
  a copy that has applied the restore. A connection opened during the restore
  may see either state, as it can without the copy.

### 3.4 Existing backends

- The heartbeat gate publishes the restore's new version only when the copy
  holds master's read time or does not serve. With 3.1 the copy holds it only
  after applying the restore. An existing backend therefore resets its caches
  only after its tserver's copy has the restore.
- No backend kill is needed for the copy. Transactions open across the
  restore are aborted as today (§2).

### 3.5 Fallback: re-seed

The copy records the oldest time its history still covers. When a restore
page's restore time is older than that, when the page lacks the in-place fields
(an older master), or when the in-place apply fails, the copy re-seeds as
today. A restore can then cost a re-seed but cannot leave a wrong copy.

## 4. Tests

| Test | Checks |
|---|---|
| `yb-admin-snapshot-schedule-test` PITR tests in release, 20 runs each of `PgsqlSequencePartialCleanupAfterRestore/*` | no stale read after RESTORED; today 10 of 15 fail |
| new `pg_local_catalog-test`: restore with a schedule, then a new connection reads the restored catalog from the copy | `local_catalog_reads_served` grows, no re-seed (`local_catalog_reseeds` unchanged) |
| new: restore time older than the copy's history | the copy re-seeds and serves the restored catalog |
| new: a lease holder that does not answer the restore page | RESTORED is reported only after its lease expires |
| new: two databases, restore one | the other database's reads stay on the copy except during the reopen |

## 5. Open items

1. Does the PITR counter bump renew YSQL leases and kill backends (Slack
   claim, §2)? Not found at this base and not seen in 1.5 s. Check the current
   master branch and a longer run.
2. Does a backend reset its whole catalog cache on a version with no
   invalidation messages? Check in PG (`YbGetInvalidationMessages` and its
   callers).
3. Master's own entries are not restored on the copy (§3.1). Check that every
   WAL record that writes them later is a whole-row write, or filter them out
   of the copy (T35).
4. Confirm the copy's history cutoff today and pick how master's bound
   reaches it (page field or heartbeat field).
5. A copy that falls behind and reaches the restore op later, or a new master
   leader, needs the restore page fields. Keep them in master's persisted
   restoration entry, not only in the pusher's memory.
6. Remote bootstrap of a copy while a restore is in flight (T31).
7. Clone ends with a snapshot restore (T40); check whether it takes the same
   path.

## 6. Evidence

- Local logs: `/tmp/s2_loop_*.log` (copy on, routing logs on),
  `/tmp/s2_partial_cleanup_off.log` (copy off, passes).
- `PgsqlCreateTable/1` "already exists" also fails with the copy off (3 of 5)
  and is not part of this problem: `/tmp/s3_ct_{on,off}_*.log`.
- CI triage of diff 320164: `~/agents/test-triage/D57801-320164/`.
- Slack thread on DB-21996:
  https://yugabyte.slack.com/archives/C0BA9H77B1D/p1782504059115909?thread_ts=1781839616.202279&cid=C0BA9H77B1D
