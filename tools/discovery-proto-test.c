#include "discovery_proto.h"
#include "lwip/ip4_addr.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int ip4addr_aton(const char *cp, ip4_addr_t *addr)
{
  (void)cp;
  if (addr != NULL)
  {
    addr->addr = 0U;
  }
  return 0;
}

static void fill_text(char *buffer, size_t size, char value)
{
  memset(buffer, value, size - 1U);
  buffer[size - 1U] = '\0';
}

static void require(int condition, const char *message)
{
  if (!condition)
  {
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
  }
}

int main(void)
{
  char payload[DISCOVERY_PROTO_ANNOUNCE_MAX_SIZE];
  char cluster[DEVICE_CONFIG_CLUSTER_NAME_MAX_LEN];
  char device[DEVICE_CONFIG_NAME_MAX_LEN];
  char cloud_cluster[DEVICE_CONFIG_CLOUD_ID_MAX_LEN];
  char cloud_device[DEVICE_CONFIG_CLOUD_ID_MAX_LEN];
  char cloud_key[DEVICE_CONFIG_CLOUD_ID_MAX_LEN];
  const uint8_t mac[6] = {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU};
  int length;

  fill_text(cluster, sizeof(cluster), 'c');
  fill_text(device, sizeof(device), 'd');
  fill_text(cloud_cluster, sizeof(cloud_cluster), 'a');
  fill_text(cloud_device, sizeof(cloud_device), 'b');
  fill_text(cloud_key, sizeof(cloud_key), 'k');

  length = discovery_proto_build_announce(
    payload,
    sizeof(payload),
    "auto-control-service-v7",
    cluster,
    device,
    cloud_cluster,
    cloud_device,
    cloud_key,
    UINT32_MAX,
    UINT8_MAX,
    UINT32_MAX,
    UINT32_MAX,
    UINT32_MAX,
    UINT32_MAX,
    UINT32_MAX,
    1U,
    UINT32_MAX,
    1U,
    1U,
    UINT8_MAX,
    UINT8_MAX,
    UINT32_MAX,
    UINT32_MAX,
    UINT32_MAX,
    1U,
    1U,
    1U,
    UINT32_MAX,
    UINT32_MAX,
    UINT32_MAX,
    UINT32_MAX,
    UINT32_MAX,
    UINT32_MAX,
    UINT32_MAX,
    UINT32_MAX,
    UINT32_MAX,
    UINT32_MAX,
    UINT32_MAX,
    UINT32_MAX,
    UINT32_MAX,
    "255.255.255.255",
    UINT16_MAX,
    mac);

  require(length > 0, "announce formatter must succeed");
  require((size_t)length < sizeof(payload), "max announce must not be truncated");
  require(strstr(payload, "ver=7") != NULL, "announce must use protocol version 7");
  require(strstr(payload, "adc_current_ma=4294967295") != NULL,
          "announce must contain current field");
  require(strstr(payload, "adc_saturation=4294967295") != NULL,
          "announce must contain diagnostics");

  printf("PASS: discovery v7 max payload uses %d/%u bytes\n",
         length,
         (unsigned int)sizeof(payload));
  return 0;
}
