/**
  ******************************************************************************
  * file           : ll_example.h
  * brief          : Header for stm32ynxx_ll_example.c files.
  *                  This file contains example-specific declarations to interface with example.c functions.
  ******************************************************************************
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
/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef LL_EXAMPLE_H
#define LL_EXAMPLE_H
/* Includes ------------------------------------------------------------------*/
#include "mx_hal_def.h"       /* aliases to the target-specific generated code */
#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */
/* Exported types ------------------------------------------------------------*/
/* Exported constants --------------------------------------------------------*/
#ifndef USE_LL_APP_TIMEOUT
#define USE_LL_APP_TIMEOUT 0U
#endif /* USE_LL_APP_TIMEOUT */

#ifndef USE_LL_APP_ERROR
#define USE_LL_APP_ERROR 0U
#endif /* USE_LL_APP_ERROR */

/* C5 #32 改写：序列 3 个通道，每次启动连续转换 ADC_SCANS 轮，一次 DMA 传完 */
#define ADC_RANKS       3U                        /* 序列长度：[0] PA4，[1] VrefInt，[2] 温度传感器 */
#define ADC_SCANS       16U                       /* 每次启动读几轮 */
#define ADC_BUFFER_SIZE (ADC_RANKS * ADC_SCANS)   /* 缓冲区半字数 48，DMA 块长度 96 字节 */
#define ADC_SENTINEL    0xFFFFU                   /* 12 位右对齐的数据不会出现的值，表示这个槽还没被 DMA 写过 */

/* DMA 写、CPU 读，所以是 volatile；下标 = 轮数 × ADC_RANKS + 位次 */
extern volatile uint16_t ADCRawData[ADC_BUFFER_SIZE];

/* Exported macros -----------------------------------------------------------*/
/* Exported functions ------------------------------------------------------- */

/** brief:  Activates the MX_ADCx instance.
  * retval: system status (see system_status_t)
  */
system_status_t ADC_Activate(void);

/** brief:  Calibrates the MX_ADCx instance.
  * retval: system status (see system_status_t)
  */
system_status_t ADC_Calibrate(void);

/**
  * brief:  Start the MX_ADCx regular channels conversion in DMA mode.
  * retval: system status (see system_status_t)
  */
void ADC_StartConversion_DMA(void);

/** brief:  MX_ADCx conversion complete callback.
  */
void ADC_ConversionCompleteCallback(void);

#if defined(USE_LL_APP_ERROR) && (USE_LL_APP_ERROR == 1)
/** brief:  MX_ADCx conversion complete callback. */
void ADC_ConversionErrorCallback(void);
#endif /* USE_LL_APP_ERROR */

#ifdef __cplusplus
}
#endif /* __cplusplus */
#endif /* LL_EXAMPLE_H */
