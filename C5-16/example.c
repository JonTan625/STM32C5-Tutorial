/**
  ******************************************************************************
  * file           : example.c
  * brief          : This example shows how to use the TIM peripheral to measure
  *                  the frequency and the duty cycle of an external signal.
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
/* C5-16 改写：同一块板自己出信号、自己测。
 * - 信号源：TIM2 通道 1 从 PA5（D13，板载 LD1）输出 PWM，写法同 #15；
 *   每 3 s 换一组参数：1 kHz 25%、10 kHz 50%、50 Hz 10%、10 Hz 10%。
 * - 测量：官方 pwm_input 的 TIM1 配置原样保留，PA8（D7）进 TIM1_CH1，
 *   CCR1 记周期、CCR2 记高电平（复位模式，计数时钟 1 MHz）。
 * - 接线：一根杜邦线 D13 → D7。
 * - 结果每 500 ms 经 USART2（ST-LINK 虚拟串口，115200 8-N-1）打印一行。
 */
/* Includes ------------------------------------------------------------------*/
#include "example.h"
#include <stdarg.h> /* va_list */
#include <stdio.h>  /* vsnprintf */

/* Private typedef -----------------------------------------------------------*/
typedef struct
{
  uint32_t arr;   /* TIM2 ARR：周期 = (ARR + 1) µs */
  uint32_t ccr;   /* TIM2 CCR1：高电平 = CCR1 µs */
} pwm_preset_t;

/* Private define ------------------------------------------------------------*/
#define GEN_PSC          143U   /* TIM2：144 MHz / 144 = 1 MHz，1 µs 一个计数 */
#define PRESET_MS        3000U
#define REPORT_MS        500U
#define TX_TIMEOUT_MS    100U

/* Private variables ---------------------------------------------------------*/
hal_tim_handle_t *pTim;              /* TIM1：官方生成代码里的测量定时器 */
static hal_tim_handle_t hTIM2;       /* TIM2：本文的信号源 */
static hal_uart_handle_t *pUART;     /* USART2：mx_system_init() 已经初始化 */

static const pwm_preset_t presets[] =
{
  {999U, 250U},     /* 1 kHz，25% */
  {99U, 50U},       /* 10 kHz，50% */
  {19999U, 2000U},  /* 50 Hz，10% */
  {99999U, 10000U}, /* 10 Hz，10%：周期 100000 µs，超过 TIM1 的 16 位计数范围 */
};
#define PRESET_NUM (sizeof(presets) / sizeof(presets[0]))

/* 以下变量放进 Watch；由输入捕获中断写 */
volatile uint32_t g_cap_period;      /* 最近一次 CCR1：上升沿到上升沿的计数 */
volatile uint32_t g_cap_high;        /* 最近一次 CCR2：上升沿到下降沿的计数 */
volatile uint32_t g_caps;            /* 捕获中断次数 */

static uint32_t preset_idx;
static uint32_t preset_ms;
static uint32_t report_ms;
static uint32_t caps_at_report;
static uint32_t tim1_cnt_clk;

/* Private functions prototype -----------------------------------------------*/
static void InputCaptureCallback(hal_tim_handle_t *hTIM, hal_tim_channel_t channel);
static app_status_t tim2_gen_init(void);
static app_status_t gen_apply(uint32_t idx);
static app_status_t say(const char *fmt, ...);

app_status_t app_init(void)
{
  /** ########## Step 1 ##########
    * TIM1 按官方配置初始化，用于测量；TIM2 作为信号源。
    */
  pTim = mx_example_tim_init();
  pUART = mx_usart2_uart_gethandle();
  if ((pTim == NULL) || (pUART == NULL))
  {
    return EXEC_STATUS_ERROR;
  }
  if (HAL_TIM_RegisterInputCaptureCallback(pTim, InputCaptureCallback) != HAL_OK)
  {
    return EXEC_STATUS_ERROR;
  }

  /** ########## Step 2 ##########
    * 两个通道以中断方式开始捕获。
    */
  if ((HAL_TIM_IC_StartChannel_IT(pTim, MX_TIM_CHANNEL_Y) != HAL_OK)
      || (HAL_TIM_IC_StartChannel_IT(pTim, MX_TIM_CHANNEL_Z) != HAL_OK))
  {
    return EXEC_STATUS_ERROR;
  }
  tim1_cnt_clk = HAL_RCC_TIM_GetKernelClkFreq(MX_TIMX) / (MX_TIM_PRESCALER + 1U);

  if ((tim2_gen_init() != EXEC_STATUS_OK) || (gen_apply(0U) != EXEC_STATUS_OK))
  {
    return EXEC_STATUS_ERROR;
  }

  preset_ms = HAL_GetTick();
  report_ms = preset_ms;
  if (say("\r\n=== C5-16 capture lab v1.0 === TIM1 counter %lu Hz, wire D13(PA5) -> D7(PA8)\r\n",
          (unsigned long)tim1_cnt_clk) != EXEC_STATUS_OK)
  {
    return EXEC_STATUS_ERROR;
  }
  return EXEC_STATUS_INIT_OK;
} /* end app_init */

app_status_t app_process(void)
{
  uint32_t now = HAL_GetTick();
  uint32_t period;
  uint32_t high;
  uint32_t set_period;

  /* 每 3 s 换一组信号参数 */
  if ((uint32_t)(now - preset_ms) >= PRESET_MS)
  {
    preset_ms += PRESET_MS;
    preset_idx = (preset_idx + 1U) % PRESET_NUM;
    if (gen_apply(preset_idx) != EXEC_STATUS_OK)
    {
      return EXEC_STATUS_ERROR;
    }
  }

  /** ########## Step 4 ##########
    * 每 500 ms 打印一行：设定值 | 捕获到的计数 | 换算结果。
    */
  if ((uint32_t)(now - report_ms) < REPORT_MS)
  {
    return EXEC_STATUS_OK;
  }
  report_ms += REPORT_MS;

  period = g_cap_period;
  high = g_cap_high;
  set_period = presets[preset_idx].arr + 1U;

  if (g_caps == caps_at_report)
  {
    return say("[P%lu] set %lu Hz %lu.%lu%% | no capture: check wire D13 -> D7\r\n",
               (unsigned long)preset_idx, (unsigned long)(1000000U / set_period),
               (unsigned long)((presets[preset_idx].ccr * 1000U / set_period) / 10U),
               (unsigned long)((presets[preset_idx].ccr * 1000U / set_period) % 10U));
  }
  caps_at_report = g_caps;

  if (period == 0U)
  {
    return EXEC_STATUS_OK;
  }
  return say("[P%lu] set %lu Hz %lu.%lu%% | CCR1=%lu CCR2=%lu | meas %lu Hz %lu.%lu%%\r\n",
             (unsigned long)preset_idx, (unsigned long)(1000000U / set_period),
             (unsigned long)((presets[preset_idx].ccr * 1000U / set_period) / 10U),
             (unsigned long)((presets[preset_idx].ccr * 1000U / set_period) % 10U),
             (unsigned long)period, (unsigned long)high,
             (unsigned long)(tim1_cnt_clk / period),
             (unsigned long)((high * 1000U / period) / 10U), (unsigned long)((high * 1000U / period) % 10U));
} /* end app_process */

/** The app_deinit() function is provided as a reference only.
  */
app_status_t app_deinit(void)
{
  app_status_t return_status = EXEC_STATUS_ERROR;

  if (HAL_TIM_IC_StopChannel_IT(pTim, MX_TIM_CHANNEL_Y) != HAL_OK)
  {
    goto _app_deinit_exit;
  }
  if (HAL_TIM_IC_StopChannel_IT(pTim, MX_TIM_CHANNEL_Z) != HAL_OK)
  {
    goto _app_deinit_exit;
  }
  mx_example_tim_deinit();
  return_status = EXEC_STATUS_OK;
_app_deinit_exit:
  return return_status;
} /* end app_deinit */

/** 输入捕获中断（TIM1_CC_IRQn → HAL_TIM_IRQHandler → 本回调）。
  * 只在通道 1（上升沿）时取两个捕获值：此时 CCR2 里是上一个周期的下降沿。 */
static void InputCaptureCallback(hal_tim_handle_t *hTIM, hal_tim_channel_t channel)
{
  if (channel == MX_TIM_CHANNEL_Y)
  {
    g_cap_period = HAL_TIM_IC_ReadChannelCapturedValue(hTIM, MX_TIM_CHANNEL_Y);
    g_cap_high = HAL_TIM_IC_ReadChannelCapturedValue(hTIM, MX_TIM_CHANNEL_Z);
    g_caps++;
  }
}

/** TIM2 通道 1 → PA5（AF1），PWM mode 1，ARR、CCR1 预装载打开（同 #15） */
static app_status_t tim2_gen_init(void)
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

  config.prescaler              = GEN_PSC;
  config.counter_mode           = HAL_TIM_COUNTER_UP;
  config.period                 = presets[0].arr;
  config.repetition_counter     = 0;
  config.clock_sel.clock_source = HAL_TIM_CLK_INTERNAL;
  if ((HAL_TIM_SetConfig(&hTIM2, &config) != HAL_OK)
      || (HAL_TIM_OC_EnableComparePreload(&hTIM2, HAL_TIM_OC_COMPARE_UNIT_1) != HAL_OK)
      || (HAL_TIM_EnableAutoReloadPreload(&hTIM2) != HAL_OK))
  {
    return EXEC_STATUS_ERROR;
  }

  oc_config.polarity = HAL_TIM_OC_HIGH;
  oc_compare_unit_config.mode  = HAL_TIM_OC_PWM1;
  oc_compare_unit_config.pulse = presets[0].ccr;
  if ((HAL_TIM_OC_SetConfigChannel(&hTIM2, HAL_TIM_CHANNEL_1, &oc_config) != HAL_OK)
      || (HAL_TIM_OC_SetConfigCompareUnit(&hTIM2, HAL_TIM_OC_COMPARE_UNIT_1, &oc_compare_unit_config) != HAL_OK))
  {
    return EXEC_STATUS_ERROR;
  }

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

/** 换一组 ARR、CCR1：写进预装载寄存器，再写 UG 立即装入并让计数器从 0 开始 */
static app_status_t gen_apply(uint32_t idx)
{
  if ((HAL_TIM_SetPeriod(&hTIM2, presets[idx].arr) != HAL_OK)
      || (HAL_TIM_OC_SetCompareUnitPulse(&hTIM2, HAL_TIM_OC_COMPARE_UNIT_1, presets[idx].ccr) != HAL_OK)
      || (HAL_TIM_GenerateEvent(&hTIM2, HAL_TIM_SW_EVENT_UPD) != HAL_OK))
  {
    return EXEC_STATUS_ERROR;
  }
  return EXEC_STATUS_OK;
}

/** 格式化后经 USART2 轮询发送 */
static app_status_t say(const char *fmt, ...)
{
  char buf[160];
  va_list ap;
  int n;

  va_start(ap, fmt);
  n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n < 0)
  {
    return EXEC_STATUS_ERROR;
  }
  if ((uint32_t)n >= sizeof(buf))
  {
    n = (int)(sizeof(buf) - 1U);
  }
  return (HAL_UART_Transmit(pUART, buf, (uint32_t)n, TX_TIMEOUT_MS) == HAL_OK) ? EXEC_STATUS_OK : EXEC_STATUS_ERROR;
}
