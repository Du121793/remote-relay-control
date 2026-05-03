#ifndef RELAY_DOMAIN_H
#define RELAY_DOMAIN_H

#include <stdint.h>

#include "control_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
  control_proto_error_t error;
  uint32_t relay_mask;
} relay_domain_result_t;

void relay_domain_execute(const control_proto_command_t *command, relay_domain_result_t *result);

#ifdef __cplusplus
}
#endif

#endif /* RELAY_DOMAIN_H */
