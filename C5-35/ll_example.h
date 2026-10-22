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
#include "stm32c5xx_ll_dac.h"  /* C5 #35：生成层没有 DAC、COMP，LL 为头文件内联接口，无需新增 .c */
#include "stm32c5xx_ll_comp.h"

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

/* C5 #35：DAC1 两个通道都只接片内外设，COMP1 只用一路（ES0676 §2.4.1：Rev Z 芯片 COMP1、COMP2 不能同时使能） */
#define MX_DACx            DAC1
#define MX_COMPx           COMP1
#define ADC_AVG_COUNT      (16U)  /* 每次回读平均次数 */

/* Exported macros -----------------------------------------------------------*/
/* Exported functions ------------------------------------------------------- */

/** brief: Busy wait based on DWT->CYCCNT (DWT must be enabled first).
  */
void DelayUs(uint32_t delay_us);

/** brief: Configure MX_ADCx: PA6 (ADC1_IN6, OPAMP1_VOUT) then VrefInt, software trigger, single sequence.
  */
void ADC_Config(void);

/** brief: Calibrate and enable MX_ADCx.
  * retval: 1 on success, 0 on time-out
  */
uint32_t ADC_CalibrateEnable(void);

/** brief: Run ADC_AVG_COUNT sequences, return the rounded averages of the two ranks.
  * retval: 0 OK, 1 time-out, 2 overrun
  */
uint32_t ADC_ReadAvg(uint32_t *p_pa6_avg, uint32_t *p_vrefint_avg);

/** brief: Configure both DAC1 channels: buffer off, on-chip peripherals only (MODEx = 011).
  */
void DAC_Config(void);

/** brief: Enable one DAC1 channel and wait for its DACxRDY flag.
  * retval: 1 when the flag was seen before time-out, else 0
  */
uint32_t DAC_StartChannel(uint32_t channel);

/** brief: Write a 12-bit right-aligned code (TENx = 0); read DOR back after the caller's settling wait.
  */
void DAC_Write(uint32_t channel, uint32_t code);

/** brief: Reconfigure MX_COMPx (disabled first), then enable it and wait for the scaler.
  */
void COMP_Restart(uint32_t input_minus, uint32_t hysteresis);

/** brief: Reconfigure OPAMP1 (disabled first) as follower (gain 1) or non-inverting PGA, input DAC1 channel 2.
  */
void OPAMP_Restart(uint32_t gain);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* LL_EXAMPLE_H */
