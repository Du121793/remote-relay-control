#ifndef BACKEND_LINK_SERVICE_H
#define BACKEND_LINK_SERVICE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
  uint8_t connected;
  uint8_t should_run;
  uint16_t port;
  uint32_t connect_attempts;
  uint32_t connect_failures;
  uint32_t sent_hello;
  uint32_t sent_heartbeat;
  uint32_t sent_status;
  uint32_t sent_fault_reports;
  uint32_t received_commands;
  uint32_t command_errors;
  uint32_t last_error;
} backend_link_stats_t;

void backend_link_service_init(void);
void backend_link_service_poll(void);
void backend_link_service_get_stats(backend_link_stats_t *stats);
void backend_link_service_mark_status_dirty(void);

#ifdef __cplusplus
}
#endif

#endif /* BACKEND_LINK_SERVICE_H */
