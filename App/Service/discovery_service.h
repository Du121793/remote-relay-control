#ifndef DISCOVERY_SERVICE_H
#define DISCOVERY_SERVICE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
  uint32_t rx_packets;
  uint32_t tx_attempts;
  uint32_t announce_packets;
  uint32_t tx_failures;
  uint32_t discover_packets;
  uint32_t last_src_ip;
  uint32_t last_src_port;
  int32_t last_tx_err;
  uint32_t last_payload_len;
} discovery_service_stats_t;

void discovery_service_init(void);
void discovery_service_poll(void);
void discovery_service_force_announce(void);
void discovery_service_get_stats(discovery_service_stats_t *stats);

#ifdef __cplusplus
}
#endif
UBaseType_t discovery_service_stack_high_water(void);

#endif /* DISCOVERY_SERVICE_H */
