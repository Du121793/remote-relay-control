#include "board_relay.h"

static GPIO_TypeDef *const relay_ports[BOARD_RELAY_CHANNEL_COUNT] = {
  Relay1_GPIO_Port, Relay2_GPIO_Port, Relay3_GPIO_Port, Relay4_GPIO_Port,
  Relay5_GPIO_Port, Relay6_GPIO_Port, Relay7_GPIO_Port
};

static const uint16_t relay_pins[BOARD_RELAY_CHANNEL_COUNT] = {
  Relay1_Pin, Relay2_Pin, Relay3_Pin, Relay4_Pin,
  Relay5_Pin, Relay6_Pin, Relay7_Pin
};

HAL_StatusTypeDef board_relay_set(uint8_t channel, GPIO_PinState state)
{
  if ((channel == 0U) || (channel > BOARD_RELAY_CHANNEL_COUNT))
  {
    return HAL_ERROR;
  }

  HAL_GPIO_WritePin(relay_ports[channel - 1U], relay_pins[channel - 1U], state);
  return HAL_OK;
}

GPIO_PinState board_relay_get(uint8_t channel)
{
  if ((channel == 0U) || (channel > BOARD_RELAY_CHANNEL_COUNT))
  {
    return GPIO_PIN_RESET;
  }

  return HAL_GPIO_ReadPin(relay_ports[channel - 1U], relay_pins[channel - 1U]);
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

  return bits;
}

HAL_StatusTypeDef board_relay_apply_mask(uint32_t mask)
{
  uint8_t channel;

  for (channel = 1U; channel <= BOARD_RELAY_CHANNEL_COUNT; channel++)
  {
    if (board_relay_set(channel, (mask & (1UL << (channel - 1U))) != 0U ? GPIO_PIN_SET : GPIO_PIN_RESET) != HAL_OK)
    {
      return HAL_ERROR;
    }
  }

  return HAL_OK;
}
