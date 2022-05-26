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

public class AlterUniverseReplicationRequest extends YRpc<AlterUniverseReplicationResponse> {

  private final String replicationGroupName;
  private final List<String> sourceTableIdsToAdd;
  private final List<String> sourceBootstrapIdstoAdd;
  private final List<String> sourceTableIdsToRemove;
  private final Set<HostPortPB> sourceMasterAddresses;
  private final String newReplicationGroupName;

  AlterUniverseReplicationRequest(
    YBTable table,
    String replicationGroupName,
    List<String> sourceTableIdsToAdd,
    List<String> sourceBootstrapIdstoAdd,
    List<String> sourceTableIdsToRemove,
    Set<CommonNet.HostPortPB> sourceMasterAddresses,
    String newReplicationGroupName) {
    super(table);
    this.replicationGroupName = replicationGroupName;
    this.sourceTableIdsToAdd = sourceTableIdsToAdd;
    this.sourceBootstrapIdstoAdd = sourceBootstrapIdstoAdd;
    this.sourceTableIdsToRemove = sourceTableIdsToRemove;
    this.sourceMasterAddresses = sourceMasterAddresses;
    this.newReplicationGroupName = newReplicationGroupName;
  }

  @Override
  ChannelBuffer serialize(Message header) {
    assert header.isInitialized();

    final MasterReplicationOuterClass.AlterUniverseReplicationRequestPB.Builder builder =
      MasterReplicationOuterClass.AlterUniverseReplicationRequestPB.newBuilder()
        .setProducerId(replicationGroupName)
        .addAllProducerMasterAddresses(sourceMasterAddresses)
        .addAllProducerTableIdsToAdd(sourceTableIdsToAdd)
        .addAllProducerTableIdsToRemove(sourceTableIdsToRemove);
    if (newReplicationGroupName != null) {
      builder.setNewProducerUniverseId(newReplicationGroupName);
    }
    if (sourceBootstrapIdstoAdd != null) {
      builder.addAllProducerBootstrapIdsToAdd(sourceBootstrapIdstoAdd);
    }

    return toChannelBuffer(header, builder.build());
  }

  @Override
  String serviceName() {
    return MASTER_SERVICE_NAME;
  }

  @Override
  String method() {
    return "AlterUniverseReplication";
  }

  @Override
  Pair<AlterUniverseReplicationResponse, Object> deserialize(
    CallResponse callResponse, String tsUUID) throws Exception {
    final MasterReplicationOuterClass.AlterUniverseReplicationResponsePB.Builder builder =
      MasterReplicationOuterClass.AlterUniverseReplicationResponsePB.newBuilder();

    readProtobuf(callResponse.getPBMessage(), builder);

    final MasterTypes.MasterErrorPB error = builder.hasError() ? builder.getError() : null;

    AlterUniverseReplicationResponse response =
      new AlterUniverseReplicationResponse(deadlineTracker.getElapsedMillis(),
        tsUUID, error);

    return new Pair<>(response, error);
  }
}
