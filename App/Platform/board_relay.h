#ifndef BOARD_RELAY_H
#define BOARD_RELAY_H

#include "main.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOARD_RELAY_CHANNEL_COUNT 7U

HAL_StatusTypeDef board_relay_set(uint8_t channel, GPIO_PinState state);
GPIO_PinState board_relay_get(uint8_t channel);
uint32_t board_relay_snapshot(void);
HAL_StatusTypeDef board_relay_apply_mask(uint32_t mask);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_RELAY_H */
