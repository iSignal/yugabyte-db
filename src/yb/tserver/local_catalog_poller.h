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
class XClusterWriteInterface;

// Pulls the master system catalog tablet's WAL and applies it to the tserver-local copy of that
// tablet.
//
// One poll does all of: create or reopen the local tablet if it is not open, fetch the records
// after the persisted WAL position, apply every record of the response, and only then publish the
// response's safe time as C together with the catalog versions the response carried. Publishing
// before the apply finished would let a read at C miss rows that master had certified as present
// at C.
class LocalCatalogPoller : public MasterLeaderPollerInterface {
 public:
  LocalCatalogPoller(LocalCatalogReplica& replica, TabletServer& server);
  ~LocalCatalogPoller();

  Status Start();
  void Shutdown();

  // Wakes the poll thread so that a waiter for a version or a hybrid time does not have to wait
  // out the idle interval.
  void RequestImmediatePoll();

  // Applies a batch of change records that master carried on a lock release, on the poll thread,
  // and waits for the outcome. Returns an error when the batch does not continue this copy's
  // applied position, when the apply fails, or when the deadline passes; the caller then falls
  // back to waiting for the poller.
  Status ApplyPushedBatch(
      const cdc::GetSysCatalogChangesResponsePB& batch, CoarseTimePoint deadline);

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

  Status FetchAndApplyOnce();

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

  Status ApplyRecords(
      const cdc::GetSysCatalogChangesResponsePB& resp, AppliedRecordStats* stats);

  // Advances the applied position to the batch's end and publishes the safe time it carries, with
  // the per-database versions read from the copy at that time. Shared by the poll and the pushed
  // batch so that both publish on identical terms.
  Status PublishAppliedBatch(
      const cdc::GetChangesResponsePB& changes, const AppliedRecordStats& stats,
      HybridTime* new_c, std::unordered_map<uint32_t, uint64_t>* applied_versions);

  // Owned by both sides for the whole handoff: the waiter may give up on its deadline while the
  // poll thread is still applying, and both the batch and the outcome have to outlive that.
  struct PendingPush {
    cdc::GetSysCatalogChangesResponsePB batch;
    Status result;
    bool done = false;
  };

  // Applies a batch that arrived on a lock release. Runs on the poll thread.
  Status DoApplyPushedBatch(const cdc::GetSysCatalogChangesResponsePB& batch);

  // Completes a handed-off batch with the given outcome and wakes whoever is waiting on it.
  void CompletePendingPush(const std::shared_ptr<PendingPush>& push, const Status& status);

  // Applies the batch a release handler is waiting on, if there is one. Runs on the poll thread.
  void ApplyPendingPush();

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

  // Owned by LocalCatalogReplica once published; kept here for the poll thread's own use.
  tablet::TabletPeerPtr tablet_peer_;

  // The WAL position of the master system catalog tablet up to which records have been applied.
  // Whether a setup attempt has been made since this poller started. Only the poll thread reads
  // and writes it.
  bool setup_attempted_ = false;

  // A batch of records that arrived on a lock release, handed from the release handler's thread to
  // the poll thread. One at a time: a second release finds the slot taken and waits for the poller
  // instead.
  std::mutex push_mutex_;
  std::condition_variable push_cond_;
  std::shared_ptr<PendingPush> pending_push_ GUARDED_BY(push_mutex_);

  OpId checkpoint_;
  OpId persisted_checkpoint_;
  MonoTime last_checkpoint_flush_;
  bool have_more_messages_ = false;
};

}  // namespace yb::tserver
