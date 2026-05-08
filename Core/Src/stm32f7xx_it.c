/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stm32f7xx_it.c
  * @brief   Interrupt Service Routines.
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
#include "stm32f7xx_it.h"
#include "FreeRTOS.h"
#include "task.h"
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>
#include "ethernetif.h"
#include "fault_service.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN TD */

/* USER CODE END TD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */
extern UART_HandleTypeDef huart3;
extern volatile uint32_t g_eth_irq_count;
volatile uint32_t g_tick_probe_armed = 0U;
volatile uint32_t g_tick_probe_hits = 0U;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
static volatile uint32_t g_panic_latched = 0U;

static void panic_uart_write(const char *text)
{
  if ((text != NULL) && (huart3.Instance != NULL))
  {
    (void)HAL_UART_Transmit(&huart3, (uint8_t *)text, (uint16_t)strlen(text), 100U);
  }
}

static void panic_uart_write_once(const char *text)
{
  if (g_panic_latched == 0U)
  {
    g_panic_latched = 1U;
    panic_uart_write(text);
  }
}

void hardfault_dump(uint32_t *stack, uint32_t exc_lr)
{
  char message[256];
  uint32_t r0 = 0U;
  uint32_t r1 = 0U;
  uint32_t r2 = 0U;
  uint32_t r3 = 0U;
  uint32_t r12 = 0U;
  uint32_t lr = 0U;
  uint32_t pc = 0U;
  uint32_t psr = 0U;

  if (g_panic_latched != 0U)
  {
    for (;;)
    {
    }
  }
  g_panic_latched = 1U;

  if (stack != NULL)
  {
    r0 = stack[0];
    r1 = stack[1];
    r2 = stack[2];
    r3 = stack[3];
    r12 = stack[4];
    lr = stack[5];
    pc = stack[6];
    psr = stack[7];
  }

  (void)snprintf(message, sizeof(message),
                 "\r\n[fault] HardFault CFSR=0x%08lX HFSR=0x%08lX MMFAR=0x%08lX BFAR=0x%08lX EXC_LR=0x%08lX SP=0x%08lX PC=0x%08lX LR=0x%08lX PSR=0x%08lX R0=0x%08lX R1=0x%08lX R2=0x%08lX R3=0x%08lX R12=0x%08lX\r\n",
                 (unsigned long)SCB->CFSR,
                 (unsigned long)SCB->HFSR,
                 (unsigned long)SCB->MMFAR,
                 (unsigned long)SCB->BFAR,
                 (unsigned long)exc_lr,
                 (unsigned long)stack,
                 (unsigned long)pc,
                 (unsigned long)lr,
                 (unsigned long)psr,
                 (unsigned long)r0,
                 (unsigned long)r1,
                 (unsigned long)r2,
                 (unsigned long)r3,
                 (unsigned long)r12);
  panic_uart_write(message);
  fault_service_record_hardfault(stack, exc_lr);
  HAL_Delay(20U);
  NVIC_SystemReset();
}

/* USER CODE END 0 */

/* External variables --------------------------------------------------------*/
extern ETH_HandleTypeDef heth;
/* USER CODE BEGIN EV */

/* USER CODE END EV */

/******************************************************************************/
/*           Cortex-M7 Processor Interruption and Exception Handlers          */
/******************************************************************************/
/**
  * @brief This function handles Non maskable interrupt.
  */
void NMI_Handler(void)
{
  /* USER CODE BEGIN NonMaskableInt_IRQn 0 */

  /* USER CODE END NonMaskableInt_IRQn 0 */
  /* USER CODE BEGIN NonMaskableInt_IRQn 1 */
  panic_uart_write_once("\r\n[fault] NMI\r\n");
  while (1)
  {
  }
  /* USER CODE END NonMaskableInt_IRQn 1 */
}

/**
  * @brief This function handles Hard fault interrupt.
  */
__attribute__((naked)) void HardFault_Handler(void)
{
  __asm volatile
  (
    "tst lr, #4            \n"
    "ite eq                \n"
    "mrseq r0, msp         \n"
    "mrsne r0, psp         \n"
    "mov r1, lr            \n"
    "b hardfault_dump      \n"
  );
}

/**
  * @brief This function handles Memory management fault.
  */
void MemManage_Handler(void)
{
  /* USER CODE BEGIN MemoryManagement_IRQn 0 */

  /* USER CODE END MemoryManagement_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_MemoryManagement_IRQn 0 */
    char message[160];
    if (g_panic_latched == 0U)
    {
      g_panic_latched = 1U;
      (void)snprintf(message, sizeof(message),
                     "\r\n[fault] MemManage CFSR=0x%08lX HFSR=0x%08lX MMFAR=0x%08lX BFAR=0x%08lX\r\n",
                     (unsigned long)SCB->CFSR,
                     (unsigned long)SCB->HFSR,
                     (unsigned long)SCB->MMFAR,
                     (unsigned long)SCB->BFAR);
      panic_uart_write(message);
      fault_service_record_simple(FAULT_SERVICE_TYPE_MEMMANAGE);
      HAL_Delay(20U);
      NVIC_SystemReset();
    }
    /* USER CODE END W1_MemoryManagement_IRQn 0 */
  }
}

/**
  * @brief This function handles Pre-fetch fault, memory access fault.
  */
void BusFault_Handler(void)
{
  /* USER CODE BEGIN BusFault_IRQn 0 */

  /* USER CODE END BusFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_BusFault_IRQn 0 */
    panic_uart_write_once("\r\n[fault] BusFault\r\n");
    fault_service_record_simple(FAULT_SERVICE_TYPE_BUSFAULT);
    HAL_Delay(20U);
    NVIC_SystemReset();
    /* USER CODE END W1_BusFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Undefined instruction or illegal state.
  */
void UsageFault_Handler(void)
{
  /* USER CODE BEGIN UsageFault_IRQn 0 */

  /* USER CODE END UsageFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_UsageFault_IRQn 0 */
    panic_uart_write_once("\r\n[fault] UsageFault\r\n");
    fault_service_record_simple(FAULT_SERVICE_TYPE_USAGEFAULT);
    HAL_Delay(20U);
    NVIC_SystemReset();
    /* USER CODE END W1_UsageFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Debug monitor.
  */
void DebugMon_Handler(void)
{
  /* USER CODE BEGIN DebugMonitor_IRQn 0 */

  /* USER CODE END DebugMonitor_IRQn 0 */
  /* USER CODE BEGIN DebugMonitor_IRQn 1 */

  /* USER CODE END DebugMonitor_IRQn 1 */
}

/**
  * @brief This function handles System tick timer.
  */
void SysTick_Handler(void)
{
  /* USER CODE BEGIN SysTick_IRQn 0 */
  if (g_tick_probe_armed != 0U)
  {
    g_tick_probe_hits++;
  }

  /* USER CODE END SysTick_IRQn 0 */
  HAL_IncTick();
#if (INCLUDE_xTaskGetSchedulerState == 1 )
  if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
  {
#endif /* INCLUDE_xTaskGetSchedulerState */
  xPortSysTickHandler();
#if (INCLUDE_xTaskGetSchedulerState == 1 )
  }
#endif /* INCLUDE_xTaskGetSchedulerState */
  /* USER CODE BEGIN SysTick_IRQn 1 */

  /* USER CODE END SysTick_IRQn 1 */
}

/******************************************************************************/
/* STM32F7xx Peripheral Interrupt Handlers                                    */
/* Add here the Interrupt Handlers for the used peripherals.                  */
/* For the available peripheral interrupt handler names,                      */
/* please refer to the startup file (startup_stm32f7xx.s).                    */
/******************************************************************************/

/**
  * @brief This function handles Ethernet global interrupt.
  */
void ETH_IRQHandler(void)
{
  /* USER CODE BEGIN ETH_IRQn 0 */
  g_eth_irq_count++;

  /* USER CODE END ETH_IRQn 0 */
  HAL_ETH_IRQHandler(&heth);
  /* USER CODE BEGIN ETH_IRQn 1 */

  /* USER CODE END ETH_IRQn 1 */
}

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
