#ifndef CONTROL_PROTO_H
#define CONTROL_PROTO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
  CONTROL_PROTO_CMD_INVALID = 0,
  CONTROL_PROTO_CMD_PING,
  CONTROL_PROTO_CMD_GET_RELAY,
  CONTROL_PROTO_CMD_SET_RELAY,
  CONTROL_PROTO_CMD_SET_RELAY_MASK,
  CONTROL_PROTO_CMD_SET_RELAY_ALL
} control_proto_command_type_t;

typedef enum
{
  CONTROL_PROTO_ERR_NONE = 0,
  CONTROL_PROTO_ERR_BAD_COMMAND,
  CONTROL_PROTO_ERR_BAD_CHANNEL,
  CONTROL_PROTO_ERR_BAD_VALUE
} control_proto_error_t;

typedef struct
{
  control_proto_command_type_t type;
  uint8_t channel;
  uint8_t value;
  uint32_t mask;
  control_proto_error_t error;
} control_proto_command_t;

void control_proto_parse(const char *request, control_proto_command_t *command);
int control_proto_format_pong(char *buffer, size_t buffer_size, const char *ip_text, uint32_t relay_mask);
int control_proto_format_relay_state(char *buffer, size_t buffer_size, uint32_t relay_mask);
int control_proto_format_error(char *buffer, size_t buffer_size, control_proto_error_t error);

#ifdef __cplusplus
}
#endif

#endif /* CONTROL_PROTO_H */
