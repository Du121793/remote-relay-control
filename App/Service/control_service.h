#ifndef CONTROL_SERVICE_H
#define CONTROL_SERVICE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
  uint32_t connections;
  uint32_t commands;
  uint32_t errors;
  uint32_t listener_up;
} control_service_stats_t;

void control_service_init(void);
void control_service_poll(void);
void control_service_get_stats(control_service_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* CONTROL_SERVICE_H */
