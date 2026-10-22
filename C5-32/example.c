/**
  ******************************************************************************
  * file           : example.c
  * brief          : ADC multi channels conversion triggered by software in DMA mode with LL API
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
/* C5 #32 改写（相对官方 ll/adc/multi_channels_dma），改动只在 application 目录，生成代码不动：
 * 1. 溢出模式改为"保留"（OVRMOD = 0）。官方是"覆盖"，RM0522 第 598 页写明 DMA 下覆盖会让 DMA 漏数据，
 *    缓冲区下标与通道的对应关系就会错位；保留模式下溢出时 ADC 停发 DMA 请求，已搬进内存的数据全部有效。
 * 2. 改为连续转换 + DMA 单次：一次启动读 ADC_SCANS 轮，DMA 传完最后一个数据后 ADC 与 DMA 一起停止（RM 第 599 页）。
 * 3. ADC/DAC 内核时钟分频由 4 改为 8（36 MHz → 18 MHz）：温度传感器采样 289 周期 = 16.06 µs，满足不少于 13 µs（同 #30）。
 * 4. 启动前缓冲区填哨兵 ADC_SENTINEL，传完后数还有几个槽没被写；缓冲区声明为 volatile（见 ll_example.h）。
 * 5. 温度改用 int32_t（官方用 uint16_t，负温度会出错）；VREF+ 用 VrefInt 实测，同时按官方假设的 3300 mV 再算一次作对照。
 * 6. 启动 ADC_PEEK_US 微秒后偷看一次缓冲区：演示 DMA 还在写的时候读它，读到的是什么。
 * 7. 等待 DMA 传完改为 DWT 计时的轮询加 ADC_TIMEOUT_MS 超时。本例 SysTick 没开中断（LL_Init1msTick 只置 CLKSOURCE 和 ENABLE），
 *    __WFI 只有 ADC、LPDMA 的中断能唤醒，超时靠不上它；超时返回错误，main 会进入 LED 闪烁（50 ms 亮、2 s 灭）。
 * 本例没有串口：结果全放在下面的全局变量里，用调试器的 Watch 窗口看。
 */
#define ADC_PEEK_US     400U   /* 启动后多久偷看缓冲区 */
#define ADC_TIMEOUT_MS  20U    /* 整次转换理论上约 0.8 ms，超时只是兜底 */

/* Private macro -------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
/* 序列位次：[0] PA4，[1] VrefInt，[2] 温度传感器（与 mx_adc1.c 里 rank 1、2、3 一一对应） */
uint16_t g_raw_avg[ADC_RANKS];     /* 每个通道 ADC_SCANS 次的平均（计数） */
uint16_t g_raw_min[ADC_RANKS];
uint16_t g_raw_max[ADC_RANKS];
uint32_t g_vrefplus_mv;            /* 由 VrefInt 平均值求出的 VREF+（mV） */
uint32_t g_pa4_mv;                 /* PA4 电压（mV），按实测 VREF+ 换算 */
uint32_t g_pa4_mv_assumed;         /* PA4 电压（mV），按官方假设的 3300 mV 换算，用来对照 */
int32_t  g_temp_degc;              /* 温度（°C），按实测 VREF+ 换算 */
int32_t  g_temp_degc_assumed;      /* 温度（°C），按官方假设的 3300 mV 换算 */

/* 验证 DMA 与 ADC 行为的观测量，都待上板读取 */
uint32_t g_unwritten;              /* 传完后仍是哨兵的槽数，预期 0 */
uint32_t g_peek_written;           /* 启动 ADC_PEEK_US 后已被 DMA 写过的槽数 */
uint32_t g_dma_remain;             /* 传完时 DMA 剩余字节数（BNDT），预期 0 */
uint32_t g_adstart_after;          /* 传完时 ADSTART 位，RM 没明说，待实测 */
uint32_t g_ovr_after;              /* 传完时 OVR 标志，预期 0 */
uint32_t g_run_cycles;             /* 启动到传输完成中断的 CPU 周期数（DWT） */
uint32_t g_rounds;                 /* 已完成的轮数 */

static volatile uint32_t g_tc_stamp;           /* 传输完成中断里记下的 DWT 计数 */
/* Set to 1 if the ADC conversion is correctly completed */
volatile uint8_t ADCConversionComplete = 0U;
/* Set to 1 if a transmission or a reception error is detected */
#if defined(USE_LL_APP_ERROR) && (USE_LL_APP_ERROR == 1)
volatile uint8_t ADCConversionError;
#endif /* USE_LL_APP_ERROR */

/* Private functions ---------------------------------------------------------*/
/* Private functions prototype -----------------------------------------------*/
static uint32_t CountWritten(void);

app_status_t app_init(void)
{
  app_status_t return_status = EXEC_STATUS_ERROR;

  /** ########## Step 1 ##########
    * The init of the MX_ADCx instance is triggered by the applicative code.
    */
  if (mx_adcx_init() == NULL)
  {
    goto _app_init_exit;
  }

  /** C5 #32 改写 1～3：必须放在 ADC 使能之前。
    * ADCDACPRE 使能后不得改（RM0522 第 316 页），CONT、OVRMOD 要求 ADSTART = 0（第 643 页）。
    */
  LL_RCC_SetADCDACPrescaler(LL_RCC_ADCDAC_PRESCALER_8);
  LL_ADC_REG_SetOverrun(MX_ADCx, LL_ADC_REG_OVR_DATA_PRESERVED);
  LL_ADC_REG_SetContinuousMode(MX_ADCx, LL_ADC_REG_CONV_CONTINUOUS);

  DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;  /* DWT 周期计数器，用来量时间（#14） */
  DWT->CYCCNT = 0U;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  /** ########## Step 2 ##########
    * Performs the MX_ADCx activation and self calibration to improve measurements accuracy.
    * The calibration is a prerequisite to any ADC operation. It removes the systematic errors that
    * might vary from chip to chip.
    */
  if (ADC_Activate() != SYSTEM_OK)
  {
    goto _app_init_exit;
  }

  if (ADC_Calibrate() != SYSTEM_OK)
  {
    goto _app_init_exit;
  }

  return_status = EXEC_STATUS_INIT_OK;

_app_init_exit:
  return return_status;
} /* end app_init */


app_status_t app_process(void)
{
  uint32_t sum[ADC_RANKS] = {0U};
  uint32_t scan;
  uint32_t rank;
  uint32_t t0;
  uint32_t peek_cycles    = (SystemCoreClock / 1000000U) * ADC_PEEK_US;
  uint32_t timeout_cycles = (SystemCoreClock / 1000U) * ADC_TIMEOUT_MS;

  ADCConversionComplete = 0U;

  /** ########## Step 3 ##########
    * 缓冲区填哨兵后启动 DMA 与 ADC。之后 ADC 连续转换，每个通道转换完成就由 DMA 搬一个半字，
    * 缓冲区下标 = 轮数 × ADC_RANKS + 位次，一共搬 ADC_BUFFER_SIZE 个就停。
    */
  t0 = DWT->CYCCNT;
  ADC_StartConversion_DMA();

  /** ########## Step 4 ##########
    * 演示：DMA 还在写的时候读缓冲区，这里只数写过几个槽，读到的数据不使用。
    */
  while ((DWT->CYCCNT - t0) < peek_cycles)
  {
  }
  g_peek_written = CountWritten();

  /** ########## Step 5 ##########
    * 等 LPDMA 传输完成中断（回调里置标志）；超时则放弃本轮。
    */
  while ((ADCConversionComplete == 0U) && ((DWT->CYCCNT - t0) < timeout_cycles))
  {
  }

  /* 传完（或超时）的瞬间先记下 DMA 与 ADC 的状态，再收尾 */
  g_dma_remain    = LL_DMA_GetBlkDataLength(MX_LPDMAX_CHY_CH);
  g_adstart_after = LL_ADC_REG_IsConversionOngoing(MX_ADCx);
  g_ovr_after     = LL_ADC_IsActiveFlag_OVR(MX_ADCx);
  if (ADCConversionComplete == 0U)
  {
    return EXEC_STATUS_ERROR;
  }
  g_run_cycles = g_tc_stamp - t0;

  /* 收尾：RM 只说传完后 ADC 与 DMA 停止；若 ADSTART 仍为 1，用 ADSTP 停掉，保证下一轮能重新启动 */
  if (g_adstart_after != 0U)
  {
    LL_ADC_REG_StopConversion(MX_ADCx);
    while ((LL_ADC_REG_IsStopConversionOngoing(MX_ADCx) != 0U) && ((DWT->CYCCNT - t0) < timeout_cycles))
    {
    }
  }
  LL_ADC_ClearFlag_OVR(MX_ADCx);

  /** ########## Step 6 ##########
    * 传完之后才读缓冲区：按 下标 = 轮数 × ADC_RANKS + 位次 分通道统计。
    */
  g_unwritten = ADC_BUFFER_SIZE - CountWritten();
  for (rank = 0U; rank < ADC_RANKS; rank++)
  {
    g_raw_min[rank] = ADC_SENTINEL;
    g_raw_max[rank] = 0U;
  }
  for (scan = 0U; scan < ADC_SCANS; scan++)
  {
    for (rank = 0U; rank < ADC_RANKS; rank++)
    {
      uint16_t v = ADCRawData[(scan * ADC_RANKS) + rank];
      sum[rank] += v;
      if (v < g_raw_min[rank])
      {
        g_raw_min[rank] = v;
      }
      if (v > g_raw_max[rank])
      {
        g_raw_max[rank] = v;
      }
    }
  }
  for (rank = 0U; rank < ADC_RANKS; rank++)
  {
    g_raw_avg[rank] = (uint16_t)((sum[rank] + (ADC_SCANS / 2U)) / ADC_SCANS);
  }

  g_vrefplus_mv       = LL_ADC_CALC_VREFANALOG_VOLTAGE(g_raw_avg[1], LL_ADC_RESOLUTION_12B);
  g_pa4_mv            = LL_ADC_CALC_DATA_TO_VOLTAGE(g_vrefplus_mv, g_raw_avg[0], LL_ADC_RESOLUTION_12B);
  g_pa4_mv_assumed    = LL_ADC_CALC_DATA_TO_VOLTAGE(VDD_VALUE, g_raw_avg[0], LL_ADC_RESOLUTION_12B);
  g_temp_degc         = LL_ADC_CALC_TEMPERATURE(g_vrefplus_mv, g_raw_avg[2], LL_ADC_RESOLUTION_12B);
  g_temp_degc_assumed = LL_ADC_CALC_TEMPERATURE(VDD_VALUE, g_raw_avg[2], LL_ADC_RESOLUTION_12B);
  g_rounds++;

  /* 传输完成了却还有槽没被写，说明缓冲区与 DMA 的约定被破坏，按错误处理 */
  return (g_unwritten == 0U) ? EXEC_STATUS_OK : EXEC_STATUS_ERROR;
} /* end app_process */


/**
  * Deinitializes the MX_ADCx instance.
  * In this example, app_deinit is never called and it is provided as a reference only.
  */
app_status_t app_deinit(void)
{
  mx_adcx_deinit();

  return EXEC_STATUS_OK;
} /* end app_deinit */


/** MX_ADCx conversion complete callback
  * user: This implementation of the MX_ADCx conversion complete callback can be customized.
  * This function is executed if the MX_ADCx conversion complete interrupt is generated.
  */
void ADC_ConversionCompleteCallback(void)
{
  /* Asynchronous processing related to step 3 */
  g_tc_stamp = DWT->CYCCNT;
  ADCConversionComplete = 1U;
}


#if defined(USE_LL_APP_ERROR) && (USE_LL_APP_ERROR == 1)
/** MX_ADCx conversion error callback
  * user: This implementation of the ADC conversion error callback can be customized.
  * This function is executed if the ADC conversion error interrupt is generated.
  */
void ADC_ConversionErrorCallback(void)
{
  /* Asynchronous processing related to step 3 */
  ADCConversionError = 1U;
}
#endif /* USE_LL_APP_ERROR */


/** 数缓冲区里已经被 DMA 写过的槽（值不是哨兵的） */
static uint32_t CountWritten(void)
{
  uint32_t i;
  uint32_t n = 0U;

  for (i = 0U; i < ADC_BUFFER_SIZE; i++)
  {
    if (ADCRawData[i] != ADC_SENTINEL)
    {
      n++;
    }
  }
  return n;
}
