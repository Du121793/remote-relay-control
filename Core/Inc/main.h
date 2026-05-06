/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
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

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f7xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define Relay10_Pin GPIO_PIN_12
#define Relay10_GPIO_Port GPIOF
#define Relay11_Pin GPIO_PIN_13
#define Relay11_GPIO_Port GPIOF
#define Relay12_Pin GPIO_PIN_14
#define Relay12_GPIO_Port GPIOF
#define Relay13_Pin GPIO_PIN_15
#define Relay13_GPIO_Port GPIOF
#define Relay14_Pin GPIO_PIN_0
#define Relay14_GPIO_Port GPIOG
#define Relay15_Pin GPIO_PIN_1
#define Relay15_GPIO_Port GPIOG
#define Relay1_Pin GPIO_PIN_7
#define Relay1_GPIO_Port GPIOE
#define Relay2_Pin GPIO_PIN_8
#define Relay2_GPIO_Port GPIOE
#define Relay3_Pin GPIO_PIN_9
#define Relay3_GPIO_Port GPIOE
#define Relay4_Pin GPIO_PIN_10
#define Relay4_GPIO_Port GPIOE
#define Relay5_Pin GPIO_PIN_11
#define Relay5_GPIO_Port GPIOE
#define Relay6_Pin GPIO_PIN_12
#define Relay6_GPIO_Port GPIOE
#define Relay7_Pin GPIO_PIN_13
#define Relay7_GPIO_Port GPIOE
#define Relay8_Pin GPIO_PIN_14
#define Relay8_GPIO_Port GPIOE
#define Relay9_Pin GPIO_PIN_15
#define Relay9_GPIO_Port GPIOE
#define Relay16_Pin GPIO_PIN_4
#define Relay16_GPIO_Port GPIOG

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
