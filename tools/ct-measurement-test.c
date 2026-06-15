#include "ct_measurement.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_SAMPLE_RATE_HZ  2000U
#define TEST_WINDOW_SAMPLES  400U
#define TEST_LINE_HZ         50.0
#define TEST_PI              3.14159265358979323846

static void require(int condition, const char *message)
{
  if (!condition)
  {
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
  }
}

static uint16_t millivolts_to_raw(double millivolts)
{
  double raw = (millivolts * CT_MEASUREMENT_ADC_FULL_SCALE) /
               CT_MEASUREMENT_REFERENCE_MV;

  if (raw < 0.0)
  {
    raw = 0.0;
  }
  if (raw > CT_MEASUREMENT_ADC_FULL_SCALE)
  {
    raw = CT_MEASUREMENT_ADC_FULL_SCALE;
  }
  return (uint16_t)lround(raw);
}

static ct_measurement_result_t measure_sine(double bias_millivolts,
                                            double rms_millivolts)
{
  ct_measurement_window_t window;
  ct_measurement_result_t result;
  uint32_t index;
  const double peak_millivolts = rms_millivolts * sqrt(2.0);

  ct_measurement_window_reset(&window);
  for (index = 0U; index < TEST_WINDOW_SAMPLES; ++index)
  {
    const double phase = 2.0 * TEST_PI * TEST_LINE_HZ *
                         (double)index / TEST_SAMPLE_RATE_HZ;
    ct_measurement_window_add(
      &window,
      millivolts_to_raw(bias_millivolts +
                        (peak_millivolts * sin(phase))));
  }
  ct_measurement_calculate(&window, TEST_WINDOW_SAMPLES, &result);
  return result;
}

static void test_zero_current(void)
{
  const ct_measurement_result_t result = measure_sine(1650.0, 0.0);

  require(result.valid == 1U, "zero-current window must be valid");
  require(result.ct_valid == 1U, "1.65 V bias must be accepted");
  require(result.signal_present == 0U, "zero current must be below noise threshold");
  require(result.current_milliamps == 0U, "zero current must report 0 mA");
}

static void test_known_currents(void)
{
  const ct_measurement_result_t one_amp = measure_sine(1650.0, 50.0);
  const ct_measurement_result_t four_amp = measure_sine(1650.0, 200.0);

  require(one_amp.ct_valid == 1U, "1 A waveform must have valid CT bias");
  require(one_amp.ac_rms_millivolts >= 48U &&
          one_amp.ac_rms_millivolts <= 52U,
          "1 A waveform must measure about 50 mV RMS");
  require(one_amp.current_milliamps >= 960U &&
          one_amp.current_milliamps <= 1040U,
          "1 A waveform must measure about 1000 mA");

  require(four_amp.ct_valid == 1U, "4 A waveform must have valid CT bias");
  require(four_amp.ac_rms_millivolts >= 197U &&
          four_amp.ac_rms_millivolts <= 203U,
          "4 A waveform must measure about 200 mV RMS");
  require(four_amp.current_milliamps >= 3940U &&
          four_amp.current_milliamps <= 4060U,
          "4 A waveform must measure about 4000 mA");
}

static void test_invalid_input_detection(void)
{
  ct_measurement_window_t window;
  ct_measurement_result_t result;
  uint32_t index;

  result = measure_sine(900.0, 50.0);
  require(result.valid == 1U, "low-bias waveform must still be sampled");
  require(result.ct_valid == 0U, "low bias must invalidate CT measurement");
  require(result.signal_present == 0U,
          "invalid CT input must not report a signal");
  require(result.current_milliamps == 0U,
          "invalid CT input must not report current");

  ct_measurement_window_reset(&window);
  for (index = 0U; index < TEST_WINDOW_SAMPLES; ++index)
  {
    ct_measurement_window_add(&window, (index == 0U) ? 0U : 2048U);
  }
  ct_measurement_calculate(&window, TEST_WINDOW_SAMPLES, &result);
  require(result.saturation_count == 1U, "rail sample must be counted");
  require(result.ct_valid == 0U, "rail sample must invalidate CT measurement");

  ct_measurement_window_reset(&window);
  for (index = 0U; index < (TEST_WINDOW_SAMPLES - 1U); ++index)
  {
    ct_measurement_window_add(&window, 2048U);
  }
  ct_measurement_calculate(&window, TEST_WINDOW_SAMPLES, &result);
  require(result.valid == 0U, "short window must be invalid");
  require(result.ct_valid == 0U, "short window must invalidate CT measurement");
}

static void test_vref_calibration(void)
{
  require(ct_measurement_calculate_vdda_mv(1500U, 1500U) == 3300U,
          "equal VREF raw and calibration values must report 3300 mV");
  require(ct_measurement_calculate_vdda_mv(1650U, 1500U) == 3000U,
          "higher VREF raw value must report lower VDDA");
  require(ct_measurement_calculate_vdda_mv(0U, 1500U) == 0U,
          "zero VREF raw value must be rejected");
}

static void test_overcurrent_protection(void)
{
  ct_overcurrent_state_t state;

  memset(&state, 0, sizeof(state));
  require(ct_overcurrent_update(&state, 0U, 5000U, 4000U, 1000U,
                                500U, 100U) == 0U,
          "disabled protection must not trip");
  require(ct_overcurrent_update(&state, 1U, 4500U, 4000U, 1000U,
                                500U, 100U) == 0U,
          "overcurrent delay must start without tripping");
  require(ct_overcurrent_update(&state, 1U, 4500U, 4000U, 1000U,
                                500U, 1099U) == 0U,
          "overcurrent must not trip before delay expires");
  require(ct_overcurrent_update(&state, 1U, 4500U, 4000U, 1000U,
                                500U, 1100U) == 1U,
          "overcurrent must trip when delay expires");
  require(ct_overcurrent_update(&state, 1U, 4500U, 4000U, 1000U,
                                500U, 1200U) == 0U,
          "latched trip must not repeat");
  require(ct_overcurrent_update(&state, 1U, 3600U, 4000U, 1000U,
                                500U, 1300U) == 0U,
          "hysteresis band must keep the trip latched");
  require(state.tripped == 1U,
          "trip must remain latched inside hysteresis band");
  require(ct_overcurrent_update(&state, 1U, 3400U, 4000U, 1000U,
                                500U, 1400U) == 0U,
          "current below reset threshold must clear the latch");
  require(state.tripped == 0U,
          "trip latch must clear below threshold minus hysteresis");
  require(state.trip_count == 1U,
          "one overcurrent episode must count exactly one trip");
}

int main(void)
{
  test_zero_current();
  test_known_currents();
  test_invalid_input_detection();
  test_vref_calibration();
  test_overcurrent_protection();
  puts("PASS: CT conversion, VREF calibration and overcurrent protection");
  return 0;
}
