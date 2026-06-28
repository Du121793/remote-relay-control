#include "board_relay.h"
#include <stdio.h>

/* Keep board-level relay access silent during normal operation. */
#define printf(...) ((void)0)

static GPIO_TypeDef *const relay_ports[BOARD_RELAY_CHANNEL_COUNT] = {
  Relay1_GPIO_Port, Relay2_GPIO_Port, Relay3_GPIO_Port, Relay4_GPIO_Port,
  Relay5_GPIO_Port, Relay6_GPIO_Port, Relay7_GPIO_Port, Relay8_GPIO_Port,
  Relay9_GPIO_Port, Relay10_GPIO_Port, Relay11_GPIO_Port, Relay12_GPIO_Port,
  Relay13_GPIO_Port, Relay14_GPIO_Port, Relay15_GPIO_Port, Relay16_GPIO_Port
};

static const uint16_t relay_pins[BOARD_RELAY_CHANNEL_COUNT] = {
  Relay1_Pin, Relay2_Pin, Relay3_Pin, Relay4_Pin,
  Relay5_Pin, Relay6_Pin, Relay7_Pin, Relay8_Pin,
  Relay9_Pin, Relay10_Pin, Relay11_Pin, Relay12_Pin,
  Relay13_Pin, Relay14_Pin, Relay15_Pin, Relay16_Pin
};

HAL_StatusTypeDef board_relay_set(uint8_t channel, GPIO_PinState state)
{
  GPIO_TypeDef *port;
  uint16_t pin;

  if ((channel == 0U) || (channel > BOARD_RELAY_CHANNEL_COUNT))
  {
    printf("[RELAY] board_relay_set: channel %u out of bounds\r\n", channel);
    return HAL_ERROR;
  }

  port = relay_ports[channel - 1U];
  pin = relay_pins[channel - 1U];

  printf("[RELAY] board_relay_set: ch=%u, state=%d (Port=%p, Pin=%u)\r\n",
         channel, state, (void*)relay_ports[channel - 1U], relay_pins[channel - 1U]);
  HAL_GPIO_WritePin(port, pin, state);

  return HAL_OK;
}

GPIO_PinState board_relay_get(uint8_t channel)
{
  if ((channel == 0U) || (channel > BOARD_RELAY_CHANNEL_COUNT))
  {
    // printf("[RELAY] board_relay_get: channel %u out of bounds\r\n", channel); // Too verbose
    return GPIO_PIN_RESET;
  }

  GPIO_PinState state = HAL_GPIO_ReadPin(relay_ports[channel - 1U], relay_pins[channel - 1U]);
  // printf("[RELAY] board_relay_get: ch=%u, state=%d\r\n", channel, state); // Too verbose
  return state;
}

uint32_t board_relay_snapshot(void)
{
  uint32_t bits = 0U;
  uint8_t channel;

  for (channel = 1U; channel <= BOARD_RELAY_CHANNEL_COUNT; channel++)
  {
    if (board_relay_get(channel) == GPIO_PIN_SET)
    {
      bits |= (1UL << (channel - 1U));
    }
  }
  printf("[RELAY] board_relay_snapshot: mask=%02X\r\n", bits);
  return bits;
}

HAL_StatusTypeDef board_relay_apply_mask(uint32_t mask)
{
  uint8_t channel;
  HAL_StatusTypeDef status = HAL_OK;

  printf("[RELAY] board_relay_apply_mask: mask=%02X\r\n", mask);

  for (channel = 1U; channel <= BOARD_RELAY_CHANNEL_COUNT; channel++)
  {
    if (board_relay_set(channel, (mask & (1UL << (channel - 1U))) != 0U ? GPIO_PIN_SET : GPIO_PIN_RESET) != HAL_OK)
    {
      status = HAL_ERROR;
      // Continue to set other relays even if one fails
    }
  }

  return status;
}
