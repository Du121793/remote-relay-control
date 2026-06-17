#ifndef ADC_SERVICE_H
#define ADC_SERVICE_H

#include "main.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ADC_SERVICE_CHANNEL_COUNT 16U

typedef struct
{
  uint8_t available;
  uint8_t valid;
  uint8_t ct_valid;
  uint8_t signal_present;
  uint16_t raw_latest;
  uint16_t raw_min;
  uint16_t raw_max;
  uint16_t raw_average;
  uint16_t raw_rms;
  uint16_t millivolts;
  uint16_t ac_rms_millivolts;
  uint32_t current_milliamps;
  uint32_t sample_count;
  uint32_t batch_count;
  uint32_t saturation_count;
  uint32_t last_sample_ms;
} adc_service_channel_state_t;

typedef struct
{
  uint8_t above_threshold;
  uint8_t tripped;
  uint16_t reserved;
  uint32_t threshold_since_ms;
  uint32_t trip_count;
  uint32_t last_trip_ms;
} adc_service_protection_state_t;

typedef struct
{
  uint8_t initialized;
  uint8_t valid;
  uint8_t ct_valid;
  uint8_t signal_present;
  uint8_t channel_count;
  uint8_t vref_valid;
  uint16_t vref_raw_average;
  uint32_t available_mask;
  uint32_t vdda_millivolts;
  uint16_t raw_latest;
  uint16_t raw_min;
  uint16_t raw_max;
  uint16_t raw_average;
  uint16_t raw_rms;
  uint16_t millivolts;
  uint16_t ac_rms_millivolts;
  uint32_t current_milliamps;
  uint32_t sample_rate_hz;
  uint32_t window_samples;
  uint32_t sample_count;
  uint32_t batch_count;
  uint32_t error_count;
  uint32_t dma_overrun_count;
  uint32_t saturation_count;
  uint32_t last_sample_ms;
  uint32_t last_hal_status;
} adc_service_state_t;

void adc_service_init(ADC_HandleTypeDef *adc1, ADC_HandleTypeDef *adc3, TIM_HandleTypeDef *timer);
void adc_service_poll(void);
void adc_service_get_state(adc_service_state_t *state);
uint8_t adc_service_get_channel_state(uint8_t channel,
                                      adc_service_channel_state_t *state);
uint8_t adc_service_get_protection_state(
  uint8_t relay_channel,
  adc_service_protection_state_t *state);

#ifdef __cplusplus
}
#endif

#endif /* ADC_SERVICE_H */
