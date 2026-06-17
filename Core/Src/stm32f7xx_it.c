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

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
static volatile uint32_t g_panic_latched = 0U;
const char memmanage_label[] = "MemManage";
const char busfault_label[] = "BusFault";
const char usagefault_label[] = "UsageFault";

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

void memmanage_fault_bridge(const char *label, uint32_t *stack, uint32_t exc_lr, uint32_t type);

static void fault_dump_and_reset(const char *label,
                                 fault_service_type_t type,
                                 uint32_t *stack,
                                 uint32_t exc_lr)
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
                 "\r\n[fault] %s CFSR=0x%08lX HFSR=0x%08lX MMFAR=0x%08lX BFAR=0x%08lX EXC_LR=0x%08lX SP=0x%08lX PC=0x%08lX LR=0x%08lX PSR=0x%08lX R0=0x%08lX R1=0x%08lX R2=0x%08lX R3=0x%08lX R12=0x%08lX\r\n",
                 label,
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
  if (type == FAULT_SERVICE_TYPE_HARDFAULT)
  {
    fault_service_record_hardfault(stack, exc_lr);
  }
  else
  {
    fault_service_record_simple(type);
  }
  HAL_Delay(20U);
  NVIC_SystemReset();
}

/* USER CODE END 0 */

/* External variables --------------------------------------------------------*/
extern ETH_HandleTypeDef heth;
extern DMA_HandleTypeDef hdma_adc1;
extern DMA_HandleTypeDef hdma_adc3;
extern TIM_HandleTypeDef htim6;
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
__attribute__((naked)) void MemManage_Handler(void)
{
  __asm volatile
  (
    "tst lr, #4            \n"
    "ite eq                \n"
    "mrseq r1, msp         \n"
    "mrsne r1, psp         \n"
    "mov r2, lr            \n"
    "ldr r0, =memmanage_label \n"
    "movs r3, #2           \n"
    "b memmanage_fault_bridge \n"
  );
}

void memmanage_fault_bridge(const char *label, uint32_t *stack, uint32_t exc_lr, uint32_t type)
{
  fault_dump_and_reset(label, (fault_service_type_t)type, stack, exc_lr);
}

/**
  * @brief This function handles Pre-fetch fault, memory access fault.
  */
__attribute__((naked)) void BusFault_Handler(void)
{
  __asm volatile
  (
    "tst lr, #4            \n"
    "ite eq                \n"
    "mrseq r1, msp         \n"
    "mrsne r1, psp         \n"
    "mov r2, lr            \n"
    "ldr r0, =busfault_label \n"
    "movs r3, #3           \n"
    "b memmanage_fault_bridge \n"
  );
}

/**
  * @brief This function handles Undefined instruction or illegal state.
  */
__attribute__((naked)) void UsageFault_Handler(void)
{
  __asm volatile
  (
    "tst lr, #4            \n"
    "ite eq                \n"
    "mrseq r1, msp         \n"
    "mrsne r1, psp         \n"
    "mov r2, lr            \n"
    "ldr r0, =usagefault_label \n"
    "movs r3, #4           \n"
    "b memmanage_fault_bridge \n"
  );
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

/******************************************************************************/
/* STM32F7xx Peripheral Interrupt Handlers                                    */
/* Add here the Interrupt Handlers for the used peripherals.                  */
/* For the available peripheral interrupt handler names,                      */
/* please refer to the startup file (startup_stm32f7xx.s).                    */
/******************************************************************************/

/**
  * @brief This function handles TIM6 global interrupt, DAC1 and DAC2 underrun errors.
 */
void DMA2_Stream0_IRQHandler(void)
{
  HAL_DMA_IRQHandler(&hdma_adc1);
}

/**
  * @brief This function handles DMA2 Stream1 interrupt (ADC3).
  */
void DMA2_Stream1_IRQHandler(void)
{
  HAL_DMA_IRQHandler(&hdma_adc3);
}

void TIM6_DAC_IRQHandler(void)
{
  HAL_TIM_IRQHandler(&htim6);
}

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
