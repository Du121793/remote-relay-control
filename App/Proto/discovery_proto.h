#ifndef DISCOVERY_PROTO_H
#define DISCOVERY_PROTO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int discovery_proto_build_announce(char *buffer,
                                   size_t buffer_size,
                                   const char *service,
                                   const char *cluster,
                                   const char *device,
                                   const char *ip_text,
                                   uint16_t port,
                                   const uint8_t mac[6]);
uint8_t discovery_proto_is_discover(const void *payload, uint16_t length);

#ifdef __cplusplus
}
#endif

#endif /* DISCOVERY_PROTO_H */
