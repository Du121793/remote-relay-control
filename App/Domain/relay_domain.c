#include "relay_domain.h"
#include "board_relay.h"
#include <stdio.h>

/* Keep relay command processing silent during normal operation. */
#define printf(...) ((void)0)

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
    printf("[RELAY] domain execute: command is NULL\r\n");
    return;
  }

  printf("[RELAY] domain execute: command type=%d, channel=%u, value=%u, mask=%02X\r\n",
         command->type, command->channel, command->value, command->mask);

  switch (command->type)
  {
    case CONTROL_PROTO_CMD_GET_RELAY:
    case CONTROL_PROTO_CMD_PING:
      result->error = CONTROL_PROTO_ERR_NONE;
      printf("[RELAY] domain execute: GET_RELAY/PING, mask=%02X\r\n", result->relay_mask);
      break;

    case CONTROL_PROTO_CMD_SET_RELAY:
      if (board_relay_set(command->channel, command->value ? GPIO_PIN_SET : GPIO_PIN_RESET) == HAL_OK)
      {
        result->error = CONTROL_PROTO_ERR_NONE;
        result->relay_mask = board_relay_snapshot();
        printf("[RELAY] domain execute: SET_RELAY ch=%u, val=%u, new_mask=%02X\r\n",
               command->channel, command->value, result->relay_mask);
      }
      else
      {
        result->error = CONTROL_PROTO_ERR_BAD_CHANNEL;
        printf("[RELAY] domain execute: SET_RELAY ch=%u, val=%u, HAL_ERROR\r\n",
               command->channel, command->value);
      }
      break;

    case CONTROL_PROTO_CMD_SET_RELAY_MASK:
      if (board_relay_apply_mask(command->mask) == HAL_OK)
      {
        result->error = CONTROL_PROTO_ERR_NONE;
        result->relay_mask = board_relay_snapshot();
        printf("[RELAY] domain execute: SET_RELAY_MASK mask=%02X, new_mask=%02X\r\n",
               command->mask, result->relay_mask);
      }
      else
      {
        result->error = CONTROL_PROTO_ERR_BAD_VALUE;
        printf("[RELAY] domain execute: SET_RELAY_MASK mask=%02X, HAL_ERROR\r\n", command->mask);
      }
      break;

    case CONTROL_PROTO_CMD_SET_RELAY_ALL:
      if (board_relay_apply_mask(command->value ? BOARD_RELAY_ALL_MASK : 0x00U) == HAL_OK)
      {
        result->error = CONTROL_PROTO_ERR_NONE;
        result->relay_mask = board_relay_snapshot();
        printf("[RELAY] domain execute: SET_RELAY_ALL val=%u, new_mask=%02X\r\n",
               command->value, result->relay_mask);
      }
      else
      {
        result->error = CONTROL_PROTO_ERR_BAD_VALUE;
        printf("[RELAY] domain execute: SET_RELAY_ALL val=%u, HAL_ERROR\r\n", command->value);
      }
      break;

    case CONTROL_PROTO_CMD_INVALID:
    default:
      result->error = command->error;
      printf("[RELAY] domain execute: INVALID command, err=%d\r\n", command->error);
      break;
  }
}
