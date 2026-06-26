/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
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
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <string.h>
#include "stm32f7xx_hal.h"

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
/* USER CODE BEGIN Variables */

/* USER CODE END Variables */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

/* USER CODE END FunctionPrototypes */

/* GetIdleTaskMemory prototype (linked to static allocation support) */
void vApplicationGetIdleTaskMemory( StaticTask_t **ppxIdleTaskTCBBuffer, StackType_t **ppxIdleTaskStackBuffer, uint32_t *pulIdleTaskStackSize );

/* USER CODE BEGIN GET_IDLE_TASK_MEMORY */
static StaticTask_t xIdleTaskTCBBuffer;
static StackType_t xIdleStack[configMINIMAL_STACK_SIZE];

void vApplicationGetIdleTaskMemory( StaticTask_t **ppxIdleTaskTCBBuffer, StackType_t **ppxIdleTaskStackBuffer, uint32_t *pulIdleTaskStackSize )
{
  *ppxIdleTaskTCBBuffer = &xIdleTaskTCBBuffer;
  *ppxIdleTaskStackBuffer = &xIdleStack[0];
  *pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
  /* place for user code */
}
/* USER CODE END GET_IDLE_TASK_MEMORY */

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */
extern UART_HandleTypeDef huart3;

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
  static const char prefix[] = "\r\n[rtos] stack overflow: task=0x";
  static const char delim[] = " name=";
  static const char suffix[] = "\r\n";
  char hex_buf[9];
  uint16_t i;
  uint32_t handle_val = (uint32_t)xTask;

  /* Convert task handle to hex (8 nibbles, bounded) */
  for (i = 0U; i < 8U; i++)
  {
    uint8_t nibble = (uint8_t)((handle_val >> (28U - i * 4U)) & 0x0FU);
    hex_buf[i] = (char)((nibble < 10U) ? ('0' + nibble) : ('A' + nibble - 10U));
  }
  hex_buf[8] = '\0';

  HAL_UART_Transmit(&huart3, (uint8_t *)prefix, sizeof(prefix) - 1U, 100U);
  HAL_UART_Transmit(&huart3, (uint8_t *)hex_buf, 8U, 100U);
  HAL_UART_Transmit(&huart3, (uint8_t *)delim, sizeof(delim) - 1U, 100U);

  if (pcTaskName != NULL)
  {
    /* Bounded output — max 16 chars to avoid strlen on corrupted memory */
    uint16_t len = 0U;
    while (len < 16U && pcTaskName[len] != '\0') { len++; }
    HAL_UART_Transmit(&huart3, (uint8_t *)pcTaskName, len, 100U);
  }
  else
  {
    static const char null_name[] = "(null)";
    HAL_UART_Transmit(&huart3, (uint8_t *)null_name, sizeof(null_name) - 1U, 100U);
  }
  HAL_UART_Transmit(&huart3, (uint8_t *)suffix, sizeof(suffix) - 1U, 100U);

  taskDISABLE_INTERRUPTS();
  for (;;)
  {
  }
}

void vApplicationMallocFailedHook(void)
{
  static const char msg[] = "\r\n[rtos] malloc failed\r\n";

  HAL_UART_Transmit(&huart3, (uint8_t *)msg, sizeof(msg) - 1U, 100U);
  taskDISABLE_INTERRUPTS();
  for (;;)
  {
  }
}

/* USER CODE END Application */

