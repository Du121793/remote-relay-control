/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "cmsis_os.h"
#include "lwip.h"
#include "mbedtls.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>
#include "board_relay.h"
#include "cluster_service.h"
#include "../../App/Config/device_config.h"
#include "http_service.h"
#include "control_service.h"
#include "mqtt_link_service.h"
#include "discovery_service.h"
#include "adc_service.h"
#include "relay_protection.h"
#include "scheduler.h"
#include "time_sync_service.h"
#include "https_client_service.h"
#include "lwip/stats.h"
#include "lwip/dhcp.h"
#include "lwip/ip4_addr.h"
#include "lwip/prot/dhcp.h"
#include "stm32f7xx_hal_eth.h"
#include "lan8742.h"
#include "FreeRTOS.h"
#include "portable.h"
#include "task.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;
DMA_HandleTypeDef hdma_adc1;
ADC_HandleTypeDef hadc3;
DMA_HandleTypeDef hdma_adc3;

RNG_HandleTypeDef hrng;

RTC_HandleTypeDef hrtc;

TIM_HandleTypeDef htim2;

UART_HandleTypeDef huart3;

osThreadId defaultTaskHandle;
/* USER CODE BEGIN PV */
extern struct netif gnetif;
extern ETH_HandleTypeDef heth;
extern lan8742_Object_t LAN8742;
extern volatile uint32_t g_eth_irq_count;
extern volatile uint32_t g_eth_rx_cplt_count;
extern volatile uint32_t g_eth_link_thread_started;
extern volatile uint32_t g_eth_link_thread_loops;
extern volatile int32_t g_eth_last_phy_link_state;
extern volatile uint32_t g_eth_link_up_events;
extern volatile uint32_t g_eth_link_down_events;
extern volatile int32_t g_eth_phy_disable_power_status;
extern volatile int32_t g_eth_phy_start_autoneg_status;
extern volatile uint32_t g_eth_phy_bcr_after_cfg;
extern volatile uint32_t g_eth_phy_bsr_after_cfg;
extern volatile uint32_t g_eth_phy_physcsr_after_cfg;
extern volatile uint32_t g_eth_phy_retry_count;
extern volatile int32_t g_eth_phy_forced_mode;
extern volatile uint32_t g_eth_tx_packets;
extern volatile uint32_t g_eth_tx_free_packets;

static char g_uart_message[384];
static char g_ip_text[16];
static char g_mask_text[16];
static char g_gw_text[16];
static uint32_t next_dhcp_report_ms = 0U;
static const uint32_t g_dhcp_report_interval_ms = 2000U;
static uint8_t g_last_link_up = 0xFFU;
static uint8_t g_last_dhcp_bound = 0xFFU;
static uint8_t g_last_dhcp_state = 0xFFU;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MPU_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_ADC1_Init(void);
static void MX_ADC3_Init(void);
static void MX_TIM2_Init(void);
static void MX_USART3_UART_Init(void);
static void MX_RNG_Init(void);
static void MX_RTC_Init(void);
void StartDefaultTask(void const * argument);

/* USER CODE BEGIN PFP */
static void uart3_write(const char *text);
void lwip_platform_assert(const char *msg, int line, const char *file);
static void log_startup_banner(void);
static const char *dhcp_state_to_string(uint8_t state);
static void log_link_state(uint8_t link_up);
static void log_dhcp_state(void);
static void disable_unaligned_trap_for_lwip(void);
__attribute__((noinline, optnone, used)) static void aeabi_copy_bytes_forward(void *dst, const void *src, size_t len);
__attribute__((noinline, optnone, used)) static void aeabi_move_bytes_safe(void *dst, const void *src, size_t len);
void __aeabi_memcpy(void *dest, const void *src, size_t n);
void __aeabi_memcpy4(void *dest, const void *src, size_t n);
void __aeabi_memcpy8(void *dest, const void *src, size_t n);
void __aeabi_memmove(void *dest, const void *src, size_t n);
void __aeabi_memmove4(void *dest, const void *src, size_t n);
void __aeabi_memmove8(void *dest, const void *src, size_t n);

__asm(".global __use_no_semihosting_swi");

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
__attribute__((noinline, optnone, used)) static void aeabi_copy_bytes_forward(void *dst, const void *src, size_t len)
{
  volatile uint8_t *dst_bytes = (volatile uint8_t *)dst;
  const volatile uint8_t *src_bytes = (const volatile uint8_t *)src;
  size_t i;

  for (i = 0U; i < len; ++i)
  {
    dst_bytes[i] = src_bytes[i];
  }
}

__attribute__((noinline, optnone, used)) static void aeabi_move_bytes_safe(void *dst, const void *src, size_t len)
{
  volatile uint8_t *dst_bytes = (volatile uint8_t *)dst;
  const volatile uint8_t *src_bytes = (const volatile uint8_t *)src;
  size_t i;

  if ((dst_bytes == src_bytes) || (len == 0U))
  {
    return;
  }

  if (dst_bytes < src_bytes)
  {
    aeabi_copy_bytes_forward((void *)dst_bytes, (const void *)src_bytes, len);
    return;
  }

  for (i = len; i > 0U; --i)
  {
    dst_bytes[i - 1U] = src_bytes[i - 1U];
  }
}

void __aeabi_memcpy(void *dest, const void *src, size_t n)
{
  aeabi_copy_bytes_forward(dest, src, n);
}

void __aeabi_memcpy4(void *dest, const void *src, size_t n)
{
  aeabi_copy_bytes_forward(dest, src, n);
}

void __aeabi_memcpy8(void *dest, const void *src, size_t n)
{
  aeabi_copy_bytes_forward(dest, src, n);
}

void __aeabi_memmove(void *dest, const void *src, size_t n)
{
  aeabi_move_bytes_safe(dest, src, n);
}

void __aeabi_memmove4(void *dest, const void *src, size_t n)
{
  aeabi_move_bytes_safe(dest, src, n);
}

void __aeabi_memmove8(void *dest, const void *src, size_t n)
{
  aeabi_move_bytes_safe(dest, src, n);
}

static void uart3_write(const char *text)
{
  size_t len = strlen(text);

  if (len > 0U)
  {
    (void)HAL_UART_Transmit(&huart3, (uint8_t *)text, (uint16_t)len, 100U);
  }
}

int fputc(int ch, FILE *f)
{
  uint8_t byte = (uint8_t)ch;
  (void)f;

  if (huart3.Instance != NULL)
  {
    (void)HAL_UART_Transmit(&huart3, &byte, 1U, 10U);
  }

  return ch;
}

void _ttywrch(int ch)
{
  (void)fputc(ch, stdout);
}

void _sys_exit(int return_code)
{
  (void)return_code;
  while (1)
  {
  }
}

void lwip_platform_assert(const char *msg, int line, const char *file)
{
  (void)snprintf(g_uart_message, sizeof(g_uart_message),
                 "\r\n[lwip] assert \"%s\" line=%d file=%s\r\n",
                 (msg != NULL) ? msg : "?",
                 line,
                 (file != NULL) ? file : "?");
  uart3_write(g_uart_message);
}

static void log_startup_banner(void)
{
  cluster_service_state_t cluster_state;

  cluster_service_get_state(&cluster_state);
  (void)snprintf(g_uart_message, sizeof(g_uart_message),
                 "[boot] rtc lse=%u source=%u quality=%u bdcr=0x%08lX drive=%lu bypass=%u\r\n",
                 (unsigned int)cluster_state.rtc_lse_ready,
                 (unsigned int)cluster_state.time_source,
                 (unsigned int)cluster_state.time_quality,
                 (unsigned long)RCC->BDCR,
                 (unsigned long)((RCC->BDCR & RCC_BDCR_LSEDRV) >>
                                 RCC_BDCR_LSEDRV_Pos),
                 (unsigned int)(((RCC->BDCR & RCC_BDCR_LSEBYP) != 0U)
                                ? 1U
                                : 0U));
  uart3_write(g_uart_message);
  uart3_write("\r\n[boot] autoControl startup complete\r\n");
}

static const char *dhcp_state_to_string(uint8_t state)
{
  switch (state)
  {
    case DHCP_STATE_OFF: return "off";
    case DHCP_STATE_REQUESTING: return "requesting";
    case DHCP_STATE_INIT: return "init";
    case DHCP_STATE_REBOOTING: return "rebooting";
    case DHCP_STATE_REBINDING: return "rebinding";
    case DHCP_STATE_RENEWING: return "renewing";
    case DHCP_STATE_SELECTING: return "selecting";
    case DHCP_STATE_INFORMING: return "informing";
    case DHCP_STATE_CHECKING: return "checking";
    case DHCP_STATE_BOUND: return "bound";
    case DHCP_STATE_RELEASING: return "releasing";
    case DHCP_STATE_BACKING_OFF: return "backing_off";
    case DHCP_STATE_PERMANENT: return "permanent";
    default: return "unknown";
  }
}

static void log_link_state(uint8_t link_up)
{
  (void)snprintf(g_uart_message, sizeof(g_uart_message),
                 (link_up != 0U) ? "[net] link up phy=%ld\r\n" : "[net] link down phy=%ld\r\n",
                 (long)g_eth_last_phy_link_state);
  uart3_write(g_uart_message);
}

static void log_dhcp_state(void)
{
  struct dhcp *dhcp = netif_dhcp_data(&gnetif);
  uint8_t bound = dhcp_supplied_address(&gnetif);
  uint8_t state = (dhcp != NULL) ? dhcp->state : DHCP_STATE_OFF;

  if (bound != 0U)
  {
    (void)ip4addr_ntoa_r(netif_ip4_addr(&gnetif), g_ip_text, sizeof(g_ip_text));
    (void)ip4addr_ntoa_r(netif_ip4_netmask(&gnetif), g_mask_text, sizeof(g_mask_text));
    (void)ip4addr_ntoa_r(netif_ip4_gw(&gnetif), g_gw_text, sizeof(g_gw_text));
    (void)snprintf(g_uart_message, sizeof(g_uart_message),
                   "[net] dhcp bound ip=%s mask=%s gw=%s\r\n",
                   g_ip_text, g_mask_text, g_gw_text);
  }
  else
  {
    (void)snprintf(g_uart_message, sizeof(g_uart_message),
                   "[net] dhcp %s\r\n",
                   dhcp_state_to_string(state));
  }
  uart3_write(g_uart_message);
}

static void disable_unaligned_trap_for_lwip(void)
{
  /*
   * armclang still emits word-sized accesses for some packed lwIP paths.
   * Keep UNALIGN_TRP cleared so a missed packed-field site does not turn
   * normal network traffic into a UsageFault during bring-up.
   */
  SCB->CCR &= ~SCB_CCR_UNALIGN_TRP_Msk;
  __DSB();
  __ISB();
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MPU Configuration--------------------------------------------------------*/
  MPU_Config();

  /* Enable the CPU Cache */

  /* Enable I-Cache---------------------------------------------------------*/
  SCB_EnableICache();

  /* Enable D-Cache---------------------------------------------------------*/
  /* Keep D-Cache disabled while validating the ETH DMA RX/TX path. */
  /* SCB_EnableDCache(); */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */
  disable_unaligned_trap_for_lwip();

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */
  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_ADC1_Init();
  MX_ADC3_Init();
  MX_TIM2_Init();
  MX_USART3_UART_Init();
  uart3_write("\r\n[boot] uart ready\r\n");
  MX_RNG_Init();
  MX_MBEDTLS_Init();
  /* USER CODE BEGIN 2 */
  relay_protection_init();  /* force all relays OFF before config applies startup mask */
  scheduler_init();
  device_config_init();
  /* Startup relay mask is now managed by relay_protection:
     boot_default_on / remember_state become pending requests
     that are only applied after boot delay expires. */
  (void)board_relay_apply_mask(0U);  /* all OFF — protection module owns boot state */
  cluster_service_init();
  MX_RTC_Init();
  log_startup_banner();

  /* USER CODE END 2 */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* definition and creation of defaultTask */
  /* defaultTask stack is now statically allocated to guard against heap-
     edge corruption of the FreeRTOS stack canary.  3072 words (12 KB)
     provides headroom for the 3616-byte device_config_t copy in save paths. */
  static StackType_t defaultTaskStack[4096];
  static StaticTask_t defaultTaskTCB;
  defaultTaskHandle = xTaskCreateStatic((TaskFunction_t)StartDefaultTask, "defaultTask",
                                        4096, NULL,
                                        osPriorityNormal, defaultTaskStack,
                                        &defaultTaskTCB);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  /* USER CODE END RTOS_THREADS */

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 216;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 9;
  RCC_OscInitStruct.PLL.PLLR = 2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Activate the Over-Drive mode
  */
  if (HAL_PWREx_EnableOverDrive() != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_7) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief ADC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC1_Init(void)
{

  /* USER CODE BEGIN ADC1_Init 0 */

  /* USER CODE END ADC1_Init 0 */

  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC1_Init 1 */

  /* USER CODE END ADC1_Init 1 */

  /** Configure the global features of the ADC (Clock, Resolution, Data Alignment and number of conversion)
  */
  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.ScanConvMode = ADC_SCAN_ENABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_RISING;
  hadc1.Init.ExternalTrigConv = ADC_EXTERNALTRIGCONV_T2_TRGO;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.NbrOfConversion = 9;
  hadc1.Init.DMAContinuousRequests = ENABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
  */
  sConfig.Channel = ADC_CHANNEL_0;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_144CYCLES;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK) { Error_Handler(); }
  sConfig.Channel = ADC_CHANNEL_10;
  sConfig.Rank = ADC_REGULAR_RANK_2;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK) { Error_Handler(); }
  sConfig.Channel = ADC_CHANNEL_12;
  sConfig.Rank = ADC_REGULAR_RANK_3;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK) { Error_Handler(); }
  sConfig.Channel = ADC_CHANNEL_13;
  sConfig.Rank = ADC_REGULAR_RANK_4;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK) { Error_Handler(); }
  sConfig.Channel = ADC_CHANNEL_3;
  sConfig.Rank = ADC_REGULAR_RANK_5;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK) { Error_Handler(); }
  sConfig.Channel = ADC_CHANNEL_4;
  sConfig.Rank = ADC_REGULAR_RANK_6;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK) { Error_Handler(); }
  sConfig.Channel = ADC_CHANNEL_5;
  sConfig.Rank = ADC_REGULAR_RANK_7;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK) { Error_Handler(); }
  sConfig.Channel = ADC_CHANNEL_6;
  sConfig.Rank = ADC_REGULAR_RANK_8;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK) { Error_Handler(); }
  sConfig.Channel = ADC_CHANNEL_VREFINT;
  sConfig.Rank = ADC_REGULAR_RANK_9;
  sConfig.SamplingTime = ADC_SAMPLETIME_480CYCLES;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK) { Error_Handler(); }
  /* USER CODE BEGIN ADC1_Init 2 */

  /* USER CODE END ADC1_Init 2 */

}

static void MX_ADC3_Init(void)
{
  ADC_ChannelConfTypeDef sConfig = {0};
  __HAL_RCC_ADC3_CLK_ENABLE();
  __HAL_RCC_GPIOF_CLK_ENABLE();
  hadc3.Instance = ADC3;
  hadc3.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  hadc3.Init.Resolution = ADC_RESOLUTION_12B;
  hadc3.Init.ScanConvMode = ADC_SCAN_ENABLE;
  hadc3.Init.ContinuousConvMode = DISABLE;
  hadc3.Init.DiscontinuousConvMode = DISABLE;
  hadc3.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_RISING;
  hadc3.Init.ExternalTrigConv = ADC_EXTERNALTRIGCONV_T2_TRGO;
  hadc3.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc3.Init.NbrOfConversion = 8;
  hadc3.Init.DMAContinuousRequests = ENABLE;
  hadc3.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  if (HAL_ADC_Init(&hadc3) != HAL_OK) { Error_Handler(); }
  sConfig.Channel = ADC_CHANNEL_9;  sConfig.Rank = ADC_REGULAR_RANK_1; sConfig.SamplingTime = ADC_SAMPLETIME_144CYCLES;
  if (HAL_ADC_ConfigChannel(&hadc3, &sConfig) != HAL_OK) { Error_Handler(); }
  sConfig.Channel = ADC_CHANNEL_14; sConfig.Rank = ADC_REGULAR_RANK_2;
  if (HAL_ADC_ConfigChannel(&hadc3, &sConfig) != HAL_OK) { Error_Handler(); }
  sConfig.Channel = ADC_CHANNEL_15; sConfig.Rank = ADC_REGULAR_RANK_3;
  if (HAL_ADC_ConfigChannel(&hadc3, &sConfig) != HAL_OK) { Error_Handler(); }
  sConfig.Channel = ADC_CHANNEL_4;  sConfig.Rank = ADC_REGULAR_RANK_4;
  if (HAL_ADC_ConfigChannel(&hadc3, &sConfig) != HAL_OK) { Error_Handler(); }
  sConfig.Channel = ADC_CHANNEL_5;  sConfig.Rank = ADC_REGULAR_RANK_5;
  if (HAL_ADC_ConfigChannel(&hadc3, &sConfig) != HAL_OK) { Error_Handler(); }
  sConfig.Channel = ADC_CHANNEL_6;  sConfig.Rank = ADC_REGULAR_RANK_6;
  if (HAL_ADC_ConfigChannel(&hadc3, &sConfig) != HAL_OK) { Error_Handler(); }
  sConfig.Channel = ADC_CHANNEL_7;  sConfig.Rank = ADC_REGULAR_RANK_7;
  if (HAL_ADC_ConfigChannel(&hadc3, &sConfig) != HAL_OK) { Error_Handler(); }
  sConfig.Channel = ADC_CHANNEL_8;  sConfig.Rank = ADC_REGULAR_RANK_8;
  if (HAL_ADC_ConfigChannel(&hadc3, &sConfig) != HAL_OK) { Error_Handler(); }
}

/**
  * @brief TIM2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM2_Init(void)
{
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 107U;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 499U;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }

  sMasterConfig.MasterOutputTrigger = TIM_TRGO_UPDATE;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * Enable DMA controller clock.
  */
static void MX_DMA_Init(void)
{
  __HAL_RCC_DMA2_CLK_ENABLE();

  HAL_NVIC_SetPriority(DMA2_Stream0_IRQn, 6, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream0_IRQn);

  HAL_NVIC_SetPriority(DMA2_Stream1_IRQn, 6, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream1_IRQn);
}

/**
  * @brief RNG Initialization Function
  * @param None
  * @retval None
  */
static void MX_RNG_Init(void)
{

  /* USER CODE BEGIN RNG_Init 0 */

  /* USER CODE END RNG_Init 0 */

  /* USER CODE BEGIN RNG_Init 1 */

  /* USER CODE END RNG_Init 1 */
  hrng.Instance = RNG;
  if (HAL_RNG_Init(&hrng) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN RNG_Init 2 */

  /* USER CODE END RNG_Init 2 */

}

/**
  * @brief RTC Initialization Function
  * @param None
  * @retval None
  */
static void MX_RTC_Init(void)
{
  if (__HAL_RCC_GET_FLAG(RCC_FLAG_LSERDY) == RESET)
  {
    memset(&hrtc, 0, sizeof(hrtc));
    return;
  }

  hrtc.Instance = RTC;
  hrtc.Init.HourFormat = RTC_HOURFORMAT_24;
  hrtc.Init.AsynchPrediv = 127U;
  hrtc.Init.SynchPrediv = 255U;
  hrtc.Init.OutPut = RTC_OUTPUT_DISABLE;
  hrtc.Init.OutPutPolarity = RTC_OUTPUT_POLARITY_HIGH;
  hrtc.Init.OutPutType = RTC_OUTPUT_TYPE_OPENDRAIN;
  if (HAL_RTC_Init(&hrtc) != HAL_OK)
  {
    memset(&hrtc, 0, sizeof(hrtc));
  }
}

/**
  * @brief USART3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART3_UART_Init(void)
{

  /* USER CODE BEGIN USART3_Init 0 */

  /* USER CODE END USART3_Init 0 */

  /* USER CODE BEGIN USART3_Init 1 */

  /* USER CODE END USART3_Init 1 */
  huart3.Instance = USART3;
  huart3.Init.BaudRate = 115200;
  huart3.Init.WordLength = UART_WORDLENGTH_8B;
  huart3.Init.StopBits = UART_STOPBITS_1;
  huart3.Init.Parity = UART_PARITY_NONE;
  huart3.Init.Mode = UART_MODE_TX_RX;
  huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart3.Init.OverSampling = UART_OVERSAMPLING_16;
  huart3.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart3.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart3) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART3_Init 2 */

  /* USER CODE END USART3_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOF_CLK_ENABLE();
  __HAL_RCC_GPIOG_CLK_ENABLE();
  __HAL_RCC_GPIOE_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOF, Relay10_Pin|Relay11_Pin|Relay12_Pin|Relay13_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOG, Relay14_Pin|Relay15_Pin|Relay16_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOE, Relay1_Pin|Relay2_Pin|Relay3_Pin|Relay4_Pin
                          |Relay5_Pin|Relay6_Pin|Relay7_Pin|Relay8_Pin
                          |Relay9_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pins : Relay10_Pin Relay11_Pin Relay12_Pin Relay13_Pin */
  GPIO_InitStruct.Pin = Relay10_Pin|Relay11_Pin|Relay12_Pin|Relay13_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOF, &GPIO_InitStruct);

  /*Configure GPIO pins : Relay14_Pin Relay15_Pin Relay16_Pin */
  GPIO_InitStruct.Pin = Relay14_Pin|Relay15_Pin|Relay16_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOG, &GPIO_InitStruct);

  /*Configure GPIO pins : Relay1_Pin Relay2_Pin Relay3_Pin Relay4_Pin
                           Relay5_Pin Relay6_Pin Relay7_Pin Relay8_Pin
                           Relay9_Pin */
  GPIO_InitStruct.Pin = Relay1_Pin|Relay2_Pin|Relay3_Pin|Relay4_Pin
                          |Relay5_Pin|Relay6_Pin|Relay7_Pin|Relay8_Pin
                          |Relay9_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void const * argument)
{
  /* init code for LWIP */
  MX_LWIP_Init();
  uart3_write("[boot] lwip initialized\r\n");
  adc_service_init(&hadc1, &hadc3, &htim2);
  /* relay_protection_init() now called earlier, before device_config_init() */
  next_dhcp_report_ms = HAL_GetTick() + g_dhcp_report_interval_ms;
  /* USER CODE BEGIN 5 */
  /* Infinite loop */
  for(;;)
  {
    uint32_t now = HAL_GetTick();
    static uint32_t last_stack_log_ms = 0U;
    struct dhcp *dhcp = netif_dhcp_data(&gnetif);
    uint8_t link_up = netif_is_link_up(&gnetif) ? 1U : 0U;
    uint8_t dhcp_bound = dhcp_supplied_address(&gnetif) ? 1U : 0U;
    uint8_t dhcp_state = (dhcp != NULL) ? dhcp->state : DHCP_STATE_OFF;

    if (link_up != g_last_link_up)
    {
      g_last_link_up = link_up;
      log_link_state(link_up);
    }

    if ((dhcp_bound != g_last_dhcp_bound) || (dhcp_state != g_last_dhcp_state))
    {
      g_last_dhcp_bound = dhcp_bound;
      g_last_dhcp_state = dhcp_state;
      log_dhcp_state();
      next_dhcp_report_ms = now + g_dhcp_report_interval_ms;
    }
    else if ((link_up != 0U) && (dhcp_bound == 0U) && ((int32_t)(now - next_dhcp_report_ms) >= 0))
    {
      log_dhcp_state();
      next_dhcp_report_ms = now + g_dhcp_report_interval_ms;
    }

    ethernetif_poll_tx_reclaim();
    adc_service_poll();
    relay_protection_poll();

    /* Scheduler + Auto Hold tick */
    {
      uint8_t time_valid;
      uint32_t unix_time = cluster_service_get_unix_time(&time_valid);
      scheduler_tick(HAL_GetTick(), unix_time, time_valid);
    }

    if ((last_stack_log_ms == 0U) || ((int32_t)(now - last_stack_log_ms) >= 30000))
    {
      char stack_msg[160];
      extern UBaseType_t device_config_stack_high_water(void);
      extern UBaseType_t control_queue_stack_high_water(void);
      extern UBaseType_t discovery_service_stack_high_water(void);
      TaskHandle_t tcpip = xTaskGetHandle("tcpip_thread");
      (void)snprintf(stack_msg, sizeof(stack_msg),
                     "[rtos] stacks: def=%lu cfg=%lu ctl=%lu disc=%lu tcpip=%lu heap_min=%lu\r\n",
                     (unsigned long)uxTaskGetStackHighWaterMark(NULL),
                     (unsigned long)device_config_stack_high_water(),
                     (unsigned long)control_queue_stack_high_water(),
                     (unsigned long)discovery_service_stack_high_water(),
                     (unsigned long)(tcpip ? uxTaskGetStackHighWaterMark(tcpip) : 0UL),
                     (unsigned long)xPortGetMinimumEverFreeHeapSize());
      uart3_write(stack_msg);
      last_stack_log_ms = now;
    }

    control_service_poll();
    discovery_service_poll();
    /* backend TCP (:42001) has been retired — commands now flow via MQTT */
    mqtt_link_service_poll();
    https_client_service_poll();
    time_sync_service_poll();
    http_service_poll();
    cluster_service_poll();
    device_config_poll();

    osDelay(100);
  }
  /* USER CODE END 5 */
}

 /* MPU Configuration */

void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};

  /* Disables the MPU */
  HAL_MPU_Disable();

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x0;
  MPU_InitStruct.Size = MPU_REGION_SIZE_4GB;
  MPU_InitStruct.SubRegionDisable = 0x87;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /* Keep ETH DMA descriptors and TX staging buffers outside D-Cache. */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER1;
  MPU_InitStruct.BaseAddress = 0x2007C000U;
  MPU_InitStruct.Size = MPU_REGION_SIZE_16KB;
  MPU_InitStruct.SubRegionDisable = 0x00;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_BUFFERABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);
  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  if (htim->Instance == TIM6)
  {
    HAL_IncTick();
  }
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
