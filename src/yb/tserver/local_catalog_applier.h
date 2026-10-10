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

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "yb/cdc/cdc_service.pb.h"
#include "yb/cdc/cdc_service.proxy.h"

#include "yb/common/hybrid_time.h"
#include "yb/common/opid.h"

#include "yb/tablet/tablet_fwd.h"

#include "yb/tserver/master_leader_poller.h"
#include "yb/tserver/tserver_fwd.h"

#include "yb/util/monotime.h"
#include "yb/util/net/net_util.h"
#include "yb/util/status_fwd.h"

namespace yb::tserver {

class LocalCatalogReplica;
class PushSysCatalogChangesRequestPB;
class XClusterWriteInterface;

// What applying a push of pages did to the copy.
struct LocalCatalogPushOutcome {
  // The applied position after the pages, or the position at which a gap was found.
  OpId applied;
  // C after the pages.
  HybridTime safe_time;
  // A page started above the applied position and nothing from it on was applied.
  bool gap = false;
};

// Applies master's system catalog WAL, which master pushes in pages, to the tserver-local copy of
// that tablet.
//
// One thread does all of: create or reopen the local tablet if it is not open, apply every record
// of a page in WAL order, and only then publish the safe time the page carries as C together with
// the catalog versions the copy holds at C. Publishing before the apply finished would let a read
// at C miss rows that master had certified as present at C. The thread is driven by a
// MasterLeaderPollScheduler: it runs when pages arrive or a waiter asks for a push, and otherwise
// once per local_catalog_floor_request_interval_ms to repeat an open floor request.
class LocalCatalogApplier : public MasterLeaderPollerInterface {
 public:
  LocalCatalogApplier(LocalCatalogReplica& replica, TabletServer& server);
  ~LocalCatalogApplier();

  Status Start();
  void Shutdown();

  // A waiter needs the copy to be complete up to floor. Asks master to push up to the highest such
  // floor, at most once per local_catalog_floor_request_interval_ms while it is not reached.
  void RequestPush(HybridTime floor = HybridTime::kInvalid);

  // Applies pages that master pushed or carried on a lock release, on the apply thread, in the
  // order of their starting positions. With wait set, waits for the outcome until the deadline;
  // without it, queues the pages and returns at once. Returns an error when the apply fails or
  // the deadline passes.
  Result<LocalCatalogPushOutcome> ApplyPushedPages(
      std::shared_ptr<const PushSysCatalogChangesRequestPB> pages, CoarseTimePoint deadline,
      bool wait);

  // MasterLeaderPollerInterface.
  Status Poll() override;
  MonoDelta IntervalToNextPoll(int32_t consecutive_failures) override;
  void Init() override;
  void ResetProxy() override;
  std::string name() override;
  const std::string& LogPrefix() const override;

 private:
  // Creates or reopens the local tablet and loads the persisted WAL position. A no-op once the
  // tablet is open and no re-seed is pending.
  Status EnsureLocalTablet();

  // Applies every record of the response to the local tablet, in WAL order.
  // What one response's records changed, for logging. The map is keyed by the name of the table
  // the record belongs to: a cotable of the system catalog for a PG catalog row, and
  // "sys.catalog" for master's own metadata rows, which the copy applies as well but PG never
  // reads.
  struct AppliedRecordStats {
    size_t writes = 0;
    size_t deletes = 0;
    // Writes and deletes that belong to a transaction, so they land in the intents db and stay
    // invisible until that transaction's APPLY record arrives.
    size_t provisional = 0;
    size_t transaction_records = 0;
    size_t metadata_records = 0;
    std::map<std::string, size_t> by_table;

    std::string ToString() const;
  };

  // Applies the records of changes above the applied position to the local tablet, in WAL order.
  Status ApplyRecords(const cdc::GetChangesResponsePB& changes, AppliedRecordStats* stats);

  // Advances the applied position to end, when end is above it.
  void AdvanceTo(const OpId& end);

  // Publishes safe_time as C, with the per-database versions read from the copy at that time.
  // Shared by pushed pages and pages carried in a lock release, so that both publish on identical
  // terms.
  Status PublishSafeTime(HybridTime safe_time);

  // Owned by both sides for the whole handoff: the waiter may give up on its deadline while the
  // apply thread is still applying, and both the pages and the outcome have to outlive that.
  struct PendingPush {
    std::shared_ptr<const PushSysCatalogChangesRequestPB> pages;
    Result<LocalCatalogPushOutcome> result{STATUS(Uninitialized, "")};
    bool done = false;
  };

  // Applies one push of pages. Runs on the apply thread.
  Result<LocalCatalogPushOutcome> DoApplyPages(const PushSysCatalogChangesRequestPB& pages);

  // Completes a handed-off push with the given outcome and wakes whoever is waiting on it.
  void CompletePendingPush(
      const std::shared_ptr<PendingPush>& push, Result<LocalCatalogPushOutcome> result);

  // Applies every queued push, lowest starting position first. Runs on the apply thread.
  void ApplyPendingPushes();

  // Asks master to push up to the highest pending floor, unless a request is already outstanding
  // or the same floor was asked for less than local_catalog_floor_request_interval_ms ago. Runs on
  // the apply thread.
  void MaybeSendFloorRequest();

  // The table a record belongs to, named if the copy's metadata knows the cotable and given as
  // the raw cotable id if it does not.
  std::string RecordTableName(const cdc::CDCRecordPB& record) const;

  // Applies one CHANGE_METADATA record: the schema and cotable set changes that a CREATE DATABASE
  // or a catalog table alter made on master.
  Status ApplyChangeMetadataRecord(const cdc::CDCRecordPB& record);

  // Writes one accumulated batch of external intents, applies and non-transactional writes into
  // the local tablet, and waits for it to be replicated in the local single-peer Raft group.
  Status WriteBatchToLocalTablet(const std::shared_ptr<WriteRequestMsg>& write_request);

  // Reads the local copy of pg_yb_catalog_version at read_time. The result is A[db] for every
  // database the copy knows about.
  Result<std::unordered_map<uint32_t, uint64_t>> ReadLocalCatalogVersions(HybridTime read_time);

  // Whether master has finished initdb. The copy is not created before then.
  Result<bool> IsInitDbDone(const HostPort& master_leader, MonoDelta timeout);

  // The permanent uuid of the server at host_port. The remote bootstrap source is named by uuid
  // in its logs and in the session id, so the copy asks the master leader for its own.
  Result<std::string> GetPeerUuid(const HostPort& host_port, MonoDelta timeout);

  Status LoadCheckpoint();
  Status PersistCheckpoint(bool force);
  Result<std::string> CheckpointPath() const;

  LocalCatalogReplica& replica_;
  TabletServer& server_;
  const std::string log_prefix_;

  MasterLeaderFinder finder_;
  MasterLeaderPollScheduler scheduler_;
  std::optional<cdc::SysCatalogChangeServiceProxy> proxy_;

  // Owned by LocalCatalogReplica once published; kept here for the apply thread's own use.
  tablet::TabletPeerPtr tablet_peer_;

  // Whether a setup attempt has been made since this applier started. Only the apply thread reads
  // and writes it.
  bool setup_attempted_ = false;

  // Pushes handed from RPC threads to the apply thread, keyed by the starting position of their
  // first page, so that pages from a release and from a separate push are applied in WAL order and
  // the later of two overlapping ones is filtered to nothing.
  std::mutex push_mutex_;
  std::condition_variable push_cond_;
  std::multimap<int64_t, std::shared_ptr<PendingPush>> pending_pushes_ GUARDED_BY(push_mutex_);
  // Set by Shutdown: new pushes are refused and waiting callers return.
  bool push_shutdown_ GUARDED_BY(push_mutex_) = false;
  // RPC threads inside ApplyPushedPages. Shutdown waits for them to leave.
  size_t push_callers_ GUARDED_BY(push_mutex_) = 0;

  // Highest master term pages were applied from. Pages from a lower term come from a deposed
  // leader and are refused. Only the apply thread uses it.
  int64_t highest_master_term_ = 0;

  // Highest floor passed to RequestPush that the copy's safe time has not reached yet, as
  // HybridTime::ToUint64(); 0 when there is none.
  std::atomic<uint64_t> wait_target_{0};

  // Floor request state, used by the apply thread and the request's callback.
  struct FloorRequest;
  std::shared_ptr<FloorRequest> floor_request_;
  uint64_t last_floor_sent_ = 0;
  MonoTime last_floor_sent_at_;

  // The WAL position of the master system catalog tablet up to which records have been applied.
  OpId checkpoint_;
  OpId persisted_checkpoint_;
  MonoTime last_checkpoint_flush_;
};

}  // namespace yb::tserver
