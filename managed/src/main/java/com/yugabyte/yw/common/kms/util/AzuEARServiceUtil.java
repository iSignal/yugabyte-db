/*
 * Copyright 2022 YugaByte, Inc. and Contributors
 *
 * Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
 * may not use this file except in compliance with the License. You
 * may obtain a copy of the License at
 *
 * https://github.com/YugaByte/yugabyte-db/blob/master/licenses/
 * POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt
 */

package com.yugabyte.yw.common.kms.util;

import com.azure.core.credential.TokenCredential;
import com.azure.core.exception.ResourceNotFoundException;
import com.azure.core.util.polling.PollResponse;
import com.azure.core.util.polling.SyncPoller;
import com.azure.identity.ClientSecretCredential;
import com.azure.identity.ClientSecretCredentialBuilder;
import com.azure.security.keyvault.keys.KeyClient;
import com.azure.security.keyvault.keys.KeyClientBuilder;
import com.azure.security.keyvault.keys.cryptography.CryptographyClient;
import com.azure.security.keyvault.keys.cryptography.CryptographyClientBuilder;
import com.azure.security.keyvault.keys.cryptography.models.KeyWrapAlgorithm;
import com.azure.security.keyvault.keys.cryptography.models.UnwrapResult;
import com.azure.security.keyvault.keys.cryptography.models.WrapResult;
import com.azure.security.keyvault.keys.models.CreateRsaKeyOptions;
import com.azure.security.keyvault.keys.models.DeletedKey;
import com.azure.security.keyvault.keys.models.KeyVaultKey;
import com.fasterxml.jackson.databind.node.ObjectNode;
import java.util.UUID;
import lombok.extern.slf4j.Slf4j;

@Slf4j
public class AzuEARServiceUtil {

  public ObjectNode getAuthConfig(UUID configUUID) {
    return EncryptionAtRestUtil.getAuthConfig(configUUID, KeyProvider.AZU);
  }

  public TokenCredential getCredentials(ObjectNode authConfig) {
    String clientId = getConfigClientId(authConfig);
    String clientSecret = getConfigClientSecret(authConfig);
    String tenantId = getConfigTenantId(authConfig);

    ClientSecretCredential clientSecretCredential =
        new ClientSecretCredentialBuilder()
            .clientId(clientId)
            .clientSecret(clientSecret)
            .tenantId(tenantId)
            .build();

    log.info("got past build");

    return clientSecretCredential;
  }

  public TokenCredential getCredentials(UUID configUUID) {
    ObjectNode authConfig = getAuthConfig(configUUID);
    return getCredentials(authConfig);
  }

  public KeyClient getKeyClient(UUID configUUID, ObjectNode authConfig) {
    if (authConfig == null) {
      authConfig = getAuthConfig(configUUID);
    }
    KeyClientBuilder keyClientBuilder = new KeyClientBuilder();
    keyClientBuilder = keyClientBuilder.vaultUrl(getConfigVaultUrl(authConfig));
    keyClientBuilder = keyClientBuilder.credential(getCredentials(authConfig));
    KeyClient keyClient = keyClientBuilder.buildClient();
    return keyClient;
  }

  public KeyClient getKeyClient(UUID configUUID) {
    ObjectNode authConfig = getAuthConfig(configUUID);
    return getKeyClient(configUUID, authConfig);
  }

  public String getKeyIdFromKeyName(UUID configUUID, String keyName) {
    KeyClient keyClient = getKeyClient(configUUID);
    String keyId = keyClient.getKey(keyName).getId();
    return keyId;
  }

  public CryptographyClient getCryptographyClient(
      UUID configUUID, UUID universeUUID, ObjectNode authConfig) {
    if (authConfig == null) {
      authConfig = getAuthConfig(configUUID);
    }
    String keyName = generateKeyName(universeUUID);
    CryptographyClientBuilder cryptoClientBuilder = new CryptographyClientBuilder();
    cryptoClientBuilder = cryptoClientBuilder.credential(getCredentials(authConfig));
    cryptoClientBuilder =
        cryptoClientBuilder.keyIdentifier(getKeyIdFromKeyName(configUUID, keyName));
    CryptographyClient cryptographyClient = cryptoClientBuilder.buildClient();

    return cryptographyClient;
  }

  public CryptographyClient getCryptographyClient(UUID configUUID, UUID universeUUID) {
    ObjectNode authConfig = getAuthConfig(configUUID);
    return getCryptographyClient(configUUID, universeUUID, authConfig);
  }

  // public String getConfigClientId(UUID configUUID){
  //   ObjectNode authConfig = getAuthConfig(configUUID);
  //   String clientId = "";
  //   if (authConfig.has("CLIENT_ID")) {
  //     clientId = authConfig.path("CLIENT_ID").asText();
  //   } else {
  //     log.info("Could not get AZU config client id. 'CLIENT_ID' not found.");
  //     return null;
  //   }
  //   return clientId;
  // }

  public String getConfigClientId(ObjectNode authConfig) {
    String clientId = "";
    if (authConfig.has("CLIENT_ID")) {
      clientId = authConfig.path("CLIENT_ID").asText();
    } else {
      log.info("Could not get AZU config client id. 'CLIENT_ID' not found.");
      return null;
    }
    return clientId;
  }

  // public String getConfigClientSecret(UUID configUUID){
  //   ObjectNode authConfig = getAuthConfig(configUUID);
  //   String clientSecret = "";
  //   if (authConfig.has("CLIENT_SECRET")) {
  //     clientSecret = authConfig.path("CLIENT_SECRET").asText();
  //   } else {
  //     log.info("Could not get AZU config client secret. 'CLIENT_SECRET' not found.");
  //     return null;
  //   }
  //   return clientSecret;
  // }

  public String getConfigClientSecret(ObjectNode authConfig) {
    String clientSecret = "";
    if (authConfig.has("CLIENT_SECRET")) {
      clientSecret = authConfig.path("CLIENT_SECRET").asText();
    } else {
      log.info("Could not get AZU config client secret. 'CLIENT_SECRET' not found.");
      return null;
    }
    return clientSecret;
  }

  // public String getConfigTenantId(UUID configUUID){
  //   ObjectNode authConfig = getAuthConfig(configUUID);
  //   String tenantId = "";
  //   if (authConfig.has("TENANT_ID")) {
  //     tenantId = authConfig.path("TENANT_ID").asText();
  //   } else {
  //     log.info("Could not get AZU config tenant id. 'TENANT_ID' not found.");
  //     return null;
  //   }
  //   return tenantId;
  // }

  public String getConfigTenantId(ObjectNode authConfig) {
    String tenantId = "";
    if (authConfig.has("TENANT_ID")) {
      tenantId = authConfig.path("TENANT_ID").asText();
    } else {
      log.info("Could not get AZU config tenant id. 'TENANT_ID' not found.");
      return null;
    }
    return tenantId;
  }

  // public String getConfigVaultUrl(UUID configUUID){
  //   ObjectNode authConfig = getAuthConfig(configUUID);
  //   String tenantId = "";
  //   if (authConfig.has("AZU_VAULT_URL")) {
  //     tenantId = authConfig.path("AZU_VAULT_URL").asText();
  //   } else {
  //     log.info("Could not get AZU config vault url. 'AZU_VAULT_URL' not found.");
  //     return null;
  //   }
  //   return tenantId;
  // }

  public String getConfigVaultUrl(ObjectNode authConfig) {
    String tenantId = "";
    if (authConfig.has("AZU_VAULT_URL")) {
      tenantId = authConfig.path("AZU_VAULT_URL").asText();
    } else {
      log.info("Could not get AZU config vault url. 'AZU_VAULT_URL' not found.");
      return null;
    }
    return tenantId;
  }

  // public String getConfigKeyId(UUID configUUID){
  //   ObjectNode authConfig = getAuthConfig(configUUID);
  //   String keyId = "";
  //   if (authConfig.has("AZU_KEY_ID")) {
  //     keyId = authConfig.path("AZU_KEY_ID").asText();
  //   } else {
  //     log.info("Could not get AZU config key id. 'AZU_KEY_ID' not found.");
  //     return null;
  //   }
  //   return keyId;
  // }

  public String getConfigKeyId(ObjectNode authConfig) {
    String keyId = "";
    if (authConfig.has("AZU_KEY_ID")) {
      keyId = authConfig.path("AZU_KEY_ID").asText();
    } else {
      log.info("Could not get AZU config key id. 'AZU_KEY_ID' not found.");
      return null;
    }
    return keyId;
  }

  public boolean checkKeyVaultisValid(UUID configUUID, ObjectNode authConfig) {
    try {
      if (authConfig == null) {
        authConfig = getAuthConfig(configUUID);
      }
      KeyClient keyClient = getKeyClient(configUUID, authConfig);
      if (keyClient != null) {
        return true;
      }
    } catch (Exception e) {
      log.info("Key vault or credentials are invalid. Config UUID = " + configUUID.toString());
      return false;
    }
    return false;
  }

  public String generateKeyName(UUID universeUUID) {
    String keyName = "yb-key-" + universeUUID.toString();
    return keyName;
  }

  public boolean checkKeyExists(UUID configUUID, String keyName) {
    KeyClient keyClient = getKeyClient(configUUID);
    try {
      KeyVaultKey keyVaultKey = keyClient.getKey(keyName);
      if (keyVaultKey == null) {
        return false;
      }
    } catch (ResourceNotFoundException e) {
      String msg = "Key does not exist in the key vault";
      log.info(msg);
      return false;
    }
    return true;
  }

  public void createKey(UUID configUUID, String keyName) {
    KeyClient keyClient = getKeyClient(configUUID);
    CreateRsaKeyOptions createRsaKeyOptions = new CreateRsaKeyOptions(keyName);
    keyClient.createRsaKey(createRsaKeyOptions);
    return;
  }

  public void checkOrCreateKey(UUID configUUID, String keyName) {
    boolean keyExists = checkKeyExists(configUUID, keyName);
    if (!keyExists) {
      createKey(configUUID, keyName);
    }
  }

  public KeyVaultKey getKey(UUID configUUID, UUID universeUUID) {
    KeyClient keyClient = getKeyClient(configUUID);
    String keyName = generateKeyName(universeUUID);
    KeyVaultKey keyVaultKey = keyClient.getKey(keyName);
    return keyVaultKey;
  }

  public byte[] generateRandomBytes(UUID configUUID, int numBytes) {
    KeyClient keyClient = getKeyClient(configUUID);
    byte[] randomBytes = keyClient.getRandomBytes(numBytes);
    return randomBytes;
  }

  public byte[] wrapKey(UUID configUUID, UUID universeUUID, byte[] keyBytes) {
    CryptographyClient cryptographyClient = getCryptographyClient(configUUID, universeUUID);
    WrapResult wrapResult = cryptographyClient.wrapKey(KeyWrapAlgorithm.RSA_OAEP, keyBytes);
    byte[] wrappedKeyBytes = wrapResult.getEncryptedKey();
    return wrappedKeyBytes;
  }

  public byte[] unwrapKey(
      UUID configUUID, UUID universeUUID, byte[] keyRef, ObjectNode authConfig) {
    CryptographyClient cryptographyClient =
        getCryptographyClient(configUUID, universeUUID, authConfig);
    UnwrapResult unwrapResult = cryptographyClient.unwrapKey(KeyWrapAlgorithm.RSA_OAEP, keyRef);
    byte[] keyBytes = unwrapResult.getKey();
    return keyBytes;
  }

  public void deleteKey(UUID configUUID, UUID universeUUID) {
    KeyClient keyClient = getKeyClient(configUUID);
    String keyName = generateKeyName(universeUUID);
    SyncPoller<DeletedKey, Void> rsaDeletedKeyPoller = keyClient.beginDeleteKey(keyName);
    PollResponse<DeletedKey> pollResponse = rsaDeletedKeyPoller.poll();

    DeletedKey rsaDeletedKey = pollResponse.getValue();
    log.info(
        "Deleted Azure KMS key with config UUID: "
            + configUUID.toString()
            + " and universe UUID: "
            + universeUUID.toString());
    log.info("Deleted Date  %s" + rsaDeletedKey.getDeletedOn().toString());
    log.info("Deleted Key's Recovery Id %s", rsaDeletedKey.getRecoveryId());

    // Key is being deleted on server.
    rsaDeletedKeyPoller.waitForCompletion();

    // If the key vault is soft-delete enabled, then for permanent deletion, deleted keys need to be
    // purged
    keyClient.purgeDeletedKey(keyName);
  }
}
