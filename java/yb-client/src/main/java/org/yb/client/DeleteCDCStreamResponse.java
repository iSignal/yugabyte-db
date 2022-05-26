package org.yb.client;

import java.util.List;
import org.yb.master.MasterTypes;

public class DeleteCDCStreamResponse extends YRpcResponse {
  private final MasterTypes.MasterErrorPB serverError;
  private final List<String> notFoundStreamIds;

  DeleteCDCStreamResponse (long elapsedMillis,
                           String tsUUID,
                           MasterTypes.MasterErrorPB serverError,
                           List<String> notFoundStreamIds) {
    super(elapsedMillis, tsUUID);
    this.serverError = serverError;
    this.notFoundStreamIds = notFoundStreamIds;
  }

  public boolean hasError() {
    return serverError != null;
  }

  public String errorMessage() {
    if (serverError == null) {
      return "";
    }

    return serverError.getStatus().getMessage();
  }

  public List<String> getNotFoundStreamIds() {
    return notFoundStreamIds;
  }
}
