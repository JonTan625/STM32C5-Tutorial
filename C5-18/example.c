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
/* C5-18 综合①：呼吸灯 + 按键调速 + 串口显示。只用 #07、#08、#11、#15 讲过的东西。
 * - 呼吸灯：TIM2 通道 1 → PA5（LD1），1 kHz PWM，写法同 #15；
 * - 调速：B1（PC13）每完整按下并释放一次，速度 1→2→3→4→1，消抖同 #08；
 * - 串口：USART2 单字节中断接收 + 环形缓冲 + 按行解析（#11 的思路，命令精简为 help/stat/speed N）；
 * - 主循环三个任务都不等待：按键任务、呼吸任务、串口任务。
 * 本工程借用官方 tim/pwm_input 的工程骨架（它同时带 TIM 与 UART 驱动），没有使用它的 TIM1 测量功能。
 */
/* Includes ------------------------------------------------------------------*/
#include "example.h"
#include <stdarg.h> /* va_list */
#include <stdio.h>  /* vsnprintf */
#include <string.h> /* strcmp */

/* Private define ------------------------------------------------------------*/
#define FW_VERSION          "C5-18 breath v1.0"
#define PWM_PSC             143U   /* TIM2：144 MHz / 144 = 1 MHz */
#define PWM_PERIOD          1000U  /* ARR = 999：1 kHz */
#define LEVEL_MAX           100U
#define SPEED_MIN           1U
#define SPEED_MAX           4U
#define SPEED_DEFAULT       2U
#define BUTTON_PORT         HAL_GPIOC
#define BUTTON_PIN          HAL_GPIO_PIN_13
#define BUTTON_SAMPLE_MS    5U     /* 同 #08：每 5 ms 采样 */
#define BUTTON_STABLE_COUNT 4U     /* 连续 4 次一致（约 20 ms）才算稳定 */
#define RX_RING_SIZE        64U    /* 必须是 2 的幂 */
#define LINE_MAX            24U
#define TX_TIMEOUT_MS       100U

/* Private variables ---------------------------------------------------------*/
static hal_tim_handle_t hTIM2;
static hal_uart_handle_t *pUART;   /* USART2：mx_system_init() 已初始化为 115200 8-N-1 */

/* 速度 1～4：每走一级亮度的间隔；一次完整呼吸 = 2 × 100 级 × 间隔 */
static const uint32_t step_ms_table[SPEED_MAX + 1U] = {0U, 20U, 10U, 5U, 2U};

/* 以下变量放进 Watch，也可以用 stat 命令读 */
volatile uint32_t g_speed = SPEED_DEFAULT;
volatile uint32_t g_level;
volatile uint32_t g_breaths;
volatile uint32_t g_clicks;
volatile uint32_t g_rx_bytes;
volatile uint32_t g_rx_dropped;
volatile uint32_t g_rx_errors;
volatile uint32_t g_bad_lines;

static int32_t breath_dir = 1;
static uint32_t breath_ms;
static uint32_t button_ms;
static hal_gpio_pin_state_t button_raw = HAL_GPIO_PIN_RESET;
static hal_gpio_pin_state_t button_stable = HAL_GPIO_PIN_RESET;
static uint32_t button_same;

static uint8_t rx_byte;
static uint8_t rx_ring[RX_RING_SIZE];
static volatile uint32_t rx_head;  /* 只由中断写 */
static volatile uint32_t rx_tail;  /* 只由主循环写 */
static char line_buf[LINE_MAX];
static uint32_t line_len;
static uint8_t line_too_long;

/* Private functions prototype -----------------------------------------------*/
static app_status_t tim2_pwm_init(void);
static app_status_t button_init(void);
static uint32_t button_task(uint32_t now);
static app_status_t breath_task(uint32_t now);
static app_status_t uart_task(void);
static app_status_t run_line(void);
static app_status_t set_speed(uint32_t speed, const char *why);
static app_status_t say(const char *fmt, ...);

app_status_t app_init(void)
{
  uint32_t now;

  pUART = mx_usart2_uart_gethandle();
  if ((pUART == NULL) || (tim2_pwm_init() != EXEC_STATUS_OK) || (button_init() != EXEC_STATUS_OK))
  {
    return EXEC_STATUS_ERROR;
  }

  /* 生成代码没有打开 USART2 中断（官方例程只用它打印），这里补上，入口函数在文件末尾 */
  HAL_CORTEX_NVIC_SetPriority(USART2_IRQn, HAL_CORTEX_NVIC_PREEMP_PRIORITY_0, HAL_CORTEX_NVIC_SUB_PRIORITY_0);
  HAL_CORTEX_NVIC_EnableIRQ(USART2_IRQn);
  /* 生成代码还只开了发送：direction 是 TX（CR1.RE=0），也只把 PA2 设成 AF7，PA3 没配。
   * 不补这两处，HAL_UART_Receive_IT 照样返回 OK，但一个字节也收不到（2026-10-14 上板发现） */
  {
    hal_gpio_config_t rx_pin;
    rx_pin.mode        = HAL_GPIO_MODE_ALTERNATE;
    rx_pin.pull        = HAL_GPIO_PULL_NO;
    rx_pin.speed       = HAL_GPIO_SPEED_FREQ_LOW;
    rx_pin.output_type = HAL_GPIO_OUTPUT_PUSHPULL;
    rx_pin.alternate   = HAL_GPIO_AF_7; /* PA3 = USART2_RX */
    rx_pin.init_state  = HAL_GPIO_PIN_RESET;
    if (HAL_GPIO_Init(HAL_GPIOA, HAL_GPIO_PIN_3, &rx_pin) != HAL_OK)
    {
      return EXEC_STATUS_ERROR;
    }
  }
  if ((HAL_UART_SetXferDirection(pUART, HAL_UART_DIRECTION_TX_RX) != HAL_OK)
      || (HAL_UART_Receive_IT(pUART, &rx_byte, 1U) != HAL_OK))
  {
    return EXEC_STATUS_ERROR;
  }

  now = HAL_GetTick();
  breath_ms = now;
  button_ms = now;
  if (say("\r\n=== %s === B1: next speed; commands: help, stat, speed 1..4\r\n", FW_VERSION) != EXEC_STATUS_OK)
  {
    return EXEC_STATUS_ERROR;
  }
  return set_speed(SPEED_DEFAULT, "boot");
} /* end app_init */

app_status_t app_process(void)
{
  uint32_t now = HAL_GetTick();

  if (button_task(now) != 0U)
  {
    g_clicks++;
    if (set_speed((g_speed >= SPEED_MAX) ? SPEED_MIN : (g_speed + 1U), "B1") != EXEC_STATUS_OK)
    {
      return EXEC_STATUS_ERROR;
    }
  }
  if (breath_task(now) != EXEC_STATUS_OK)
  {
    return EXEC_STATUS_ERROR;
  }
  return uart_task();
} /* end app_process */

/** The app_deinit() function is provided as a reference only.
  */
app_status_t app_deinit(void)
{
  return EXEC_STATUS_OK;
} /* end app_deinit */

/* ---------------- 呼吸任务（#15） ---------------- */
static app_status_t breath_task(uint32_t now)
{
  uint32_t step = step_ms_table[g_speed];
  uint32_t ccr;

  if ((uint32_t)(now - breath_ms) < step)
  {
    return EXEC_STATUS_OK;
  }
  breath_ms += step;

  if ((breath_dir > 0) && (g_level >= LEVEL_MAX))
  {
    breath_dir = -1;
  }
  else if ((breath_dir < 0) && (g_level == 0U))
  {
    breath_dir = 1;
    g_breaths++;
  }
  g_level = (uint32_t)((int32_t)g_level + breath_dir);
  ccr = (g_level * g_level * PWM_PERIOD) / (LEVEL_MAX * LEVEL_MAX);
  return (HAL_TIM_OC_SetCompareUnitPulse(&hTIM2, HAL_TIM_OC_COMPARE_UNIT_1, ccr) == HAL_OK) ? EXEC_STATUS_OK
         : EXEC_STATUS_ERROR;
}

/** 改速度：从下一级亮度起生效；节拍起点重设为现在，避免旧的积压一次补走好几级 */
static app_status_t set_speed(uint32_t speed, const char *why)
{
  uint32_t step;

  g_speed = speed;
  step = step_ms_table[speed];
  breath_ms = HAL_GetTick();
  return say("OK speed=%lu step=%lums breath=%lums (%s)\r\n", (unsigned long)speed, (unsigned long)step,
             (unsigned long)(2U * LEVEL_MAX * step), why);
}

/* ---------------- 按键任务（#08） ---------------- */
/** 返回 1：刚确认了一次完整的“按下→释放” */
static uint32_t button_task(uint32_t now)
{
  hal_gpio_pin_state_t raw;

  if ((uint32_t)(now - button_ms) < BUTTON_SAMPLE_MS)
  {
    return 0U;
  }
  button_ms = now;
  raw = HAL_GPIO_ReadPin(BUTTON_PORT, BUTTON_PIN);
  if (raw != button_raw)
  {
    button_raw = raw;
    button_same = 0U;
    return 0U;
  }
  if (button_same < BUTTON_STABLE_COUNT)
  {
    button_same++;
    if ((button_same == BUTTON_STABLE_COUNT) && (raw != button_stable))
    {
      button_stable = raw;
      if (raw == HAL_GPIO_PIN_RESET)
      {
        return 1U; /* B1 按下为高，稳定回到低电平 = 一次完整点击 */
      }
    }
  }
  return 0U;
}

static app_status_t button_init(void)
{
  hal_gpio_config_t gpio_config;

  HAL_RCC_GPIOC_EnableClock();
  gpio_config.mode        = HAL_GPIO_MODE_INPUT;
  gpio_config.pull        = HAL_GPIO_PULL_NO;   /* 板上已有外部下拉 */
  gpio_config.speed       = HAL_GPIO_SPEED_FREQ_LOW;
  gpio_config.output_type = HAL_GPIO_OUTPUT_PUSHPULL;
  gpio_config.alternate   = HAL_GPIO_AF_0;
  gpio_config.init_state  = HAL_GPIO_PIN_RESET;
  return (HAL_GPIO_Init(BUTTON_PORT, BUTTON_PIN, &gpio_config) == HAL_OK) ? EXEC_STATUS_OK : EXEC_STATUS_ERROR;
}

/* ---------------- 串口任务（#11） ---------------- */
static app_status_t uart_task(void)
{
  uint8_t c;

  while (rx_tail != rx_head)
  {
    c = rx_ring[rx_tail];
    rx_tail = (rx_tail + 1U) & (RX_RING_SIZE - 1U);

    if ((c == '\r') || (c == '\n'))
    {
      if ((line_len > 0U) || (line_too_long != 0U))
      {
        if (run_line() != EXEC_STATUS_OK)
        {
          return EXEC_STATUS_ERROR;
        }
      }
      line_len = 0U;
      line_too_long = 0U;
    }
    else if ((c >= 0x20U) && (c <= 0x7EU))
    {
      if (line_len < (LINE_MAX - 1U))
      {
        line_buf[line_len] = (char)c;
        line_len++;
      }
      else
      {
        line_too_long = 1U;
      }
    }
    else
    {
      /* 其他控制字符：丢弃 */
    }
  }
  return EXEC_STATUS_OK;
}

static app_status_t run_line(void)
{
  if (line_too_long != 0U)
  {
    g_bad_lines++;
    return say("ERR too long, max %u chars\r\n", (unsigned int)(LINE_MAX - 1U));
  }
  line_buf[line_len] = '\0';

  if (strcmp(line_buf, "help") == 0)
  {
    return say("OK help | stat | speed 1..4   (B1 also steps the speed)\r\n");
  }
  if (strcmp(line_buf, "stat") == 0)
  {
    return say("OK speed=%lu level=%lu breaths=%lu clicks=%lu rx=%lu drop=%lu err=%lu bad=%lu up=%lu\r\n",
               (unsigned long)g_speed, (unsigned long)g_level, (unsigned long)g_breaths,
               (unsigned long)g_clicks, (unsigned long)g_rx_bytes, (unsigned long)g_rx_dropped,
               (unsigned long)g_rx_errors, (unsigned long)g_bad_lines, (unsigned long)HAL_GetTick());
  }
  if ((strncmp(line_buf, "speed ", 6U) == 0) && (line_len == 7U)
      && (line_buf[6] >= (char)('0' + SPEED_MIN)) && (line_buf[6] <= (char)('0' + SPEED_MAX)))
  {
    return set_speed((uint32_t)(line_buf[6] - '0'), "uart");
  }
  g_bad_lines++;
  return say("ERR unknown: %s (try help)\r\n", line_buf);
}

/** 中断里调用：收到 1 字节，放进环形缓冲，立刻再挂下一次（#11） */
void HAL_UART_RxCpltCallback(hal_uart_handle_t *huart, uint32_t size_byte, hal_uart_rx_event_types_t rx_event)
{
  uint32_t next = (rx_head + 1U) & (RX_RING_SIZE - 1U);

  (void)size_byte;
  (void)rx_event;
  g_rx_bytes++;
  if (next != rx_tail)
  {
    rx_ring[rx_head] = rx_byte;
    rx_head = next;
  }
  else
  {
    g_rx_dropped++;
  }
  (void)HAL_UART_Receive_IT(huart, &rx_byte, 1U);
}

/** 中断里调用：ORE 会让 HAL 结束本次接收，要重新挂（#11 第 07 节） */
void HAL_UART_ErrorCallback(hal_uart_handle_t *huart)
{
  g_rx_errors++;
  if (HAL_UART_GetRxState(huart) == HAL_UART_RX_STATE_IDLE)
  {
    (void)HAL_UART_Receive_IT(huart, &rx_byte, 1U);
  }
}

/** USART2 中断入口：生成代码没有提供（官方例程不用它的中断），本文补上 */
void USART2_IRQHandler(void)
{
  HAL_UART_IRQHandler(pUART);
}

/* ---------------- TIM2 PWM（#15） ---------------- */
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
  if ((HAL_TIM_SetConfig(&hTIM2, &config) != HAL_OK)
      || (HAL_TIM_OC_EnableComparePreload(&hTIM2, HAL_TIM_OC_COMPARE_UNIT_1) != HAL_OK)
      || (HAL_TIM_EnableAutoReloadPreload(&hTIM2) != HAL_OK))
  {
    return EXEC_STATUS_ERROR;
  }

  oc_config.polarity = HAL_TIM_OC_HIGH;
  oc_compare_unit_config.mode  = HAL_TIM_OC_PWM1;
  oc_compare_unit_config.pulse = 0U;
  if ((HAL_TIM_OC_SetConfigChannel(&hTIM2, HAL_TIM_CHANNEL_1, &oc_config) != HAL_OK)
      || (HAL_TIM_OC_SetConfigCompareUnit(&hTIM2, HAL_TIM_OC_COMPARE_UNIT_1, &oc_compare_unit_config) != HAL_OK)
      || (HAL_TIM_GenerateEvent(&hTIM2, HAL_TIM_SW_EVENT_UPD) != HAL_OK))
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
