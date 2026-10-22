/**
  ******************************************************************************
  * file           : stm32c5xx_ll_example.c
  * brief          : DAC generate signals without using any trigger signal with LL API.
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

/* Includes ------------------------------------------------------------------*/
#include "ll_example.h"

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
#define DAC_DELAY_TRIM_US    (50UL)  /*!< Delay of 50 us for DAC minimum trimming time */
#define DAC_DELAY_STARTUP_US (15UL)  /*!< Delay of 15 us for DAC channel voltage settling time from DAC channel
                                          startup (transition from disable to enable) */

#define DAC_READY_TIMEOUT_US (100UL) /* C5 #34：DS15135 表 57 tWAKEUP 上限 7.5 us，留足余量 */
#define ADC_TIMEOUT_US       (2000UL)

/* Private macro -------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
/* Private functions prototype -----------------------------------------------*/
static uint32_t WaitFlag(uint32_t (*is_set)(const ADC_TypeDef *), uint32_t expected);

/* Private functions ---------------------------------------------------------*/

/**
  *  Calibrate the MX_DACx channel buffer.
  */
inline void DAC_CalibrateChannelBuffer(void)
{
  uint32_t output_buffer_mode;
  uint32_t trimming_value;
  uint32_t delta;
  uint32_t flag;

  /* Store output buffer configuration */
  output_buffer_mode = LL_DAC_GetOutputBuffer(MX_DACx, MX_DAC_CHANNEL);

  LL_DAC_Disable(MX_DACx, MX_DAC_CHANNEL);

  /* Set mode in MCR for calibration */
  LL_DAC_SetOutputBuffer(MX_DACx, MX_DAC_CHANNEL, LL_DAC_OUTPUT_BUFFER_ENABLE);

  /* Enable the selected DAC channel calibration */
  LL_DAC_SetMode(MX_DACx, MX_DAC_CHANNEL, LL_DAC_MODE_CALIBRATION);

  /* Init trimming counter */
  /* Medium value */
  trimming_value = 0x10UL;
  delta = 0x08UL;

  while (delta != 0UL)
  {
    /* Set candidate trimming */
    LL_DAC_SetTrimmingValue(MX_DACx, MX_DAC_CHANNEL, (trimming_value & DAC_CCR_OTRIM1));

    /* Wait minimum time needed between two calibration steps (OTRIM) */
    DelayUs(DAC_DELAY_TRIM_US);

    flag = LL_DAC_IsActiveFlag_CAL(MX_DACx, MX_DAC_CHANNEL);
    if (flag == 1U)
    {
      /* DAC_SR_CAL_FLAGx is HIGH try lower trimming */
      trimming_value -= delta;
    }
    else
    {
      /* DAC_SR_CAL_FLAGx is LOW try higher trimming */
      trimming_value += delta;
    }

    delta >>= 1UL;
  }

  /* Still need to check if  trimming_value calibration is current value or one step below */
  /* Indeed the first value that causes the DAC_SR_CAL_FLAGx bit to change from 0 to 1  */
  /* Set candidate trimming */
  LL_DAC_SetTrimmingValue(MX_DACx, MX_DAC_CHANNEL, (trimming_value & DAC_CCR_OTRIM1));

  /* Wait minimum time needed between two calibration steps (OTRIM) */
  DelayUs(DAC_DELAY_TRIM_US);

  flag = LL_DAC_IsActiveFlag_CAL(MX_DACx, MX_DAC_CHANNEL);

  if (flag == 0UL)
  {
    /* Check trimming value below maximum */
    if (trimming_value < DAC_CCR_OTRIM1)
    {
      /* Trimming is actually one value more */
      trimming_value++;
    }

    /* Set right trimming */
    LL_DAC_SetTrimmingValue(MX_DACx, MX_DAC_CHANNEL, (trimming_value & DAC_CCR_OTRIM1));
  }

  /* Disable the DAC channel calibration */
  LL_DAC_SetMode(MX_DACx, MX_DAC_CHANNEL, LL_DAC_MODE_NORMAL_OPERATION);

  /* Restore configuration */
  LL_DAC_SetOutputBuffer(MX_DACx, MX_DAC_CHANNEL, output_buffer_mode);
}


/**
  *  Start the MX_DACx Channel in polling mode.
  */
inline uint32_t DAC_StartChannel(void)
{
  uint32_t start;

  LL_DAC_Enable(MX_DACx, MX_DAC_CHANNEL);

  /* C5 #34：RM0522 §22.4.4 规定 DAC1RDY 置位前不得写数据或触发，先等标志再等建立 */
  start = DWT->CYCCNT;
  while (LL_DAC_IsActiveFlag_DAC1RDY(MX_DACx) == 0U)
  {
    if ((DWT->CYCCNT - start) > (SystemCoreClock / 1000000U) * DAC_READY_TIMEOUT_US)
    {
      return 0U;
    }
  }

  /* Ensure minimum wait before using peripheral after enabling it */
  DelayUs(DAC_DELAY_STARTUP_US);
  return 1U;
}


/**
  * Set the MX_DACx data voltage output.
  * C5 #34：改为 12 位右对齐（DAC_DHR12R1），码值 0…4095 直接对应 VREF+ × DOR / 4096。
  */
inline void DAC_SetChannelData(uint16_t data)
{
  LL_DAC_ConvertData12RightAligned(MX_DACx, MX_DAC_CHANNEL, data);
}


/**
  * C5 #34：ADC1 配置，逐行沿用 #32 官方 ll/adc/multi_channels_dma 生成层 mx_adc1.c 的写法，
  * 只保留 PA4、VrefInt 两个位次，不用 DMA、不开中断；OVR 设为保留旧数据，主程序把 OVR 当错误。
  * PA4 复位即为模拟模式，DAC 与 ADC 共用此脚。
  */
inline void ADC_Config(void)
{
  LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_ADC12);

  LL_ADC_SetCommonPathInternalCh(ADC_COMMON_INSTANCE(MX_ADCx), LL_ADC_CHANNEL_VREFINT);

  LL_ADC_REG_SetContinuousMode(MX_ADCx, LL_ADC_REG_CONV_SINGLE);
  LL_ADC_REG_SetTriggerSource(MX_ADCx, LL_ADC_REG_TRIG_SOFTWARE);
  LL_ADC_REG_SetOverrun(MX_ADCx, LL_ADC_REG_OVR_DATA_PRESERVED);
  LL_ADC_REG_SetSequencerLength(MX_ADCx, LL_ADC_REG_SEQ_SCAN_ENABLE_2RANKS);

  LL_ADC_REG_SetSequencerRanks(MX_ADCx, LL_ADC_REG_RANK_1, LL_ADC_CHANNEL_4);
  LL_ADC_SetChannelSamplingTime(MX_ADCx, LL_ADC_CHANNEL_4, LL_ADC_SAMPLINGTIME_289CYCLES);
  LL_ADC_SetChannelSingleDiff(MX_ADCx, LL_ADC_CHANNEL_4, LL_ADC_IN_SINGLE_ENDED);
  LL_ADC_SetChannelPreselection(MX_ADCx, LL_ADC_CHANNEL_4);

  LL_ADC_REG_SetSequencerRanks(MX_ADCx, LL_ADC_REG_RANK_2, LL_ADC_CHANNEL_VREFINT);
  LL_ADC_SetChannelSamplingTime(MX_ADCx, LL_ADC_CHANNEL_VREFINT, LL_ADC_SAMPLINGTIME_289CYCLES);
  LL_ADC_SetChannelSingleDiff(MX_ADCx, LL_ADC_CHANNEL_VREFINT, LL_ADC_IN_SINGLE_ENDED);
}


/**
  * C5 #34：校准并使能 ADC。顺序同 #33 修正版：退出深度掉电 → 开稳压器并等待 → 校准 → 等 4 个 ADC 周期 → ADEN。
  */
inline uint32_t ADC_CalibrateEnable(void)
{
  uint32_t start;

  LL_ADC_DisableDeepPowerDown(MX_ADCx);
  LL_ADC_EnableInternalRegulator(MX_ADCx);
  DelayUs(LL_ADC_DELAY_INTERNAL_REGUL_STAB_US);

  LL_ADC_StartCalibration(MX_ADCx, LL_ADC_IN_SINGLE_ENDED);
  if (WaitFlag(LL_ADC_IsCalibrationOnGoing, 0U) == 0U)
  {
    return 0U;
  }

  /* 4 个 ADC 周期；CPU 144 MHz、ADC 18 MHz，取 8 倍 CPU 周期（同 #33） */
  start = DWT->CYCCNT;
  while ((DWT->CYCCNT - start) < (LL_ADC_DELAY_CALIB_ENABLE_ADC_CYCLES * 8U))
  {
  }

  LL_ADC_ClearFlag_ADRDY(MX_ADCx);
  start = DWT->CYCCNT;
  while (LL_ADC_IsActiveFlag_ADRDY(MX_ADCx) == 0U)
  {
    /* 校准结束后过早写 ADEN 会被清掉，未使能时重写（官方 ADC_Activate 同样处理） */
    if (LL_ADC_IsEnabled(MX_ADCx) == 0U)
    {
      LL_ADC_Enable(MX_ADCx);
    }
    if ((DWT->CYCCNT - start) > (SystemCoreClock / 1000000U) * ADC_TIMEOUT_US)
    {
      return 0U;
    }
  }
  return 1U;
}


/**
  * C5 #34：软件启动 ADC_AVG_COUNT 次两位次序列，按 EOC 逐个读 DR。
  */
inline uint32_t ADC_ReadSums(uint32_t *p_pa4_sum, uint32_t *p_vrefint_sum)
{
  uint32_t i;

  *p_pa4_sum = 0U;
  *p_vrefint_sum = 0U;
  LL_ADC_ClearFlag_OVR(MX_ADCx);
  for (i = 0U; i < ADC_AVG_COUNT; i++)
  {
    LL_ADC_REG_StartConversion(MX_ADCx);
    if (WaitFlag(LL_ADC_IsActiveFlag_EOC, 1U) == 0U)
    {
      return 1U;
    }
    *p_pa4_sum += LL_ADC_REG_ReadConversionData12(MX_ADCx);   /* 读 DR 清 EOC */
    if (WaitFlag(LL_ADC_IsActiveFlag_EOC, 1U) == 0U)
    {
      return 1U;
    }
    *p_vrefint_sum += LL_ADC_REG_ReadConversionData12(MX_ADCx);
    if (WaitFlag(LL_ADC_REG_IsConversionOngoing, 0U) == 0U)
    {
      return 1U;
    }
  }
  return (LL_ADC_IsActiveFlag_OVR(MX_ADCx) != 0U) ? 2U : 0U;
}


/**
  * C5 #34：等待 ADC 状态位变为 expected，超时返回 0。
  */
static uint32_t WaitFlag(uint32_t (*is_set)(const ADC_TypeDef *), uint32_t expected)
{
  uint32_t start = DWT->CYCCNT;

  while (is_set(MX_ADCx) != expected)
  {
    if ((DWT->CYCCNT - start) > (SystemCoreClock / 1000000U) * ADC_TIMEOUT_US)
    {
      return 0U;
    }
  }
  return 1U;
}


/**
  * C5 #34：官方用 volatile 空循环估算延时（注释自称 approximate）。改用 DWT 周期计数，
  * 保证 tTRIM ≥ 50 us（DS15135 p82）在 Debug/Release 下都成立；调用前 app_init 已开启 DWT。
  */
void DelayUs(uint32_t delay_us)
{
  uint32_t start = DWT->CYCCNT;

  while ((DWT->CYCCNT - start) < (SystemCoreClock / 1000000U) * delay_us)
  {
  }
}
