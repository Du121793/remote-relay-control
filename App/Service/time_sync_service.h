#ifndef TIME_SYNC_SERVICE_H
#define TIME_SYNC_SERVICE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
  uint8_t active;
  uint8_t waiting_backend;
  uint8_t last_source;
  uint8_t reserved;
  uint32_t ntp_attempts;
  uint32_t ntp_successes;
  uint32_t ntp_failures;
  uint32_t backend_requests;
  uint32_t backend_successes;
  uint32_t backend_failures;
  uint32_t last_sync_ms;
  uint32_t next_attempt_ms;
  uint32_t last_error;
} time_sync_service_stats_t;

void time_sync_service_poll(void);
void time_sync_service_get_stats(time_sync_service_stats_t *stats);
uint8_t time_sync_service_apply_backend_time(uint32_t unix_time);

#ifdef __cplusplus
}
#endif

#endif /* TIME_SYNC_SERVICE_H */
