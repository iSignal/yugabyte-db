# Local catalog copy: PG shadow tables versus a local DocDB tablet

Inputs: `~/ysql/notes/local-catalog-cache/try2-local-tables-design.md` (PG
shadow, implemented on branch `approach3-local-catalog`, reviewed 2026-09-02)
and `plan.md` in this folder (local DocDB tablet, design pass 2 of 2026-06-19,
not implemented). Scope for both: object locking and concurrent (transactional)
DDL enabled.

The DocDB design has not been through an implementation pass. Where this
document reasons about its behavior, the reasoning is from the design notes and
from how the reused DocDB components (xCluster poller, external intents, MVCC
safe time) behave today; each such point is marked "expected".

## 1. Reference

| Term | Meaning |
|---|---|
| catalog version, V | Per-database counter incremented inside every DDL transaction. Version order equals commit order. |
| local version | The version a backend believes it is at. |
| commit HT | Hybrid time at which a DDL transaction committed. Assigned by the transaction coordinator, at or after every write of the transaction. |
| PG shadow | Per-database PG heap tables on the tserver, filled by a writer connection from a change log that the DDL serialized at commit. Freshness is an integer: published version ≥ local version. |
| local tablet | A single-replica DocDB tablet on the tserver holding a copy of the master system catalog tablet, seeded by snapshot transfer and updated by a poller that pulls the master WAL. |
| poller | Per-tserver loop that calls the master's change-stream RPC, applies the returned WAL records to the local tablet, and advances two watermarks. |
| stream safe time, S | The master sys_catalog tablet's own leader safe time (its MVCC safe time: every write at or below it is applied on that tablet), captured by the producer at a poll and reported in the change-stream response once every WAL record up to it has been shipped. Before capturing it, master resolves the status of every running catalog transaction against its coordinator (4.5) and waits until every transaction committed at or below it is applied on master, so every commit at or below S has its apply record in the shipped WAL; an open transaction delays S by a status round trip, not for its lifetime. Key property: once the poller has applied that response, the local regular store (not the intents store) holds every row committed at or below S, so a read at S needs no intent resolution. A per-tablet producer-side value; it is not the xCluster consumer's namespace-wide safe time, which aggregates many tablets and does not apply here. After local apply, S is the local tablet's "caught up to" time; the last value applied locally is called C. |
| apply record | WAL record written on the master tablet when a committed transaction's intents are applied; carries the commit HT. The poller turns staged intents into regular rows when it sees this record. |
| exposure gate | Tserver rule that publishes version V to backends (shared memory) only after the local tablet has applied everything for V. |
| cache miss reads | Catalog scans that fill the catcache or build a relcache entry. |
| user catalog reads | Executor scans of catalog tables from user SQL. |
| in DDL | The backend is executing a DDL or is in a transaction block in which a DDL already ran. |

## 2. The two designs in one picture each

PG shadow:

```
 DDL backend                master                    tserver T2
 -----------                ------                    ----------
 catalog writes → capture list
 commit: version V,
         change log (serialized rows) ──► inval-message row (db, V)
                                            heartbeat / lock release ──► shared version V
                                                                       backend: local version V,
                                                                       gate published < V → trigger writer
                                                                       writer: read change logs (published, V]
                                                                               apply to PG heaps, commit
                                                                               published = V
                                                                       backend: read PG heap under one MVCC snapshot
```

Local DocDB tablet:

```
 DDL backend                master sys_catalog tablet            tserver T2
 -----------                -------------------------            ----------
 catalog writes ───────────► intents in WAL ─ ─ ─ ─ ─ ─ ─ ─ ─ ─► poller pulls, stages as external intents
 commit at commit HT ──────► apply record (commit HT) in WAL ──► poller pulls, applies at commit HT
                            lock release (V, HT) ───────────────► exposure gate: wait S ≥ HT, then shared version V
                                                                 backend: read local tablet at C
                                                                   (C = safe time of the last poll whose records,
                                                                    apply records included, are applied locally;
                                                                    regular store complete up to C)
```

Differences visible already in the pictures:

- The PG shadow moves data through a second channel the DDL must build (the
  change log). The local tablet moves data through the channel the DDL already
  uses (the WAL).
- The PG shadow's unit of freshness is an integer version. The local tablet's
  unit is a hybrid time, with the version carried alongside.
- The PG shadow needs a writer process on the tserver. The local tablet needs
  a poller plus DocDB apply machinery.

## 3. Pros and cons by dimension

| Dimension | PG shadow | Local DocDB tablet |
|---|---|---|
| Completeness of updates | Depends on capture hooks in every catalog write path. Review found four gaps (global DDL fan-out, subtransaction rollback, DDL without version increment, unhooked paths). Each is fixable but each is a class that recurs with new code. | WAL is authoritative. Every write to the system catalog by any path (PG helpers, SQL on catalog tables, master-side writes) arrives. Subtransaction rollback and aborted transactions never produce an apply record. Expected: no per-path hooks. |
| Atomicity of one DDL | One writer pass is one PG transaction, so all shadows flip together. Correct as implemented. | One apply record applies one transaction's intents as one batch at one commit HT. Same property, and it also holds for the version row and the data rows (closes the response-cache atomicity gap). |
| Read-at-a-time | None. The shadow has one current state; readers get "≥ my version". Exact as-of-version reads need a versioned-rows redesign. | MVCC history is retained per DocDB history retention. A read at any hybrid time within retention is exact. Enables PG-style user catalog reads at the transaction read time. |
| Read latency on a miss | In-process heap and btree access; microseconds. | Loopback RPC to the local tserver plus a DocDB read; hundreds of microseconds. Both are far below a cross-region round trip. |
| Bootstrap | Per database on first use: 47 (later all) full catalog scans from master into PG heaps, serialized through one internal connection. Seconds against a remote master. | One snapshot transfer of the master tablet's files, all databases at once; schemas ride in the tablet metadata. Then the poller starts at the snapshot's WAL position. |
| Storage hygiene | PG heaps with no autovacuum: dead rows and duplicate index entries accumulate; needs a vacuum item. Unlogged files, not crash-consistent, survive restarts. | DocDB compaction handles garbage; local WAL and flush make the tablet crash-consistent. Contains non-PG system catalog rows unless filtered. Aborted transactions leave staged intents until a TTL. |
| Schema changes of catalogs | Shadow created at initdb from the catalog definition; a catalog column change needs a migration and a rebuild. | Schema-change records are in the WAL in order; the poller replays them into the local tablet's metadata. Expected to work; the replay component is new code. |
| Master-side cost | One extra column on the invalidation-message row; the change log rides inside the DDL commit. Payload size unbounded without a cap. | One change stream per tserver on one tablet: per-stream checkpoint state, WAL retention pinned by the slowest tserver (bounded by a 24 h cap and a 4 h staleness eviction), fan-out of `GetChanges` polls. Unproven at 1000 tservers. |
| DDL latency | No effect on the DDL. Readers pay a bounded wait for the writer. | With the exposure gate on lock release, the DDL's release RPC to each tserver completes only after that tserver's local tablet has caught up: one extra pull round trip per tserver, in parallel. |
| Where the freshness logic lives | PG (dispatcher, gate, writer, applier). | Tserver (poller watermarks, exposure gate, per-op version check in the perform path). PG only carries the version it already carries. |
| Maturity | Implemented; passes the shadow test suite and selected regress schedules; known defects listed with fixes. | Design only. Reuses production DocDB primitives (snapshot transfer, external intents, xCluster poller) in a new combination; one hard guard to lift on the change service; new metadata-replay component. |
| Read-your-own-writes in DDL | Not served; reads inside DDL go to master. | Same. Staged external intents are structurally invisible to reads and cannot be resolved locally, so reads inside DDL go to master. |

## 4. Read models that pick "now"

Sections 4.1 to 4.4 examine models in which a reader wants its catalog read to
reflect every DDL that completed before the read began, using only the local
tablet. Section 4.5 corrects an earlier claim about the stream safe time and
states what waiting on it costs. Section 4.6 states the carve-out none of them
removes. Section 4.7 evaluates a local transaction participant.

### 4.1 Naive model and its objection

The naive model: on every invalidated catalog snapshot the backend picks a read
time t equal to the local clock, and the tserver serves the read from the local
tablet only once the stream safe time S has reached t, polling master until it
does. A read at t is correct only if every transaction with commit HT ≤ t is
applied locally and no transaction can still commit at or below t. The second
fact is what S certifies, and S cannot be computed locally: the local tablet has
no transaction participant for its staged external intents, so only a poll
issued after t can certify t. Every invalidated snapshot with a cache miss read
therefore pays at least one master round trip (one per statement in `READ
COMMITTED`), plus clock skew (bounded by the configured maximum, 500 ms by
default), plus the poll cadence when polls are not triggered on demand.

The objection (corrected in this revision; see 4.5): the naive model spends a
master round trip on every statement that has a cache miss read, which is the
cost the local copy exists to remove. It is not worse than that: an earlier
revision claimed an idle transaction block pins S for its lifetime; that is
false for the transactional stream the plan uses, because master resolves each
running transaction's status against its coordinator before reporting S. The
per-statement cost is: one poll round trip to master, plus master's clock wait
to its leader safe time and one status round trip per running catalog
transaction (throttled to one refresh per 250 ms while transactions are
running), plus clock skew. With the transactional stream, the naive model is
the hybrid-time-guarantee model of 4.2 with the certification performed by
master on every poll.

### 4.2 Hybrid-time-guarantee model

The reader picks t = now, and the certificate "no transaction can still commit
at or below t, and every commit at or below t is delivered" is obtained from
master. The transactional xCluster producer already computes exactly this
certificate for the S it reports (4.5); the diagram shows the mechanism as a
dedicated RPC, but a poll of the transactional stream is the same thing.

//C: there is still a discrepancy here, which is that this local tablet is only using regulardb applied records but what the master is telling us does not guarantee those are flushed from intents to regulardb
//R: the guarantee holds in two links, one on master and one that the poller must provide. Master: the intent-resolution loop that runs before a safe time is captured does not return while any transaction committed at or below the resolve time is still unapplied on the master tablet (verified: it re-checks until each such transaction has a local commit time, i.e. its apply record is written, or the RPC deadline passes). The captured safe time is paired with the WAL index that was majority-replicated at that moment, and is sent only in the response whose checkpoint has reached that index. So the response that carries S also carries the apply record of every commit at or below S. Poller: it must apply every shipped record, including those apply records, which move the staged external intents into the regular store at their commit HT, before it adopts S as C. If the poller adopted S first and applied afterwards there would be exactly the discrepancy described. Stated as a requirement on the poller in 4.5 and Section 5.
```
 backend B (tserver T)         tserver T                     master                   status tablets
 ---------------------         ---------                     ------                   --------------
 pick t = now
 read at t ───────────────────► S < t → certify(t) ─────────► for each txn X with intents
                                                              in sys_catalog: ask X's
                                                              coordinator after its clock > t ──► status of X
                                                              ◄────────────────────────────────── aborted | pending | committed at c
                                                              pending after clock > t → cannot commit ≤ t
                                                              committed at c ≤ t → wait for X's apply record
                                                              return WAL up to here + "certified t"
                               ◄──────────────────────────────
                               apply; serve read at t
 ◄────────────────────────────
```

Why the certificate is sound: a commit HT is assigned by the coordinator at or
after its own clock at commit time. Once the coordinator's clock has passed t,
a transaction that has not committed can no longer commit at or below t. For a
transaction that did commit at c ≤ t, its rows are needed, so master waits for
its apply record (coordinator-to-participant apply latency, milliseconds) and
includes it in the returned WAL. Master reads do the same thing today, lazily
and only for the intents a read actually encounters.

Where it waits and how much:

//C: this per txn check might need to be repeated for every tserver so in a cluster with 100 tservers, it may still be expensive but let's park that aside for now.
//R: parked, with the shape recorded: the resolution state (captured safe time, checkpoint index, last refresh time) is kept per stream, and there is one stream per tserver, so while catalog transactions are open master issues status round trips per tserver per 250 ms refresh window, i.e. tservers × open catalog transactions every 250 ms. The status tablet is the same for a given transaction, so the answers are identical across streams; sharing the resolution per source tablet instead of per stream (one refresh serving all streams on the sys_catalog tablet) would make it tservers-independent. Added to the open questions in Section 8.
- Every invalidated catalog snapshot with a cache miss read: one master round
  trip, plus one status round trip per catalog-writing transaction in flight
  (from master to the status tablet, which lives on a tserver, possibly in
  another region), plus apply latency for transactions committed at or below t.
  The status round trips run in parallel; typically zero to a few transactions.
- An idle transaction block that ran a DDL answers "pending" with a coordinator
  time past t and is excluded; it costs a status round trip, not a wait.
- Compared with today (one master round trip per miss) it saves the per-miss
  trips within a statement. Compared with the PG shadow and with Section 5 it
  still costs a master round trip per statement that misses, which is the cost
  the local copy exists to remove.

A better way exists: the certificate says "no DDL is committing right now".
That is a mutual-exclusion fact, and object locking already provides
cluster-wide mutual exclusion, so the reader can have the guarantee without
asking master. Section 4.3.

### 4.3 Commit-lock model


Protocol. One predetermined lock object L exists cluster-wide.


```
 DDL session (any tserver)        master lock manager          tserver T local lock manager     reader on T
 -------------------------        -------------------          ----------------------------     -----------
 DDL statements (X locks on objects)
 COMMIT requested:
   acquire X(L) ─────────────────► fan out to all tservers ──► grant X(L) locally
   commit at HT_V                                                                                 new catalog snapshot needed:
   propagate: wait until every                                                                    request S(L) → blocked while X(L) held
     tserver's local copy has V                                                                   ...
   release X(L) ────────────────► fan out ────────────────────► X(L) released
                                                                                                 S(L) granted
                                                                                                 t := local caught-up time C
                                                                                                 release S(L)
                                                                                                 read local tablet at t
```

Why it works:

- A DDL takes X(L) before it commits and releases X(L) only after every
  tserver's local copy holds its commit. A reader's S(L) is granted only when
  no DDL holds X(L). So at the moment S(L) is granted, every DDL that has
  committed has also propagated to this tserver, and every DDL that has not yet
  committed will commit at an HT above the reader's now. Reading at the local
  caught-up time C is therefore equivalent to reading at now for all catalog
  changes made by lock-taking DDLs.
//C: there's a difference between C and reader's now though. reader on T does need to poll or something until C catches up to the hybrid time of DDL, no?
//R: no poll on the reader side. The catch-up is done by the DDL before it releases X(L): the "propagate" step waits until every tserver's local copy holds V, so on tserver T the local caught-up time C is already ≥ HT_V when X(L) is released, and S(L) cannot be granted before that. The interval (C, now] on T therefore contains no commit of any lock-taking DDL: a DDL that committed in it would still be holding X(L). Reading at C is equivalent to reading at now for those DDLs. C and now differ only for writers that do not take the lock (master-side writes, maintenance SQL, no-increment DDLs), which become visible when C passes their commit HT; that is the same limitation as the version-based models. If T missed the release fan-out (partition), X(L) stays held on T and readers block until T reconnects; fail-closed, not stale.
- The reader does not poll. Catch-up happens on the DDL side, inside the
  window in which it holds X(L), so C on every tserver already covers the DDL
  by the time S(L) can be granted there.
- The reader holds S(L) only long enough to read C. It does not hold it for the
  life of its snapshot: the snapshot time is fixed at C, later reads under the
  same snapshot are consistent at C, and a DDL that commits afterwards commits
  above C. Holding S(L) longer would let one long statement block every DDL
  commit in the cluster.
- S(L) is a shared object lock, granted by the local tserver's lock manager
  without contacting master; readers already take shared object locks per
  statement, so the added read-path cost is one more local acquisition per new
  catalog snapshot that performs a catalog read.
- The version machinery stays: the catalog version still drives catcache
  invalidation, and "propagate" is the same catch-up wait the hybrid model
  performs. The lock adds the "as of now" guarantee on top of the hybrid
  model's "at or beyond my version".
//C: mixing catalog version and this lock makes it complicated. is there a better way for the local tablet to pick a read time for new catalog snapshots at now and wait for local tablet to be caught up to now. Caught up to now might involve the guarantee that any DDL that commits at <= now would have already taken a lock cluster wide first.
//R: yes. The pure form below removes the version from the read path entirely and defines "caught up to now" by the lock, exactly as the comment states. One refinement: the reader still reads at C (the last certified stream safe time applied locally), not at the raw clock value, because C is the last instant at which the local copy is provably complete for every writer; under the lock, C already covers every completed DDL, so C is "now" for all lock-taking writers. Reading at the raw clock value would be exact only for lock-taking writers, and a non-lock writer's rows landing between two reads of one snapshot would make them inconsistent (the scenario in the Section 5 comment).

Pure form. The catalog version is not consulted on the read path at all; it
remains only as the carrier of catcache invalidation messages.

```
 DDL session                        every tserver (lock manager + poller)             reader on T
 -----------                        -------------------------------------             -----------
 COMMIT requested:
   acquire X(L) cluster-wide ─────► granted locally
   commit at HT_V
   propagate: wait until each tserver reports
     "certified S ≥ HT_V applied" (poll RTT + master status resolution, 4.5)
   release X(L) ──────────────────► released                                           new catalog snapshot:
                                                                                       acquire S(L) locally (blocks only during a commit window)
                                                                                       t := C  (last certified S applied here; C ≥ HT_V of every released DDL)
                                                                                       release S(L)
                                                                                       read local tablet at t
```

- "Caught up to now" is a property of the lock: any DDL that can commit at or
  below the reader's now has acquired X(L) cluster-wide before the reader's
  S(L) was granted, and a DDL releases X(L) only after this tserver's certified
  S passed its commit. So C on this tserver already includes every DDL that
  could be visible at now. No version compare, no per-op wait, no exposure
  gate on the read path.
- The DDL's propagate step waits for "certified S ≥ HT_V" on every tserver.
  With the transactional producer that is one poll round trip plus master's
  status resolution (4.5), not a lifetime pin.
- What the reader gives up versus t = now: nothing for lock-taking writers.
  For writers that do not take X(L), rows with commit HT in (C, now] appear at
  a later C; the read at C is still a consistent point for them, which a read
  at the raw clock value would not be.

Costs and conditions:

- Every DDL commit acquires and releases a cluster-wide exclusive lock: one
  master fan-out to all tservers each way, the same pattern the existing
  exclusive object locks already use. DDL commit windows are serialized
  cluster-wide across all databases, so DDL commit throughput is bounded by the
  propagation latency (on the order of ten per second at 100 ms).
- During a DDL's commit-and-propagate window, every backend that needs a new
  catalog snapshot with a catalog read waits for X(L) to be released: tens to
  hundreds of milliseconds, cluster-wide, once per DDL. Statements that hit
  only their catcache do not wait.
- The guarantee covers only writers that take X(L). Catalog writes by master,
  by maintenance SQL, or by no-increment DDLs (temporary relations) do not, and
  become visible at their commit HT when the poller reaches it. The
  read-your-own-writes floor of 4.6 still applies to the writing session.
- Lock ownership must survive the DDL backend's death exactly as other
  exclusive object locks do (release on session termination), otherwise all
  catalog snapshots stall.
- The "propagate" step waits for each tserver's certified S to pass the
  commit; each such wait costs a poll round trip plus master's status
  resolution while other catalog transactions are running (4.5). The
  contiguity condition of 4.5 is the cheaper alternative for this step when
  versions are used.

### 4.4 Reserved-commit-time model

Protocol: before committing, a DDL reserves a commit time R = now + Δ and
announces R to every tserver. Readers pick t = now and read the local tablet at
t without waiting, relying on the invariant "no catalog commit can exist in
(S, t] unless a reservation R ≤ t is known here". When t ≥ R for a known
reservation, the reader waits until S ≥ R. The DDL commits at exactly R.

Where it waits and what it needs:

- Every DDL commit is delayed by Δ. Δ must exceed the fan-out round trip plus
  clock skew or the reservation arrives after readers have already read past
  R. With Δ = 5 s, DDL latency grows by 5 s; with a tight Δ, late arrivals
  force the DDL to abort and re-reserve.
- Reservations must be acknowledged by every tserver before the DDL may
  commit, which is the same fan-out as X(L). A tserver that did not receive the
  reservation would serve a read at t ≥ R from a copy that lacks the commit;
  such a tserver must be fenced (for example, it may serve local reads only
  while it holds a fresh lease from master). Persisting reservations on master
  is needed for master failover during the window.
- The commit itself must be pinned to HT = R: the coordinator has to wait for
  its clock to pass R and commit with that HT, and R must exceed every
  participant's intent write time. This is new transaction-coordinator
  behavior.
- The invariant requires every catalog writer to reserve. Writers that do not
  (master, maintenance SQL, no-increment DDLs) break "read at now without
  certification" outright, which is worse than the lock model's degradation
  (they merely become visible later).
- Readers that hit a reservation wait for S ≥ R, a poll round trip plus
  master's status resolution (4.5), on top of the Δ the DDL already paid.

The lock model gives the same "as of now" guarantee with no fixed delay, no
commit-time pinning, no persisted reservations and no fencing beyond what
object locking already needs.

### 4.5 Correction: how S is certified, and what waiting on it costs

Earlier revisions of this section claimed that S is bounded by the oldest
transaction with unresolved intents in the master tablet, so that an idle
transaction block pins S cluster-wide. That is how the CDCSDK producer bounds
its safe time. The transactional xCluster producer, which `plan.md` selects,
does not. Verified in code:

- Before reporting a safe time, master resolves intents at its leader safe
  time: it waits until its clock has passed that time, then asks the
  coordinator of every running transaction that has intents in the tablet for
  its status.
- A transaction reported pending with a coordinator time past the resolve time
  cannot commit at or below it and is excluded. A transaction reported
  committed at or below it is waited for until its apply record is written.
  An aborted transaction's intents are cleaned up.
- The safe time is reported only once every record up to that point has been
  sent to the poller.
- Requirement on the poller: adopt the reported S as the local C only after
  every record of that response, including the apply records that move staged
  intents into the regular store, has been applied locally. Adopting first and
  applying afterwards would let a read at C miss rows master has certified.
- While transactions are running, this resolution is repeated at most once per
  250 ms (a runtime flag); with no running transactions it is skipped.

So S is the hybrid-time-guarantee certificate of 4.2, computed by master on
each poll. Corrected picture:

```
 session 1                    master (producer)                          session 2 DDL          tserver T
 ---------                    -----------------                          -------------          ---------
 BEGIN; CREATE TABLE a;
 (idle for minutes)
                                                                         CREATE TABLE b; commit at HT_V
                                                                         lock release ──────────► wait certified S ≥ HT_V
                              poll: resolve at leader safe time ≥ HT_V                            (poll)
                                ask session 1's coordinator → pending, clock past resolve time → excluded
                                ask session 2's coordinator → committed ≤ resolve time → wait apply
                                report S ≥ HT_V ─────────────────────────────────────────────────► apply; certified
                                                                         ◄────────────────────── ack
 COMMIT;  (irrelevant to the above)
```

What waiting on S costs, per wait:

- one poll round trip from the tserver to master;
- on master, a clock wait to the leader safe time, plus one status round trip
  to each running catalog transaction's coordinator (on a tserver, possibly in
  another region), plus the apply latency of transactions committed at or
  below the resolve time;
- up to the 250 ms refresh throttle while any catalog transaction is running.

This is a bounded cost, not a hang, and it is paid by whoever waits on S: the
naive reader per statement, the reserved-time reader per reservation, the
DDL's propagate step in the commit-lock model and in the `plan.md` gate.
//C: In the reserved commit model or the commit-lock model, the lock is taken only at the commit time of the ddl, so does it really cause these issues? 
//R: taking the lock only at commit avoids one problem but not this one. What pins S is the open block's intents in the master tablet, written at its first DDL statement, not any lock; session 1 in the diagram holds no lock and still pins S. Because session 1 takes X(L) only at COMMIT, it does not block readers' S(L) while idle, which is why the lock model does not have the naive model's read-path objection. But session 2's DDL, committing meanwhile, waits in its propagate step for every tserver to catch up; if that wait is "S ≥ HT_V" it is pinned by session 1's intents, and since session 2 holds X(L) during the wait, every reader needing a new catalog snapshot is pinned with it. Same for the reserved model's reader wait on S ≥ R. The fix is the wait condition, not the lock timing: propagate on the contiguity condition below (V's apply record and all earlier versions applied), which streams regardless of S. The list above was rewritten to name the exact waiting step per model.
//C: Yes if we try to resolve intents accurately we have this problem. But we need the guarantee to come from outside of intent resolution etc. I don't follow why S>= HT_B is pinned by session 1's intents since we already discussed below that resolving intents locally is not an option? are you assuming that session 1 already started to increment catalog version row, because it cannot be idle in that state for long?
//R: you are right and the response above it is withdrawn. I had carried over the CDCSDK bound (oldest running transaction) from the June notes; the transactional xCluster producer resolves running transactions against their coordinators before reporting S, so session 1's idle intents cost one status round trip per refresh, never a pin, regardless of whether it touched the version row. The section is rewritten as a correction: "pinning" is replaced by the actual per-wait cost, the guarantee does come from outside local intent resolution (master's resolution on the producer side), and the contiguity gate below is kept only as a cheaper option, no longer as a required remedy.

An optional cheaper gate for the version-based model, the alternative variant
`plan.md` records, strengthened with a contiguity condition. It avoids
consuming S on the DDL path (no clock wait, no status resolution, one poll for
the apply record only), at the price of certifying only versioned writers:

- The raw stream delivers apply records as soon as they are in the master WAL,
  without waiting for S. The poller parses the version-row write in each
  applied transaction and records, per database, the set of applied versions
  and the commit HT of each.
- Expose V for database db only when every version up to V has been applied
  for db (contiguity). Apply records of two transactions can reach the master
  WAL out of commit order, so "V applied" alone does not imply "V-1 applied";
  the contiguity check makes it so. Within one database, version order equals
  commit order, so contiguity up to V means every versioned catalog commit of
  db up to HT_V is present.
- Reads for database db are served at C_db, the commit HT of the highest
  contiguously applied version of db, not at S. A read at C_db reflects every
  versioned catalog change of db, and of the shared catalogs (global DDLs bump
  every database's version in the same transaction), up to that point.
- What C_db does not certify: catalog writes that carry no version (temporary
  relations, master-side writes, maintenance SQL). They become visible when
  the poller applies them; the writing session uses the floor of 4.6.

Trade-off: gating on S certifies every writer (including no-increment DDLs and
master-side writes) and reuses what the producer already computes; gating on
contiguity is cheaper per DDL and needs the poller to parse version-row
writes. Both are viable; the `plan.md` HT-watermark variant stands.

### 4.6 The DDL carve-out remains

None of the models gives read-your-own-writes:

- A DDL backend's uncommitted catalog writes exist as intents on master and,
  once streamed, as staged external intents in the local tablet. Staged
  external intents are stored under a metadata keyspace that the read iterator
  skips, and there is no local transaction status to resolve them against.
- Reads in DDL, and reads in a transaction block after a DDL, therefore go to
  master, exactly as in the PG shadow design.
- After the DDL commits, the same session's next cache miss read on an object
  it changed must not be served locally until the local tablet has that commit.
  For a DDL that incremented the version, the version wait covers it. For a
  DDL that did not (temporary relations), the session carries the identity of
  its own committed transaction as a floor and waits until the poller has
  applied that transaction. The PG shadow had to force a version increment
  instead.

### 4.7 A transaction participant on the local tablet


What a participant would need:

- Intents in native format. The raw stream carries each intent write with its
  transaction id and the ids of the tablets involved (verified in the producer).
  The rest of the transaction metadata a participant needs (status tablet,
  isolation level, start time) is in the master's WAL write record but is not
  copied into the stream record today; the producer would have to add it. The
  consumer today re-keys the intents into one opaque external-intent blob per
  transaction; instead it would write them as ordinary intents under the real
  transaction id, the way a replica applies a replicated write.
- Registration of each transaction with the local participant as the intents
  arrive, as a normal write does.
- Apply on the apply record: the poller invokes the participant's apply of the
  transaction's intents at the commit HT carried by the record, which is the
  same operation a real participant performs when its coordinator tells it to
  apply.
- Status resolution for transactions whose outcome has not been streamed yet:
  the participant queries the transaction's status tablet. This is exactly what
  a tablet does after bootstrap for every transaction that still has intents:
  load them, resolve their status against their status tablets, apply or
  discard. The local tablet is permanently in that mode for transactions it
  learns about from the stream.

What the local tablet must not do: it is not an involved tablet of any
transaction. The coordinator does not know it, will not send it apply or abort
notifications, and will not wait for it. Its participant is therefore a
read-only observer: it never votes, never blocks a commit, and learns outcomes
either from streamed apply records or from its own status queries.

What it buys:

- Reads resolve intents the way master reads do. A read at t that meets an
  intent asks the status tablet; "committed at c ≤ t" makes the row visible,
  "pending after the coordinator's clock passed t" or "aborted" hides it. The
  reader no longer needs "no transaction can still commit at or below t" to be
  certified in advance.
- The remaining requirement is only "every WAL write with HT ≤ t has been
  delivered here", which is master's leader safe time without the status
  resolution of 4.5. The resolution moves from master (once per poll, for
  every running transaction) to the reader (only for intents it encounters).
  Master's per-poll status round trips and the 250 ms refresh throttle
  disappear.
- Aborted transactions' intents are removed when the participant learns the
  abort by status query, instead of waiting for a TTL.

What it costs:

- The read-at-now models still need one poll round trip per invalidated
  catalog snapshot to learn that the WAL up to t has been delivered; the
  participant removes master's per-poll certification, not the round trip.
  The hybrid (version) model and the commit-lock model remain the ones with no
  read-path round trip.
- Status queries go to the transaction's status tablet, which lives on a
  tserver near where the DDL ran, possibly in another region. They are issued
  only for intents a read actually encounters, so only while a DDL on the rows
  being read is in flight.
- The consumer path changes from the external-intents reuse, chosen in
  `plan.md` because it needs no participant, to a native-intent path with a
  participant in observer mode. The apply-record path, the bootstrap-style
  status resolution, and the intent format are all existing code; running a
  participant against a stream it is not an involved tablet of is the new
  combination and needs its own verification, in particular that the
  participant never attempts to report to or heartbeat a coordinator.

Recommendation: not needed for correctness in any model, since S is already
certified by master (4.5). Adopt it only if master's per-poll status
resolution becomes a measured cost at scale and lazy per-read resolution is
preferred. In the hybrid (version) model and the commit-lock model reads must
stay intent-blind regardless (5.1): resolving intents on a read at C costs
status round trips and cannot change the result.

## 5. Hybrid model: version for exposure, commit HT for catch-up

This is the model `plan.md` decided on (gate variant D4). The catch-up
condition at lock release is either "certified S ≥ HT_V" (the plan's variant,
certifies all writers, costs master's status resolution) or the contiguity
condition of 4.5 (versioned writers only, cheaper). Reads are served at the
resulting local certified time, written C_db below. The naive model's "pick
now and wait" is replaced by "wait once, at version exposure".

//C: In the hybrid model, we wait until local version is at V. This means we either need to
// not use intents and wait for all committed/txn apply records to reach us via master OR
// resolve intents correctly using the local txn participant - however, this may involve multiple roundtrips to different
// ongoing txns to determine if they committed. The roundtrips may affect any time rows are read from local tablet so
// an ongoing DDL will cause all catalog cache misses to be delayed, so it seems we have to ignore intents and just wait for committed data.  
//R: agreed, and that is what the model below does. Reads are at C_db and see only rows written by apply records; staged intents are invisible by construction (external-intents path). A participant would make a read at C_db resolve every intent with write time ≤ C_db whose transaction is still open, one status round trip per such transaction, possibly cross-region, on every cache miss read while any DDL is in flight. The contiguity gate already guarantees that every versioned commit at or below C_db is applied, so those intents can only belong to transactions that are still open or committed after C_db, i.e. invisible at C_db anyway; resolving them buys nothing and costs the round trips. The participant of 4.7 is therefore only for the read-at-now models, and 4.7's recommendation is worded that way. Stated explicitly in 5.1 below.
//C: When a DDL commits, let's say we can send the new version and commit time of that DDL to all tservers via obj lock release call. What would the local tserver do? It first applies new catalog version to shared memory so PG backends see it. The local tablet can poll out of band or more aggressively now. A PG backend read has two fields, catalog version, read time. If local tablet is caught up to the PG backend version, the read is executed as of the time requested. Note that the time requested may be `now`, picked on the local tablet. It is then possible some other DDL actually commits at this time later and we get a new catalog version increment informing us of this, so that mean this read is not guaranteed consistent - for ex, reading pg class and pg attribute at this now don't return consistent records. This is a major concern and defeats the point of using hybrid time based reads. So what we have to do instead is set the read time a bit more conservatively to the last time we are guaranteed no higher catalog version exists.
//R: agreed, and the model is specified that way: the requested time is never used when it exceeds the local certified time. A local tablet is not a DocDB replica with its own safe time; rows appear when apply records are applied, so a read at a time t beyond the last certified time can change between two scans of one snapshot as applies land, which is the pg_class-versus-pg_attribute inconsistency described. The conservative time is C_db, the last instant at which the local copy is provably complete: with the S-based gate it is the last certified S applied here (complete for every writer); with the contiguity gate it is the commit HT of the highest contiguously applied version (complete for versioned writers; version order equals commit order, so no higher version can exist at or below it). A read requesting h > C_db is served at C_db, not at h. Ordering of exposure: the comment's sequence (publish the version first, let the read wait per op) and the exposure gate (publish only after catch-up) are both consistent with this rule; under object locking the release acknowledgement must wait for catch-up anyway, so the gate is the natural choice and the per-op wait covers only the DDL's own tserver.

### 5.1 How it works

The read time is C_db, never the requested time when that is later. C_db is
the last time at which the local copy is provably complete; a read beyond it
could observe applies landing between two scans of the same snapshot and
return inconsistent rows across catalogs. C_db advances only after the poller
has applied every record of the response that carried the new S (4.5), so the
regular store is complete at C_db without consulting intents.

Reads in this model never resolve intents. The local tablet serves rows written
by apply records only; staged intents are invisible to the read iterator. The
per-database contiguity condition (4.5) is what makes this sufficient: at C_db
every versioned catalog commit of the database at or below C_db has been
applied, so any staged intent with write time ≤ C_db belongs to a transaction
that is still open or committed after C_db, and is correctly invisible at
C_db without asking anyone.

```
 DDL backend        master                       tserver T (lock manager, poller)          backend on T
 -----------        ------                       --------------------------------          ------------
 commit V at HT_V
                    lock release (V) ───────────► wait until versions ≤ V applied for db
                                                 (eager poll: GetChanges) ─► master
                                                 ◄─ records incl. V's apply
                                                 apply; C_db := HT_V; shared version := V
                    ◄──── ack ──────────────────
 "success"                                                                                  statement start:
                                                                                            local version := V,
                                                                                            drop catalog snapshot
                                                                                            cache miss read tagged V:
                                                                                            applied[db] ≥ V → serve at C_db
```

- The tserver holds the release acknowledgement until the poller has applied
  every version of the database up to V. It then publishes V to shared memory.
- A backend that has moved to V reads the local tablet at C_db, which is at or
  beyond HT_V by construction. Every read op carries the backend's version;
  the tserver serves it only once the applied versions for that database reach
  the op's version, and waits otherwise. This per-op wait is what protects the
  DDL's own tserver, where the DDL backend's local version advances at commit,
  before the poller has pulled the apply record. The wait is bounded by one
  pull round trip. Master is used only when the poller is not running (leader
  change, re-seed), never as a normal path, so master sees a steady poll load
  rather than bursts of redirected reads.

### 5.2 Where the hybrid model waits


| Who waits | When | Expected duration |
|---|---|---|
| The DDL (its lock-release fan-out) | Once per DDL, per tserver, in parallel | One extra master round trip per tserver (release push arrives, tserver pulls the WAL), plus apply. With the S-based gate, plus master's status resolution while other catalog transactions are running (4.5); with the contiguity gate, not. Removable by carrying V's WAL records in the release message (eager apply), at the cost of ordering care. |
| Readers on the DDL's own tserver | Cache miss reads tagged V before the poller has V | Wait until applied[db] ≥ V: at most one pull round trip, usually less because the release fan-out to this tserver triggers the same pull. Not routed to master. |
| Readers elsewhere | Never | They learn V only after the local tablet has it. |
| Heartbeat path (no lock release) | Versions arriving by heartbeat are clamped to the local applied version | Readers see V up to one poll later than the heartbeat; no stall. |

Compared with the naive model: readers pay no master round trip, because they
no longer ask for "now"; they ask for "at least my version", which the exposure
gate has already satisfied. A long-open transaction block that ran a DDL costs
other DDLs at most a status round trip on master per S refresh, and nothing
with the contiguity gate.

### 5.3 Comparison with the PG shadow's gate

The hybrid model and the PG shadow enforce the same contract with the same
shape: published version (PG) or applied version (DocDB) ≥ local version.
Differences:

- The PG shadow's published version is produced by a tserver-side writer that
  must read the version together with the data (fix F2 in the PG design). The
  DocDB applied version is a property of the WAL stream: the version row and
  the data rows arrive in one apply record, so "applied V" implies "has V's
  data" without a separate argument; contiguity supplies "has everything
  before V".
- The PG shadow waits in the reader (bounded, then falls back to master). The
  DocDB model waits in the DDL's release path. In object-lock mode the second
  is the natural place: a backend acquiring a lock after the release must see
  V, and the release is now acknowledged only when the tserver can guarantee
  that from the local copy.
- Both need the same DDL carve-out. The DocDB model additionally needs the
  per-op version wait on the DDL's own tserver; the PG shadow needs its
  "in DDL" gate to include statements after a DDL in the block (fix F1).
- Semantics for cache miss reads are "≥ my version" in both. The DocDB model
  can additionally offer exact as-of-time reads (Section 6), and the
  commit-lock addition of 4.3 upgrades it to "as of now".

## 6. The three read kinds under each model


Read time rule per approach. C_db is the local certified time of Section 5:
the last certified S applied locally, or, with the contiguity gate, the commit
HT of the highest contiguously applied version of the backend's database. C is
the last certified S applied locally (commit-lock model).

- PG shadow: one MVCC snapshot per catalog snapshot; state is "at or beyond
  my version"; no notion of a hybrid time.
- Local tablet, hybrid (version) model: a read carries a requested time h and
  the backend's version V. Serve at h if h ≤ C_db. Otherwise serve at C_db.
  C_db ≥ HT_V is guaranteed by the exposure gate (or by the per-op wait on the
  DDL's own tserver), so a read at C_db always reflects V. No read waits for
  C_db to reach "now".
- Local tablet, hybrid-time-guarantee model: serve exactly at h = now, after
  one poll per invalidated catalog snapshot in which master certifies now
  (4.2, 4.5).
- Local tablet, commit-lock model (pure form): take S(L), t := C, release,
  read at C; C is equivalent to now for all lock-taking DDLs (4.3). No version
  on the read path, no master round trip.

| Read kind | PG shadow | Local tablet, hybrid (version) | Local tablet, hybrid-time-guarantee | Local tablet, commit-lock |
|---|---|---|---|---|
| Cache miss reads | Shadow snapshot; at or beyond my version. Bounded wait once per catalog snapshot if the writer is behind; master fallback after the wait. | h = the catalog snapshot's read time. Exact at h if h ≤ C_db, else at C_db. Result: at or beyond my version, exact when caught up. Wait only on the DDL's own tserver, at most one pull round trip. | Exact at now. One master round trip per invalidated snapshot, plus master's status resolution while catalog transactions are running (4.5). | Exact as of now for lock-taking DDLs, at C. One local shared lock per invalidated snapshot; waits only during a DDL commit window (tens to hundreds of ms, cluster-wide, once per DDL). |
| Internal catalog reads | Master today; F7 moves DML-time ones to the shadow with the same semantics as cache miss reads. In DDL: master. | Same rule and result as cache miss reads. In DDL: master. | Same as cache miss reads. In DDL: master. | Same as cache miss reads. In DDL: master. |
| User catalog reads | Shadow snapshot; at or beyond my version. Exact transaction-snapshot semantics would need versioned rows. In DDL: master. | h = the transaction's read time. `REPEATABLE READ` and `SERIALIZABLE`: h was fixed at transaction start and is normally ≤ C_db, so the read is exact PG semantics. `READ COMMITTED`: h is the statement's read time, normally > C_db; served at C_db, i.e. at or beyond my version. In DDL: master. | Exact at h for any isolation level; `READ COMMITTED` pays the certification per statement. In DDL: master. | `REPEATABLE READ` and `SERIALIZABLE`: exact at h when h ≤ C. `READ COMMITTED`: at C taken under S(L), which is as of now for lock-taking DDLs. In DDL: master. |

What the hybrid (version) model gives up relative to exact PG semantics: a
`READ COMMITTED` user catalog read may return a state older than the
statement's read time by the poll lag, and may miss a DDL that committed in
that window. That DDL's version has not been exposed on this tserver, and under
object locking the DDL has not been acknowledged to its client until every
tserver's release ack, so the read may be ordered before it; this is the same
premise the PG shadow relies on (design doc 6.4). The commit-lock model removes
this gap at the cost of a cluster-wide lock per DDL commit.

YB today serves user catalog reads at the catalog read time, not at the
transaction read time, and plans to move to PG semantics. The hybrid model
delivers PG semantics for `REPEATABLE READ` and `SERIALIZABLE` without waiting
and the "at or beyond my version" contract for `READ COMMITTED`.

### 6.1 Recommendation

- Default for cache miss reads and internal catalog reads: the version model.
  Readers never block. The read time is C, the safe time of the last poll
  applied locally; the DDL absorbs the catch-up by holding its lock release
  until every tserver's poll has applied its commit.
- The commit-lock model is too expensive for those two read kinds. Every new
  catalog snapshot on every tserver would block for the DDL's whole window:
  exclusive lock acquisition through master and its fan-out, the commit at the
  transaction status tablet, the release through master and its fan-out, and
  each tserver's poll to catch up, including master's status resolution when
  other catalog transactions are open. It remains an option for user catalog
  reads only, behind a setting, where exact-now visibility of catalog
  `SELECT`s may be worth a per-DDL stall.
- The version model leaves one window: between the DDL's commit on master and
  the completion of its release fan-out, the commit is visible to direct
  master reads but not to local copies. Direct master readers are backends in
  DDL and tservers whose poller is down. A backend in DDL that touches the same
  objects blocks on their exclusive locks until the release, so it sees them
  only after propagation; for unlocked objects it may see the new state while
  DML backends on its tserver still see the old one until their version bump.
  That is the pre-invalidation skew PG and YB already have between a fresh
  master read and cached entries; the version model widens it by at most one
  poll and makes every backend on a tserver flip together. Not a new
  inconsistency class.

## 7. What the DocDB approach changes in the PG shadow's finding list

| PG shadow finding | Under the local tablet |
|---|---|
| C1 global DDL replicates per-database rows into other databases | Gone. Rows are keyed by their own catalog table identity; there is no per-database change log to fan out. |
| C2 subtransaction rollback stays in the change log | Gone. Verified in code: the raw stream's apply record carries the transaction's aborted-subtransaction set, the consumer copies it into the local apply request, and the external-intents apply skips every intent whose subtransaction is in that set. Inclusion of the set on the producer is behind a flag that must be on for this stream. |
| C3 DDL without version increment leaves the copy stale | Different shape. The rows arrive by WAL, so the copy is not stale. The session's own read-your-writes needs the transaction floor of Section 4.6 instead of a forced version increment. |
| C4 missing change log ambiguous | Gone. No change log. |
| C5 unlogged files not crash-consistent | Gone. The local tablet has its own WAL and flush. |
| C6 bulk copy publishes an unobserved version | Gone. Version and data arrive in one apply record. |
| C7 per-scan snapshot | Different mechanism. Reads are at C_db, pinned per catalog snapshot by the same lifetime rules; needs the same "one read time per catalog snapshot" discipline on the tserver side. |
| C9 heap and index bloat | Gone; compaction. Replaced by staged-intent TTL for aborted DDLs. |
| C11 change-log size fails the DDL | Gone. Nothing is added to the DDL commit. |
| C13 upgrade and schema changes | Replaced by the metadata-replay component, which is new code but generic. |
| C14 statements after a DDL in the block | Same carve-out needed. |
| New | Master-side stream fan-out and retention at scale; leader-change and re-seed fallbacks; metadata replay; the guard on the change service that forbids raw streams on the system catalog; non-PG rows in the local tablet; per-tserver stream lifecycle; master's per-poll status resolution cost with many streams and open DDL transactions (4.5). All unproven. |

## 8. Position

- The PG shadow is a working system whose remaining defects are all in the
  change-log construction, and every one of them is a consequence of building
  a second replication channel in the DDL's commit path. The fixes are known
  and small, but the class does not close: any new catalog write path must
  remember to capture.
- The local tablet removes that class by reusing the WAL, gives crash
  consistency and history for free, and moves the freshness wait to the place
  object locking already waits (lock release). Its costs are master-side
  stream management at scale and a set of new DocDB components that have not
  been exercised together.
- The stream safe time is certified by master through coordinator status
  resolution on every poll (4.5). An open transaction block does not pin it;
  it costs a status round trip per refresh. The `plan.md` gate stands; the
  contiguity gate is a cheaper option for the DDL path. The naive read model is
  ruled out by its master round trip per statement, not by pinning.
- The hybrid (version) model matches the PG shadow's contract with no read-path
  round trips. The commit-lock model in its pure form (4.3) removes the version
  from the read path and gives "as of now" for one cluster-wide exclusive lock
  per DDL commit; reads are at the last certified time C, never at the raw
  clock. The hybrid-time-guarantee and reserved-commit-time models are not
  recommended.
- A read-only transaction participant on the local tablet (4.7) is not needed
  for correctness. It moves status resolution from master (per poll) to the
  reader (per encountered intent) and replaces the external-intents consumer
  path; consider it only if master's per-poll resolution becomes a measured
  cost.
- Open questions specific to the DocDB path, in order of risk: fan-out and
  retention cost with hundreds of streams on one master tablet; master's
  per-stream intent resolution while catalog transactions are open (status
  round trips scale with tserver count unless the resolution is shared per
  source tablet, 4.2); the poller's version-row parsing and contiguity
  tracking if that gate is used; fallback behavior during leader change and
  re-seed; the eager-apply optimization's ordering rules; the metadata-replay
  component for catalog schema changes.
