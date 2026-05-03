#ifndef CONTROL_QUEUE_H
#define CONTROL_QUEUE_H

#include <stdint.h>

#include "main.h"
#include "control_proto.h"
#include "relay_domain.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
  uint32_t submitted;
  uint32_t executed;
  uint32_t dropped;
} control_queue_stats_t;

void control_queue_init(void);
HAL_StatusTypeDef control_queue_submit(const control_proto_command_t *command, relay_domain_result_t *result);
void control_queue_poll(void);
void control_queue_get_stats(control_queue_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* CONTROL_QUEUE_H */
