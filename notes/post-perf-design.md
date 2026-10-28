# Master load: what the load tests showed, and the change they motivate

Date: 2026-10-01. Companion to [local_catalog_docdb_design.md](local_catalog_docdb_design.md)
(sections 5.4 and 10). Measurements: [loadtest-summary.md](loadtest-summary.md),
[loadtest-large-ddl-wal-sizes.md](loadtest-large-ddl-wal-sizes.md),
[load_user_scenario_summary.md](load_user_scenario_summary.md).

## 1. What was measured

Release build, one master, 16-vCPU host, a load client standing in for N tservers
(N distinct requester ids polling the change service), master's log cache keeping
the system catalog ops (fix 2), waiters woken only on progress (fix 1).

| Measurement | Result |
|---|---|
| A poll that returns nothing | 70–93 µs of master CPU; 1000 tservers polling every 100 ms cost 0.93 cores |
| A poll that returns records, ops in the log cache | 16 ms of master CPU per MB of response unsaturated, 25 ms/MB with the host saturated |
| Same, ops not in the log cache | 60 ms/MB: the on-disk WAL format is decoded per stream |
| Where the CPU goes | 97 % of a lagged poll builds the records from the WAL ops, serializes them and frees them, once per stream; log-cache reads, locks and intent resolution are under 3 % each |
| Response size against WAL size | 84–97 % |
| Pool exhaustion | With the service on the high-priority pool, N=2000 deadlocked every worker inside intent resolution; the lease refresh timed out and PG sessions were killed |
| Intent resolution | One status round trip per open catalog transaction per stream per 250 ms: 10–22k coordinator RPCs/s at N=2000–4000 |

What one tserver receives per DDL (two-column tables):

| DDL | Response bytes | Notes |
|---|---|---|
| `CREATE TABLE` | 13 KB | |
| `ALTER TABLE ADD COLUMN` | 7 KB | |
| `DROP TABLE` | 18 KB | |
| 20-statement migration batch, one transaction | 110–240 KB | |
| `ALTER TABLE ADD COLUMN`, 100 partitions | 0.67 MB | |
| `ANALYZE`, 100-table database | 1.5 MB | 145 version bumps unless wrapped in a transaction |
| `DROP SCHEMA ... CASCADE`, 100 tables | 1.8 MB | 4 453 WAL ops over 5 s, not one write |
| `CREATE DATABASE` | 11.6 MB, 41 831 records | the template catalog copied as rows; 3 polls at the 4 MB batch cap |

Two facts about the stream shape matter for the design below. A transaction's
row records are shipped as they are replicated, before its commit, so a long
statement is drained incrementally by whoever polls during it; only a tserver
that did not poll for the statement's duration receives it whole. And
`CREATE DATABASE` and `DROP DATABASE` are the only statements that are both
large and global (they increment every database's version), so they are the
only ones whose propagation holds every database's gate on every tserver.

## 2. What it costs at scale, as designed today (pull, log cache, no other change)

Every tserver pulls every DDL from master; 25 ms/MB; 10 Gbit NIC. Per 100
tservers, and per 1000.

| Scenario | Bytes / master CPU per 100 tservers | Per 1000 tservers | Time at 1000 (CPU on 32 vCPUs / NIC) |
|---|---|---|---|
| 100 tservers catch up on 50 `CREATE TABLE` (0.63 MB each) | 63 MB / 1.6 CPU-s | 0.63 GB / 16 CPU-s | 0.5 s / 0.5 s |
| Catch up on 2 000 DDLs (~26 MB each, in cache) | 2.6 GB / 65 CPU-s | 26 GB / 650 CPU-s | 20 s / 21 s; 2.4× the CPU if from disk |
| `DROP SCHEMA`, 100 tables, over the 1 MB push cap so everyone pulls | 182 MB / 4.6 CPU-s | 1.8 GB / 46 CPU-s | 1.4 s / 1.5 s |
| `CREATE DATABASE` | 1.16 GB / 29 CPU-s | 11.6 GB / 290 CPU-s | 9 s / 9 s, plus the local apply of 42 000 rows per tserver |

The times are lower bounds: with the default 50-entry service queue, 1000
simultaneous polls are mostly rejected and retried with a backoff of up to a
second, so the tail is set by retry waves. The CPU is the per-stream
duplication of identical work; the bytes are N times the DDL. Nothing here is a
correctness problem, and in steady state the cost is under a core at 1000
tservers, but a burst saturates master for seconds and, through its RPC pools,
can take the lease service with it.

## 3. The change: master pushes, bounded; nobody pulls

1. **Master pushes WAL; tservers never pull.** Each tserver reports its copy's
   applied position in its heartbeat. A new or returning tserver reports its
   position and waits for master to push the rest. The lock release carries
   the DDL's own records as today; a background push cycle on master carries
   everything written outside a DDL (master's own catalog writes, temporary
   relations, non-DDL catalog writes).
2. **Master bounds the work in flight.** A fixed number of push RPCs and a
   fixed number of bytes outstanding at any time, so a thousand targets are
   served in waves rather than all at once, and master's CPU, memory and NIC
   stay at a chosen level regardless of how many tservers are behind.
3. **A DDL completes only when every tserver with a live lease has applied its
   records** (is at or past the expected position). This is the existing gate;
   the push just makes master the party that knows when it is satisfied.
4. **A tserver that needs the copy to reach a point sooner asks master to push
   to it.** That covers the own-write floor after a temporary-relation DDL and
   the lease-regain bound; the request is a hint into master's scheduler,
   admission-controlled per tserver.
5. **Remote bootstrap is bounded on master too**, and a tserver can bootstrap
   its copy from a peer tserver's copy instead of from master, which also
   seeds certified state (applied rows up to C and staged intents, no
   uncommitted tail).

Why this is the right shape:

- Master's resources are always under its own control. The load of a burst is
  bounded by the configured in-flight limits, not by how many tservers decided
  to poll in the same second.
- DDL becomes slower when propagation is slow, and that is tolerable. Every
  version-incrementing DDL already holds the exclusive lock on the catalog
  version table across its fan-out, so DDLs are serialized by it; a slow push
  lengthens that serialization and nothing else. Even the worst case,
  `CREATE DATABASE` to 1000 tservers, is about 3 GB compressed and completes in
  seconds on a 10 Gbit NIC.
- Master can compute once. With the set of outstanding targets in hand, it
  serializes each WAL range once and sends the same bytes to every target whose
  position it covers, which removes the 97 % duplication measured above; the
  bound on in-flight RPCs makes the once-per-range work worth doing.

## 4. What remains

Status 2026-10-07: §3 items 1-4 are implemented ([push-plan.md](push-plan.md) §11) and
measured (push-plan §9.7; release, the same 16-vCPU host as §1, push receivers in the test
process; [loadtest-summary.md](loadtest-summary.md) "Push"):

| Measurement | Pull (§1, §2) | Push |
|---|---|---|
| Catch-up, master CPU per MB sent to one tserver | 25 ms from the log cache, 61 ms without | 1.0-1.5 ms: pages are built once and the same bytes go to every target |
| 1000 tservers catch up 2000 DDLs (26.6 MB each) | 650 CPU-s (estimate) | 26 CPU-s; all caught up in 13.6 s on loopback, at most 48 RPCs / 50 MB in flight |
| 1000 tservers catch up 50 DDLs | 16 CPU-s (estimate) | 1.3 CPU-s; all caught up in 1.2 s |
| `CREATE DATABASE` to 1000 tservers (11.85 MB each) | 290 CPU-s (estimate) | 9.2-10.5 CPU-s including the statement; all 1000 at the head 4.66 s after the statement (loopback, about 2.5 GB/s; 100 tservers: 0.49 s) |
| Steady DDLs (about 33/s) to 1000 tservers | 5.3 cores above the DDL baseline | 2.8-2.9 cores: about 75 µs of master CPU per push RPC, about one push RPC per DDL per target |
| Idle cluster, 8000 tservers | 5.5 cores of empty polls | no push cycles; one initial 247 KB push to each new target (measured before the fix that keeps target positions across a new leader's first cycle) |

What the numbers change in this document:

- The per-stream duplication of §1 (97% of a lagged poll) is gone. Steady fan-out is now
  bounded by the cost of one push RPC per DDL per target: the pusher's scheduler loop is
  20% of master CPU at 1000 targets, and the RPC reactors' sending and response handling
  about 28%. Sending several DDLs in one push per target would cut it in proportion; not
  done.
- The in-flight bound holds master's memory flat during a wave, and master's rate on
  loopback (up to about 2 GB/s) is above a 10 Gbit NIC, so the NIC sets the time, as the
  last paragraph of this section argues.
- Not measured: a DDL's completion while it waits for 1000 real tservers. The test's
  receivers hold no lease, so no DDL waited for them. With 3 real tservers
  (`RealTserversOnly`) gate waits were under 1 µs and DDL p50 was 122 ms.
- A target that does not advance is backed off exponentially, by design (`PushFixedLag`,
  loadtest-summary.md).
- The earlier 100-receiver `CREATE DATABASE` result (no receiver at the head within 30 s)
  was a harness defect: the head was read after the requested cycle and included writes
  no cycle carried. Fixed; the rerun is in the table above.

- **During a long catch-up, DDLs complete asynchronously.** While an AZ's
  tservers are being pushed hours of WAL, every DDL's synchronous release waits
  for them, reaches its deadline, and master completes it asynchronously: the
  client is told success before every node has published, and correctness on
  the lagging nodes rests on the read-side version check (a backend that has
  been told a version its copy lacks waits for it). That is the existing
  fallback path; under this design it is the normal mode for the duration of a
  large recovery, and the document should say so.
- **Writes outside DDL depend on the push cycle.** Nothing but the timer
  carries master's own catalog writes, temporary-relation DDL and non-DDL
  catalog writes, so the latency of the own-write floor is "until the requested
  push is served", which under the in-flight bound can exceed today's immediate
  poll.
- **A new master leader has no positions until the first heartbeats arrive.**
  A release in that window either waits one heartbeat interval or pushes from a
  fixed look-back position and lets tservers drop the overlap.

Speed is the remaining dimension, and master's NIC is what sets it: N times the
DDL's bytes have to leave somewhere. When that matters, the WAL can be
distributed through the tservers themselves, one level deep: master pushes
each range to at most 100 tservers and gives every other tserver a pointer to
one of them; the second tier serves the same bytes from its copy of the range.
A one-level tree is enough to take a 1000-tserver `CREATE DATABASE` from
seconds of master NIC time to a tenth of that, with master still the only party
that certifies safe times and completes DDLs.
