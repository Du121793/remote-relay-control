#include "adc_service.h"
#include "ct_measurement.h"
#include "control_queue.h"
#include "../Config/device_config.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

#define ADC_SERVICE_SAMPLE_RATE_HZ      2000U
#define ADC_SERVICE_WINDOW_SAMPLES      400U
#define ADC_SERVICE_DMA_BUFFER_SAMPLES  1800U  /* 200 × 9, double-buffered */
#define ADC_SERVICE_HALF_SAMPLES        900U
#define ADC_SERVICE_ADC3_DMA_SAMPLES    1600U  /* 200 × 8 */
#define ADC_SERVICE_ADC3_HALF_SAMPLES   800U
#define ADC_SERVICE_ADC1_CONV           9U     /* 8 CT + VREFINT */
#define ADC_SERVICE_ADC3_CONV           8U     /* 8 CT */
#define ADC_SERVICE_VREF_RANK           8U     /* 0-based within 9-sample seq */
#define ADC_SERVICE_DMA_HALF_FIRST      0x01U
#define ADC_SERVICE_DMA_HALF_SECOND     0x02U
#define ADC_SERVICE_VREF_RAW_MIN        1000U
#define ADC_SERVICE_VREF_RAW_MAX        2000U

/* DMA order matches CubeMX Rank 1-9: IN0,IN10,IN12,IN13,IN3,IN4,IN5,IN6,VREF */
/* Map DMA indices 0-7 → software channels (Relay N = ch N-1) */
/* ADC3 DMA order: IN9,IN14,IN15,IN4,IN5,IN6,IN7,IN8 → software ch 8-15 */

static ADC_HandleTypeDef *g_adc1;
static ADC_HandleTypeDef *g_adc3;
static TIM_HandleTypeDef *g_timer;
static adc_service_state_t g_state;
static adc_service_channel_state_t g_ch[ADC_SERVICE_CHANNEL_COUNT];
static ct_measurement_window_t g_win[ADC_SERVICE_CHANNEL_COUNT];
static ct_overcurrent_state_t g_prot[DEVICE_CONFIG_RELAY_COUNT];
static uint64_t g_vref_sum;
static uint32_t g_vref_count;
static volatile uint8_t g_a1_pend, g_a3_pend;
static volatile uint8_t g_a1_he, g_a3_he;
static volatile uint32_t g_a1_ovr, g_a3_ovr;
static volatile uint32_t g_a1_hec, g_a3_hec;
static uint16_t g_a1_buf[ADC_SERVICE_DMA_BUFFER_SAMPLES] __attribute__((aligned(32)));
static uint16_t g_a3_buf[ADC_SERVICE_ADC3_DMA_SAMPLES] __attribute__((aligned(32)));

/* ── DMA overrun tracking ──────────────────────────────────────── */
static void note_a1_half(uint8_t h) { if(g_a1_pend&h) g_a1_ovr++; g_a1_pend|=h; }
static void note_a3_half(uint8_t h) { if(g_a3_pend&h) g_a3_ovr++; g_a3_pend|=h; }

/* ── Process one ADC1 half-buffer (9 samples/trigger) ──────────── */
static void proc_a1(const uint16_t *s, uint32_t n)
{
  if(!s) return;
  for(uint32_t i=0U; i+ADC_SERVICE_ADC1_CONV<=n; i+=ADC_SERVICE_ADC1_CONV)
  {
    for(uint32_t ch=0U; ch<8U; ++ch)
      ct_measurement_window_add(&g_win[ch], s[i+ch]);
    g_vref_sum += s[i+ADC_SERVICE_VREF_RANK];
    g_vref_count++;
  }
}

/* ── Process one ADC3 half-buffer (8 samples/trigger) ──────────── */
static void proc_a3(const uint16_t *s, uint32_t n)
{
  if(!s) return;
  for(uint32_t i=0U; i+ADC_SERVICE_ADC3_CONV<=n; i+=ADC_SERVICE_ADC3_CONV)
    for(uint32_t ch=0U; ch<8U; ++ch)
      ct_measurement_window_add(&g_win[8U+ch], s[i+ch]);
}

/* ── Publish one channel window ─────────────────────────────────── */
static void publish_ch(uint8_t ch, uint32_t vdda, uint32_t now)
{
  ct_measurement_result_t r;
  ct_measurement_calculate_with_reference(&g_win[ch], ADC_SERVICE_WINDOW_SAMPLES, vdda, &r);
  taskENTER_CRITICAL();
  g_ch[ch].available=1U; g_ch[ch].valid=r.valid; g_ch[ch].ct_valid=r.ct_valid;
  g_ch[ch].signal_present=r.signal_present; g_ch[ch].raw_latest=r.raw_latest;
  g_ch[ch].raw_min=r.raw_minimum; g_ch[ch].raw_max=r.raw_maximum;
  g_ch[ch].raw_average=r.raw_average; g_ch[ch].raw_rms=r.raw_rms;
  g_ch[ch].millivolts=r.bias_millivolts; g_ch[ch].ac_rms_millivolts=r.ac_rms_millivolts;
  g_ch[ch].current_milliamps=r.current_milliamps;
  g_ch[ch].sample_count+=r.sample_count; g_ch[ch].batch_count++;
  g_ch[ch].saturation_count+=r.saturation_count; g_ch[ch].last_sample_ms=now;
  taskEXIT_CRITICAL();
  ct_measurement_window_reset(&g_win[ch]);

  /* Overcurrent: relay N is hard-bound to ADC N-1 */
  {
    const device_config_t *cfg = device_config_get();
    const device_config_current_t *c = &cfg->current[ch];
    uint8_t en = (r.ct_valid && c->protection_enabled) ? 1U : 0U;
    if(ct_overcurrent_update(&g_prot[ch], en, r.current_milliamps,
        c->overcurrent_milliamps, c->trip_delay_ms,
        c->reset_hysteresis_milliamps, now))
    {
      control_proto_command_t cmd; memset(&cmd,0,sizeof(cmd));
      cmd.type=CONTROL_PROTO_CMD_SET_RELAY; cmd.channel=(uint8_t)(ch+1U);
      cmd.value=0U; cmd.error=CONTROL_PROTO_ERR_NONE;
      (void)control_queue_submit_async(&cmd);
    }
  }
}

/* ── Publish all full windows ───────────────────────────────────── */
static void publish_all(void)
{
  uint32_t vdda=CT_MEASUREMENT_REFERENCE_MV, now=HAL_GetTick();
  uint8_t vref_ok=0U;
  if(g_vref_count)
  {
    uint16_t avg=(uint16_t)((g_vref_sum+g_vref_count/2U)/g_vref_count);
    if(avg>=ADC_SERVICE_VREF_RAW_MIN&&avg<=ADC_SERVICE_VREF_RAW_MAX)
    {
      uint32_t m=ct_measurement_calculate_vdda_mv(avg,*VREFINT_CAL_ADDR_CMSIS);
      if(m>=CT_MEASUREMENT_VDDA_MIN_MV&&m<=CT_MEASUREMENT_VDDA_MAX_MV) {vdda=m; vref_ok=1U;}
    }
  }
  for(uint8_t ch=0U; ch<ADC_SERVICE_CHANNEL_COUNT; ++ch)
    if(g_win[ch].count>=ADC_SERVICE_WINDOW_SAMPLES) publish_ch(ch,vdda,now);

  /* Aggregate: channel 0 for backward compat */
  taskENTER_CRITICAL();
  g_state.valid=g_ch[0].valid; g_state.ct_valid=g_ch[0].ct_valid;
  g_state.signal_present=g_ch[0].signal_present; g_state.vref_valid=vref_ok;
  g_state.vref_raw_average=(uint16_t)(g_vref_count?g_vref_sum/g_vref_count:0U);
  g_state.vdda_millivolts=vdda; g_state.raw_latest=g_ch[0].raw_latest;
  g_state.raw_min=g_ch[0].raw_min; g_state.raw_max=g_ch[0].raw_max;
  g_state.raw_average=g_ch[0].raw_average; g_state.raw_rms=g_ch[0].raw_rms;
  g_state.millivolts=g_ch[0].millivolts; g_state.ac_rms_millivolts=g_ch[0].ac_rms_millivolts;
  g_state.current_milliamps=g_ch[0].current_milliamps;
  g_state.sample_count=g_ch[0].sample_count; g_state.batch_count=g_ch[0].batch_count;
  g_state.saturation_count=g_ch[0].saturation_count; g_state.last_sample_ms=g_ch[0].last_sample_ms;
  g_state.last_hal_status=HAL_OK;
  taskEXIT_CRITICAL();
  g_vref_sum=0U; g_vref_count=0U;
}

/* ── Init ───────────────────────────────────────────────────────── */
void adc_service_init(ADC_HandleTypeDef *adc1, ADC_HandleTypeDef *adc3,
                      TIM_HandleTypeDef *tim)
{
  HAL_StatusTypeDef s;
  memset(&g_state,0,sizeof(g_state)); memset(g_ch,0,sizeof(g_ch));
  memset(g_win,0,sizeof(g_win)); memset(g_prot,0,sizeof(g_prot));
  g_vref_sum=g_vref_count=0U; g_adc1=adc1; g_adc3=adc3; g_timer=tim;
  g_a1_pend=g_a3_pend=g_a1_he=g_a3_he=0U;
  g_a1_ovr=g_a3_ovr=g_a1_hec=g_a3_hec=0U;
  g_state.sample_rate_hz=ADC_SERVICE_SAMPLE_RATE_HZ;
  g_state.window_samples=ADC_SERVICE_WINDOW_SAMPLES;
  g_state.channel_count=ADC_SERVICE_CHANNEL_COUNT;
  g_state.available_mask=0xFFFFUL;
  g_state.vdda_millivolts=CT_MEASUREMENT_REFERENCE_MV;

  if(!g_adc1||!g_adc3||!g_timer) { g_state.last_hal_status=HAL_ERROR; g_state.error_count++; return; }
  s=HAL_TIM_Base_Start(g_timer);
  if(s==HAL_OK) s=HAL_ADC_Start_DMA(g_adc1,(uint32_t*)g_a1_buf,ADC_SERVICE_DMA_BUFFER_SAMPLES);
  if(s==HAL_OK) s=HAL_ADC_Start_DMA(g_adc3,(uint32_t*)g_a3_buf,ADC_SERVICE_ADC3_DMA_SAMPLES);
  if(s!=HAL_OK)
  {
    (void)HAL_ADC_Stop_DMA(g_adc1); (void)HAL_ADC_Stop_DMA(g_adc3);
    g_state.last_hal_status=s; g_state.error_count++; return;
  }
  for(uint32_t ch=0U; ch<ADC_SERVICE_CHANNEL_COUNT; ++ch)
    ct_measurement_window_reset(&g_win[ch]);
  g_state.initialized=1U; g_state.last_hal_status=HAL_OK;
}

/* ── Poll ───────────────────────────────────────────────────────── */
void adc_service_poll(void)
{
  uint8_t p1,p3; uint32_t pm=__get_PRIMASK(); __disable_irq();
  p1=g_a1_pend; g_a1_pend=0U; p3=g_a3_pend; g_a3_pend=0U;
  if(!pm) __enable_irq();

  if(p1&ADC_SERVICE_DMA_HALF_FIRST)  proc_a1(&g_a1_buf[0], ADC_SERVICE_HALF_SAMPLES);
  if(p1&ADC_SERVICE_DMA_HALF_SECOND) proc_a1(&g_a1_buf[ADC_SERVICE_HALF_SAMPLES], ADC_SERVICE_HALF_SAMPLES);
  if(p3&ADC_SERVICE_DMA_HALF_FIRST)  proc_a3(&g_a3_buf[0], ADC_SERVICE_ADC3_HALF_SAMPLES);
  if(p3&ADC_SERVICE_DMA_HALF_SECOND) proc_a3(&g_a3_buf[ADC_SERVICE_ADC3_HALF_SAMPLES], ADC_SERVICE_ADC3_HALF_SAMPLES);

  { uint8_t need=0U; for(uint8_t ch=0U;ch<ADC_SERVICE_CHANNEL_COUNT;++ch)
    if(g_win[ch].count>=ADC_SERVICE_WINDOW_SAMPLES){need=1U;break;} if(need) publish_all(); }

  { uint32_t d1=g_a1_ovr,d3=g_a3_ovr;
    if(d1!=g_state.dma_overrun_count||d3||g_a1_he||g_a3_he)
    { taskENTER_CRITICAL(); g_state.dma_overrun_count=d1+d3;
      g_state.error_count=d1+d3+g_a1_hec+g_a3_hec; taskEXIT_CRITICAL(); } }
}

void adc_service_get_state(adc_service_state_t *s)
{ if(s){taskENTER_CRITICAL();*s=g_state;taskEXIT_CRITICAL();} }

uint8_t adc_service_get_channel_state(uint8_t ch, adc_service_channel_state_t *s)
{ if(!s||ch>=ADC_SERVICE_CHANNEL_COUNT) return 0U;
  taskENTER_CRITICAL(); *s=g_ch[ch]; taskEXIT_CRITICAL(); return s->available; }

uint8_t adc_service_get_protection_state(uint8_t rch, adc_service_protection_state_t *s)
{ if(!s||!rch||rch>DEVICE_CONFIG_RELAY_COUNT) return 0U;
  const ct_overcurrent_state_t *src=&g_prot[rch-1U];
  taskENTER_CRITICAL(); s->above_threshold=src->above_threshold; s->tripped=src->tripped;
  s->threshold_since_ms=src->threshold_since_ms; s->trip_count=src->trip_count;
  s->last_trip_ms=src->last_trip_ms; taskEXIT_CRITICAL(); return 1U; }

/* ── HAL Callbacks ──────────────────────────────────────────────── */
void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *h)
{ if(h==g_adc1) note_a1_half(ADC_SERVICE_DMA_HALF_FIRST);
  if(h==g_adc3) note_a3_half(ADC_SERVICE_DMA_HALF_FIRST); }
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *h)
{ if(h==g_adc1) note_a1_half(ADC_SERVICE_DMA_HALF_SECOND);
  if(h==g_adc3) note_a3_half(ADC_SERVICE_DMA_HALF_SECOND); }
void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *h)
{ uint32_t pm=__get_PRIMASK(); __disable_irq();
  if(h==g_adc1){g_a1_hec++;g_a1_he=1U;} if(h==g_adc3){g_a3_hec++;g_a3_he=1U;}
  if(!pm) __enable_irq(); }
