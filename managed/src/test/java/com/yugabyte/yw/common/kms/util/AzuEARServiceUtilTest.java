package com.yugabyte.yw.common.kms.util;

import static org.junit.Assert.assertNotNull;

import com.azure.core.credential.TokenCredential;
import com.azure.security.keyvault.keys.KeyClient;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.yugabyte.yw.common.FakeDBApplication;
import java.util.UUID;
import lombok.extern.slf4j.Slf4j;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.runners.MockitoJUnitRunner;
import play.libs.Json;

@RunWith(MockitoJUnitRunner.class)
@Slf4j
public class AzuEARServiceUtilTest extends FakeDBApplication {

  // @Test
  // public void testFoo() {

  //   AzuEARServiceUtil util = new AzuEARServiceUtil();
  //   ObjectNode node = Json.newObject();
  //   KeyClient azClient = util.getKeyClient(UUID.randomUUID(), node);
  //   assertNotNull(azClient);
  // }

  @Test
  public void testBar() {
    AzuEARServiceUtil util = new AzuEARServiceUtil();
    ObjectNode node = Json.newObject();
    node.put("CLIENT_ID", System.getenv("AZURE_CLIENT_ID"));
    node.put("CLIENT_SECRET", System.getenv("AZURE_CLIENT_SECRET"));
    node.put("TENANT_ID", System.getenv("AZURE_TENANT_ID"));
    TokenCredential tkc = util.getCredentials(node);
    assertNotNull(tkc);
  }
}
