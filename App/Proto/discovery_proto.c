#include "discovery_proto.h"

#include <stdio.h>
#include <string.h>

int discovery_proto_build_announce(char *buffer,
                                   size_t buffer_size,
                                   const char *service,
                                   const char *cluster,
                                   const char *device,
                                   const char *ip_text,
                                   uint16_t port,
                                   const uint8_t mac[6])
{
  return snprintf(buffer, buffer_size,
                  "type=announce ver=1 service=%s cluster=%s device=%s ip=%s port=%u mac=%02X:%02X:%02X:%02X:%02X:%02X",
                  service,
                  cluster,
                  device,
                  ip_text,
                  (unsigned int)port,
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

uint8_t discovery_proto_is_discover(const void *payload, uint16_t length)
{
  static const char token[] = "DISCOVER";

  if ((payload == NULL) || (length < (sizeof(token) - 1U)))
  {
    return 0U;
  }

  return (memcmp(payload, token, sizeof(token) - 1U) == 0) ? 1U : 0U;
}
