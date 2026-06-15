#ifndef CT_MEASUREMENT_H
#define CT_MEASUREMENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CT_MEASUREMENT_REFERENCE_MV       3300U
#define CT_MEASUREMENT_ADC_FULL_SCALE     4095U
#define CT_MEASUREMENT_MA_PER_MV          20U
#define CT_MEASUREMENT_BIAS_MIN_MV        1400U
#define CT_MEASUREMENT_BIAS_MAX_MV        1900U
#define CT_MEASUREMENT_ZERO_THRESHOLD_MV  50U
#define CT_MEASUREMENT_SATURATION_MARGIN  16U
#define CT_MEASUREMENT_VDDA_MIN_MV         2700U
#define CT_MEASUREMENT_VDDA_MAX_MV         3600U

typedef struct
{
  uint64_t sum;
  uint64_t sum_squares;
  uint32_t count;
  uint32_t saturation_count;
  uint16_t latest;
  uint16_t minimum;
  uint16_t maximum;
} ct_measurement_window_t;

typedef struct
{
  uint8_t valid;
  uint8_t ct_valid;
  uint8_t signal_present;
  uint16_t raw_latest;
  uint16_t raw_minimum;
  uint16_t raw_maximum;
  uint16_t raw_average;
  uint16_t raw_rms;
  uint16_t bias_millivolts;
  uint16_t ac_rms_millivolts;
  uint32_t current_milliamps;
  uint32_t sample_count;
  uint32_t saturation_count;
} ct_measurement_result_t;

typedef struct
{
  uint8_t above_threshold;
  uint8_t tripped;
  uint16_t reserved;
  uint32_t threshold_since_ms;
  uint32_t trip_count;
  uint32_t last_trip_ms;
} ct_overcurrent_state_t;

void ct_measurement_window_reset(ct_measurement_window_t *window);
void ct_measurement_window_add(ct_measurement_window_t *window, uint16_t raw);
uint32_t ct_measurement_calculate_vdda_mv(uint16_t vref_raw,
                                         uint16_t vref_cal_raw);
void ct_measurement_calculate_with_reference(
  const ct_measurement_window_t *window,
  uint32_t expected_samples,
  uint32_t reference_millivolts,
  ct_measurement_result_t *result);
void ct_measurement_calculate(const ct_measurement_window_t *window,
                              uint32_t expected_samples,
                              ct_measurement_result_t *result);
uint8_t ct_overcurrent_update(ct_overcurrent_state_t *state,
                              uint8_t enabled,
                              uint32_t current_milliamps,
                              uint32_t threshold_milliamps,
                              uint32_t trip_delay_ms,
                              uint32_t reset_hysteresis_milliamps,
                              uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* CT_MEASUREMENT_H */
