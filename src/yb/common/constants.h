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
//

#pragma once

#include "yb/common/common_fwd.h"

#include "yb/util/enums.h"

namespace yb {

// As a result of single dynamic tablet splitting operation we always have kDefaultNumSplitParts new
// post-split tablets.
constexpr const auto kDefaultNumSplitParts = 2;

constexpr const ColocationId kColocationIdNotSet = 0;

// Minimum colocation ID to be auto-generated, IDs below are reserved just in case we want
// some special values in the future.
// This has been chosen to match FirstNormalObjectId from Postgres code.
constexpr const ColocationId kFirstNormalColocationId = 16384;

YB_DEFINE_ENUM(SortingType,
               (kNotSpecified)
               (kAscending)           // ASC, NULLS FIRST
               (kDescending)          // DESC, NULLS FIRST
               (kAscendingNullsLast)  // ASC, NULLS LAST
               (kDescendingNullsLast) // DESC, NULLS LAST
);

static const char* const kObsoleteShortPrimaryTableId = "sys.catalog.uuid";

// The tablet id under which a tserver keeps its private copy of the master system catalog tablet.
// It is deliberately different from master::kSysCatalogTabletId so that nothing on the tserver can
// resolve a client request for master's tablet to the copy. Declared here rather than in
// yb/tserver because the tablet layer, which cannot link against yb_tserver, also has to
// recognize the copy.
static const char* const kLocalCatalogTabletId = "00000000000000000000000000000001";

constexpr auto kPitrFeatureName = "PITR";
constexpr auto kXClusterFailoverFeatureName = "xCluster failover";

constexpr auto kCDCSDKSlotEntryTabletId = "dummy_id_for_replication_slot";

// Plugin name marking a replication slot as a gRPC CDC stream. Must match YB_GRPC_STREAM_INDICATOR
// in src/postgres/src/backend/replication/slot.c.
constexpr auto kYbGrpcStreamIndicator = "yb_grpc";

// Must match YB_OUTPUT_PLUGIN in src/postgres/src/backend/replication/slot.c.
constexpr auto kYbOutputPluginName = "yboutput";

// Reserved slot-name prefix for the internal LISTEN/NOTIFY notifications replication slots. Must
// match YbNotificationsSlotPrefix in src/postgres/src/backend/utils/init/globals.c.
constexpr auto kYbNotificationsSlotPrefix = "yb_notifications_";

} // namespace yb
