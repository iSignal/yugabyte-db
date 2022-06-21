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

package com.yugabyte.yw.common.kms.services;

import java.util.UUID;

import com.fasterxml.jackson.databind.node.ObjectNode;
import com.yugabyte.yw.common.kms.algorithms.AzuAlgorithm;
import com.yugabyte.yw.common.kms.util.AzuEARServiceUtil;
import com.yugabyte.yw.common.kms.util.KeyProvider;
import com.yugabyte.yw.forms.EncryptionAtRestConfig;

/**
 * An implementation of EncryptionAtRestService to communicate with AZU KMS
 * https://azure.microsoft.com/en-in/services/key-vault/
 */
public class AzuEARService extends EncryptionAtRestService<AzuAlgorithm> {
  private AzuEARServiceUtil azuEARServiceUtil;

  public AzuEARService() {
    super(KeyProvider.AZU);
  }

  public AzuEARServiceUtil getAzuEarServiceUtil() {
    return new AzuEARServiceUtil();
  }

  @Override
  protected AzuAlgorithm[] getSupportedAlgorithms() {
    return AzuAlgorithm.values();
  }

  @Override
  protected ObjectNode createAuthConfigWithService(UUID configUUID, ObjectNode config) {
    this.azuEARServiceUtil = getAzuEarServiceUtil();
    // Need to check if key vault is valid or not
    try {
      // Create a key ring
      // boolean validKeyVault = true;
      boolean validKeyVault = azuEARServiceUtil.checkKeyVaultisValid(configUUID, config);
      LOG.info("AzuEARService-createAuthConfigWithService: Checked key vault is valid.");
      // Add the key ring resource name to the stored config
      if (!validKeyVault) {
        String errMsg = "Key vault or the credentials are invalid";
        LOG.error(errMsg);
        throw new RuntimeException(errMsg);
      }
    } catch (Exception e) {
      final String errMsg =
          String.format(
              "Error attempting to validate key vault in AZU KMS with config %s",
              configUUID.toString());
      LOG.error(errMsg, e);
    }
    return config;
  }

  @Override
  protected byte[] createKeyWithService(
      UUID universeUUID, UUID configUUID, EncryptionAtRestConfig config) {
    this.azuEARServiceUtil = getAzuEarServiceUtil();
    byte[] result = null;
    String keyName = azuEARServiceUtil.generateKeyName(universeUUID);

    // Ensure the key vault exists in AZU KMS
    if (!azuEARServiceUtil.checkKeyVaultisValid(configUUID, null)) {
      String errMsg = "Key vault or the credentials are invalid";
      LOG.error(errMsg);
      throw new RuntimeException(errMsg);
    }
    // Ensure the key exists or create new one from AZU KMS to universe UUID
    azuEARServiceUtil.checkOrCreateKey(configUUID, keyName);
    switch (config.type) {
      case CMK:
        result = azuEARServiceUtil.getKey(configUUID, universeUUID).getKey().getK();
        break;
      default:
      case DATA_KEY:
        // Generate random byte array and encrypt it.
        // Store the encrypted byte array locally in the db.
        int numBytes = 32;
        byte[] keyBytes = azuEARServiceUtil.generateRandomBytes(configUUID, numBytes);
        result = azuEARServiceUtil.wrapKey(configUUID, universeUUID, keyBytes);
        break;
    }
    return result;
  }

  @Override
  protected byte[] rotateKeyWithService(
      UUID universeUUID, UUID configUUID, EncryptionAtRestConfig config) {
    this.azuEARServiceUtil = getAzuEarServiceUtil();
    byte[] result = null;
    String keyName = azuEARServiceUtil.generateKeyName(universeUUID);
    // Ensure the key ring exists from AZU KMS to universe UUID
    if (!azuEARServiceUtil.checkKeyVaultisValid(configUUID, null)) {
      String errMsg = "Key vault or the credentials are invalid";
      LOG.error(errMsg);
      throw new RuntimeException(errMsg);
    }
    // Ensure the key exists or create new one from AZU KMS to universe UUID
    azuEARServiceUtil.checkOrCreateKey(configUUID, keyName);
    // Generate random byte array and encrypt it.
    // Store the encrypted byte array locally in the db.
    int numBytes = 32;
    byte[] keyBytes = azuEARServiceUtil.generateRandomBytes(configUUID, numBytes);
    result = azuEARServiceUtil.wrapKey(configUUID, universeUUID, keyBytes);
    return result;
  }

  @Override
  public byte[] retrieveKeyWithService(
      UUID universeUUID, UUID configUUID, byte[] keyRef, EncryptionAtRestConfig config) {
    this.azuEARServiceUtil = getAzuEarServiceUtil();
    byte[] keyVal = null;
    try {
      switch (config.type) {
        case CMK:
          keyVal = keyRef;
          break;
        default:
        case DATA_KEY:
          // Decrypt the locally stored encrypted byte array to give the universe key.
          keyVal = azuEARServiceUtil.unwrapKey(configUUID, universeUUID, keyRef, null);
          if (keyVal == null) {
            LOG.warn("Could not retrieve key from key ref through AZU KMS");
          }
          break;
      }
    } catch (Exception e) {
      final String errMsg = "Error occurred retrieving encryption key";
      LOG.error(errMsg, e);
      throw new RuntimeException(errMsg, e);
    }
    return keyVal;
  }

  @Override
  protected byte[] validateRetrieveKeyWithService(
      UUID universeUUID,
      UUID configUUID,
      byte[] keyRef,
      EncryptionAtRestConfig config,
      ObjectNode authConfig) {
    this.azuEARServiceUtil = getAzuEarServiceUtil();
    byte[] keyVal = null;
    try {
      switch (config.type) {
        case CMK:
          keyVal = keyRef;
          break;
        default:
        case DATA_KEY:
          // Decrypt the locally stored encrypted byte array to give the universe key.
          keyVal = azuEARServiceUtil.unwrapKey(configUUID, universeUUID, keyRef, authConfig);
          if (keyVal == null) {
            LOG.warn("Could not retrieve key from key ref through AZU KMS");
          }
          break;
      }
    } catch (Exception e) {
      final String errMsg = "Error occurred retrieving encryption key";
      LOG.error(errMsg, e);
      throw new RuntimeException(errMsg, e);
    }
    return keyVal;
  }

  @Override
  protected void cleanupWithService(UUID universeUUID, UUID configUUID) {
    this.azuEARServiceUtil = getAzuEarServiceUtil();
    // Delete the AZU key vault key that maps from a key vault associated with a config UUID to a
    // universe UUID.
    String keyName = azuEARServiceUtil.getKey(configUUID, universeUUID).getName();
    if (azuEARServiceUtil.checkKeyExists(configUUID, keyName)) {
      azuEARServiceUtil.deleteKey(configUUID, universeUUID);
    }
  }
}
