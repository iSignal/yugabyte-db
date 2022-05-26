package com.yugabyte.yw.forms;

import com.yugabyte.yw.common.PlatformServiceException;
import io.swagger.annotations.ApiModel;
import io.swagger.annotations.ApiModelProperty;
import java.util.HashSet;
import java.util.List;
import java.util.UUID;
import javax.validation.Valid;
import play.data.validation.Constraints;
import play.data.validation.Constraints.MaxLength;
import play.data.validation.Constraints.Required;
import play.mvc.Http;

@ApiModel(description = "xcluster create form")
public class XClusterConfigCreateFormData {

  @Required
  @MaxLength(256)
  @ApiModelProperty(value = "Name", example = "Repl-config1", required = true)
  public String name;

  @Required
  @ApiModelProperty(value = "Source Universe UUID", required = true)
  public UUID sourceUniverseUUID;

  @Required
  @ApiModelProperty(value = "Target Universe UUID", required = true)
  public UUID targetUniverseUUID;

  @Required
  @ApiModelProperty(
      value = "Source Universe table IDs",
      example = "[000033df000030008000000000004006, 000033df00003000800000000000400b]",
      required = true)
  public List<String> tables;

  @Valid
  @ApiModelProperty("Parameters needed for the bootstrap flow including backup/restore")
  public BootstrapParams bootstrapParams;

  @ApiModel(description = "Bootstrap parameters")
  public static class BootstrapParams {
    @Required
    @ApiModelProperty(
        value = "Whether bootstrap is required for each table specified in tables",
        required = true)
    public List<Boolean> isBootstrapRequiredPerTable;

    @Required
    @ApiModelProperty(value = "Parameters used to do Backup/restore", required = true)
    public BackupRequestParams backupRequestParams;
  }

  public void validate() {
    // Ensure there is no duplicates in the list of tables.
    if (new HashSet<>(this.tables).size() != this.tables.size()) {
      throw new PlatformServiceException(
          Http.Status.BAD_REQUEST, "There are duplicates in the list of tables: " + this.tables);
    }
    if (this.bootstrapParams != null) {
      // Ensure there is a corresponding field in isBootstrapRequiredPerTable for each table.
      if (this.tables.size() != this.bootstrapParams.isBootstrapRequiredPerTable.size()) {
        String errMsg =
            String.format(
                "Number of tables must be equal to the number of values in "
                    + "isBootstrapRequiredPerTable %d != %d",
                this.tables.size(), this.bootstrapParams.isBootstrapRequiredPerTable.size());
        throw new PlatformServiceException(Http.Status.BAD_REQUEST, errMsg);
      }
    }
  }
}
