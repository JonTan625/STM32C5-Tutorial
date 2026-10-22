/**
  ******************************************************************************
  * file           : example.c
  * brief          : Use a timer instead of the Cortex SysTick as the HAL timebase.
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
/* C5-14 改写：HAL 时基仍由 TIM6 提供（官方 timebase/tim 例程的做法）。
 * 新增两件事：
 *   1. 用 DWT 周期计数器量出每一次 HAL_Delay(500) 实际用了多少 HCLK 周期；
 *   2. 把 TIM6、SysTick 的关键寄存器存进全局变量。
 * 结果全部放在全局变量里，用调试器 Watch 观察，不需要串口。
 * 改 TICK_FREQ 可以在 1 kHz / 100 Hz / 10 Hz 三档之间切换（第 09 节练习）。
 */
/* Includes ------------------------------------------------------------------*/
#include "example.h"

/* Private define ------------------------------------------------------------*/
#define TICK_FREQ        HAL_TICK_FREQ_100HZ /* 官方例程同样是 100 Hz */
#define EXAMPLE_DELAY_MS 500U

/* Private variables ---------------------------------------------------------*/
/* 以下变量放进 Watch */
volatile uint32_t g_loops;           /* app_process 运行次数，也就是 LED 翻转次数 */
volatile uint32_t g_delay_cycles;    /* 上一次 HAL_Delay(500) 用掉的 HCLK 周期数 */
volatile uint32_t g_delay_us;        /* 换算成微秒 */
volatile uint32_t g_delay_us_min = 0xFFFFFFFFU;
volatile uint32_t g_delay_us_max;
volatile uint32_t g_tick_step_ms;    /* 每次节拍中断 uwTick 加多少，即 uwTickFreq */
volatile uint32_t g_tim6_psc;
volatile uint32_t g_tim6_arr;
volatile uint32_t g_tim6_cr1;
volatile uint32_t g_tim6_dier;
volatile uint32_t g_systick_ctrl;    /* SysTick 是否还在跑 */

/** ########## Step 1 ##########
  * 按 TICK_FREQ 重新配置 TIM6 时基，打开 DWT 周期计数器，记下寄存器。
  */
app_status_t app_init(void)
{
  if (HAL_InitTick(TICK_FREQ, USE_HAL_TICK_INT_PRIORITY) != HAL_OK)
  {
    return EXEC_STATUS_ERROR;
  }

  /* DWT->CYCCNT 每个 HCLK 周期加 1；先打开跟踪总开关 TRCENA */
  DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0U;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  g_tick_step_ms = (uint32_t)HAL_GetTickFreq();
  g_tim6_psc     = TIM6->PSC;
  g_tim6_arr     = TIM6->ARR;
  g_tim6_cr1     = TIM6->CR1;
  g_tim6_dier    = TIM6->DIER;
  g_systick_ctrl = SysTick->CTRL;

  return EXEC_STATUS_INIT_OK;
} /* end app_init */

/** ########## Step 2 ##########
  * 翻转 LED，再用 DWT 量这一次 HAL_Delay() 的实际长度。
  */
app_status_t app_process(void)
{
  uint32_t t0;
  uint32_t us;

  HAL_GPIO_TogglePin(MX_STATUS_LED_GPIO_PORT, MX_STATUS_LED_PIN);

  t0 = DWT->CYCCNT;
  HAL_Delay(EXAMPLE_DELAY_MS);
  g_delay_cycles = DWT->CYCCNT - t0;  /* 无符号相减，CYCCNT 回绕也不影响 */

  us = g_delay_cycles / (SystemCoreClock / 1000000U);
  g_delay_us = us;
  if (g_loops > 0U)                   /* 第一次 HAL_Delay 的起点相位不定，不计入最小、最大值 */
  {
    if (us < g_delay_us_min)
    {
      g_delay_us_min = us;
    }
    if (us > g_delay_us_max)
    {
      g_delay_us_max = us;
    }
  }
  g_loops++;

  return EXEC_STATUS_OK;
} /* end app_process */

/** In this example, app_deinit is never called and it is provided as a reference only.
  */
app_status_t app_deinit(void)
{
  /** This API is not used in this example (infinite loop).
    * It is optimized out by the toolchain.
    */
  return EXEC_STATUS_OK;
} /* end app_deinit */
