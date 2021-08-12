package group;

import java.util.UUID;
// Import classes:
import com.yugabyte.ApiClient;
import com.yugabyte.ApiException;
import com.yugabyte.Configuration;
import com.yugabyte.auth.*;
import com.yugabyte.platform_client.UniverseApi;
import com.yugabyte.platform_client.models.UniverseResp;
import java.util.List;

public class App {
    public static void main(String[] args) {
        System.out.println("app started\n");
        ApiClient defaultClient = Configuration.getDefaultApiClient();
        defaultClient.setBasePath("http://portal.dev.yugabyte.com:9000");



        // Configure API key authorization: apiKeyAuth
        ApiKeyAuth apiKeyAuth = (ApiKeyAuth) defaultClient.getAuthentication("apiKeyAuth");
        apiKeyAuth.setApiKey("<API KEY>");
        // Uncomment the following line to set a prefix for the API key, e.g. "Token" (defaults to null)
        //apiKeyAuth.setApiKeyPrefix("Token");

        UniverseApi apiInstance = new UniverseApi(defaultClient);
        UUID cUUID = java.util.UUID.fromString("<CUSTOMER UUID>"); // UUID |
        //UUID uniUUID = new UUID(); // UUID |
        //Boolean isForceDelete = false; // Boolean |
        String name = "sanketh-test-1"; // String
        //Boolean isDeleteBackups = false; // Boolean |
        try {
            List<UniverseResp> result = apiInstance.getListOfUniverses(cUUID, null);
            System.out.println(result);
        } catch (com.yugabyte.ApiException e) {
            System.out.println("app started 3 \n");
            System.err.println("Exception when calling UniverseApi#deleteUniverse");
            System.err.println("Status code: " + e.getCode());
            System.err.println("Reason: " + e.getResponseBody());
            System.err.println("Response headers: " + e.getResponseHeaders());
        }
    }
}
