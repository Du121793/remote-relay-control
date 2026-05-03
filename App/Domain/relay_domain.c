#include "relay_domain.h"

#include "board_relay.h"

void relay_domain_execute(const control_proto_command_t *command, relay_domain_result_t *result)
{
  if (result == NULL)
  {
    return;
  }

  result->error = CONTROL_PROTO_ERR_BAD_COMMAND;
  result->relay_mask = board_relay_snapshot();

  if (command == NULL)
  {
    return;
  }

  switch (command->type)
  {
    case CONTROL_PROTO_CMD_GET_RELAY:
    case CONTROL_PROTO_CMD_PING:
      result->error = CONTROL_PROTO_ERR_NONE;
      break;

    case CONTROL_PROTO_CMD_SET_RELAY:
      if (board_relay_set(command->channel, command->value ? GPIO_PIN_SET : GPIO_PIN_RESET) == HAL_OK)
      {
        result->error = CONTROL_PROTO_ERR_NONE;
        result->relay_mask = board_relay_snapshot();
      }
      else
      {
        result->error = CONTROL_PROTO_ERR_BAD_CHANNEL;
      }
      break;

    case CONTROL_PROTO_CMD_SET_RELAY_MASK:
      if (board_relay_apply_mask(command->mask) == HAL_OK)
      {
        result->error = CONTROL_PROTO_ERR_NONE;
        result->relay_mask = board_relay_snapshot();
      }
      else
      {
        result->error = CONTROL_PROTO_ERR_BAD_VALUE;
      }
      break;

    case CONTROL_PROTO_CMD_SET_RELAY_ALL:
      if (board_relay_apply_mask(command->value ? 0x7FU : 0x00U) == HAL_OK)
      {
        result->error = CONTROL_PROTO_ERR_NONE;
        result->relay_mask = board_relay_snapshot();
      }
      else
      {
        result->error = CONTROL_PROTO_ERR_BAD_VALUE;
      }
      break;

    case CONTROL_PROTO_CMD_INVALID:
    default:
      result->error = command->error;
      break;
  }
}
