/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stm32f7xx_hal_timebase_tim.c
  * @brief   HAL time base based on the hardware TIM.
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

#include "stm32f7xx_hal.h"
#include "stm32f7xx_hal_tim.h"

TIM_HandleTypeDef htim6;

HAL_StatusTypeDef HAL_InitTick(uint32_t TickPriority)
{
  RCC_ClkInitTypeDef clkconfig;
  uint32_t timclock;
  uint32_t apb1_prescaler;
  uint32_t prescaler_value;
  uint32_t flash_latency;
  HAL_StatusTypeDef status;

  __HAL_RCC_TIM6_CLK_ENABLE();

  HAL_RCC_GetClockConfig(&clkconfig, &flash_latency);
  apb1_prescaler = clkconfig.APB1CLKDivider;
  if (apb1_prescaler == RCC_HCLK_DIV1)
  {
    timclock = HAL_RCC_GetPCLK1Freq();
  }
  else
  {
    timclock = 2UL * HAL_RCC_GetPCLK1Freq();
  }

  prescaler_value = (timclock / 1000000U) - 1U;
  htim6.Instance = TIM6;
  htim6.Init.Period = 999U;
  htim6.Init.Prescaler = prescaler_value;
  htim6.Init.ClockDivision = 0U;
  htim6.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim6.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;

  status = HAL_TIM_Base_Init(&htim6);
  if (status == HAL_OK)
  {
    status = HAL_TIM_Base_Start_IT(&htim6);
    if (status == HAL_OK)
    {
      HAL_NVIC_EnableIRQ(TIM6_DAC_IRQn);
      if (TickPriority < (1UL << __NVIC_PRIO_BITS))
      {
        HAL_NVIC_SetPriority(TIM6_DAC_IRQn, TickPriority, 0U);
        uwTickPrio = TickPriority;
      }
      else
      {
        status = HAL_ERROR;
      }
    }
  }

  return status;
}

void HAL_SuspendTick(void)
{
  __HAL_TIM_DISABLE_IT(&htim6, TIM_IT_UPDATE);
}

void HAL_ResumeTick(void)
{
  __HAL_TIM_ENABLE_IT(&htim6, TIM_IT_UPDATE);
}
