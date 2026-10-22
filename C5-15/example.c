/**
  ******************************************************************************
  * file           : example.c
  * brief          : This example shows how to configure the TIM peripheral
  *                  in PWM (Pulse Width Modulation) mode.
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
/* C5-15 改写：不用 #06 的 TIM1（PA8/PA9），改由 TIM2 通道 1 从 PA5 输出 1 kHz PWM，
 * PA5 正是板载 LD1，占空比在 0～100% 之间来回变化，LD1 就“呼吸”起来。
 * - TIM2 的初始化写在本文件里，写法照官方 oc_toggle 例程生成的 mx_tim2.c；
 * - 运行中要改 CCR1，所以先开 CCR1 预装载与 ARR 预装载（#06 第 08 节）；
 * - 主循环每 10 ms 改一次亮度，沿用 #07 的 now - last 写法，不阻塞。
 */
/* Includes ------------------------------------------------------------------*/
#include "example.h"

/* Private define ------------------------------------------------------------*/
#define PWM_PSC          143U   /* 144 MHz / (143 + 1) = 1 MHz，计数节拍 1 µs */
#define PWM_PERIOD       1000U  /* ARR = 999：一圈 1000 µs，PWM 1 kHz */
#define LEVEL_MAX        100U   /* 亮度分 0～100 共 101 级 */
#define BREATH_STEP_MS   10U    /* 每 10 ms 走一级：亮 1 s、暗 1 s */

/* Private variables ---------------------------------------------------------*/
static hal_tim_handle_t hTIM2;

/* 以下变量放进 Watch */
volatile uint32_t g_level;          /* 当前亮度级 0～100 */
volatile uint32_t g_ccr;            /* 当前写入的 CCR1 */
volatile uint32_t g_breaths;        /* 完成的呼吸次数 */
volatile uint32_t g_tim2_psc;
volatile uint32_t g_tim2_arr;
volatile uint32_t g_tim2_cr1;
volatile uint32_t g_tim2_ccmr1;
volatile uint32_t g_tim2_ccer;
volatile uint32_t g_gpioa_moder;
volatile uint32_t g_gpioa_afrl;

static int32_t dir = 1;
static uint32_t last_ms;

/* Private functions prototype -----------------------------------------------*/
static app_status_t tim2_pwm_init(void);
static uint32_t level_to_ccr(uint32_t level);

/** ########## Step 1 ##########
  * 初始化 TIM2 通道 1 的 PWM，并把 PA5 交给 AF1。
  */
app_status_t app_init(void)
{
  if (tim2_pwm_init() != EXEC_STATUS_OK)
  {
    return EXEC_STATUS_ERROR;
  }

  g_tim2_psc    = TIM2->PSC;
  g_tim2_arr    = TIM2->ARR;
  g_tim2_cr1    = TIM2->CR1;
  g_tim2_ccmr1  = TIM2->CCMR1;
  g_tim2_ccer   = TIM2->CCER;
  g_gpioa_moder = GPIOA->MODER;
  g_gpioa_afrl  = GPIOA->AFR[0];

  last_ms = HAL_GetTick();
  return EXEC_STATUS_INIT_OK;
} /* end app_init */

/** ########## Step 2 ##########
  * 每 BREATH_STEP_MS 走一级亮度；到顶、到底时掉头。
  */
app_status_t app_process(void)
{
  uint32_t now = HAL_GetTick();

  if ((uint32_t)(now - last_ms) < BREATH_STEP_MS)
  {
    return EXEC_STATUS_OK;
  }
  last_ms += BREATH_STEP_MS;

  if ((dir > 0) && (g_level >= LEVEL_MAX))
  {
    dir = -1;
  }
  else if ((dir < 0) && (g_level == 0U))
  {
    dir = 1;
    g_breaths++;
  }
  g_level = (uint32_t)((int32_t)g_level + dir);

  g_ccr = level_to_ccr(g_level);
  /* CCR1 预装载已打开：这次写入先进预装载寄存器，下一次更新事件（周期边界）才生效 */
  if (HAL_TIM_OC_SetCompareUnitPulse(&hTIM2, HAL_TIM_OC_COMPARE_UNIT_1, g_ccr) != HAL_OK)
  {
    return EXEC_STATUS_ERROR;
  }
  return EXEC_STATUS_OK;
} /* end app_process */

/** 亮度级 → CCR：按平方走，k = 0..100 对应 CCR = k² / 10 = 0..1000。
  * 人眼对低亮度更敏感，线性占空比会显得“很快变亮、然后一直很亮”。 */
static uint32_t level_to_ccr(uint32_t level)
{
  return (level * level * PWM_PERIOD) / (LEVEL_MAX * LEVEL_MAX);
}

/** TIM2 通道 1：PWM mode 1、高有效，PA5 = TIM2_CH1（AF1） */
static app_status_t tim2_pwm_init(void)
{
  hal_tim_config_t config;
  hal_tim_oc_channel_config_t oc_config;
  hal_tim_oc_compare_unit_config_t oc_compare_unit_config;
  hal_gpio_config_t gpio_config;

  if (HAL_TIM_Init(&hTIM2, HAL_TIM2) != HAL_OK)
  {
    return EXEC_STATUS_ERROR;
  }
  HAL_RCC_TIM2_EnableClock();

  config.prescaler              = PWM_PSC;
  config.counter_mode           = HAL_TIM_COUNTER_UP;
  config.period                 = PWM_PERIOD - 1U;
  config.repetition_counter     = 0;
  config.clock_sel.clock_source = HAL_TIM_CLK_INTERNAL;
  if (HAL_TIM_SetConfig(&hTIM2, &config) != HAL_OK)
  {
    return EXEC_STATUS_ERROR;
  }

  /* 运行中要改占空比：先开预装载，再写 CCR（手册要求 PWM 模式使能 OCxPE） */
  if ((HAL_TIM_OC_EnableComparePreload(&hTIM2, HAL_TIM_OC_COMPARE_UNIT_1) != HAL_OK)
      || (HAL_TIM_EnableAutoReloadPreload(&hTIM2) != HAL_OK))
  {
    return EXEC_STATUS_ERROR;
  }

  oc_config.polarity = HAL_TIM_OC_HIGH;
  if (HAL_TIM_OC_SetConfigChannel(&hTIM2, HAL_TIM_CHANNEL_1, &oc_config) != HAL_OK)
  {
    return EXEC_STATUS_ERROR;
  }

  oc_compare_unit_config.mode  = HAL_TIM_OC_PWM1;
  oc_compare_unit_config.pulse = 0U;                 /* 从全灭开始 */
  if (HAL_TIM_OC_SetConfigCompareUnit(&hTIM2, HAL_TIM_OC_COMPARE_UNIT_1, &oc_compare_unit_config) != HAL_OK)
  {
    return EXEC_STATUS_ERROR;
  }

  /* 启动前产生一次更新事件，把预装载里的 ARR、CCR1 装进工作寄存器 */
  if (HAL_TIM_GenerateEvent(&hTIM2, HAL_TIM_SW_EVENT_UPD) != HAL_OK)
  {
    return EXEC_STATUS_ERROR;
  }

  /* PA5：原来是 LD1 的普通输出，改成复用功能 AF1 = TIM2_CH1 */
  HAL_RCC_GPIOA_EnableClock();
  gpio_config.mode        = HAL_GPIO_MODE_ALTERNATE;
  gpio_config.output_type = HAL_GPIO_OUTPUT_PUSHPULL;
  gpio_config.pull        = HAL_GPIO_PULL_NO;
  gpio_config.speed       = HAL_GPIO_SPEED_FREQ_LOW;
  gpio_config.alternate   = HAL_GPIO_AF_1;
  HAL_GPIO_Init(HAL_GPIOA, HAL_GPIO_PIN_5, &gpio_config);

  if ((HAL_TIM_OC_StartChannel(&hTIM2, HAL_TIM_CHANNEL_1) != HAL_OK)
      || (HAL_TIM_Start(&hTIM2) != HAL_OK))
  {
    return EXEC_STATUS_ERROR;
  }
  return EXEC_STATUS_OK;
}

/** The app_deinit() function is provided as a reference only.
  */
app_status_t app_deinit(void)
{
  (void)HAL_TIM_OC_StopChannel(&hTIM2, HAL_TIM_CHANNEL_1);
  (void)HAL_TIM_Stop(&hTIM2);
  (void)HAL_TIM_DeInit(&hTIM2);
  return EXEC_STATUS_OK;
} /* end app_deinit */
