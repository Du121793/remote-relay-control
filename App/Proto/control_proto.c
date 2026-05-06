#include "control_proto.h"
#include "board_relay.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void trim_request(char *text)
{
  size_t len;

  if (text == NULL)
  {
    return;
  }

  while ((*text != '\0') && isspace((unsigned char)*text))
  {
    memmove(text, text + 1, strlen(text));
  }

  len = strlen(text);
  while ((len > 0U) && isspace((unsigned char)text[len - 1U]))
  {
    text[len - 1U] = '\0';
    len--;
  }
}

static uint8_t parse_u8_token(const char *text, uint8_t *value_out)
{
  unsigned long value;
  char *endptr = NULL;

  if ((text == NULL) || (value_out == NULL) || (*text == '\0'))
  {
    return 0U;
  }

  value = strtoul(text, &endptr, 0);
  if ((*endptr != '\0') || (value > 255UL))
  {
    return 0U;
  }

  *value_out = (uint8_t)value;
  return 1U;
}

void control_proto_parse(const char *request, control_proto_command_t *command)
{
  char buffer[96];
  char *tokens[5] = {0};
  size_t index = 0U;

  if (command == NULL)
  {
    return;
  }

  memset(command, 0, sizeof(*command));
  command->type = CONTROL_PROTO_CMD_INVALID;
  command->error = CONTROL_PROTO_ERR_BAD_COMMAND;

  if (request == NULL)
  {
    return;
  }

  (void)snprintf(buffer, sizeof(buffer), "%s", request);
  trim_request(buffer);

  tokens[index] = strtok(buffer, " ");
  while ((tokens[index] != NULL) && (index < (sizeof(tokens) / sizeof(tokens[0])) - 1U))
  {
    index++;
    tokens[index] = strtok(NULL, " ");
  }

  if ((tokens[0] == NULL) || (*tokens[0] == '\0'))
  {
    return;
  }

  if ((strcmp(tokens[0], "PING") == 0) && (tokens[1] == NULL))
  {
    command->type = CONTROL_PROTO_CMD_PING;
    command->error = CONTROL_PROTO_ERR_NONE;
    return;
  }

  if ((strcmp(tokens[0], "GET") == 0) && (tokens[1] != NULL) &&
      (strcmp(tokens[1], "RELAY") == 0) && (tokens[2] == NULL))
  {
    command->type = CONTROL_PROTO_CMD_GET_RELAY;
    command->error = CONTROL_PROTO_ERR_NONE;
    return;
  }

  if ((strcmp(tokens[0], "SET") == 0) && (tokens[1] != NULL) &&
      (strcmp(tokens[1], "RELAY") == 0))
  {
    if ((tokens[2] != NULL) && (strcmp(tokens[2], "MASK") == 0) && (tokens[3] != NULL) && (tokens[4] == NULL))
    {
      char *endptr = NULL;
      unsigned long mask = strtoul(tokens[3], &endptr, 0);

      if ((*endptr == '\0') && (mask <= BOARD_RELAY_ALL_MASK))
      {
        command->type = CONTROL_PROTO_CMD_SET_RELAY_MASK;
        command->mask = (uint32_t)mask;
        command->error = CONTROL_PROTO_ERR_NONE;
      }
      else
      {
        command->error = CONTROL_PROTO_ERR_BAD_VALUE;
      }
      return;
    }

    if ((tokens[2] != NULL) && (strcmp(tokens[2], "ALL") == 0) && (tokens[3] != NULL) && (tokens[4] == NULL))
    {
      if (parse_u8_token(tokens[3], &command->value) && (command->value <= 1U))
      {
        command->type = CONTROL_PROTO_CMD_SET_RELAY_ALL;
        command->error = CONTROL_PROTO_ERR_NONE;
      }
      else
      {
        command->error = CONTROL_PROTO_ERR_BAD_VALUE;
      }
      return;
    }

    if ((tokens[2] != NULL) && (tokens[3] != NULL) && (tokens[4] == NULL))
    {
      if (!parse_u8_token(tokens[2], &command->channel))
      {
        command->error = CONTROL_PROTO_ERR_BAD_CHANNEL;
        return;
      }

      if ((command->channel == 0U) || (command->channel > BOARD_RELAY_CHANNEL_COUNT))
      {
        command->error = CONTROL_PROTO_ERR_BAD_CHANNEL;
        return;
      }

      if (!parse_u8_token(tokens[3], &command->value) || (command->value > 1U))
      {
        command->error = CONTROL_PROTO_ERR_BAD_VALUE;
        return;
      }

      command->type = CONTROL_PROTO_CMD_SET_RELAY;
      command->error = CONTROL_PROTO_ERR_NONE;
      return;
    }
  }
}

int control_proto_format_pong(char *buffer, size_t buffer_size, const char *ip_text, uint32_t relay_mask)
{
  return snprintf(buffer, buffer_size, "OK type=pong ip=%s relay=0x%04lX\r\n",
                  ip_text, (unsigned long)relay_mask);
}

int control_proto_format_relay_state(char *buffer, size_t buffer_size, uint32_t relay_mask)
{
  return snprintf(buffer, buffer_size,
                  "OK type=relay relay=0x%04lX "
                  "ch1=%u ch2=%u ch3=%u ch4=%u ch5=%u ch6=%u ch7=%u ch8=%u "
                  "ch9=%u ch10=%u ch11=%u ch12=%u ch13=%u ch14=%u ch15=%u ch16=%u\r\n",
                  (unsigned long)relay_mask,
                  (unsigned int)((relay_mask >> 0) & 0x1U),
                  (unsigned int)((relay_mask >> 1) & 0x1U),
                  (unsigned int)((relay_mask >> 2) & 0x1U),
                  (unsigned int)((relay_mask >> 3) & 0x1U),
                  (unsigned int)((relay_mask >> 4) & 0x1U),
                  (unsigned int)((relay_mask >> 5) & 0x1U),
                  (unsigned int)((relay_mask >> 6) & 0x1U),
                  (unsigned int)((relay_mask >> 7) & 0x1U),
                  (unsigned int)((relay_mask >> 8) & 0x1U),
                  (unsigned int)((relay_mask >> 9) & 0x1U),
                  (unsigned int)((relay_mask >> 10) & 0x1U),
                  (unsigned int)((relay_mask >> 11) & 0x1U),
                  (unsigned int)((relay_mask >> 12) & 0x1U),
                  (unsigned int)((relay_mask >> 13) & 0x1U),
                  (unsigned int)((relay_mask >> 14) & 0x1U),
                  (unsigned int)((relay_mask >> 15) & 0x1U));
}

int control_proto_format_error(char *buffer, size_t buffer_size, control_proto_error_t error)
{
  const char *code = "bad_command";

  switch (error)
  {
    case CONTROL_PROTO_ERR_BAD_CHANNEL:
      code = "bad_channel";
      break;

    case CONTROL_PROTO_ERR_BAD_VALUE:
      code = "bad_value";
      break;

    case CONTROL_PROTO_ERR_BAD_COMMAND:
    default:
      code = "bad_command";
      break;
  }

  return snprintf(buffer, buffer_size, "ERR code=%s\r\n", code);
}
