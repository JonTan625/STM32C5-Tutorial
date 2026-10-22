/**
  ******************************************************************************
  * file           : example.c
  * brief          : example program body
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

/* C5 #21 改写：开机先报复位原因，再启动 1 s 的 IWDG；主循环每 200 ms 喂一次狗。
 * 按住 B1（PC13）就进入死循环、不再喂狗，约 1 s 后 IWDG 复位，下一次开机会报 IWDG。
 */

/* Private define ------------------------------------------------------------*/
#define IWDG_MIN_TIME_VALUE   0     /* 不用窗口 */
#define IWDG_MAX_TIME_VALUE   1000  /* ms，同官方 */
#define IWDG_EWU_TIME_VALUE   0     /* 不用提前唤醒中断 */
#define FEED_PERIOD_MS        200U
#define BUTTON_PORT           HAL_GPIOC
#define BUTTON_PIN            HAL_GPIO_PIN_13

/* Private variables ---------------------------------------------------------*/
hal_iwdg_handle_t *pIWDG;  /* pointer referencing the IWDG handle from the generated code */
uint32_t g_reset_flags;    /* 本次开机读到的 RCC_RSR 复位标志 */
uint32_t g_iwdg_pr;        /* HAL_IWDG_Start 之后读回的 IWDG_PR */
uint32_t g_iwdg_rlr;       /* HAL_IWDG_Start 之后读回的 IWDG_RLR */
uint32_t g_feed_count;     /* 本次开机喂狗次数 */

/* Private functions prototype -----------------------------------------------*/
static void print_reset_flags(uint32_t flags);
static app_status_t button_init(void);

app_status_t app_init(void)
{
  g_reset_flags = HAL_RCC_GetResetSource();
  HAL_RCC_ClearResetFlags(); /* 不清的话，下一次复位时旧标志还在，分不清是哪一次 */
  print_reset_flags(g_reset_flags);

  if (button_init() != EXEC_STATUS_OK)
  {
    return EXEC_STATUS_ERROR;
  }

  pIWDG = mx_example_iwdg_init();
  if (pIWDG == NULL)
  {
    return EXEC_STATUS_ERROR;
  }
  if (HAL_IWDG_Start(pIWDG, IWDG_MIN_TIME_VALUE, IWDG_MAX_TIME_VALUE, IWDG_EWU_TIME_VALUE) != HAL_OK)
  {
    return EXEC_STATUS_ERROR;
  }
  /* HAL_IWDG_Start 返回前已等 PVU/RVU 清零，此时读回的是生效值 */
  g_iwdg_pr  = IWDG->PR;
  g_iwdg_rlr = IWDG->RLR;
  PRINTF("[C5#21] IWDG PR=%" PRIu32 " RLR=%" PRIu32 ", feed every %u ms, hold B1 to stop feeding\n",
         g_iwdg_pr, g_iwdg_rlr, FEED_PERIOD_MS);
  return EXEC_STATUS_INIT_OK;
} /* end app_init */

app_status_t app_process(void)
{
  while (1)
  {
    HAL_Delay(FEED_PERIOD_MS);
    if (HAL_GPIO_ReadPin(BUTTON_PORT, BUTTON_PIN) == HAL_GPIO_PIN_SET)
    {
      PRINTF("[C5#21] B1 pressed at %" PRIu32 " ms, fed %" PRIu32 " times, now stuck\n",
             HAL_GetTick(), g_feed_count);
      while (1)
      {
        /* 模拟程序跑飞：既不喂狗也不返回 */
      }
    }
    if (HAL_IWDG_Refresh(pIWDG) != HAL_OK)
    {
      return EXEC_STATUS_ERROR;
    }
    g_feed_count++;
    led_toggle(MX_STATUS_LED);
  }
} /* end app_process */

/** Nothing to do because the IWDG IP cannot be de-initialized.
  */
app_status_t app_deinit(void)
{
  return EXEC_STATUS_OK;
} /* end app_deinit */

static void print_reset_flags(uint32_t flags)
{
  PRINTF("[C5#21] reset flags=0x%08" PRIX32 ":", flags);
  if ((flags & HAL_RCC_RESET_FLAG_PIN) != 0U)  { PRINTF(" PIN"); }
  if ((flags & HAL_RCC_RESET_FLAG_PWR) != 0U)  { PRINTF(" POR/BOR"); }
  if ((flags & HAL_RCC_RESET_FLAG_SW) != 0U)   { PRINTF(" SW"); }
  if ((flags & HAL_RCC_RESET_FLAG_IWDG) != 0U) { PRINTF(" IWDG"); }
  if ((flags & HAL_RCC_RESET_FLAG_WWDG) != 0U) { PRINTF(" WWDG"); }
  if ((flags & HAL_RCC_RESET_FLAG_LPWR) != 0U) { PRINTF(" LPWR"); }
  PRINTF("\n");
}

static app_status_t button_init(void)
{
  hal_gpio_config_t gpio_config;

  HAL_RCC_GPIOC_EnableClock();
  gpio_config.mode        = HAL_GPIO_MODE_INPUT;
  gpio_config.pull        = HAL_GPIO_PULL_NO;   /* 板上已有外部下拉（#08） */
  gpio_config.speed       = HAL_GPIO_SPEED_FREQ_LOW;
  gpio_config.output_type = HAL_GPIO_OUTPUT_PUSHPULL;
  gpio_config.alternate   = HAL_GPIO_AF_0;
  gpio_config.init_state  = HAL_GPIO_PIN_RESET;
  return (HAL_GPIO_Init(BUTTON_PORT, BUTTON_PIN, &gpio_config) == HAL_OK) ? EXEC_STATUS_OK : EXEC_STATUS_ERROR;
}
