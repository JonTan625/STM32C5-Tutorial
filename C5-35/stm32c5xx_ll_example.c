/**
  ******************************************************************************
  * file           : stm32c5xx_ll_example.c
  * brief          : OPAMP programmable gain amplifier based on STM32 LL
  *                  C5 #35：加 DAC1 两通道（只接片内）与 COMP1，ADC1 回读 PA6 与 VrefInt。
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
/* MX_ADCx delay between end of calibration and enable operations (unit: CPU clock cycles). */
#define ADC_DELAY_CALIB_ENABLE_CPU_CYCLES (LL_ADC_DELAY_CALIB_ENABLE_ADC_CYCLES * 4U)

#define ADC_TIMEOUT_US        (2000UL)
#define DAC_READY_TIMEOUT_US  (100UL)  /* C5 #35：DS15135 表 57 缓冲关 tWAKEUP 上限 5 us，留足余量 */
#define COMP_STAB_US          (500UL)  /* DS15135 表 59 tSTART_SCALER 上限 250 us；LL 常量为 220 us，不用 */
#define OPAMP_STAB_US         (100UL)  /* DS15135 表 60 tWAKEUP 上限 2 us；LL 常量为 30 us */

/* Private macro -------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
/* Private functions prototype -----------------------------------------------*/
static uint32_t WaitFlag(uint32_t (*is_set)(const ADC_TypeDef *), uint32_t expected);

/* Private functions ---------------------------------------------------------*/

/**
  * C5 #35：官方 DelayUs 为 volatile 空循环估算。改用 DWT 周期计数，Debug/Release 下等待时间一致；
  * 调用前 app_init 已开启 DWT。
  */
void DelayUs(uint32_t delay_us)
{
  uint32_t start = DWT->CYCCNT;

  while ((DWT->CYCCNT - start) < (SystemCoreClock / 1000000U) * delay_us)
  {
  }
}


/**
  * C5 #35：生成层 mx_adc1_init 已配好 rank 1 = CH6（PA6）、289 周期、PSIS ÷4 = 36 MHz（mx_rcc.c）。
  * 这里补 rank 2 = VrefInt，并把 OVR 改为保留旧数据，主程序把 OVR 当错误。ADC 此时未使能。
  */
inline void ADC_Config(void)
{
  LL_ADC_SetCommonPathInternalCh(ADC_COMMON_INSTANCE(MX_ADCx), LL_ADC_CHANNEL_VREFINT);

  LL_ADC_REG_SetOverrun(MX_ADCx, LL_ADC_REG_OVR_DATA_PRESERVED);
  LL_ADC_REG_SetSequencerLength(MX_ADCx, LL_ADC_REG_SEQ_SCAN_ENABLE_2RANKS);

  /* 289 周期 / 36 MHz ≈ 8.0 us，满足 tS_vrefint ≥ 4.3 us（DS15135 p53） */
  LL_ADC_REG_SetSequencerRanks(MX_ADCx, LL_ADC_REG_RANK_2, LL_ADC_CHANNEL_VREFINT);
  LL_ADC_SetChannelSamplingTime(MX_ADCx, LL_ADC_CHANNEL_VREFINT, LL_ADC_SAMPLINGTIME_289CYCLES);
  LL_ADC_SetChannelSingleDiff(MX_ADCx, LL_ADC_CHANNEL_VREFINT, LL_ADC_IN_SINGLE_ENDED);
}


/**
  * C5 #35：校准并使能 ADC。顺序同 #33/#34：退出深度掉电 → 开稳压器并等待 → 校准 → 等 4 个 ADC 周期 → ADEN。
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

  /* 4 个 ADC 周期：CPU 144 MHz、ADC 36 MHz，即官方的 ADC_DELAY_CALIB_ENABLE_CPU_CYCLES */
  start = DWT->CYCCNT;
  while ((DWT->CYCCNT - start) < ADC_DELAY_CALIB_ENABLE_CPU_CYCLES)
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
  * C5 #35：软件启动 ADC_AVG_COUNT 次两位次序列，按 EOC 逐个读 DR，返回四舍五入平均值。
  */
inline uint32_t ADC_ReadAvg(uint32_t *p_pa6_avg, uint32_t *p_vrefint_avg)
{
  uint32_t i;
  uint32_t pa6_sum = 0U;
  uint32_t vrefint_sum = 0U;

  LL_ADC_ClearFlag_OVR(MX_ADCx);
  for (i = 0U; i < ADC_AVG_COUNT; i++)
  {
    LL_ADC_REG_StartConversion(MX_ADCx);
    if (WaitFlag(LL_ADC_IsActiveFlag_EOC, 1U) == 0U)
    {
      return 1U;
    }
    pa6_sum += LL_ADC_REG_ReadConversionData12(MX_ADCx);   /* 读 DR 清 EOC */
    if (WaitFlag(LL_ADC_IsActiveFlag_EOC, 1U) == 0U)
    {
      return 1U;
    }
    vrefint_sum += LL_ADC_REG_ReadConversionData12(MX_ADCx);
    if (WaitFlag(LL_ADC_REG_IsConversionOngoing, 0U) == 0U)
    {
      return 1U;
    }
  }
  *p_pa6_avg     = (pa6_sum + (ADC_AVG_COUNT / 2U)) / ADC_AVG_COUNT;
  *p_vrefint_avg = (vrefint_sum + (ADC_AVG_COUNT / 2U)) / ADC_AVG_COUNT;
  return (LL_ADC_IsActiveFlag_OVR(MX_ADCx) != 0U) ? 2U : 0U;
}


/**
  * C5 #35：DAC1 时钟与 HFSEL 写法沿用 #34 官方 ll/dac/simple_conversion 生成层 mx_dac1.c；
  * 内核时钟 ADCDACSEL/PRE 与 ADC 共用，已由生成层设为 PSIS ÷4。
  * MODEx = 011：缓冲关、只接片内外设（RM0522 §22.4.12 p683），PA4、PA5 引脚不被 DAC 占用，
  * PA5 仍由 LED 的 GPIO 驱动。MODEx 只能在 ENx = 0 时写（RM0522 DAC_MCR p705～706）。
  * 缓冲关无需偏置校准（校准针对输出缓冲，§22.4.12）。
  */
inline void DAC_Config(void)
{
  LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_DAC1);
  LL_DAC_SetHighFrequencyMode(MX_DACx, LL_DAC_HIGH_FREQ_MODE_ABOVE_80MHZ);
  LL_DAC_ConfigOutput(MX_DACx, LL_DAC_CHANNEL_1, LL_DAC_OUTPUT_MODE_NORMAL, LL_DAC_OUTPUT_BUFFER_DISABLE,
                      LL_DAC_OUTPUT_CONNECT_INTERNAL);
  LL_DAC_ConfigOutput(MX_DACx, LL_DAC_CHANNEL_2, LL_DAC_OUTPUT_MODE_NORMAL, LL_DAC_OUTPUT_BUFFER_DISABLE,
                      LL_DAC_OUTPUT_CONNECT_INTERNAL);
}


/**
  * C5 #35：RM0522 §22.4.4 规定 DACxRDY 置位前不得写数据；通道 2 的 DAC2RDY 等不到即记为通道不可用。
  */
inline uint32_t DAC_StartChannel(uint32_t channel)
{
  uint32_t start;

  LL_DAC_Enable(MX_DACx, channel);
  start = DWT->CYCCNT;
  while (((channel == LL_DAC_CHANNEL_1) ? LL_DAC_IsActiveFlag_DAC1RDY(MX_DACx)
          : LL_DAC_IsActiveFlag_DAC2RDY(MX_DACx)) == 0U)
  {
    if ((DWT->CYCCNT - start) > (SystemCoreClock / 1000000U) * DAC_READY_TIMEOUT_US)
    {
      return 0U;
    }
  }
  return 1U;
}


/**
  * C5 #35：12 位右对齐写入；TENx = 0 时一个 dac_hclk 周期后进入 DOR（RM0522 §22.4.6），
  * 调用方等待建立后再读 DOR 核对，不在写后立即读。
  */
inline void DAC_Write(uint32_t channel, uint32_t code)
{
  LL_DAC_ConvertData12RightAligned(MX_DACx, channel, code);
}


/**
  * C5 #35：COMP1 同相端 = DAC1 通道 1（INPSEL = 101，RM0522 表 169），反相端 = VrefInt 或其分压（表 172）。
  * 先关再改再开；开启后等待 COMP_STAB_US 覆盖 tSTART 与 tSTART_SCALER。
  */
inline void COMP_Restart(uint32_t input_minus, uint32_t hysteresis)
{
  LL_COMP_Disable(MX_COMPx);
  LL_COMP_SetPowerMode(MX_COMPx, LL_COMP_POWER_MODE_HIGH_SPEED);
  LL_COMP_ConfigInputs(MX_COMPx, input_minus, LL_COMP_INPUT_PLUS_DAC1_CH1);
  LL_COMP_SetInputHysteresis(MX_COMPx, hysteresis);
  LL_COMP_SetOutputPolarity(MX_COMPx, LL_COMP_OUTPUTPOL_NONINVERTED);
  LL_COMP_Enable(MX_COMPx);
  DelayUs(COMP_STAB_US);
}


/**
  * C5 #35：OPAMP1 同相端 = VP_SEL 11（“VINP3 pin or DAC output channel”，RM0522 p743），
  * 输出仍到 PA6（OPAINTOEN = 0）。寄存器在 OPAEN = 0 时修改（RM0522 §24.4.7 p730）。
  * gain = 1 为跟随（VM_SEL = 11），2/4/8/16 为同相 PGA（VM_SEL = 10，PGA_GAIN = 00xxx）。
  */
inline void OPAMP_Restart(uint32_t gain)
{
  LL_OPAMP_Disable(MX_OPAMPx);
  LL_OPAMP_SetInputNonInverting(MX_OPAMPx, LL_OPAMP_INPUT_NONINVERT_DAC1_CH2);
  if (gain == 1U)
  {
    LL_OPAMP_SetConfigurationMode(MX_OPAMPx, LL_OPAMP_MODE_FOLLOWER);
  }
  else
  {
    LL_OPAMP_SetConfigurationMode(MX_OPAMPx, LL_OPAMP_MODE_PGA);
    LL_OPAMP_SetPGAGain(MX_OPAMPx, (gain == 2U) ? LL_OPAMP_PGA_GAIN_2
                        : (gain == 4U) ? LL_OPAMP_PGA_GAIN_4
                        : (gain == 8U) ? LL_OPAMP_PGA_GAIN_8 : LL_OPAMP_PGA_GAIN_16);
  }
  LL_OPAMP_Enable(MX_OPAMPx);
  DelayUs(OPAMP_STAB_US);
}


/**
  * C5 #35：等待 ADC 状态位变为 expected，超时返回 0。
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
