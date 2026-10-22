/**
  ******************************************************************************
  * file           : example.c
  * brief          : This example shows how to configure the LPTIM to count
  *                  external pulses.
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
/* C5-17 改写：自己造脉冲，让 LPTIM1 数，看它什么时候开始漏数。
 * - LPTIM1 按官方 pulse_counter 的配置初始化：内核时钟 LSI，PA1 = LPTIM1_IN1，
 *   “外部同步”计数（输入被 LSI 采样），上升沿计数；
 * - 脉冲源：PA5（D13，板载 LD1）由软件写 BSRR 翻转，DWT 周期计数器定时；
 * - 接线：一根杜邦线 D13 → A1（PA1）；
 * - 每轮依次用 6 种半周期各发 200 个脉冲，记下 LPTIM1 数到多少；
 * - 不进 Stop 模式（官方例程会进），结果放在全局变量里，用调试器 Watch 看。
 */
/* Includes ------------------------------------------------------------------*/
#include "example.h"

/* Private define ------------------------------------------------------------*/
#define PULSES        200U
#define STEPS         6U
#define SETTLE_US     1000U  /* 复位计数器、发完脉冲后各等 1 ms：COUNTRST 要在 LSI 时钟域同步几拍 */
#define PA5_SET       (1UL << 5)
#define PA5_RESET     (1UL << (5 + 16))

/* Private variables ---------------------------------------------------------*/
hal_lptim_handle_t *pLPTIM;  /* pointer referencing the LPTIM handle from the generated code */

/* 高、低电平各持续多少微秒；LSI 约 32 kHz，一拍约 31 µs */
static const uint32_t half_us[STEPS] = {500U, 100U, 40U, 20U, 10U, 5U};

/* 以下变量放进 Watch */
volatile uint32_t g_counted[STEPS];  /* 每种半周期下 LPTIM1 数到的脉冲数（应为 200） */
volatile uint32_t g_sweeps;          /* 已完成的轮数 */
volatile uint32_t g_cnt_retry;       /* 读 CNT 时两次不一致、重读的次数 */

static uint32_t cycles_per_us;

/* Private functions prototype -----------------------------------------------*/
static void delay_us(uint32_t us);
static uint32_t lptim_read_cnt(void);

/** ########## Step 1 ##########
  * 初始化 LPTIM1（官方生成代码），打开 DWT，启动计数（不开中断）。
  */
app_status_t app_init(void)
{
  pLPTIM = mx_example_lptim_init();
  if (pLPTIM == NULL)
  {
    return EXEC_STATUS_ERROR;
  }

  DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0U;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  cycles_per_us = SystemCoreClock / 1000000U;

  GPIOA->BSRR = PA5_RESET; /* 脉冲源先拉低 */

  if (HAL_LPTIM_Start(pLPTIM) != HAL_OK)
  {
    return EXEC_STATUS_ERROR;
  }
  return EXEC_STATUS_INIT_OK;
} /* end app_init */

/** ########## Step 2 ##########
  * 一轮：6 种半周期，每种发 200 个脉冲，读 LPTIM1 的计数。
  */
app_status_t app_process(void)
{
  uint32_t i;
  uint32_t n;
  uint32_t start;

  for (i = 0U; i < STEPS; i++)
  {
    if (HAL_LPTIM_ResetCounter(pLPTIM) != HAL_OK)
    {
      return EXEC_STATUS_ERROR;
    }
    delay_us(SETTLE_US);
    start = lptim_read_cnt();

    for (n = 0U; n < PULSES; n++)
    {
      GPIOA->BSRR = PA5_SET;
      delay_us(half_us[i]);
      GPIOA->BSRR = PA5_RESET;
      delay_us(half_us[i]);
    }

    delay_us(SETTLE_US);
    g_counted[i] = (lptim_read_cnt() - start) & 0xFFFFU;
  }
  g_sweeps++;

  HAL_Delay(1000U);
  return EXEC_STATUS_INIT_OK; /* 与官方 main.c 的约定一致：返回后 LED 翻转一次 */
} /* end app_process */

/** De-initializes the LPTIM instance before leaving the scenario.
  * In this example, app_deinit is never called and it is provided as a reference only.
  */
app_status_t app_deinit(void)
{
  mx_example_lptim_deinit();
  return EXEC_STATUS_OK;
} /* end app_deinit */

/** DWT 忙等：us 微秒 */
static void delay_us(uint32_t us)
{
  uint32_t t0 = DWT->CYCCNT;
  uint32_t ticks = us * cycles_per_us;

  while ((DWT->CYCCNT - t0) < ticks)
  {
  }
}

/** CNT 在 LSI 时钟域里变化，手册要求连读两次、相等才可信 */
static uint32_t lptim_read_cnt(void)
{
  uint32_t a;
  uint32_t b;

  do
  {
    a = HAL_LPTIM_GetCounter(pLPTIM);
    b = HAL_LPTIM_GetCounter(pLPTIM);
    if (a != b)
    {
      g_cnt_retry++;
    }
  } while (a != b);
  return a;
}
