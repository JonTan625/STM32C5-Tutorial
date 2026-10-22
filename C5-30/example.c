/**
  ******************************************************************************
  * file           : example.c
  * brief          : ADC internal sensor temperature reading
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
#include "example.h"

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/

/* Assumption: pin Vref+ connected to Vdd at board level */
#define VREFPLUS_APPLI    VDD_VALUE

/* @user: set ADC conversion time out value in milliseconds */
#define TIME_OUT          10U

/* C5 #30 改写（相对官方 temperature_sensor）：
 * 1. ADC 时钟由 PSIS/4（36 MHz）改为 PSIS/8（18 MHz），289 个周期的采样时间从 8.03 us 变为 16.06 us，
 *    满足 DS15135 表 55 的 tS_temp >= 13 us；
 * 2. 规则组改为两个通道：第 1 位 VrefInt，第 2 位温度传感器；用 VREFINT_CAL 算出实际 VREF+，再代入温度公式；
 * 3. 温度变量改为有符号 int32_t（官方 uint16_t 存负温度会变成 65000 多）；
 * 4. 转换出错时 goto 退出（官方会被后面一行改回 EXEC_STATUS_OK）。
 */

/* Value of temperature calculated from ADC conversion data (unit: degree Celsius) */
int32_t TempDegreeCelsius = 0;

uint16_t g_vrefint_raw;          /* VrefInt 原始值 */
uint16_t g_tempsensor_raw;       /* 温度传感器原始值 */
uint32_t g_vrefplus_mv;          /* 由 VrefInt 算出的 VREF+（mV） */
int32_t  g_temp_assumed_3300;    /* 假设 VREF+ = 3300 mV 时算出的温度，用来对照 */

/* Private macro -------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
hal_adc_handle_t *pADC;  /* pointer referencing the ADC handle from the generated code */

/* Private functions prototype -----------------------------------------------*/

app_status_t app_init(void)
{
  app_status_t return_status = EXEC_STATUS_ERROR;

  /** ########## Step 1 ##########
    * The init of the ADC instance is triggered by the applicative code.
    */
  pADC = mx_example_adc_init();
  if (pADC == NULL)
  {
    goto _app_init_exit;
  }

  PRINTF("[INFO] Step 1: ADC initialization COMPLETED.\n");

  /* 改写 1：ADCDACPRE 只能在 ADC、DAC 都未使能时修改（RM0522 第 316 页），所以放在 HAL_ADC_Start() 之前。
   * 注意 ADC 与 DAC 共用这个分频。 */
  if (HAL_RCC_ADCDAC_SetKernelClkPrescaler(HAL_RCC_ADCDAC_PRESCALER8) != HAL_OK)
  {
    goto _app_init_exit;
  }

  /* 改写 2：规则组序列长度改为 2，第 1 位 VrefInt，第 2 位温度传感器，两者都用 289 个周期 */
  {
    hal_adc_reg_config_t reg_config;
    hal_adc_channel_config_t channel_config;

    reg_config.trigger_src       = HAL_ADC_REG_TRIG_SOFTWARE;
    reg_config.sequencer_length  = 2;
    reg_config.sequencer_discont = HAL_ADC_REG_SEQ_DISCONT_DISABLE;
    reg_config.continuous        = HAL_ADC_REG_CONV_SINGLE;
    reg_config.overrun           = HAL_ADC_REG_OVR_DATA_PRESERVED;   /* OVRMOD = 0：ADC_DR 带 8 级 FIFO（RM0522 第 591 页） */
    if (HAL_ADC_REG_SetConfig(pADC, &reg_config) != HAL_OK)
    {
      goto _app_init_exit;
    }

    channel_config.group          = HAL_ADC_GROUP_REGULAR;
    channel_config.sampling_time  = HAL_ADC_SAMPLING_TIME_289CYCLES;
    channel_config.input_mode     = HAL_ADC_IN_SINGLE_ENDED;
    channel_config.sequencer_rank = 1;
    if (HAL_ADC_SetConfigChannel(pADC, HAL_ADC_CHANNEL_VREFINT, &channel_config) != HAL_OK)
    {
      goto _app_init_exit;
    }
    channel_config.sequencer_rank = 2;
    if (HAL_ADC_SetConfigChannel(pADC, HAL_ADC_CHANNEL_TEMPSENSOR, &channel_config) != HAL_OK)
    {
      goto _app_init_exit;
    }
  }

  PRINTF("[C5#30] ADC clock PSIS/8, sampling 289 cycles; VREFINT_CAL %" PRIu16 ", TS_CAL1 %" PRIu16
         ", TS_CAL2 %" PRIu16 "\n", *LL_ADC_VREFINT_CAL_ADDR, *LL_ADC_TEMPSENSOR_CAL1_ADDR,
         *LL_ADC_TEMPSENSOR_CAL2_ADDR);

  /** ########## Step 2 ##########
    * Performs the ADC activation and self calibration to improve measurements accuracy.
    * The calibration is a prerequisite to any ADC operation. It removes the systematic errors that
    * might vary from chip to chip.
    */
  if (HAL_ADC_Start(pADC) != HAL_OK)
  {
    goto _app_init_exit;
  }

  if (HAL_ADC_Calibrate(pADC) != HAL_OK)
  {
    goto _app_init_exit;
  }

  PRINTF("[INFO] Step 2: ADC start and calibration COMPLETED.\n");
  return_status = EXEC_STATUS_INIT_OK;


_app_init_exit:
  return return_status;
} /* end app_init */


app_status_t app_process(void)
{
  app_status_t return_status = EXEC_STATUS_ERROR;
  /* ADC conversion data of channel temperature sensor (unit: digital value corresponding to ADC resolution) */
  uint16_t adc_conv_data_tempsensor_raw;
  /** ########## Step 3 ##########
    * Starts the ADC conversion in polling mode.
    */
  if (HAL_ADC_REG_StartConv(pADC) != HAL_OK)
  {
    goto _app_process_exit;
  }
  PRINTF("[INFO] Step 3: ADC start conversion COMPLETED.\n");
  /** ########## Step 4 ##########
    * Waits until the conversion is completed (or an error is raised).
    * Retrieves the raw ADC value, and computes the corresponding temperature.
  */
  /* 序列第 1 位：VrefInt。读 ADC_DR 会清 EOC（RM0522 第 588 页），再等第 2 位 */
  if (HAL_ADC_REG_PollForConv(pADC, TIME_OUT) != HAL_OK)
  {
    PRINTF("[ERROR] Step 4: ADC Error detected \n");
    goto _app_process_exit;   /* 改写 4：官方这里没有 goto，错误状态会被后面改回 OK */
  }
  g_vrefint_raw = HAL_ADC_REG_ReadConversionData(pADC);

  /* 序列第 2 位：温度传感器 */
  if (HAL_ADC_REG_PollForConv(pADC, TIME_OUT) != HAL_OK)
  {
    PRINTF("[ERROR] Step 4: ADC Error detected \n");
    goto _app_process_exit;
  }
  /* ADC conversion data obtained from the internal temperature sensor. */
  adc_conv_data_tempsensor_raw = HAL_ADC_REG_ReadConversionData(pADC);
  g_tempsensor_raw = adc_conv_data_tempsensor_raw;
  if (g_vrefint_raw == 0U)
  {
    goto _app_process_exit;   /* 防止下面除以 0 */
  }

  /* VREF+ = 3300 mV x VREFINT_CAL / VrefInt 原始值（RM0522 第 632 页） */
  g_vrefplus_mv = HAL_ADC_CALC_VREFANALOG_VOLTAGE(g_vrefint_raw, HAL_ADC_RESOLUTION_12_BIT);

  /* Compute the temperature sensor data temperature in DegC. */
  TempDegreeCelsius = HAL_ADC_CALC_TEMPERATURE(
                        g_vrefplus_mv,
                        adc_conv_data_tempsensor_raw,
                        HAL_ADC_RESOLUTION_12_BIT
                      );
  g_temp_assumed_3300 = HAL_ADC_CALC_TEMPERATURE(VREFPLUS_APPLI, adc_conv_data_tempsensor_raw,
                                                 HAL_ADC_RESOLUTION_12_BIT);
  PRINTF("[C5#30] raw vrefint %" PRIu16 ", temp %" PRIu16 "; VREF+ %" PRIu32 " mV; T %" PRId32
         " C (with 3300 mV assumed: %" PRId32 " C)\n", g_vrefint_raw, adc_conv_data_tempsensor_raw,
         g_vrefplus_mv, TempDegreeCelsius, g_temp_assumed_3300);
  return_status = EXEC_STATUS_OK;
_app_process_exit:
  return return_status;

} /* end app_process */

/** Deinitializes the ADC instance.
  * In this example, app_deinit is never called and it is provided as a reference only.
  */
app_status_t app_deinit(void)
{
  mx_example_adc_deinit();

  return EXEC_STATUS_OK;
} /* end app_deinit */
