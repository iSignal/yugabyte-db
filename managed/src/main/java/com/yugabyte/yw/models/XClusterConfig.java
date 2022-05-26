// Copyright (c) YugaByte, Inc.
package com.yugabyte.yw.models;

import static play.mvc.Http.Status.BAD_REQUEST;

import com.fasterxml.jackson.annotation.JsonFormat;
import com.fasterxml.jackson.annotation.JsonIgnore;
import com.fasterxml.jackson.annotation.JsonProperty;
import com.google.common.annotations.VisibleForTesting;
import com.yugabyte.yw.common.PlatformServiceException;
import com.yugabyte.yw.forms.XClusterConfigCreateFormData;
import io.ebean.Finder;
import io.ebean.Model;
import io.ebean.annotation.DbEnumValue;
import io.ebean.annotation.Transactional;
import io.swagger.annotations.ApiModel;
import io.swagger.annotations.ApiModelProperty;
import java.util.Collection;
import java.util.Date;
import java.util.HashSet;
import java.util.List;
import java.util.Optional;
import java.util.Set;
import java.util.UUID;
import java.util.stream.Collectors;
import javax.persistence.CascadeType;
import javax.persistence.Column;
import javax.persistence.Entity;
import javax.persistence.Id;
import javax.persistence.JoinColumn;
import javax.persistence.ManyToOne;
import javax.persistence.OneToMany;
import lombok.extern.slf4j.Slf4j;

@Slf4j
@Entity
@ApiModel(description = "xcluster config object")
public class XClusterConfig extends Model {

  private static final Finder<UUID, XClusterConfig> find =
      new Finder<UUID, XClusterConfig>(XClusterConfig.class) {};

  @Id
  @ApiModelProperty(value = "XCluster config UUID")
  public UUID uuid;

  @Column(name = "config_name")
  @ApiModelProperty(value = "XCluster config name")
  public String name;

  @ManyToOne
  @JoinColumn(name = "source_universe_uuid")
  @ApiModelProperty(value = "Source Universe UUID")
  public UUID sourceUniverseUUID;

  @ManyToOne
  @JoinColumn(name = "target_universe_uuid")
  @ApiModelProperty(value = "Target Universe UUID")
  public UUID targetUniverseUUID;

  @Column(name = "status")
  @ApiModelProperty(
      value = "Status",
      allowableValues = "Init, Bootstrapping, Running, Updating, Paused, Failed")
  public XClusterConfigStatusType status;

  public enum XClusterConfigStatusType {
    Init("Init"),
    Bootstrapping("Bootstrapping"),
    Running("Running"),
    Updating("Updating"),
    Paused("Paused"),
    Deleted("Deleted"),
    Failed("Failed");

    private final String status;

    XClusterConfigStatusType(String status) {
      this.status = status;
    }

    @Override
    @DbEnumValue
    public String toString() {
      return this.status;
    }
  }

  @JsonFormat(shape = JsonFormat.Shape.STRING, pattern = "yyyy-MM-dd HH:mm:ss")
  @ApiModelProperty(
      value = "Create time of the xCluster config",
      example = "2022-04-26 15:37:32.610000")
  public Date createTime;

  @JsonFormat(shape = JsonFormat.Shape.STRING, pattern = "yyyy-MM-dd HH:mm:ss")
  @ApiModelProperty(
      value = "Last modify time of the xCluster config",
      example = "2022-04-26 15:37:32.610000")
  public Date modifyTime;

  @OneToMany(mappedBy = "config", cascade = CascadeType.ALL, orphanRemoval = true)
  @ApiModelProperty(value = "Tables participating in this xCluster config")
  @JsonIgnore
  public Set<XClusterTableConfig> tables;

  public Optional<XClusterTableConfig> maybeGetTableById(String tableId) {
    // There will be at most one tableConfig for a tableId within each xCluster config.
    return this.tables
        .stream()
        .filter(tableConfig -> tableConfig.tableId.equals(tableId))
        .findAny();
  }

  public Set<XClusterTableConfig> getTableDetails() {
    return this.tables;
  }

  @JsonProperty
  public Set<String> getTables() {
    return this.tables.stream().map(table -> table.tableId).collect(Collectors.toSet());
  }

  @JsonIgnore
  public Set<String> getTableIdsWithReplicationSetup() {
    return this.tables
        .stream()
        .filter(table -> table.replicationSetupDone)
        .map(table -> table.tableId)
        .collect(Collectors.toSet());
  }

  public void setTables(Collection<String> tableIds) {
    this.tables = new HashSet<>();
    tableIds.forEach(tableId -> tables.add(new XClusterTableConfig(this, tableId)));
    update();
  }

  @Transactional
  public void setTables(List<String> tableIds, List<Boolean> isBootstrapRequiredPerTableId) {
    if (tableIds.size() != isBootstrapRequiredPerTableId.size()) {
      throw new RuntimeException(
          "tableIds must have the same size as isBootstrapRequiredPerTableId list");
    }
    this.tables = new HashSet<>();
    for (int i = 0; i < tableIds.size(); i++) {
      XClusterTableConfig tableConfig = new XClusterTableConfig(this, tableIds.get(i));
      tableConfig.needBootstrap = isBootstrapRequiredPerTableId.get(i);
      this.tables.add(tableConfig);
    }
    update();
  }

  @Transactional
  public void addTables(List<String> tableIds) {
    if (this.tables == null) {
      this.tables = new HashSet<>();
    }
    for (String tableId : tableIds) {
      if (!this.tables.add(new XClusterTableConfig(this, tableId))) {
        log.debug("Table with id {} already exists in xCluster config ({})", tableId, this.uuid);
      }
    }
    update();
  }

  @Transactional
  public void addTables(List<String> tableIds, List<String> streamIds) {
    if (streamIds == null) {
      addTables(tableIds);
      return;
    }
    if (tableIds.size() != streamIds.size()) {
      throw new RuntimeException("tableIds must have the same size as streamIds list");
    }
    if (this.tables == null) {
      this.tables = new HashSet<>();
    }
    for (int i = 0; i < tableIds.size(); i++) {
      XClusterTableConfig tableConfig = new XClusterTableConfig(this, tableIds.get(i));
      tableConfig.streamId = streamIds.get(i);
      if (!this.tables.add(tableConfig)) {
        log.debug(
            "Table with id {} already exists in xCluster config ({})", tableIds.get(i), this.uuid);
      }
    }
    update();
  }

  @JsonIgnore
  public Set<String> getStreamIdsWithReplicationSetup() {
    return this.tables
        .stream()
        .filter(table -> table.replicationSetupDone)
        .map(table -> table.streamId)
        .collect(Collectors.toSet());
  }

  @Transactional
  public void replicationSetupIsDoneForTables(List<String> tableIds) {
    for (String tableId : tableIds) {
      Optional<XClusterTableConfig> tableConfig = maybeGetTableById(tableId);
      if (tableConfig.isPresent()) {
        tableConfig.get().replicationSetupDone = true;
        log.info("Replication for table {} in xCluster config {} is set up", tableId, name);
      } else {
        String errMsg =
            String.format(
                "Could not find tableId (%s) in the xCluster config with uuid (%s)", tableId, uuid);
        throw new RuntimeException(errMsg);
      }
    }
    update();
  }

  @Transactional
  public void removeTables(List<String> tableIds) {
    if (this.tables == null) {
      log.debug("No tables is set for xCluster config {}", this.uuid);
      return;
    }
    for (String tableId : tableIds) {
      if (!this.tables.removeIf(tableConfig -> tableConfig.tableId.equals(tableId))) {
        log.debug(
            "Table with id {} was not found to delete in xCluster config {}", tableId, this.uuid);
      }
    }
    update();
  }

  @Transactional
  public void setBackupForTables(Collection<String> tableIds, Backup backup) {
    ensureTableIdsExist(tableIds);
    this.tables
        .stream()
        .filter(tableConfig -> tableIds.contains(tableConfig.tableId))
        .forEach(tableConfig -> tableConfig.backup = backup);
    update();
  }

  @Transactional
  public void setRestoreTimeForTables(Collection<String> tableIds, Date restoreTime) {
    ensureTableIdsExist(tableIds);
    this.tables
        .stream()
        .filter(tableConfig -> tableIds.contains(tableConfig.tableId))
        .forEach(tableConfig -> tableConfig.restoreTime = restoreTime);
    update();
  }

  @Transactional
  public void setNeedBootstrapForTables(Collection<String> tableIds, boolean needBootstrap) {
    ensureTableIdsExist(tableIds);
    this.tables
        .stream()
        .filter(tableConfig -> tableIds.contains(tableConfig.tableId))
        .forEach(tableConfig -> tableConfig.needBootstrap = needBootstrap);
    update();
  }

  @Transactional
  public void setBootstrapCreateTimeForTables(Collection<String> tableIds, Date moment) {
    ensureTableIdsExist(tableIds);
    this.tables
        .stream()
        .filter(tableConfig -> tableIds.contains(tableConfig.tableId))
        .forEach(tableConfig -> tableConfig.bootstrapCreateTime = moment);
    update();
  }

  public void ensureTableIdsExist(Collection<String> tableIds) {
    Collection<String> tableIdsInXClusterConfig = getTables();
    tableIds
        .stream()
        .forEach(
            tableId -> {
              if (!tableIdsInXClusterConfig.contains(tableId)) {
                throw new RuntimeException(
                    String.format(
                        "Could not find tableId (%s) in the xCluster config with uuid (%s)",
                        tableId, this.uuid));
              }
            });
  }

  @JsonIgnore
  public String getReplicationGroupName() {
    return this.sourceUniverseUUID + "_" + this.name;
  }

  @Transactional
  public static XClusterConfig create(
      String name,
      UUID sourceUniverseUUID,
      UUID targetUniverseUUID,
      XClusterConfigStatusType status) {
    XClusterConfig xClusterConfig = new XClusterConfig();
    xClusterConfig.uuid = UUID.randomUUID();
    xClusterConfig.name = name;
    xClusterConfig.sourceUniverseUUID = sourceUniverseUUID;
    xClusterConfig.targetUniverseUUID = targetUniverseUUID;
    xClusterConfig.status = status;
    xClusterConfig.createTime = new Date();
    xClusterConfig.modifyTime = new Date();
    xClusterConfig.save();
    return xClusterConfig;
  }

  @Transactional
  public static XClusterConfig create(
      String name, UUID sourceUniverseUUID, UUID targetUniverseUUID) {
    return create(name, sourceUniverseUUID, targetUniverseUUID, XClusterConfigStatusType.Init);
  }

  @Transactional
  public static XClusterConfig create(XClusterConfigCreateFormData createFormData) {
    XClusterConfig xClusterConfig =
        create(
            createFormData.name,
            createFormData.sourceUniverseUUID,
            createFormData.targetUniverseUUID);
    if (createFormData.bootstrapParams != null) {
      xClusterConfig.setTables(
          createFormData.tables, createFormData.bootstrapParams.isBootstrapRequiredPerTable);
    } else {
      xClusterConfig.setTables(createFormData.tables);
    }
    return xClusterConfig;
  }

  @VisibleForTesting
  @Transactional
  public static XClusterConfig create(
      XClusterConfigCreateFormData createFormData, XClusterConfigStatusType status) {
    XClusterConfig xClusterConfig =
        create(
            createFormData.name,
            createFormData.sourceUniverseUUID,
            createFormData.targetUniverseUUID,
            status);
    if (createFormData.bootstrapParams != null) {
      xClusterConfig.setTables(
          createFormData.tables, createFormData.bootstrapParams.isBootstrapRequiredPerTable);
    } else {
      xClusterConfig.setTables(createFormData.tables);
    }
    if (status == XClusterConfigStatusType.Running) {
      xClusterConfig.replicationSetupIsDoneForTables(createFormData.tables);
    }
    return xClusterConfig;
  }

  @Override
  public void update() {
    this.modifyTime = new Date();
    super.update();
  }

  public static XClusterConfig getValidConfigOrBadRequest(
      Customer customer, UUID xClusterConfigUUID) {
    XClusterConfig xClusterConfig = getOrBadRequest(xClusterConfigUUID);
    checkXClusterConfigInCustomer(xClusterConfig, customer);
    return xClusterConfig;
  }

  public static XClusterConfig getOrBadRequest(UUID xClusterConfigUUID) {
    return maybeGet(xClusterConfigUUID)
        .orElseThrow(
            () ->
                new PlatformServiceException(
                    BAD_REQUEST, "Cannot find XClusterConfig " + xClusterConfigUUID));
  }

  public static Optional<XClusterConfig> maybeGet(UUID xClusterConfigUUID) {
    XClusterConfig xClusterConfig =
        find.query().fetch("tables").where().eq("uuid", xClusterConfigUUID).findOne();
    if (xClusterConfig == null) {
      log.info("Cannot find XClusterConfig {}", xClusterConfigUUID);
      return Optional.empty();
    }
    return Optional.of(xClusterConfig);
  }

  public static List<XClusterConfig> getByTargetUniverseUUID(UUID targetUniverseUUID) {
    return find.query()
        .fetch("tables")
        .where()
        .eq("target_universe_uuid", targetUniverseUUID)
        .findList();
  }

  public static List<XClusterConfig> getBySourceUniverseUUID(UUID sourceUniverseUUID) {
    return find.query()
        .fetch("tables")
        .where()
        .eq("source_universe_uuid", sourceUniverseUUID)
        .findList();
  }

  public static List<XClusterConfig> getBetweenUniverses(
      UUID sourceUniverseUUID, UUID targetUniverseUUID) {
    return find.query()
        .fetch("tables")
        .where()
        .eq("source_universe_uuid", sourceUniverseUUID)
        .eq("target_universe_uuid", targetUniverseUUID)
        .findList();
  }

  public static XClusterConfig getByNameSourceTarget(
      String name, UUID sourceUniverseUUID, UUID targetUniverseUUID) {
    return find.query()
        .fetch("tables")
        .where()
        .eq("config_name", name)
        .eq("source_universe_uuid", sourceUniverseUUID)
        .eq("target_universe_uuid", targetUniverseUUID)
        .findOne();
  }

  private static void checkXClusterConfigInCustomer(
      XClusterConfig xClusterConfig, Customer customer) {
    if (!customer.getUniverseUUIDs().contains(xClusterConfig.sourceUniverseUUID)
        || !customer.getUniverseUUIDs().contains(xClusterConfig.targetUniverseUUID)) {
      throw new PlatformServiceException(
          BAD_REQUEST,
          String.format(
              "XClusterConfig %s doesn't belong to Customer %s",
              xClusterConfig.uuid, customer.uuid));
    }
  }
}
