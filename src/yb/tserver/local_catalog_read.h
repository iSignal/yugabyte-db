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

#include "yb/common/read_hybrid_time.h"

#include "yb/rpc/rpc_fwd.h"

#include "yb/tablet/tablet_fwd.h"
#include "yb/tablet/tablet.h"

#include "yb/tserver/tserver_fwd.h"

#include "yb/util/monotime.h"
#include "yb/util/status_fwd.h"

namespace yb::tserver {

// Whether every operation of req is a read of a table that the tserver-local copy of the master
// system catalog tablet holds. A request with a write, or with a read of any other table, cannot
// be served from the copy and goes to master whole.
bool AllOpsAreReadsOfLocalCatalogTables(
    const tablet::TabletPeerPtr& tablet_peer, const PgPerformRequestMsg& req);

// Waits for the local copy's safe time to reach read_time and registers read_time with the copy's
// history retention policy, returning the registration. Fails with SnapshotTooOld when the copy no
// longer keeps history that far back, which is the caller's signal to route the read to master.
Result<tablet::ScopedReadOperation> PrepareLocalCatalogRead(
    const tablet::TabletPeerPtr& tablet_peer, const ReadHybridTime& read_time,
    CoarseTimePoint deadline);

// Runs every read operation of req against the local copy at exactly the time held by read_op,
// appending one response per operation to resp and each operation's rows to sidecars.
//
// That time is a single hybrid time: read time, local limit and global limit are all equal, so no
// read restart is possible. Nothing on the copy can appear at or below it after the fact -- the
// poller publishes C only once every record of the response that certified it is applied -- so a
// restart would have nothing to correct.
Status ExecuteLocalCatalogReads(
    const tablet::TabletPeerPtr& tablet_peer, const tablet::ScopedReadOperation& read_op,
    CoarseTimePoint deadline, const PgPerformRequestMsg& req, rpc::Sidecars& sidecars,
    PgPerformResponseMsg* resp);

}  // namespace yb::tserver
