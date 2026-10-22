/**
  ******************************************************************************
  * file           : example.c
  * brief          : ADC multi channels conversion triggered by software in DMA mode with LL API
  *                  C5 #36：串口可控的采集终端——应用层。状态机 IDLE/RUN，命令 START/STOP/STATUS/HELP，
  *                  主循环从 DMA 乒乓缓冲取半块、统计后每块输出一行。
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
#include <stdio.h>   /* snprintf */
#include <string.h>  /* strcmp */

/* Private typedef -----------------------------------------------------------*/
typedef enum
{
  STATE_IDLE = 0,
  STATE_RUN
} term_state_t;

/* Private define ------------------------------------------------------------*/
#define FW_VERSION     "C5-36 daq v1.0"
#define LINE_MAX       32U   /* 一行最多 31 个字符，加结尾 '\0'（同 #11） */
#define OUT_MAX        160U
#define LED_EVERY      8U    /* 运行时每 8 块翻转一次 LD1，约 0.5 s */

/* @user: 置 1 构造一个教学故障——每处理完一块就阻塞 150 ms，主循环跟不上 DMA。仅用于观察 lost 计数。 */
#ifndef FAULT_DEMO
#define FAULT_DEMO     0
#endif /* FAULT_DEMO */

/* Private macro -------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
/* 以下计数可放进 Watch，也可以用 STATUS 读 */
volatile uint32_t g_error;        /* 0 正常；1 时钟；2 采样模块初始化；3 停止超时 */
uint32_t g_state;                 /* term_state_t */
uint32_t g_blocks;                /* 本次 START 以来输出的块数 */
uint32_t g_lost;                  /* 主循环迟到、直接跳过的半块数 */
uint32_t g_overwritten;           /* 拷贝期间被 DMA 覆盖而丢弃的半块数 */
uint32_t g_adc_ovr;               /* ADC 溢出次数（每次都会自动 STOP） */
uint32_t g_dma_err_seen;          /* 已处理过的 DMA 错误数 */
uint32_t g_lines;                 /* 非空命令行 */
uint32_t g_bad_lines;             /* 未知命令或参数不对 */
uint32_t g_long_lines;            /* 超长行 */
uint32_t g_uptime_ms;             /* 由 DWT 累计 */

static uint32_t s_consumed;       /* 已处理（或丢弃）的半块数，与 g_sampler.halves 比较 */
static uint16_t s_copy[HALF_SIZE];
static char s_line[LINE_MAX];
static uint32_t s_line_len;
static uint8_t s_line_too_long;
static uint8_t s_prev_was_cr;
static uint32_t s_last_cyc;
static uint32_t s_cyc_rem;

/* Private functions prototype -----------------------------------------------*/
static void Reply(const char *text);
static void HandleByte(uint8_t c);
static void HandleLine(void);
static void RunCommand(const char *cmd);
static void CmdStart(void);
static void CmdStop(const char *why);
static void CmdStatus(void);
static void ServiceSampler(void);
static void UpdateUptime(void);


app_status_t app_init(void)
{
  if ((SystemCoreClock != 144000000U) || (mx_adcx_init() == NULL))
  {
    g_error = 1U;
    return EXEC_STATUS_ERROR;
  }

  DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0U;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  s_last_cyc = DWT->CYCCNT;

  if (Sampler_Init() != SYSTEM_OK)
  {
    g_error = 2U;
    return EXEC_STATUS_ERROR;
  }
  LL_Delay_NoISR(1U); /* VrefInt 通路稳定等待，同 #33 */

  Term_Init();
  g_state = STATE_IDLE;
  Reply("\r\n=== " FW_VERSION " ===\r\nPA4 (CN8 A2), 1000 Hz, 64-sample blocks\r\n"
        "commands: START STOP STATUS HELP\r\n> ");
  return EXEC_STATUS_INIT_OK;
} /* end app_init */


/** C5 #36：main 每 1 ms 调一次。先处理串口输入，再处理采样数据；两件事都不等待。 */
app_status_t app_process(void)
{
  uint8_t c;

  UpdateUptime();
  while (Term_GetByte(&c) != 0U)
  {
    HandleByte(c);
  }
  if (g_state == STATE_RUN)
  {
    ServiceSampler();
  }
  return (g_error == 0U) ? EXEC_STATUS_OK : EXEC_STATUS_ERROR;
} /* end app_process */


app_status_t app_deinit(void)
{
  (void)Sampler_Stop();
  mx_adcx_deinit();
  return EXEC_STATUS_OK;
} /* end app_deinit */


/**
  * C5 #36：DMA 与主循环的交接。g_sampler.halves 由中断加 1，第 k 个半块（从 0 数）在缓冲区的第 k % 2 半。
  * ① 落后两个以上：只留最新写满的那一半，其余记 lost；
  * ② 先把这一半拷出来，拷完再看计数：若已到 k + 2，说明 DMA 已开始重写这一半，拷贝作废，记 overwritten；
  * ③ 用拷贝出来的数据统计、输出。
  */
static void ServiceSampler(void)
{
  uint32_t done = g_sampler.halves;
  uint32_t k;
  uint32_t i;
  uint32_t sum = 0U;
  uint32_t vsum = 0U;
  uint16_t vmin = 0xFFFFU;
  uint16_t vmax = 0U;
  uint32_t avg;
  uint32_t vref_avg;
  uint32_t vref_mv;
  uint32_t mv;
  char out[OUT_MAX];
  int n;

  if (Sampler_IsOverrun() != 0U)
  {
    g_adc_ovr++;
    CmdStop("ERR adc overrun, stopped\r\n");
    return;
  }
  if (g_sampler.dma_errors != g_dma_err_seen)
  {
    g_dma_err_seen = g_sampler.dma_errors;
    CmdStop("ERR dma error, stopped\r\n");
    return;
  }
  if (done == s_consumed)
  {
    return;
  }
  if ((done - s_consumed) > 1U)
  {
    g_lost += (done - s_consumed) - 1U;
    s_consumed = done - 1U;
  }

  k = s_consumed;
  for (i = 0U; i < HALF_SIZE; i++)
  {
    s_copy[i] = ADCRawData[((k & 1U) * HALF_SIZE) + i];
  }
  s_consumed = k + 1U;
  if ((g_sampler.halves - k) >= 2U)
  {
    g_overwritten++;
    return;
  }

  for (i = 0U; i < BLOCK_SCANS; i++)
  {
    uint16_t v = s_copy[(i * ADC_RANKS) + 0U];
    sum += v;
    vsum += s_copy[(i * ADC_RANKS) + 1U];
    if (v < vmin)
    {
      vmin = v;
    }
    if (v > vmax)
    {
      vmax = v;
    }
  }
  avg = (sum + (BLOCK_SCANS / 2U)) / BLOCK_SCANS;
  vref_avg = (vsum + (BLOCK_SCANS / 2U)) / BLOCK_SCANS;
  vref_mv = (vref_avg != 0U) ? LL_ADC_CALC_VREFANALOG_VOLTAGE(vref_avg, LL_ADC_RESOLUTION_12B) : 0U;
  mv = LL_ADC_CALC_DATA_TO_VOLTAGE(vref_mv, avg, LL_ADC_RESOLUTION_12B);

  g_blocks++;
  n = snprintf(out, sizeof(out), "D %lu %lu %u %u %lu %lu\r\n", (unsigned long)g_blocks, (unsigned long)avg,
               (unsigned)vmin, (unsigned)vmax, (unsigned long)mv, (unsigned long)vref_mv);
  if ((n > 0) && ((uint32_t)n < sizeof(out)))
  {
    (void)Term_Write(out, (uint32_t)n);   /* 放不下就丢这一行，tx_drop 加 1 */
  }
  if ((g_blocks % LED_EVERY) == 0U)
  {
    LL_GPIO_TogglePin(MX_STATUS_LED_PORT, MX_STATUS_LED_PIN);
  }
#if (FAULT_DEMO != 0)
  DelayUs(150000U);   /* 构造故障：主循环阻塞 150 ms，DMA 照常写，约每次丢两个半块 */
#endif /* FAULT_DEMO */
}


static void CmdStart(void)
{
  if (g_state == STATE_RUN)
  {
    Reply("OK already running\r\n");
    return;
  }
  s_consumed = 0U;
  g_blocks = 0U;
  g_lost = 0U;
  g_overwritten = 0U;
  g_dma_err_seen = g_sampler.dma_errors;
  Sampler_Start();
  g_state = STATE_RUN;
  Reply("OK START 1000 Hz, D <block> <avg> <min> <max> <mV> <VREF+ mV>\r\n");
}


static void CmdStop(const char *why)
{
  if (g_state != STATE_RUN)
  {
    Reply("OK already stopped\r\n");
    return;
  }
  if (Sampler_Stop() == 0U)
  {
    g_error = 3U;   /* 停不下来：退出主循环，LD1 报错闪烁 */
  }
  g_state = STATE_IDLE;
  LL_GPIO_SetOutputPin(MX_STATUS_LED_PORT, MX_STATUS_LED_PIN);
  Reply(why);
}


static void CmdStatus(void)
{
  char out[OUT_MAX];
  int n;

  n = snprintf(out, sizeof(out), "OK %s up=%lus blocks=%lu lost=%lu overwritten=%lu adc_ovr=%lu dma_err=%lu\r\n",
               (g_state == STATE_RUN) ? "RUN" : "IDLE", (unsigned long)(g_uptime_ms / 1000U),
               (unsigned long)g_blocks, (unsigned long)g_lost, (unsigned long)g_overwritten,
               (unsigned long)g_adc_ovr, (unsigned long)g_sampler.dma_errors);
  if ((n > 0) && ((uint32_t)n < sizeof(out)))
  {
    (void)Term_Write(out, (uint32_t)n);
  }
  n = snprintf(out, sizeof(out), "   rx=%lu rx_drop=%lu rx_err=%lu tx_drop=%lu lines=%lu bad=%lu long=%lu\r\n",
               (unsigned long)g_term.rx_bytes, (unsigned long)g_term.rx_dropped, (unsigned long)g_term.rx_errors,
               (unsigned long)g_term.tx_dropped, (unsigned long)g_lines, (unsigned long)g_bad_lines,
               (unsigned long)g_long_lines);
  if ((n > 0) && ((uint32_t)n < sizeof(out)))
  {
    (void)Term_Write(out, (uint32_t)n);
  }
}


static void Reply(const char *text)
{
  (void)Term_Write(text, (uint32_t)strlen(text));
}


/** 逐字节处理，规则同 #11：CR、LF、CR+LF 只算一次回车；退格删一个字；控制字符丢弃；超长不存、回车时报错。 */
static void HandleByte(uint8_t c)
{
  if ((c == '\r') || (c == '\n'))
  {
    if ((c == '\n') && (s_prev_was_cr != 0U))
    {
      s_prev_was_cr = 0U;
      return;
    }
    s_prev_was_cr = (c == '\r') ? 1U : 0U;
    HandleLine();
    return;
  }
  s_prev_was_cr = 0U;
  if ((c == 0x08U) || (c == 0x7FU))
  {
    if ((s_line_len > 0U) && (s_line_too_long == 0U))
    {
      s_line_len--;
      Reply("\b \b");
    }
    return;
  }
  if ((c < 0x20U) || (c > 0x7EU))
  {
    return;
  }
  if (s_line_len < (LINE_MAX - 1U))
  {
    if ((c >= 'a') && (c <= 'z'))
    {
      c = (uint8_t)(c - 'a' + 'A');   /* 命令不分大小写 */
    }
    s_line[s_line_len] = (char)c;
    s_line_len++;
    (void)Term_Write((const char *)&c, 1U);
    return;
  }
  s_line_too_long = 1U;
}


static void HandleLine(void)
{
  char *p = s_line;
  char *end;

  Reply("\r\n");
  if (s_line_too_long != 0U)
  {
    g_long_lines++;
    Reply("ERR too long, max 31 chars\r\n");
  }
  else
  {
    s_line[s_line_len] = '\0';
    while (*p == ' ')
    {
      p++;
    }
    end = p + strlen(p);
    while ((end > p) && (end[-1] == ' '))
    {
      end--;
    }
    *end = '\0';
    if (*p != '\0')   /* 空行或全是空格：只重新给提示符 */
    {
      g_lines++;
      RunCommand(p);
    }
  }
  s_line_len = 0U;
  s_line_too_long = 0U;
  Reply("> ");
}


static void RunCommand(const char *cmd)
{
  if (strcmp(cmd, "START") == 0)
  {
    CmdStart();
  }
  else if (strcmp(cmd, "STOP") == 0)
  {
    CmdStop("OK STOP\r\n");
  }
  else if (strcmp(cmd, "STATUS") == 0)
  {
    CmdStatus();
  }
  else if (strcmp(cmd, "HELP") == 0)
  {
    Reply("OK START | STOP | STATUS | HELP\r\n");
  }
  else
  {
    g_bad_lines++;
    Reply("ERR unknown command, try HELP\r\n");
  }
}


/** DWT 32 位计数约 29.8 s 回绕；每 1 ms 累计一次差值，不会漏圈。 */
static void UpdateUptime(void)
{
  uint32_t now = DWT->CYCCNT;
  uint32_t per_ms = SystemCoreClock / 1000U;

  s_cyc_rem += now - s_last_cyc;
  s_last_cyc = now;
  g_uptime_ms += s_cyc_rem / per_ms;
  s_cyc_rem %= per_ms;
}
