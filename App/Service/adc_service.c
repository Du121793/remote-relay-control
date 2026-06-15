#include "adc_service.h"

#include "ct_measurement.h"
#include "control_queue.h"
#include "../Config/device_config.h"

#include "FreeRTOS.h"
#include "task.h"

#include <string.h>

#define ADC_SERVICE_SAMPLE_RATE_HZ      2000U
#define ADC_SERVICE_WINDOW_SAMPLES      400U
#define ADC_SERVICE_DMA_BUFFER_SAMPLES  1600U
#define ADC_SERVICE_DMA_HALF_SAMPLES    (ADC_SERVICE_DMA_BUFFER_SAMPLES / 2U)
#define ADC_SERVICE_SEQUENCE_SAMPLES    2U
#define ADC_SERVICE_DMA_HALF_FIRST      0x01U
#define ADC_SERVICE_DMA_HALF_SECOND     0x02U
#define ADC_SERVICE_VREF_RAW_MIN        1000U
#define ADC_SERVICE_VREF_RAW_MAX        2000U

#if ADC_SERVICE_DMA_HALF_SAMPLES != \
    (ADC_SERVICE_WINDOW_SAMPLES * ADC_SERVICE_SEQUENCE_SAMPLES)
#error "ADC DMA half-buffer must contain exactly one CT/VREF measurement window"
#endif

static ADC_HandleTypeDef *g_adc_handle;
static TIM_HandleTypeDef *g_timer_handle;
static adc_service_state_t g_adc_state;
static adc_service_channel_state_t
  g_adc_channels[ADC_SERVICE_CHANNEL_COUNT];
static ct_measurement_window_t g_adc_window;
static ct_overcurrent_state_t
  g_adc_protection_state[DEVICE_CONFIG_RELAY_COUNT];
static uint64_t g_adc_vref_sum;
static uint32_t g_adc_vref_count;
static volatile uint8_t g_adc_dma_pending;
static volatile uint8_t g_adc_hal_error_pending;
static volatile uint32_t g_adc_dma_overrun_count;
static volatile uint32_t g_adc_hal_error_count;
static uint16_t g_adc_dma_buffer[ADC_SERVICE_DMA_BUFFER_SAMPLES]
  __attribute__((aligned(32)));

static void adc_service_note_dma_half(uint8_t half)
{
  if ((g_adc_dma_pending & half) != 0U)
  {
    g_adc_dma_overrun_count++;
  }
  g_adc_dma_pending |= half;
}

static void adc_service_publish_window(void)
{
  ct_measurement_result_t result;
  adc_service_channel_state_t *channel = &g_adc_channels[0];
  uint32_t vdda_millivolts = CT_MEASUREMENT_REFERENCE_MV;
  uint16_t vref_raw_average = 0U;
  uint8_t vref_valid = 0U;
  uint32_t now_ms = HAL_GetTick();
  uint32_t relay_index;

  if (g_adc_vref_count != 0U)
  {
    uint16_t vref_cal_raw = *VREFINT_CAL_ADDR_CMSIS;

    vref_raw_average =
      (uint16_t)((g_adc_vref_sum + (g_adc_vref_count / 2U)) /
                 g_adc_vref_count);
    if ((vref_raw_average >= ADC_SERVICE_VREF_RAW_MIN) &&
        (vref_raw_average <= ADC_SERVICE_VREF_RAW_MAX))
    {
      uint32_t measured_vdda =
        ct_measurement_calculate_vdda_mv(vref_raw_average, vref_cal_raw);

      if ((measured_vdda >= CT_MEASUREMENT_VDDA_MIN_MV) &&
          (measured_vdda <= CT_MEASUREMENT_VDDA_MAX_MV))
      {
        vdda_millivolts = measured_vdda;
        vref_valid = 1U;
      }
    }
  }

  ct_measurement_calculate_with_reference(&g_adc_window,
                                          ADC_SERVICE_WINDOW_SAMPLES,
                                          vdda_millivolts,
                                          &result);

  taskENTER_CRITICAL();
  channel->available = 1U;
  channel->valid = result.valid;
  channel->ct_valid = result.ct_valid;
  channel->signal_present = result.signal_present;
  channel->raw_latest = result.raw_latest;
  channel->raw_min = result.raw_minimum;
  channel->raw_max = result.raw_maximum;
  channel->raw_average = result.raw_average;
  channel->raw_rms = result.raw_rms;
  channel->millivolts = result.bias_millivolts;
  channel->ac_rms_millivolts = result.ac_rms_millivolts;
  channel->current_milliamps = result.current_milliamps;
  channel->sample_count += result.sample_count;
  channel->batch_count++;
  channel->saturation_count += result.saturation_count;
  channel->last_sample_ms = now_ms;
  g_adc_state.valid = result.valid;
  g_adc_state.ct_valid = result.ct_valid;
  g_adc_state.signal_present = result.signal_present;
  g_adc_state.vref_valid = vref_valid;
  g_adc_state.vref_raw_average = vref_raw_average;
  g_adc_state.vdda_millivolts = vdda_millivolts;
  g_adc_state.raw_latest = result.raw_latest;
  g_adc_state.raw_min = result.raw_minimum;
  g_adc_state.raw_max = result.raw_maximum;
  g_adc_state.raw_average = result.raw_average;
  g_adc_state.raw_rms = result.raw_rms;
  g_adc_state.millivolts = result.bias_millivolts;
  g_adc_state.ac_rms_millivolts = result.ac_rms_millivolts;
  g_adc_state.current_milliamps = result.current_milliamps;
  g_adc_state.sample_count += result.sample_count;
  g_adc_state.batch_count++;
  g_adc_state.saturation_count += result.saturation_count;
  g_adc_state.dma_overrun_count = g_adc_dma_overrun_count;
  g_adc_state.last_sample_ms = now_ms;
  g_adc_state.last_hal_status = HAL_OK;
  taskEXIT_CRITICAL();

  for (relay_index = 0U;
       relay_index < DEVICE_CONFIG_RELAY_COUNT;
       ++relay_index)
  {
    const device_config_t *config = device_config_get();
    const device_config_current_t *current = &config->current[relay_index];
    uint8_t should_trip;
    uint8_t protection_enabled =
      ((current->adc_channel == 0U) &&
       (result.ct_valid != 0U) &&
       (current->protection_enabled != 0U)) ? 1U : 0U;

    taskENTER_CRITICAL();
    should_trip = ct_overcurrent_update(
      &g_adc_protection_state[relay_index],
      protection_enabled,
      result.current_milliamps,
      current->overcurrent_milliamps,
      current->trip_delay_ms,
      current->reset_hysteresis_milliamps,
      now_ms);
    taskEXIT_CRITICAL();
    if (should_trip != 0U)
    {
      control_proto_command_t command;

      memset(&command, 0, sizeof(command));
      command.type = CONTROL_PROTO_CMD_SET_RELAY;
      command.channel = (uint8_t)(relay_index + 1U);
      command.value = 0U;
      command.error = CONTROL_PROTO_ERR_NONE;
      (void)control_queue_submit_async(&command);
    }
  }

  ct_measurement_window_reset(&g_adc_window);
  g_adc_vref_sum = 0U;
  g_adc_vref_count = 0U;
}

static void adc_service_process_samples(const uint16_t *samples, uint32_t count)
{
  uint32_t index;

  if (samples == NULL)
  {
    return;
  }

  for (index = 0U;
       (index + (ADC_SERVICE_SEQUENCE_SAMPLES - 1U)) < count;
       index += ADC_SERVICE_SEQUENCE_SAMPLES)
  {
    ct_measurement_window_add(&g_adc_window, samples[index]);
    g_adc_vref_sum += samples[index + 1U];
    g_adc_vref_count++;
    if (g_adc_window.count >= ADC_SERVICE_WINDOW_SAMPLES)
    {
      adc_service_publish_window();
    }
  }
}

void adc_service_init(ADC_HandleTypeDef *adc_handle, TIM_HandleTypeDef *timer_handle)
{
  HAL_StatusTypeDef status;

  memset(&g_adc_state, 0, sizeof(g_adc_state));
  memset(g_adc_channels, 0, sizeof(g_adc_channels));
  memset(g_adc_dma_buffer, 0, sizeof(g_adc_dma_buffer));
  ct_measurement_window_reset(&g_adc_window);
  memset(g_adc_protection_state, 0, sizeof(g_adc_protection_state));
  g_adc_vref_sum = 0U;
  g_adc_vref_count = 0U;
  g_adc_handle = adc_handle;
  g_timer_handle = timer_handle;
  g_adc_dma_pending = 0U;
  g_adc_hal_error_pending = 0U;
  g_adc_dma_overrun_count = 0U;
  g_adc_hal_error_count = 0U;
  g_adc_state.sample_rate_hz = ADC_SERVICE_SAMPLE_RATE_HZ;
  g_adc_state.window_samples = ADC_SERVICE_WINDOW_SAMPLES;
  g_adc_state.channel_count = ADC_SERVICE_CHANNEL_COUNT;
  g_adc_state.available_mask = 1UL;
  g_adc_state.vdda_millivolts = CT_MEASUREMENT_REFERENCE_MV;
  g_adc_channels[0].available = 1U;

  if ((g_adc_handle == NULL) || (g_timer_handle == NULL))
  {
    g_adc_state.last_hal_status = HAL_ERROR;
    g_adc_state.error_count++;
    return;
  }

  /* Start TIM2 first so TRGO is stable before ADC DMA begins,
     avoiding spurious overrun events on the first trigger edge. */
  status = HAL_TIM_Base_Start(g_timer_handle);
  if (status == HAL_OK)
  {
    status = HAL_ADC_Start_DMA(g_adc_handle,
                               (uint32_t *)g_adc_dma_buffer,
                               ADC_SERVICE_DMA_BUFFER_SAMPLES);
  }
  if (status != HAL_OK)
  {
    (void)HAL_ADC_Stop_DMA(g_adc_handle);
    g_adc_state.last_hal_status = status;
    g_adc_state.error_count++;
    return;
  }

  g_adc_state.initialized = 1U;
  g_adc_state.last_hal_status = HAL_OK;
}

void adc_service_poll(void)
{
  uint8_t pending;
  uint8_t hal_error_pending;
  uint32_t dma_overrun_count;
  uint32_t hal_error_count;
  uint32_t primask = __get_PRIMASK();

  __disable_irq();
  pending = g_adc_dma_pending;
  g_adc_dma_pending = 0U;
  hal_error_pending = g_adc_hal_error_pending;
  g_adc_hal_error_pending = 0U;
  dma_overrun_count = g_adc_dma_overrun_count;
  hal_error_count = g_adc_hal_error_count;
  if (primask == 0U)
  {
    __enable_irq();
  }

  if ((pending & ADC_SERVICE_DMA_HALF_FIRST) != 0U)
  {
    adc_service_process_samples(&g_adc_dma_buffer[0], ADC_SERVICE_DMA_HALF_SAMPLES);
  }
  if ((pending & ADC_SERVICE_DMA_HALF_SECOND) != 0U)
  {
    adc_service_process_samples(&g_adc_dma_buffer[ADC_SERVICE_DMA_HALF_SAMPLES],
                                ADC_SERVICE_DMA_HALF_SAMPLES);
  }

  if ((dma_overrun_count != g_adc_state.dma_overrun_count) ||
      (hal_error_pending != 0U))
  {
    taskENTER_CRITICAL();
    g_adc_state.dma_overrun_count = dma_overrun_count;
    g_adc_state.error_count = dma_overrun_count + hal_error_count;
    if (hal_error_pending != 0U)
    {
      g_adc_state.last_hal_status = HAL_ERROR;
    }
    taskEXIT_CRITICAL();
  }
}

void adc_service_get_state(adc_service_state_t *state)
{
  if (state == NULL)
  {
    return;
  }

  taskENTER_CRITICAL();
  *state = g_adc_state;
  taskEXIT_CRITICAL();
}

uint8_t adc_service_get_channel_state(uint8_t channel,
                                      adc_service_channel_state_t *state)
{
  if ((state == NULL) || (channel >= ADC_SERVICE_CHANNEL_COUNT))
  {
    return 0U;
  }

  taskENTER_CRITICAL();
  *state = g_adc_channels[channel];
  taskEXIT_CRITICAL();
  return state->available;
}

uint8_t adc_service_get_protection_state(
  uint8_t relay_channel,
  adc_service_protection_state_t *state)
{
  const ct_overcurrent_state_t *source;

  if ((state == NULL) || (relay_channel == 0U) ||
      (relay_channel > DEVICE_CONFIG_RELAY_COUNT))
  {
    return 0U;
  }

  source = &g_adc_protection_state[relay_channel - 1U];
  taskENTER_CRITICAL();
  state->above_threshold = source->above_threshold;
  state->tripped = source->tripped;
  state->reserved = 0U;
  state->threshold_since_ms = source->threshold_since_ms;
  state->trip_count = source->trip_count;
  state->last_trip_ms = source->last_trip_ms;
  taskEXIT_CRITICAL();
  return 1U;
}

void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *hadc)
{
  if (hadc == g_adc_handle)
  {
    adc_service_note_dma_half(ADC_SERVICE_DMA_HALF_FIRST);
  }
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
  if (hadc == g_adc_handle)
  {
    adc_service_note_dma_half(ADC_SERVICE_DMA_HALF_SECOND);
  }
}

void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *hadc)
{
  if (hadc == g_adc_handle)
  {
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    g_adc_hal_error_count++;
    g_adc_hal_error_pending = 1U;
    if (primask == 0U)
    {
      __enable_irq();
    }
  }
}
