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

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "yb/cdc/cdc_service.service.h"

#include "yb/common/opid.h"

#include "yb/tablet/tablet_fwd.h"

#include "yb/util/mem_tracker.h"
#include "yb/util/monotime.h"
#include "yb/util/status_fwd.h"

namespace yb::cdc {

class StreamMetadata;

// Master-side service that ships the master system catalog tablet's WAL to the tserver-local
// copies of it.
//
// One record stream exists per requesting tserver, keyed on the tserver's permanent uuid and held
// only in memory. The per-requestor state is what makes the reported safe time trustworthy: the
// producer reports a safe time S only after every WAL record up to the position it captured with S
// has been included in a response to that requestor, so a requestor that has applied every record
// of a response holds every catalog row committed at or below S.
class SysCatalogChangeServiceImpl : public SysCatalogChangeServiceIf {
 public:
  using GetTabletPeerFunc = std::function<Result<tablet::TabletPeerPtr>()>;

  SysCatalogChangeServiceImpl(
      const scoped_refptr<MetricEntity>& metric_entity, GetTabletPeerFunc get_tablet_peer,
      const MemTrackerPtr& parent_mem_tracker);
  ~SysCatalogChangeServiceImpl();

  void GetSysCatalogChanges(
      const GetSysCatalogChangesRequestPB* req, GetSysCatalogChangesResponsePB* resp,
      rpc::RpcContext context) override;

  // Fetches one batch for master's own use, without going through the RPC layer, for the change
  // records a lock release carries. Uses a stream of its own, so it neither disturbs nor is
  // disturbed by the tservers' streams, and demands a safe time computed on this call: the caller
  // is releasing the locks of a transaction that has just committed, and a safe time from before
  // that commit would not let any tserver publish the new versions.
  Status GetChangesForRelease(
      const OpId& from_op_id, GetSysCatalogChangesResponsePB* resp, CoarseTimePoint deadline);

 private:
  Status DoGetSysCatalogChanges(
      const GetSysCatalogChangesRequestPB& req, GetSysCatalogChangesResponsePB* resp,
      CoarseTimePoint deadline, bool force_apply_safe_time = false);

  // Returns the requestor's stream, creating it on first use, and drops the streams of tservers
  // that have not polled for sys_catalog_change_stream_idle_timeout_sec. Without that, a
  // decommissioned tserver's apply-safe-time state would stay on master for the life of the
  // process.
  std::shared_ptr<StreamMetadata> GetOrCreateStream(const std::string& requestor_uuid);

  const GetTabletPeerFunc get_tablet_peer_;
  MemTrackerPtr mem_tracker_;

  struct StreamEntry {
    std::shared_ptr<StreamMetadata> stream;
    CoarseTimePoint last_used;
  };

  std::mutex mutex_;
  std::unordered_map<std::string, StreamEntry> streams_;
};

}  // namespace yb::cdc
