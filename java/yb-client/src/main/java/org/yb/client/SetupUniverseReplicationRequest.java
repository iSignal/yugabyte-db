// Copyright (c) YugaByte, Inc.
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
package org.yb.client;

import com.google.protobuf.Message;
import java.util.List;
import java.util.Set;
import org.jboss.netty.buffer.ChannelBuffer;
import org.yb.CommonNet;
import org.yb.CommonNet.HostPortPB;
import org.yb.master.MasterReplicationOuterClass;
import org.yb.master.MasterTypes;
import org.yb.util.Pair;

public class SetupUniverseReplicationRequest extends YRpc<SetupUniverseReplicationResponse> {

  private final String replicationGroupName;
  private final Set<CommonNet.HostPortPB> sourceMasterAddresses;
  // List is used because order of table ids matters.
  private final List<String> sourceTableIds;
  // An optional list of bootstrap ids. Each id corresponds to a table in the sourceTableIds list.
  private final List<String> sourceBootstrapIds;

  SetupUniverseReplicationRequest(
    YBTable table,
    String replicationGroupName,
    List<String> sourceTableIds,
    Set<HostPortPB> sourceMasterAddresses,
    List<String> sourceBootstrapIds) {
    super(table);
    this.replicationGroupName = replicationGroupName;
    this.sourceTableIds = sourceTableIds;
    this.sourceMasterAddresses = sourceMasterAddresses;
    this.sourceBootstrapIds = sourceBootstrapIds;
  }

  @Override
  ChannelBuffer serialize(Message header) {
    assert header.isInitialized();

    final MasterReplicationOuterClass.SetupUniverseReplicationRequestPB.Builder builder =
      MasterReplicationOuterClass.SetupUniverseReplicationRequestPB.newBuilder()
        .setProducerId(replicationGroupName)
        .addAllProducerTableIds(sourceTableIds)
        .addAllProducerMasterAddresses(sourceMasterAddresses);
    if (sourceBootstrapIds != null) {
      builder.addAllProducerBootstrapIds(sourceBootstrapIds);
    }

    return toChannelBuffer(header, builder.build());
  }

  @Override
  String serviceName() {
    return MASTER_SERVICE_NAME;
  }

  @Override
  String method() {
    return "SetupUniverseReplication";
  }

  @Override
  Pair<SetupUniverseReplicationResponse, Object> deserialize(
    CallResponse callResponse, String tsUUID) throws Exception {
    final MasterReplicationOuterClass.SetupUniverseReplicationResponsePB.Builder builder =
      MasterReplicationOuterClass.SetupUniverseReplicationResponsePB.newBuilder();

    readProtobuf(callResponse.getPBMessage(), builder);

    final MasterTypes.MasterErrorPB error = builder.hasError() ? builder.getError() : null;

    SetupUniverseReplicationResponse response =
      new SetupUniverseReplicationResponse(deadlineTracker.getElapsedMillis(), tsUUID, error);

    return new Pair<>(response, error);
  }
}
