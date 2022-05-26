// Copyright (c) YugaByte, Inc.
package com.yugabyte.yw.commissioner.tasks.subtasks.xcluster;

import com.yugabyte.yw.commissioner.BaseTaskDependencies;
import com.yugabyte.yw.commissioner.tasks.XClusterConfigTaskBase;
import com.yugabyte.yw.forms.XClusterConfigTaskParams;
import com.yugabyte.yw.models.HighAvailabilityConfig;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.XClusterConfig;
import com.yugabyte.yw.models.XClusterTableConfig;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import java.util.Optional;
import javax.inject.Inject;
import lombok.extern.slf4j.Slf4j;
import org.yb.client.AlterUniverseReplicationResponse;
import org.yb.client.GetMasterClusterConfigResponse;
import org.yb.client.YBClient;

@Slf4j
public class XClusterConfigModifyTables extends XClusterConfigTaskBase {

  @Inject
  protected XClusterConfigModifyTables(BaseTaskDependencies baseTaskDependencies) {
    super(baseTaskDependencies);
  }

  public static class Params extends XClusterConfigTaskParams {
    // The target universe UUID must be stored in universeUUID field.
    // The parent xCluster config must be stored in xClusterConfig field.
    // Table ids to add to the replication.
    public List<String> tableIdsToAdd;
    // Table ids to remove from the replication.
    public List<String> tableIdsToRemove;
  }

  @Override
  protected Params taskParams() {
    return (Params) taskParams;
  }

  @Override
  public void run() {
    log.info("Running {}", getName());

    // Each modify tables task must belong to a parent xCluster config.
    XClusterConfig xClusterConfig = taskParams().xClusterConfig;
    if (xClusterConfig == null) {
      throw new RuntimeException(
          "taskParams().xClusterConfig is null. Each modify tables subtask must belong to an "
              + "xCluster config");
    }

    List<String> tableIdsToAdd =
        taskParams().tableIdsToAdd == null ? new ArrayList<>() : taskParams().tableIdsToAdd;
    List<String> BootstrapIdstoAdd = new ArrayList<>();
    List<String> tableIdsToRemove =
        taskParams().tableIdsToRemove == null ? new ArrayList<>() : taskParams().tableIdsToRemove;

    // Ensure each tableId exits in the xCluster config and replication is not set up for it. Also,
    // get the bootstrapIds if there is any.
    for (String tableId : tableIdsToAdd) {
      Optional<XClusterTableConfig> tableConfig = xClusterConfig.maybeGetTableById(tableId);
      if (!tableConfig.isPresent()) {
        String errMsg =
            String.format(
                "Table with id (%s) already is part of xCluster config (%s) and cannot be "
                    + "added again",
                tableId, xClusterConfig.uuid);
        throw new IllegalArgumentException(errMsg);
      }
      if (tableConfig.get().replicationSetupDone) {
        String errMsg =
            String.format(
                "Replication is already set up for table with id (%s) and cannot be set up again",
                tableId);
        throw new IllegalArgumentException(errMsg);
      }
      BootstrapIdstoAdd.add(tableConfig.get().streamId);
    }
    // No table was bootstrapped, or tableIdsToAdd is empty.
    if (BootstrapIdstoAdd.stream().allMatch(Objects::isNull)) {
      BootstrapIdstoAdd = null;
    }
    // Either all tables should need bootstrap, or none should.
    if (BootstrapIdstoAdd != null && BootstrapIdstoAdd.contains(null)) {
      throw new IllegalArgumentException(
          String.format(
              "Failed to create XClusterConfig(%s) because some tables went through "
                  + "bootstrap and some did not, You must create XClusterConfigSetup subtask "
                  + "separately for them",
              xClusterConfig.uuid));
    }

    // Ensure each tableId exits in the xCluster config and replication is set up for it.
    for (String tableId : tableIdsToRemove) {
      Optional<XClusterTableConfig> tableConfig = xClusterConfig.maybeGetTableById(tableId);
      if (!tableConfig.isPresent()) {
        String errMsg =
            String.format(
                "Table with id (%s) is not part of xCluster config (%s) and cannot be removed",
                tableId, xClusterConfig.uuid);
        throw new IllegalArgumentException(errMsg);
      }
      if (!tableConfig.get().replicationSetupDone) {
        String errMsg =
            String.format(
                "Replication is NOT set up for table with id (%s) and thus cannot stop "
                    + "replication for it",
                tableId);
        throw new IllegalArgumentException(errMsg);
      }
    }

    Universe targetUniverse = Universe.getOrBadRequest(xClusterConfig.targetUniverseUUID);
    String targetUniverseMasterAddresses = targetUniverse.getMasterAddresses();
    String targetUniverseCertificate = targetUniverse.getCertificateNodetoNode();
    YBClient client = ybService.getClient(targetUniverseMasterAddresses, targetUniverseCertificate);
    try {
      log.info("Modifying tables in XClusterConfig({})", xClusterConfig.uuid);

      if (tableIdsToAdd.size() > 0) {
        log.info(
            "Adding tables to XClusterConfig({}): {} with bootstrapIds {}",
            xClusterConfig.uuid,
            tableIdsToAdd,
            BootstrapIdstoAdd);
        AlterUniverseReplicationResponse resp =
            client.alterUniverseReplicationAddTables(
                xClusterConfig.getReplicationGroupName(), tableIdsToAdd, BootstrapIdstoAdd);
        if (resp.hasError()) {
          String errMsg =
              String.format(
                  "Failed to add tables to XClusterConfig(%s): %s",
                  xClusterConfig.uuid, resp.errorMessage());
          throw new RuntimeException(errMsg);
        }
        waitForXClusterOperation(client::isAlterUniverseReplicationDone);

        // Persist that replicationSetupDone is true for the tables in taskParams. We have checked
        // that taskParams().tableIdsToAdd exist in the xCluster config, so it will not throw an
        // exception.
        xClusterConfig.replicationSetupIsDoneForTables(taskParams().tableIdsToAdd);

        // Get the stream ids from the target universe and put it in the Platform DB for the
        // added tables to the xCluster config.
        GetMasterClusterConfigResponse clusterConfigResp = client.getMasterClusterConfig();
        if (clusterConfigResp.hasError()) {
          String errMsg =
              String.format(
                  "Failed to getMasterClusterConfig from target universe (%s) for xCluster config "
                      + "(%s): %s",
                  targetUniverse.universeUUID,
                  xClusterConfig.uuid,
                  clusterConfigResp.errorMessage());
          throw new RuntimeException(errMsg);
        }
        updateStreamIdsFromTargetUniverseClusterConfig(
            clusterConfigResp.getConfig(), xClusterConfig, taskParams().tableIdsToAdd);

        if (HighAvailabilityConfig.get().isPresent()) {
          // Note: We increment version twice for adding tables: once for setting up the .ALTER
          // replication group, and once for merging the .ALTER replication group
          getUniverse(true).incrementVersion();
          getUniverse(true).incrementVersion();
        }
      }

      if (tableIdsToRemove.size() > 0) {
        log.info(
            "Removing tables from XClusterConfig({}): {}", xClusterConfig.uuid, tableIdsToRemove);
        AlterUniverseReplicationResponse resp =
            client.alterUniverseReplicationRemoveTables(
                xClusterConfig.getReplicationGroupName(), tableIdsToRemove);
        if (resp.hasError()) {
          String errMsg =
              String.format(
                  "Failed to remove tables from XClusterConfig(%s): %s",
                  xClusterConfig.uuid, resp.errorMessage());
          throw new RuntimeException(errMsg);
        }
        xClusterConfig.removeTables(tableIdsToRemove);

        if (HighAvailabilityConfig.get().isPresent()) {
          getUniverse(true).incrementVersion();
        }
      }
    } catch (Exception e) {
      log.error("{} hit error : {}", getName(), e.getMessage());
      throw new RuntimeException(e);
    } finally {
      ybService.closeClient(client, targetUniverseMasterAddresses);
    }

    log.info("Completed {}", getName());
  }
}
