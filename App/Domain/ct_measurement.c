#include "ct_measurement.h"

#include <string.h>

static uint32_t ct_measurement_integer_sqrt(uint64_t value)
{
  uint64_t bit = (uint64_t)1U << 62U;
  uint64_t result = 0U;

  while (bit > value)
  {
    bit >>= 2U;
  }

  while (bit != 0U)
  {
    if (value >= (result + bit))
    {
      value -= result + bit;
      result = (result >> 1U) + bit;
    }
    else
    {
      result >>= 1U;
    }
    bit >>= 2U;
  }

  return (uint32_t)result;
}

static uint16_t ct_measurement_raw_to_millivolts(uint32_t raw,
                                                 uint32_t reference_millivolts)
{
  return (uint16_t)(((raw * reference_millivolts) +
                     (CT_MEASUREMENT_ADC_FULL_SCALE / 2U)) /
                    CT_MEASUREMENT_ADC_FULL_SCALE);
}

uint32_t ct_measurement_calculate_vdda_mv(uint16_t vref_raw,
                                         uint16_t vref_cal_raw)
{
  if ((vref_raw == 0U) || (vref_cal_raw == 0U))
  {
    return 0U;
  }

  return (((uint32_t)vref_cal_raw * CT_MEASUREMENT_REFERENCE_MV) +
          (vref_raw / 2U)) / vref_raw;
}

void ct_measurement_window_reset(ct_measurement_window_t *window)
{
  if (window == NULL)
  {
    return;
  }

  memset(window, 0, sizeof(*window));
  window->minimum = CT_MEASUREMENT_ADC_FULL_SCALE;
}

void ct_measurement_window_add(ct_measurement_window_t *window, uint16_t raw)
{
  if (window == NULL)
  {
    return;
  }

  if (raw > CT_MEASUREMENT_ADC_FULL_SCALE)
  {
    raw = CT_MEASUREMENT_ADC_FULL_SCALE;
  }

  window->latest = raw;
  if (raw < window->minimum)
  {
    window->minimum = raw;
  }
  if (raw > window->maximum)
  {
    window->maximum = raw;
  }
  if ((raw <= CT_MEASUREMENT_SATURATION_MARGIN) ||
      (raw >= (CT_MEASUREMENT_ADC_FULL_SCALE - CT_MEASUREMENT_SATURATION_MARGIN)))
  {
    window->saturation_count++;
  }
  window->sum += raw;
  window->sum_squares += (uint64_t)raw * raw;
  window->count++;
}

void ct_measurement_calculate_with_reference(
  const ct_measurement_window_t *window,
  uint32_t expected_samples,
  uint32_t reference_millivolts,
  ct_measurement_result_t *result)
{
  uint64_t variance_numerator;
  uint32_t rms_numerator;
  uint32_t raw_average;
  uint32_t raw_rms;

  if (result == NULL)
  {
    return;
  }

  memset(result, 0, sizeof(*result));
  if ((window == NULL) || (window->count == 0U) ||
      (reference_millivolts < CT_MEASUREMENT_VDDA_MIN_MV) ||
      (reference_millivolts > CT_MEASUREMENT_VDDA_MAX_MV))
  {
    return;
  }

  raw_average = (uint32_t)((window->sum + (window->count / 2U)) / window->count);
  variance_numerator =
    (window->sum_squares * window->count) - (window->sum * window->sum);
  rms_numerator = ct_measurement_integer_sqrt(variance_numerator);
  raw_rms = (rms_numerator + (window->count / 2U)) / window->count;

  result->valid = ((expected_samples == 0U) || (window->count == expected_samples)) ? 1U : 0U;
  result->raw_latest = window->latest;
  result->raw_minimum = window->minimum;
  result->raw_maximum = window->maximum;
  result->raw_average = (uint16_t)raw_average;
  result->raw_rms = (uint16_t)raw_rms;
  result->bias_millivolts =
    ct_measurement_raw_to_millivolts(raw_average, reference_millivolts);
  result->ac_rms_millivolts =
    ct_measurement_raw_to_millivolts(raw_rms, reference_millivolts);
  result->sample_count = window->count;
  result->saturation_count = window->saturation_count;
  result->ct_valid =
    ((result->valid != 0U) &&
     (result->bias_millivolts >= CT_MEASUREMENT_BIAS_MIN_MV) &&
     (result->bias_millivolts <= CT_MEASUREMENT_BIAS_MAX_MV) &&
     (result->saturation_count == 0U)) ? 1U : 0U;

  /* Only report current when CT measurement is fully valid.
     ct_valid requires: correct sample count, bias in range, no ADC saturation. */
  if ((result->ct_valid != 0U) &&
      (result->ac_rms_millivolts >= CT_MEASUREMENT_ZERO_THRESHOLD_MV))
  {
    result->signal_present = 1U;
    result->current_milliamps =
      (uint32_t)result->ac_rms_millivolts * CT_MEASUREMENT_MA_PER_MV;
  }
}

void ct_measurement_calculate(const ct_measurement_window_t *window,
                              uint32_t expected_samples,
                              ct_measurement_result_t *result)
{
  ct_measurement_calculate_with_reference(window,
                                          expected_samples,
                                          CT_MEASUREMENT_REFERENCE_MV,
                                          result);
}

uint8_t ct_overcurrent_update(ct_overcurrent_state_t *state,
                              uint8_t enabled,
                              uint32_t current_milliamps,
                              uint32_t threshold_milliamps,
                              uint32_t trip_delay_ms,
                              uint32_t reset_hysteresis_milliamps,
                              uint32_t now_ms)
{
  uint32_t reset_threshold;

  if (state == NULL)
  {
    return 0U;
  }

  if ((enabled == 0U) || (threshold_milliamps == 0U))
  {
    state->above_threshold = 0U;
    state->tripped = 0U;
    state->threshold_since_ms = 0U;
    return 0U;
  }

  reset_threshold = (reset_hysteresis_milliamps < threshold_milliamps)
                    ? (threshold_milliamps - reset_hysteresis_milliamps)
                    : 0U;
  if (current_milliamps < reset_threshold)
  {
    state->above_threshold = 0U;
    state->tripped = 0U;
    state->threshold_since_ms = 0U;
    return 0U;
  }

  if (current_milliamps < threshold_milliamps)
  {
    state->above_threshold = 0U;
    state->threshold_since_ms = 0U;
    return 0U;
  }

  if (state->tripped != 0U)
  {
    return 0U;
  }

  if (state->above_threshold == 0U)
  {
    state->above_threshold = 1U;
    state->threshold_since_ms = now_ms;
  }

  if ((trip_delay_ms == 0U) ||
      ((uint32_t)(now_ms - state->threshold_since_ms) >= trip_delay_ms))
  {
    state->tripped = 1U;
    state->trip_count++;
    state->last_trip_ms = now_ms;
    return 1U;
  }

  return 0U;
}
