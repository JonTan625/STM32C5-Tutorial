/**
  ******************************************************************************
  * file           : example.c
  * brief          : DAC generate signals without using any trigger signal with LL API.
  *                  C5 #34：12 位七档输出，ADC1 在同一脚 PA4 回读比对。
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
#include "ll_example.h"

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/
#define BUFFER_SIZE     (7U)

/** C5 #34：按码值而不是按 mV 给档位，DAC 与 ADC 同用 VREF+，比较码值时 VREF+ 误差相消。
  * 0 与 4095 落在带缓冲输出范围（0.2 V ～ VDDA - 0.2 V，DS15135 表 57）之外，只观察、不验收。
  * @user: you can change the values to change the output voltage of the DAC.
  */
const uint16_t DACCodeArray[BUFFER_SIZE] = {0U, 400U, 1024U, 2048U, 3072U, 3700U, 4095U};

#define DAC_SETTLE_US      (1000U) /* 写码后等待；DS15135 p82 tSETTLING 上限 3 us */
#define BUDGET_LSB         (29)    /* DAC TUECal ±23 LSB（表 58）+ ADC ET 5.7 LSB（表 54），推算值 */

/* C5 #34：观测量，全部在调试器 Watch 中查看 */
typedef struct
{
  uint32_t valid;        /* 1：本档已测 */
  uint32_t code;         /* 写入 DHR12R1 的码值 */
  uint32_t dor;          /* 回读 DAC_DOR1 */
  uint32_t adc_avg;      /* PA4 16 次平均原始值 */
  int32_t  diff_lsb;     /* adc_avg - code */
  int32_t  expect_mv;    /* VREF+ × code / 4096（RM0522 §22.4.7） */
  int32_t  meas_mv;      /* ADC 换算 */
  uint32_t observe_only; /* 1：两端饱和档，不参与判据 */
  uint32_t in_budget;    /* 1：|diff_lsb| ≤ BUDGET_LSB */
} dac_point_t;

volatile dac_point_t g_points[BUFFER_SIZE];
volatile uint32_t g_step;            /* 已执行档数 */
volatile uint32_t g_sweeps;          /* 已完成整轮数 */
volatile uint32_t g_error;           /* 0 正常；1 时钟；2 ADC 校准；3 DAC 就绪；4 ADC 超时；5 OVR；6 DOR 不符；7 VrefInt 为 0 */
volatile uint32_t g_ccipr2;          /* RCC_CCIPR2 快照：ADCDACSEL / ADCDACPRE */
volatile uint32_t g_dac_mcr;         /* DAC_MCR 快照：HFSEL / MODE1 */
volatile uint32_t g_otrim_factory;   /* 用户校准前的 OTRIM1（出厂值） */
volatile uint32_t g_otrim_user;      /* 用户校准后的 OTRIM1 */
volatile uint32_t g_vrefint_avg;     /* 最近一档 VrefInt 平均原始值 */
volatile uint32_t g_vrefplus_mv;     /* 由 VrefInt 校准值反推的 VREF+ */

uint8_t DACArrayIndex = 0U;

/* Private functions prototype -----------------------------------------------*/

/** ########## Step 1 ##########
  * The init of the MX_DACx instance and the conversion start is triggered by the applicative code.
  * - MX_DACx is configured.
  * - MX_DACx channel buffer is calibrated.
  * - MX_DACx channel is started.
  */
app_status_t app_init(void)
{
  app_status_t return_status = EXEC_STATUS_ERROR;

  if (SystemCoreClock != 144000000U)
  {
    g_error = 1U;
    goto _app_init_exit;
  }

  if (mx_dacx_init() == NULL)
  {
    goto _app_init_exit;
  }

  /* C5 #34：ADC 与 DAC 共用内核时钟及分频（RM0522 §9.8.30，p316），PRE 只能在两者都未使能时改。
     PSIS 144 MHz ÷ 8 = 18 MHz，满足 ADC 8～36 MHz（DS15135 表 53）。此时 DAC EN1=0、ADC ADEN=0。 */
  LL_RCC_SetADCDACPrescaler(LL_RCC_ADCDAC_PRESCALER_8);
  g_ccipr2 = RCC->CCIPR2;

  DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0U;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  ADC_Config();
  if (ADC_CalibrateEnable() == 0U)
  {
    g_error = 2U;
    goto _app_init_exit;
  }

  /* The calibration allows a better output voltage precision */
  g_otrim_factory = LL_DAC_GetTrimmingValue(MX_DACx, MX_DAC_CHANNEL);
  DAC_CalibrateChannelBuffer();
  g_otrim_user = LL_DAC_GetTrimmingValue(MX_DACx, MX_DAC_CHANNEL);
  g_dac_mcr = MX_DACx->MCR;

  /* Start the MX_DACx channel */
  if (DAC_StartChannel() == 0U)
  {
    g_error = 3U;
    goto _app_init_exit;
  }

  LL_Delay_NoISR(1U); /* VrefInt 通路稳定等待，同 #33 */

  return_status = EXEC_STATUS_INIT_OK;

_app_init_exit:
  return return_status;
} /* end app_init */


/** ########## Step 2 ##########
  * At each call, the MX_DACx output the following code stored in the DACCodeArray array,
  * then ADC1 reads PA4 back (C5 #34).
  * The MX_DACx conversion is automatically done by setting a new value.
  */
app_status_t app_process(void)
{
  volatile dac_point_t *p = &g_points[DACArrayIndex];
  uint32_t code = DACCodeArray[DACArrayIndex];
  uint32_t pa4_sum;
  uint32_t vrefint_sum;
  uint32_t status;

  /* TEN1=0：写 DHR 后一个 dac_hclk 周期进入 DOR（RM0522 §22.4.6） */
  DAC_SetChannelData((uint16_t)code);
  DelayUs(DAC_SETTLE_US);

  p->valid = 0U;
  p->code  = code;
  p->dor   = LL_DAC_RetrieveOutputData(MX_DACx, MX_DAC_CHANNEL);
  if (p->dor != code)
  {
    g_error = 6U;
    return EXEC_STATUS_ERROR;
  }

  status = ADC_ReadSums(&pa4_sum, &vrefint_sum);
  if (status != 0U)
  {
    g_error = (status == 1U) ? 4U : 5U;
    return EXEC_STATUS_ERROR;
  }

  p->adc_avg     = (pa4_sum + (ADC_AVG_COUNT / 2U)) / ADC_AVG_COUNT;
  g_vrefint_avg  = (vrefint_sum + (ADC_AVG_COUNT / 2U)) / ADC_AVG_COUNT;
  if (g_vrefint_avg == 0U)
  {
    g_error = 7U;   /* VrefInt 为 0，不做除法换算 */
    return EXEC_STATUS_ERROR;
  }
  g_vrefplus_mv  = LL_ADC_CALC_VREFANALOG_VOLTAGE(g_vrefint_avg, LL_ADC_RESOLUTION_12B);
  p->diff_lsb    = (int32_t)p->adc_avg - (int32_t)code;
  p->expect_mv   = (int32_t)((g_vrefplus_mv * code) / 4096U);
  p->meas_mv     = LL_ADC_CALC_DATA_TO_VOLTAGE(g_vrefplus_mv, p->adc_avg, LL_ADC_RESOLUTION_12B);
  p->observe_only = ((code == 0U) || (code == 4095U)) ? 1U : 0U;
  p->in_budget   = ((p->diff_lsb <= BUDGET_LSB) && (p->diff_lsb >= -BUDGET_LSB)) ? 1U : 0U;
  p->valid       = 1U;

  g_step++;

  /* The DACArrayIndex is updated to change the next output voltage value */
  DACArrayIndex = (DACArrayIndex + 1) % BUFFER_SIZE;
  if (DACArrayIndex == 0U)
  {
    g_sweeps++;
  }

  return EXEC_STATUS_OK;
} /* end app_process */


/**
  * Deinitializes the MX_DACx instance.
  * In this example, app_deinit is never called and it is provided as a reference only.
  */
app_status_t app_deinit(void)
{
  mx_dacx_deinit();

  return EXEC_STATUS_OK;
} /* end app_deinit */
