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

#include "yb/tserver/local_catalog_read.h"

#include "yb/common/pgsql_protocol.messages.h"

#include "yb/rpc/sidecars.h"

#include "yb/tablet/read_result.h"
#include "yb/tablet/tablet.h"
#include "yb/tablet/tablet_metadata.h"
#include "yb/tablet/tablet_peer.h"

#include "yb/tserver/local_catalog_replica.h"
#include "yb/tserver/pg_client.messages.h"

#include "yb/util/logging.h"
#include "yb/util/result.h"
#include "yb/util/status_format.h"

using namespace std::literals;

namespace yb::tserver {

bool AllOpsAreReadsOfLocalCatalogTables(
    const tablet::TabletPeerPtr& tablet_peer, const PgPerformRequestMsg& req) {
  if (req.ops().empty()) {
    return false;
  }
  auto tablet = tablet_peer->shared_tablet_maybe_null();
  if (!tablet) {
    return false;
  }
  const auto& metadata = *tablet->metadata();
  for (const auto& op : req.ops()) {
    if (!op.has_read()) {
      return false;
    }
    // A table the copy does not hold cannot be read from it. This also rejects a user table that
    // happened to share the request with a catalog table.
    if (!metadata.GetTableInfo(op.read().table_id().ToBuffer()).ok()) {
      return false;
    }
  }
  return true;
}

Result<tablet::ScopedReadOperation> PrepareLocalCatalogRead(
    const tablet::TabletPeerPtr& tablet_peer, const ReadHybridTime& read_time,
    CoarseTimePoint deadline) {
  auto tablet = VERIFY_RESULT(tablet_peer->shared_tablet());

  // The copy is written by the poller with the hybrid times master committed at, so its MVCC safe
  // time trails those writes. Waiting here is what makes a read at C see every record of the
  // response that published C.
  // RequireLease::kFalse deliberately. The lease bound exists so that a deposed leader cannot
  // answer a read whose writes a new leader has already superseded, and the copy's group has one
  // voter, so no other peer can ever hold that lease. Requiring it only made the copy refuse reads
  // for the remainder of leader_lease_duration_ms after its own election, which is every restart.
  // What the wait still provides is unchanged: the in-flight local operations that carry master's
  // records must have drained before a read at C can see them.
  const auto wait_start = MonoTime::Now();
  auto safe_time = tablet->SafeTime(tablet::RequireLease::kFalse, read_time.read, deadline);
  const auto waited = MonoTime::Now() - wait_start;
  // A freshly elected leader cannot answer a read until the previous term's leader lease can no
  // longer be held, so this is where a read pays for the copy's own election.
  if (waited > 1ms) {
    VLOG(1) << "Local catalog read at " << LocalCatalogHybridTimeForLog(read_time.read)
            << " waited " << waited
            << " for the copy's tablet safe time, leader status "
            << AsString(tablet_peer->LeaderStatus());
  } else {
    VLOG(3) << "Local catalog read at " << LocalCatalogHybridTimeForLog(read_time.read)
            << " did not wait for the tablet safe time";
  }
  RETURN_NOT_OK(safe_time);

  // The lease argument is unused when an explicit read time is supplied, which it always is here;
  // what this call does for us is register the read against the tablet's retention policy, which
  // rejects a read below the copy's history cutoff.
  return tablet::ScopedReadOperation::Create(
      tablet.get(), tablet::RequireLease::kFalse, read_time);
}

Status ExecuteLocalCatalogReads(
    const tablet::TabletPeerPtr& tablet_peer, const tablet::ScopedReadOperation& read_op,
    CoarseTimePoint deadline, const PgPerformRequestMsg& req, rpc::Sidecars& sidecars,
    PgPerformResponseMsg* resp) {
  auto tablet = VERIFY_RESULT(tablet_peer->shared_tablet());

  auto& arena = resp->arena();
  // Catalog reads under a catalog snapshot are not part of any distributed transaction, so there
  // is no transaction or subtransaction metadata to apply.
  auto* empty_txn = arena.NewArenaObject<TransactionMetadataMsg>();
  auto* empty_subtxn = arena.NewArenaObject<SubTransactionMetadataMsg>();

  docdb::ReadOperationData read_operation_data = {
      .deadline = deadline,
      .read_time = read_op.read_time(),
  };

  auto& responses = *resp->mutable_responses();
  for (const auto& op : req.ops()) {
    tablet::PgsqlReadRequestResult result(arena, &sidecars.Start());
    RETURN_NOT_OK(tablet->HandlePgsqlReadRequest(
        read_operation_data, /* is_explicit_request_read_time= */ true, op.read(), *empty_txn,
        *empty_subtxn, &result));
    // A single read time leaves no uncertainty window, so the read cannot ask to be restarted.
    SCHECK(
        !result.read_restart_data.is_valid(), IllegalState,
        "A local catalog read at a single hybrid time asked for a read restart");
    result.response->set_rows_data_sidecar(narrow_cast<int32_t>(sidecars.Complete()));
    if (result.response->has_paging_state()) {
      // Later pages of this scan must run at the same time as this page, whether they are served
      // locally or, if serving stops, by master.
      read_operation_data.read_time.ToPB(result.response->mutable_paging_state()->
                                             mutable_read_time());
    }
    responses.push_back_ref(result.response);
  }
  return Status::OK();
}

}  // namespace yb::tserver
