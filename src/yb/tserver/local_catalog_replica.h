// Copyright (c) YugabyteDB, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.

#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "yb/common/entity_ids_types.h"
#include "yb/common/hybrid_time.h"
#include "yb/common/opid.h"

#include "yb/gutil/thread_annotations.h"

#include "yb/tablet/tablet_fwd.h"

#include "yb/tserver/tserver_fwd.h"

#include "yb/util/enums.h"
#include "yb/util/metrics_fwd.h"
#include "yb/util/monotime.h"
#include "yb/util/status_fwd.h"

namespace yb::cdc {
class GetSysCatalogChangesResponsePB;
}  // namespace yb::cdc

namespace yb::tserver {

class LocalCatalogPoller;

// Whether the tserver may serve catalog reads from its local copy of the master system catalog
// tablet.
//   kBootstrapping - the local tablet is being created, or the poller has not applied a response
//                    yet, so C is not a certified value.
//   kServing       - the local copy is complete up to C and reads may be routed to it.
//   kDisabledLease - the YSQL lease lapsed, so master may have committed DDLs whose lock release
//                    this tserver never acknowledged. The copy cannot be trusted until the poller
//                    has applied a response at or after the new lease grant.
//   kReseeding     - the local tablet is being replaced by a fresh copy of master's.
YB_DEFINE_ENUM(LocalCatalogServingState, (kBootstrapping)(kServing)(kDisabledLease)(kReseeding));

// The tablet id under which the tserver keeps its copy of the master system catalog tablet. It is
// deliberately different from master::kSysCatalogTabletId so that nothing on the tserver can
// resolve a client request for the master's tablet to this copy.
extern const char* const kLocalCatalogTabletId;

// A hybrid time rendered for logs as the physical microseconds it carries followed by the time of
// day they correspond to. Both are needed: the microseconds compare directly against the values in
// other hybrid times and in the safe time gauge, and the time of day lines up with the timestamps
// the log itself is prefixed with.
std::string LocalCatalogHybridTimeForLog(HybridTime ht);

// Tserver-local DocDB copy of the master system catalog tablet.
//
// The copy is a single-peer tablet holding the same table ids, schemas and row keys as the master
// system catalog tablet. Master remains the only writer; the copy is advanced by pulling the
// master system catalog tablet's WAL through a change stream and applying the records locally.
//
// Two published values drive every consumer:
//   C          - the highest hybrid time for which the local regular store is complete for every
//                writer. Every catalog row committed on master at or below C is present locally,
//                and no row committed above C is.
//   A[db_oid]  - the highest catalog version of database db_oid whose DDL transaction has been
//                applied locally.
// Whether the flags the tserver-local copy of the master system catalog tablet depends on are
// set. The copy's correctness rests on object locking serializing DDL against catalog readers and
// on DDL running in the user's transaction, so with either off the copy must not serve and
// catalog reads stay on the master path.
bool LocalCatalogPrerequisitesMet();

class LocalCatalogReplica {
 public:
  LocalCatalogReplica(TabletServer& server, const scoped_refptr<MetricEntity>& metric_entity);
  ~LocalCatalogReplica();

  // Registers metrics and creates the poller. Does not touch master and does not create the local
  // tablet.
  void Init();

  // Starts the poller. Its first poll creates or reopens the local tablet, and serving begins
  // once a response has been fully applied, so that C is a value master certified rather than a
  // clock reading.
  Status Start();

  void StartShutdown();
  void CompleteShutdown();

  LocalCatalogServingState state() const { return state_.load(std::memory_order_acquire); }
  bool IsServing() const;

  // C. Invalid until the poller has applied its first response.
  HybridTime safe_time() const { return HybridTime(c_.load(std::memory_order_acquire)); }

  // A[db_oid], or 0 when the local copy has no version row for that database yet.
  uint64_t applied_version(uint32_t db_oid) const EXCLUDES(mutex_);

  // Blocks until A[db_oid] >= version, asking the poller to poll immediately. Returns TimedOut if
  // the deadline passes first.
  Status WaitForAppliedVersion(uint32_t db_oid, uint64_t version, CoarseTimePoint deadline)
      EXCLUDES(mutex_);

  // Blocks until C >= target, asking the poller to poll immediately. Returns TimedOut if the
  // deadline passes first.
  Status WaitForSafeTime(HybridTime target, CoarseTimePoint deadline) EXCLUDES(mutex_);

  // Whether the copy already holds every catalog change committed at or below target. When it
  // does not, an immediate poll is requested so that the caller's next attempt is likely to
  // succeed. Used by the heartbeat path, which cannot block.
  bool HoldsCatalogStateAt(HybridTime target);

  // The local tablet peer, or nullptr while the copy is not open.
  tablet::TabletPeerPtr tablet_peer() const EXCLUDES(mutex_);

  // Called by the poller after every fully applied response: publishes C and A[db] together and
  // wakes every waiter.
  void PublishAfterApply(
      HybridTime new_c, const std::unordered_map<uint32_t, uint64_t>& applied_versions)
      EXCLUDES(mutex_);

  // Called by the poller when master has garbage collected WAL it had not consumed, or when the
  // local tablet cannot be opened. Stops the poller and re-runs the bootstrap.
  void RequestReseed();

  // Lease hooks.
  void OnLeaseLost();
  void OnLeaseGained(HybridTime lease_grant_time);

  // Takes the copy out of the read path while master reports a YSQL major version upgrade in
  // progress. In that window master holds two catalog version tables and DDLs still run, and the
  // copy's read rule for that state has not been worked out, so every catalog read goes to master.
  void SetMajorVersionUpgradeInProgress(bool in_progress);

  void IncPollFailures();
  void IncApplyFailures();
  void IncReadsServed();
  void IncReadsWaitedForVersion();
  void IncReadsWaitedForOwnWrites();
  void IncReadsToMasterInDdl();
  void IncReadsToMasterSerializable();
  void IncReadsToMasterNotServing();
  void RecordPollLatency(MonoDelta delta);
  void RecordApplyLatency(MonoDelta delta);
  void RecordGateWait(MonoDelta delta);
  void RecordLag(MonoDelta delta);

  const std::string& LogPrefix() const { return log_prefix_; }

  TabletServer& server() { return server_; }

  // The position in master's system catalog WAL that this copy has applied. Published by the
  // poller after each batch, read by the heartbeat thread. Invalid until the copy exists.
  void SetAppliedOpId(const OpId& op_id);
  OpId applied_op_id() const;

  // Applies change records that master carried on a lock release, so that this copy can reach the
  // release's catalog version read time without waiting for a poll. Errors leave the copy
  // untouched beyond what was applied and the caller waits for the poller as before.
  Status ApplyPushedBatch(
      const cdc::GetSysCatalogChangesResponsePB& batch, CoarseTimePoint deadline);
  void RequestImmediatePoll();

  void IncPushesReceived();
  void IncPushesApplied();
  void IncPushesRefused();

 private:
  friend class LocalCatalogPoller;

  void SetState(LocalCatalogServingState state);

  // Consumes a pending re-seed request, returning whether one was pending.
  bool TakeReseedRequest();

  void SetTabletPeer(const tablet::TabletPeerPtr& peer) EXCLUDES(mutex_);

  void OnReseedStarted();

  // Moves to kServing once the copy is complete past whatever floor its current state requires:
  // the first applied response for a fresh or re-seeded copy, and the lease grant time for a copy
  // that was disabled by lease loss.
  void OnSafeTimePublished(HybridTime c);

  TabletServer& server_;

  OpId applied_op_id_ GUARDED_BY(mutex_);

  scoped_refptr<Counter> pushes_received_;
  scoped_refptr<Counter> pushes_applied_;
  scoped_refptr<Counter> pushes_refused_;
  const scoped_refptr<MetricEntity> metric_entity_;
  const std::string log_prefix_;

  std::atomic<LocalCatalogServingState> state_{LocalCatalogServingState::kBootstrapping};

  // C, held as a raw uint64 so that reads on the request path are lock free.
  std::atomic<uint64_t> c_{0};

  // The hybrid time the poller must reach before versions may be published again after a lease
  // regain. Zero when no lease gap is outstanding.
  std::atomic<uint64_t> lease_gap_floor_{0};

  std::atomic<bool> major_version_upgrade_in_progress_{false};

  mutable std::mutex mutex_;
  mutable std::condition_variable cond_;
  std::unordered_map<uint32_t, uint64_t> applied_version_ GUARDED_BY(mutex_);
  tablet::TabletPeerPtr tablet_peer_ GUARDED_BY(mutex_);

  std::atomic<bool> shutdown_{false};
  std::atomic<bool> reseed_requested_{false};

  // Created by Init(), before any request thread can reach this object, and destroyed by
  // CompleteShutdown() after the poll thread has been joined, so readers need no lock.
  std::unique_ptr<LocalCatalogPoller> poller_;

  scoped_refptr<Counter> poll_failures_;
  scoped_refptr<Counter> apply_failures_;
  scoped_refptr<Counter> reseeds_;
  scoped_refptr<Counter> serving_disabled_lease_;
  scoped_refptr<Counter> reads_served_;
  scoped_refptr<Counter> reads_waited_for_version_;
  scoped_refptr<Counter> reads_waited_for_own_writes_;
  scoped_refptr<Counter> reads_to_master_in_ddl_;
  scoped_refptr<Counter> reads_to_master_serializable_;
  scoped_refptr<Counter> reads_to_master_not_serving_;
  scoped_refptr<AtomicGauge<uint32_t>> serving_state_;
  scoped_refptr<AtomicGauge<uint64_t>> safe_time_micros_;
  scoped_refptr<EventStats> poll_latency_;
  scoped_refptr<EventStats> apply_latency_;
  scoped_refptr<EventStats> lag_;
  scoped_refptr<EventStats> gate_wait_;
};

}  // namespace yb::tserver
