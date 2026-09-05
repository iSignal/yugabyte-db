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

#include "yb/cdc/sys_catalog_change_service.h"

#include "yb/cdc/cdc_error.h"
#include "yb/cdc/cdc_producer.h"
#include "yb/cdc/xrepl_stream_metadata.h"

#include "yb/consensus/replicate_msgs_holder.h"

#include "yb/master/sys_catalog_constants.h"

#include "yb/rpc/rpc_context.h"

#include "yb/tablet/tablet_peer.h"

#include "yb/util/flags.h"
#include "yb/util/logging.h"
#include "yb/util/metrics.h"
#include "yb/util/status_format.h"

using namespace std::literals;

DEFINE_RUNTIME_uint32(sys_catalog_change_stream_idle_timeout_sec, 3600,
    "How long master keeps the apply-safe-time state of a tserver's system catalog change stream "
    "after that tserver last polled. A tserver that is removed from the cluster would otherwise "
    "leave its state behind for the life of the master process.");

DEFINE_RUNTIME_uint32(sys_catalog_change_rpc_timeout_ms, 30000,
    "Deadline, in milliseconds, that master applies to the work of one system catalog change "
    "request when the caller did not supply one.");

namespace yb::cdc {

SysCatalogChangeServiceImpl::SysCatalogChangeServiceImpl(
    const scoped_refptr<MetricEntity>& metric_entity, GetTabletPeerFunc get_tablet_peer,
    const MemTrackerPtr& parent_mem_tracker)
    : SysCatalogChangeServiceIf(metric_entity),
      get_tablet_peer_(std::move(get_tablet_peer)),
      mem_tracker_(MemTracker::FindOrCreateTracker("SysCatalogChangeService", parent_mem_tracker)) {
}

SysCatalogChangeServiceImpl::~SysCatalogChangeServiceImpl() = default;

std::shared_ptr<StreamMetadata> SysCatalogChangeServiceImpl::GetOrCreateStream(
    const std::string& requestor_uuid) {
  const auto now = CoarseMonoClock::Now();
  std::lock_guard lock(mutex_);

  const auto idle_timeout = FLAGS_sys_catalog_change_stream_idle_timeout_sec * 1s;
  for (auto it = streams_.begin(); it != streams_.end();) {
    if (it->first != requestor_uuid && now - it->second.last_used > idle_timeout) {
      LOG(INFO) << "Dropping the system catalog change stream of tserver " << it->first
                << ", which has not polled for " << MonoDelta(now - it->second.last_used);
      it = streams_.erase(it);
    } else {
      ++it;
    }
  }

  auto it = streams_.find(requestor_uuid);
  if (it != streams_.end()) {
    it->second.last_used = now;
    return it->second.stream;
  }
  auto stream = std::make_shared<StreamMetadata>();
  stream->InitForSysCatalogChangeStream(xrepl::StreamId::GenerateRandom());
  LOG(INFO) << "Created system catalog change stream " << stream->GetStreamId()
            << " for tserver " << requestor_uuid;
  return streams_.emplace(requestor_uuid, StreamEntry{std::move(stream), now}).first->second.stream;
}

namespace {

// The requestor name master uses for its own stream, which cannot collide with a tserver uuid.
const char* const kReleasePushRequestor = "master-release-push";

}  // namespace

Status SysCatalogChangeServiceImpl::GetChangesForRelease(
    const OpId& from_op_id, GetSysCatalogChangesResponsePB* resp, CoarseTimePoint deadline) {
  GetSysCatalogChangesRequestPB req;
  req.set_requestor_uuid(kReleasePushRequestor);
  if (!from_op_id.empty()) {
    from_op_id.ToPB(req.mutable_from_checkpoint()->mutable_op_id());
  }
  return DoGetSysCatalogChanges(req, resp, deadline, /* force_apply_safe_time= */ true);
}

Status SysCatalogChangeServiceImpl::DoGetSysCatalogChanges(
    const GetSysCatalogChangesRequestPB& req, GetSysCatalogChangesResponsePB* resp,
    CoarseTimePoint deadline, bool force_apply_safe_time) {
  SCHECK(
      !req.requestor_uuid().empty(), InvalidArgument,
      "System catalog change request must carry the requesting tserver's uuid");

  auto tablet_peer = VERIFY_RESULT(get_tablet_peer_());
  SCHECK(tablet_peer, NotFound, "System catalog tablet not found");

  auto stream = GetOrCreateStream(req.requestor_uuid());
  const auto stream_id = stream->GetStreamId();

  auto from_op_id =
      req.has_from_checkpoint() ? OpId::FromPB(req.from_checkpoint().op_id()) : OpId();

  int64_t last_readable_index = 0;
  consensus::ReplicateMsgsHolder msgs_holder;
  consensus::HaveMoreMessages have_more_messages{false};

  XClusterGetChangesContext context = {
      .stream_id = stream_id,
      .tablet_id = master::kSysCatalogTabletId,
      .from_op_id = from_op_id,
      .tablet_peer = tablet_peer,
      // The system catalog tablet never splits, so a SPLIT_OP can only be a corrupt or
      // misdirected record; refusing it fails the request rather than shipping it.
      .update_on_split_op_func =
          [](const consensus::ReplicateMsg&) -> Status {
            return STATUS(
                IllegalState, "System catalog tablet cannot be split; refusing to ship a SPLIT_OP");
          },
      .mem_tracker = mem_tracker_,
      .deadline = deadline,
      .stream_metadata = stream.get(),
      .msgs_holder = &msgs_holder,
      .resp = resp->mutable_changes(),
      .have_more_messages = &have_more_messages,
      .last_readable_opid_index = &last_readable_index,
      .force_apply_safe_time = force_apply_safe_time,
  };

  RETURN_NOT_OK(GetChangesForXCluster(context));
  resp->set_have_more_messages(have_more_messages == consensus::HaveMoreMessages::kTrue);
  return Status::OK();
}

void SysCatalogChangeServiceImpl::GetSysCatalogChanges(
    const GetSysCatalogChangesRequestPB* req, GetSysCatalogChangesResponsePB* resp,
    rpc::RpcContext context) {
  auto deadline = context.GetClientDeadline();
  if (deadline == CoarseTimePoint::max()) {
    deadline = CoarseMonoClock::Now() +
               MonoDelta::FromMilliseconds(FLAGS_sys_catalog_change_rpc_timeout_ms);
  }
  auto status = DoGetSysCatalogChanges(*req, resp, deadline);
  if (!status.ok()) {
    resp->Clear();
    StatusToPB(status, resp->mutable_error()->mutable_status());
    resp->mutable_error()->set_code(
        CDCError::ValueFromStatus(status).value_or(CDCErrorPB::UNKNOWN_ERROR));
  }
  context.RespondSuccess();
}

}  // namespace yb::cdc
