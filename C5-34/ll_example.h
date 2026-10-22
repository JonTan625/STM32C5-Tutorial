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
#include "stm32c5xx_ll_adc.h" /* C5 #34：ADC1 回读 PA4，LL ADC 为头文件内联接口，无需新增 .c */

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* Exported types ------------------------------------------------------------*/
/* Exported constants --------------------------------------------------------*/
/* C5 #34：生成层只有 DAC1，ADC1 由应用层自行初始化（配置沿用 #32 官方 ll/adc/multi_channels_dma 的生成层）。 */
#define MX_ADCx            ADC1
#define ADC_AVG_COUNT      (16U)  /* 每档回读平均次数 */

/* Exported macros -----------------------------------------------------------*/
/* Exported functions ------------------------------------------------------- */

/**
  * brief:  Calibrate the MX_DACx channel buffer.
  */
void DAC_CalibrateChannelBuffer(void);

/** brief:  Start the MX_DACx Channel in polling mode.
  * retval: 1 when DAC1RDY was seen before time-out, else 0
  */
uint32_t DAC_StartChannel(void);

/** brief: Set the MX_DACx 12-bit right-aligned data (C5 #34：官方为 8 位).
  */
void DAC_SetChannelData(uint16_t data);

/** brief: Configure MX_ADCx: PA4 (ADC1_IN4) then VrefInt, software trigger, single sequence.
  */
void ADC_Config(void);

/** brief: Calibrate and enable MX_ADCx.
  * retval: 1 on success, 0 on time-out
  */
uint32_t ADC_CalibrateEnable(void);

/** brief: Run ADC_AVG_COUNT sequences, return the sums of the two ranks.
  * retval: 0 OK, 1 time-out, 2 overrun
  */
uint32_t ADC_ReadSums(uint32_t *p_pa4_sum, uint32_t *p_vrefint_sum);

/** brief: Busy wait based on DWT->CYCCNT (DWT must be enabled first).
  */
void DelayUs(uint32_t delay_us);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* LL_EXAMPLE_H */
