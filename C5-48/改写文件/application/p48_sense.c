/**
  ******************************************************************************
  * file           : p48_sense.c
  * brief          : C5 #48 采样模块：ADC1 规则组两位次（VREFINT、温度传感器），软件触发、轮询。
  *                  通道配置与换算沿用 #30（已上板）；每次采样：HAL_ADC_Start（退出深度掉电、开稳压器并等待、
  *                  等内部通道稳定）→ 校准 → 两次转换 → 停止 → 深度掉电（RM0522 §21.4.6 p572）。
  *                  内核时钟改为 HCLK：醒来 48 MHz 时 ÷4 = 12 MHz；命令模式 144 MHz 时 ÷16 = 9 MHz。
  *                  fADC 须在 8～36 MHz（DS15135 表 53 p76），289 周期在两种时钟下都 ≥ 温度传感器要求的 13 µs。
  ******************************************************************************
  */
#include "p48.h"

#define P48_ADC_TIMEOUT_MS  2U

static hal_adc_handle_t *s_adc;

int32_t sense_init(void)
{
  hal_adc_reg_config_t reg_config;
  hal_adc_channel_config_t channel_config;

  s_adc = mx_example_adc_init();
  if (s_adc == NULL)
  {
    return -1;
  }
  /* 生成代码选的是 PSIS；Stop 醒来时 PSI 是停的，改用 HCLK（RM0522 p316，ADC 使能前才能改） */
  if (HAL_RCC_ADCDAC_SetKernelClkSource(HAL_RCC_ADCDAC_CLK_SRC_HCLK) != HAL_OK)
  {
    return -1;
  }
  reg_config.trigger_src       = HAL_ADC_REG_TRIG_SOFTWARE;
  reg_config.sequencer_length  = 2;
  reg_config.sequencer_discont = HAL_ADC_REG_SEQ_DISCONT_DISABLE;
  reg_config.continuous        = HAL_ADC_REG_CONV_SINGLE;
  reg_config.overrun           = HAL_ADC_REG_OVR_DATA_PRESERVED;
  if (HAL_ADC_REG_SetConfig(s_adc, &reg_config) != HAL_OK)
  {
    return -1;
  }
  channel_config.group          = HAL_ADC_GROUP_REGULAR;
  channel_config.sampling_time  = HAL_ADC_SAMPLING_TIME_289CYCLES;
  channel_config.input_mode     = HAL_ADC_IN_SINGLE_ENDED;
  channel_config.sequencer_rank = 1;
  if (HAL_ADC_SetConfigChannel(s_adc, HAL_ADC_CHANNEL_VREFINT, &channel_config) != HAL_OK)
  {
    return -1;
  }
  channel_config.sequencer_rank = 2;
  if (HAL_ADC_SetConfigChannel(s_adc, HAL_ADC_CHANNEL_TEMPSENSOR, &channel_config) != HAL_OK)
  {
    return -1;
  }
  return 0;
}

int32_t sense_sample(uint16_t *p_temp_raw, uint16_t *p_vref_raw)
{
  int32_t ret = -1;

  /* ADC 此时处于深度掉电、未使能，可以改分频 */
  if (HAL_RCC_ADCDAC_SetKernelClkPrescaler((SystemCoreClock > 100000000UL) ? HAL_RCC_ADCDAC_PRESCALER16
                                           : HAL_RCC_ADCDAC_PRESCALER4) != HAL_OK)
  {
    return -1;
  }
  if ((HAL_ADC_Start(s_adc) == HAL_OK) && (HAL_ADC_Calibrate(s_adc) == HAL_OK)
      && (HAL_ADC_REG_StartConv(s_adc) == HAL_OK) && (HAL_ADC_REG_PollForConv(s_adc, P48_ADC_TIMEOUT_MS) == HAL_OK))
  {
    *p_vref_raw = (uint16_t)HAL_ADC_REG_ReadConversionData(s_adc);   /* 位次 1：VREFINT */
    if (HAL_ADC_REG_PollForConv(s_adc, P48_ADC_TIMEOUT_MS) == HAL_OK)
    {
      *p_temp_raw = (uint16_t)HAL_ADC_REG_ReadConversionData(s_adc); /* 位次 2：温度 */
      ret = 0;
    }
  }
  (void)HAL_ADC_Stop(s_adc);
  LL_ADC_EnableDeepPowerDown(ADC1);   /* 校准随之丢失，下次醒来重新校准（DS 表 53 注：每次上电后建议校准） */
  return ret;
}

uint32_t sense_vdda_mv(uint16_t vref_raw)
{
  if (vref_raw == 0U)
  {
    return 0U;
  }
  return (LL_ADC_VREFINT_CAL_VREF * (uint32_t)(*LL_ADC_VREFINT_CAL_ADDR)) / vref_raw;
}

/* 两点插值：TS_CAL1 为 30 °C、TS_CAL2 为 140 °C，均在 VDDA = 3.3 V 下采得（DS15135 表 56 p81） */
int32_t sense_temp_x10(uint16_t temp_raw, uint16_t vref_raw)
{
  int32_t cal1 = (int32_t)(*LL_ADC_TEMPSENSOR_CAL1_ADDR);
  int32_t cal2 = (int32_t)(*LL_ADC_TEMPSENSOR_CAL2_ADDR);
  int32_t vdda = (int32_t)sense_vdda_mv(vref_raw);
  int32_t ts_at_cal;

  if ((vdda == 0) || (cal2 == cal1))
  {
    return INT32_MIN;
  }
  ts_at_cal = ((int32_t)temp_raw * vdda) / (int32_t)LL_ADC_TEMPSENSOR_CAL_VREFANALOG;
  return ((ts_at_cal - cal1) * (LL_ADC_TEMPSENSOR_CAL2_TEMP - LL_ADC_TEMPSENSOR_CAL1_TEMP) * 10) / (cal2 - cal1)
         + (LL_ADC_TEMPSENSOR_CAL1_TEMP * 10);
}
