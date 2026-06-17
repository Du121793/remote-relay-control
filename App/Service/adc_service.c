#include "adc_service.h"

#include "ct_measurement.h"
#include "control_queue.h"
#include "../Config/device_config.h"

#include "FreeRTOS.h"
#include "task.h"

#include <string.h>

#define ADC_SERVICE_SAMPLE_RATE_HZ        2000U
#define ADC_SERVICE_WINDOW_SAMPLES        400U
#define ADC_SERVICE_ADC1_CONVERSIONS      9U    /* 8 CT + 1 VREFINT */
#define ADC_SERVICE_ADC3_CONVERSIONS      8U    /* 8 CT, no VREFINT */
#define ADC_SERVICE_ADC1_DMA_SAMPLES      1800U /* 200 × 9, double-buffered */
#define ADC_SERVICE_ADC3_DMA_SAMPLES      1600U /* 200 × 8 */
#define ADC_SERVICE_ADC1_HALF_SAMPLES     (ADC_SERVICE_ADC1_DMA_SAMPLES / 2U)
#define ADC_SERVICE_ADC3_HALF_SAMPLES     (ADC_SERVICE_ADC3_DMA_SAMPLES / 2U)
#define ADC_SERVICE_ADC1_CT_OFFSET        0U
#define ADC_SERVICE_ADC1_VREF_RANK        8U    /* 0-based: rank 8 = the 9th slot */
#define ADC_SERVICE_DMA_HALF_FIRST        0x01U
#define ADC_SERVICE_DMA_HALF_SECOND       0x02U
#define ADC_SERVICE_VREF_RAW_MIN          1000U
#define ADC_SERVICE_VREF_RAW_MAX          2000U

/* Physical-ADC to software-channel mapping table.
   ADC1 ranks 0-7 → software channels 0-7
   ADC3 ranks 0-7 → software channels 8-15 */
static const uint8_t g_adc1_to_channel[8] = {0, 1, 2, 3, 4, 5, 6, 7};
static const uint8_t g_adc3_to_channel[8] = {8, 9, 10, 11, 12, 13, 14, 15};

static ADC_HandleTypeDef *g_adc1_handle;
static ADC_HandleTypeDef *g_adc3_handle;
static TIM_HandleTypeDef *g_timer_handle;
static adc_service_state_t g_adc_state;
static adc_service_channel_state_t
  g_adc_channels[ADC_SERVICE_CHANNEL_COUNT];
static ct_measurement_window_t g_adc_windows[ADC_SERVICE_CHANNEL_COUNT];
static ct_overcurrent_state_t
  g_adc_protection_state[DEVICE_CONFIG_RELAY_COUNT];
static uint64_t g_adc_vref_sum;
static uint32_t g_adc_vref_count;
static volatile uint8_t g_adc1_dma_pending;
static volatile uint8_t g_adc3_dma_pending;
static volatile uint8_t g_adc1_hal_error_pending;
static volatile uint8_t g_adc3_hal_error_pending;
static volatile uint32_t g_adc1_dma_overrun_count;
static volatile uint32_t g_adc3_dma_overrun_count;
static volatile uint32_t g_adc1_hal_error_count;
static volatile uint32_t g_adc3_hal_error_count;
static uint16_t g_adc1_dma_buffer[ADC_SERVICE_ADC1_DMA_SAMPLES]
  __attribute__((aligned(32)));
static uint16_t g_adc3_dma_buffer[ADC_SERVICE_ADC3_DMA_SAMPLES]
  __attribute__((aligned(32)));

static void adc_service_note_dma1_half(uint8_t half)
{
  if ((g_adc1_dma_pending & half) != 0U) { g_adc1_dma_overrun_count++; }
  g_adc1_dma_pending |= half;
}

static void adc_service_note_dma3_half(uint8_t half)
{
  if ((g_adc3_dma_pending & half) != 0U) { g_adc3_dma_overrun_count++; }
  g_adc3_dma_pending |= half;
}

/* ── VREFINT → VDDA ────────────────────────────────────────────── */
static uint32_t adc_service_vdda_from_vref(uint16_t vref_raw)
{
  uint16_t vref_cal = *VREFINT_CAL_ADDR_CMSIS;
  if (vref_raw == 0U || vref_cal == 0U) { return CT_MEASUREMENT_REFERENCE_MV; }
  uint32_t vdda = ((uint32_t)vref_cal * CT_MEASUREMENT_REFERENCE_MV +
                   (vref_raw / 2U)) / vref_raw;
  if (vdda < CT_MEASUREMENT_VDDA_MIN_MV || vdda > CT_MEASUREMENT_VDDA_MAX_MV)
    return CT_MEASUREMENT_REFERENCE_MV;
  return vdda;
}

/* ── Process one ADC1 half-buffer ───────────────────────────────── */
static void adc_service_process_adc1(const uint16_t *samples, uint32_t count)
{
  uint32_t i;
  if (samples == NULL) return;
  for (i = 0U; (i + ADC_SERVICE_ADC1_CONVERSIONS) <= count;
       i += ADC_SERVICE_ADC1_CONVERSIONS)
  {
    uint32_t ch;
    /* Feed CT samples to their respective per-channel windows */
    for (ch = 0U; ch < 8U; ++ch)
    {
      uint8_t sw_ch = g_adc1_to_channel[ch];
      ct_measurement_window_add(&g_adc_windows[sw_ch],
                                samples[i + ch]);
    }
    /* Accumulate VREFINT */
    g_adc_vref_sum += samples[i + ADC_SERVICE_ADC1_VREF_RANK];
    g_adc_vref_count++;
  }
}

/* ── Process one ADC3 half-buffer ───────────────────────────────── */
static void adc_service_process_adc3(const uint16_t *samples, uint32_t count)
{
  uint32_t i;
  if (samples == NULL) return;
  for (i = 0U; (i + ADC_SERVICE_ADC3_CONVERSIONS) <= count;
       i += ADC_SERVICE_ADC3_CONVERSIONS)
  {
    uint32_t ch;
    for (ch = 0U; ch < 8U; ++ch)
    {
      uint8_t sw_ch = g_adc3_to_channel[ch];
      ct_measurement_window_add(&g_adc_windows[sw_ch],
                                samples[i + ch]);
    }
  }
}

/* ── Publish one CT window ──────────────────────────────────────── */
static void adc_service_publish_channel(uint8_t sw_ch, uint32_t vdda_mv,
                                         uint32_t now_ms)
{
  ct_measurement_result_t result;
  adc_service_channel_state_t *ch_state = &g_adc_channels[sw_ch];

  ct_measurement_calculate_with_reference(&g_adc_windows[sw_ch],
                                          ADC_SERVICE_WINDOW_SAMPLES,
                                          vdda_mv, &result);

  taskENTER_CRITICAL();
  ch_state->available = 1U;
  ch_state->valid = result.valid;
  ch_state->ct_valid = result.ct_valid;
  ch_state->signal_present = result.signal_present;
  ch_state->raw_latest = result.raw_latest;
  ch_state->raw_min = result.raw_minimum;
  ch_state->raw_max = result.raw_maximum;
  ch_state->raw_average = result.raw_average;
  ch_state->raw_rms = result.raw_rms;
  ch_state->millivolts = result.bias_millivolts;
  ch_state->ac_rms_millivolts = result.ac_rms_millivolts;
  ch_state->current_milliamps = result.current_milliamps;
  ch_state->sample_count += result.sample_count;
  ch_state->batch_count++;
  ch_state->saturation_count += result.saturation_count;
  ch_state->last_sample_ms = now_ms;
  taskEXIT_CRITICAL();

  ct_measurement_window_reset(&g_adc_windows[sw_ch]);

  /* Overcurrent check — submit shutdown via control_queue */
  {
    const device_config_t *config = device_config_get();
    const device_config_current_t *current = &config->current[sw_ch];
    /* ADC channels are hard-bound to relays: relay N → ADC(N-1).
       adc_channel in flash config is ignored for channel routing. */
    uint8_t prot_en = ((result.ct_valid != 0U) &&
                       (current->protection_enabled != 0U)) ? 1U : 0U;
    if (ct_overcurrent_update(&g_adc_protection_state[sw_ch],
                              prot_en, result.current_milliamps,
                              current->overcurrent_milliamps,
                              current->trip_delay_ms,
                              current->reset_hysteresis_milliamps,
                              now_ms) != 0U)
    {
      control_proto_command_t cmd;
      memset(&cmd, 0, sizeof(cmd));
      cmd.type = CONTROL_PROTO_CMD_SET_RELAY;
      cmd.channel = (uint8_t)(sw_ch + 1U);
      cmd.value = 0U;
      cmd.error = CONTROL_PROTO_ERR_NONE;
      (void)control_queue_submit_async(&cmd);
    }
  }
}

/* ── Publish all completed windows ─────────────────────────────── */
static void adc_service_publish_all(void)
{
  uint32_t vdda_mv = CT_MEASUREMENT_REFERENCE_MV;
  uint32_t now_ms = HAL_GetTick();
  uint8_t vref_valid = 0U;
  uint32_t ch;

  if (g_adc_vref_count != 0U)
  {
    uint16_t vref_avg = (uint16_t)((g_adc_vref_sum +
                                    (g_adc_vref_count / 2U)) /
                                   g_adc_vref_count);
    if (vref_avg >= ADC_SERVICE_VREF_RAW_MIN &&
        vref_avg <= ADC_SERVICE_VREF_RAW_MAX)
    {
      vdda_mv = adc_service_vdda_from_vref(vref_avg);
      vref_valid = 1U;
    }
  }

  for (ch = 0U; ch < ADC_SERVICE_CHANNEL_COUNT; ++ch)
  {
    if (g_adc_windows[ch].count >= ADC_SERVICE_WINDOW_SAMPLES)
    {
      adc_service_publish_channel(ch, vdda_mv, now_ms);
    }
  }

  /* Update aggregate state from channel 0 for backward compatibility */
  taskENTER_CRITICAL();
  g_adc_state.valid = g_adc_channels[0].valid;
  g_adc_state.ct_valid = g_adc_channels[0].ct_valid;
  g_adc_state.signal_present = g_adc_channels[0].signal_present;
  g_adc_state.vref_valid = vref_valid;
  g_adc_state.vref_raw_average = (uint16_t)(g_adc_vref_count > 0U ?
    (g_adc_vref_sum / g_adc_vref_count) : 0U);
  g_adc_state.vdda_millivolts = vdda_mv;
  g_adc_state.raw_latest = g_adc_channels[0].raw_latest;
  g_adc_state.raw_min = g_adc_channels[0].raw_min;
  g_adc_state.raw_max = g_adc_channels[0].raw_max;
  g_adc_state.raw_average = g_adc_channels[0].raw_average;
  g_adc_state.raw_rms = g_adc_channels[0].raw_rms;
  g_adc_state.millivolts = g_adc_channels[0].millivolts;
  g_adc_state.ac_rms_millivolts = g_adc_channels[0].ac_rms_millivolts;
  g_adc_state.current_milliamps = g_adc_channels[0].current_milliamps;
  g_adc_state.sample_count = g_adc_channels[0].sample_count;
  g_adc_state.batch_count = g_adc_channels[0].batch_count;
  g_adc_state.saturation_count = g_adc_channels[0].saturation_count;
  g_adc_state.last_sample_ms = g_adc_channels[0].last_sample_ms;
  g_adc_state.last_hal_status = HAL_OK;
  taskEXIT_CRITICAL();

  g_adc_vref_sum = 0U;
  g_adc_vref_count = 0U;
}

/* ── Init ───────────────────────────────────────────────────────── */
void adc_service_init(ADC_HandleTypeDef *adc1, ADC_HandleTypeDef *adc3,
                      TIM_HandleTypeDef *timer)
{
  HAL_StatusTypeDef s;

  memset(&g_adc_state, 0, sizeof(g_adc_state));
  memset(g_adc_channels, 0, sizeof(g_adc_channels));
  memset(g_adc_windows, 0, sizeof(g_adc_windows));
  memset(g_adc_protection_state, 0, sizeof(g_adc_protection_state));
  g_adc_vref_sum = 0U; g_adc_vref_count = 0U;
  g_adc1_handle = adc1; g_adc3_handle = adc3; g_timer_handle = timer;
  g_adc1_dma_pending = 0U; g_adc3_dma_pending = 0U;
  g_adc1_hal_error_pending = 0U; g_adc3_hal_error_pending = 0U;
  g_adc1_dma_overrun_count = 0U; g_adc3_dma_overrun_count = 0U;
  g_adc1_hal_error_count = 0U; g_adc3_hal_error_count = 0U;
  g_adc_state.sample_rate_hz = ADC_SERVICE_SAMPLE_RATE_HZ;
  g_adc_state.window_samples = ADC_SERVICE_WINDOW_SAMPLES;
  g_adc_state.channel_count = ADC_SERVICE_CHANNEL_COUNT;
  g_adc_state.available_mask = 0xFFFFUL; /* all 16 channels */
  g_adc_state.vdda_millivolts = CT_MEASUREMENT_REFERENCE_MV;

  if (g_adc1_handle == NULL || g_adc3_handle == NULL || g_timer_handle == NULL)
  { g_adc_state.last_hal_status = HAL_ERROR; g_adc_state.error_count++; return; }

  /* Start TIM2 first for clean trigger */
  s = HAL_TIM_Base_Start(g_timer_handle);
  if (s == HAL_OK) s = HAL_ADC_Start_DMA(g_adc1_handle,
    (uint32_t *)g_adc1_dma_buffer, ADC_SERVICE_ADC1_DMA_SAMPLES);
  if (s == HAL_OK) s = HAL_ADC_Start_DMA(g_adc3_handle,
    (uint32_t *)g_adc3_dma_buffer, ADC_SERVICE_ADC3_DMA_SAMPLES);
  if (s != HAL_OK)
  {
    (void)HAL_ADC_Stop_DMA(g_adc1_handle);
    (void)HAL_ADC_Stop_DMA(g_adc3_handle);
    g_adc_state.last_hal_status = s;
    g_adc_state.error_count++;
    return;
  }
  for (uint32_t ch = 0U; ch < ADC_SERVICE_CHANNEL_COUNT; ++ch)
    ct_measurement_window_reset(&g_adc_windows[ch]);
  g_adc_state.initialized = 1U;
  g_adc_state.last_hal_status = HAL_OK;
}

/* ── Poll ───────────────────────────────────────────────────────── */
void adc_service_poll(void)
{
  uint8_t p1, p3;
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  p1 = g_adc1_dma_pending; g_adc1_dma_pending = 0U;
  p3 = g_adc3_dma_pending; g_adc3_dma_pending = 0U;
  if (primask == 0U) __enable_irq();

  if ((p1 & ADC_SERVICE_DMA_HALF_FIRST) != 0U)
    adc_service_process_adc1(&g_adc1_dma_buffer[0], ADC_SERVICE_ADC1_HALF_SAMPLES);
  if ((p1 & ADC_SERVICE_DMA_HALF_SECOND) != 0U)
    adc_service_process_adc1(&g_adc1_dma_buffer[ADC_SERVICE_ADC1_HALF_SAMPLES],
                             ADC_SERVICE_ADC1_HALF_SAMPLES);
  if ((p3 & ADC_SERVICE_DMA_HALF_FIRST) != 0U)
    adc_service_process_adc3(&g_adc3_dma_buffer[0], ADC_SERVICE_ADC3_HALF_SAMPLES);
  if ((p3 & ADC_SERVICE_DMA_HALF_SECOND) != 0U)
    adc_service_process_adc3(&g_adc3_dma_buffer[ADC_SERVICE_ADC3_HALF_SAMPLES],
                             ADC_SERVICE_ADC3_HALF_SAMPLES);

  /* Publish windows */
  {
    uint8_t need_publish = 0U; uint32_t ch;
    for (ch = 0U; ch < ADC_SERVICE_CHANNEL_COUNT; ++ch)
    { if (g_adc_windows[ch].count >= ADC_SERVICE_WINDOW_SAMPLES) { need_publish = 1U; break; } }
    if (need_publish != 0U) adc_service_publish_all();
  }

  /* Update stats */
  {
    uint32_t dma1 = g_adc1_dma_overrun_count, dma3 = g_adc3_dma_overrun_count;
    if (dma1 != g_adc_state.dma_overrun_count || dma3 != 0U ||
        g_adc1_hal_error_pending != 0U || g_adc3_hal_error_pending != 0U)
    {
      taskENTER_CRITICAL();
      g_adc_state.dma_overrun_count = dma1 + dma3;
      g_adc_state.error_count = dma1 + dma3 + g_adc1_hal_error_count + g_adc3_hal_error_count;
      taskEXIT_CRITICAL();
    }
  }
}

void adc_service_get_state(adc_service_state_t *state)
{
  if (state) { taskENTER_CRITICAL(); *state = g_adc_state; taskEXIT_CRITICAL(); }
}

uint8_t adc_service_get_channel_state(uint8_t channel, adc_service_channel_state_t *state)
{
  if (!state || channel >= ADC_SERVICE_CHANNEL_COUNT) return 0U;
  taskENTER_CRITICAL(); *state = g_adc_channels[channel]; taskEXIT_CRITICAL();
  return state->available;
}

uint8_t adc_service_get_protection_state(uint8_t relay_ch, adc_service_protection_state_t *state)
{
  if (!state || relay_ch == 0U || relay_ch > DEVICE_CONFIG_RELAY_COUNT) return 0U;
  const ct_overcurrent_state_t *src = &g_adc_protection_state[relay_ch - 1U];
  taskENTER_CRITICAL();
  state->above_threshold = src->above_threshold; state->tripped = src->tripped;
  state->threshold_since_ms = src->threshold_since_ms;
  state->trip_count = src->trip_count; state->last_trip_ms = src->last_trip_ms;
  taskEXIT_CRITICAL(); return 1U;
}

/* ── HAL Callbacks ──────────────────────────────────────────────── */
void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *hadc)
{
  if (hadc == g_adc1_handle) adc_service_note_dma1_half(ADC_SERVICE_DMA_HALF_FIRST);
  if (hadc == g_adc3_handle) adc_service_note_dma3_half(ADC_SERVICE_DMA_HALF_FIRST);
}
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
  if (hadc == g_adc1_handle) adc_service_note_dma1_half(ADC_SERVICE_DMA_HALF_SECOND);
  if (hadc == g_adc3_handle) adc_service_note_dma3_half(ADC_SERVICE_DMA_HALF_SECOND);
}
void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *hadc)
{
  uint32_t primask = __get_PRIMASK(); __disable_irq();
  if (hadc == g_adc1_handle) { g_adc1_hal_error_count++; g_adc1_hal_error_pending = 1U; }
  if (hadc == g_adc3_handle) { g_adc3_hal_error_count++; g_adc3_hal_error_pending = 1U; }
  if (primask == 0U) __enable_irq();
}
