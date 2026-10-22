/**
  ******************************************************************************
  * file           : example.c
  * brief          : C5 #46 项目二：可配置采集报警终端——唯一的状态机所有者（STOP/RUN/ALARM/FAULT）。
  *                  基线：官方 hal/adc/single_conversion_trigger_sw_it；需求与验收：#45 F01～F10、R45-01～18。
  *                  主循环每轮：取一行命令 → 处理采样/超时/过期 → 调度下一次转换 → 故障闪灯 → 推进后喂狗。
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
#include <string.h>
#include "p46.h"

/* Private typedef -----------------------------------------------------------*/
typedef enum
{
  P46_FAULT_NONE = 0,
  P46_FAULT_ADC_ERR,      /* 驱动报错或拒绝启动转换 */
  P46_FAULT_TIMEOUT,      /* 在途请求 >= 5 ms 未完成 */
  P46_FAULT_STALE         /* 距 START 或最后有效样本 >= 300 ms */
} p46_fault_t;

typedef enum
{
  P46_EV_NONE = 0,
  P46_EV_BOOT,
  P46_EV_START,
  P46_EV_STOP,
  P46_EV_ALARM_ON,
  P46_EV_ALARM_OFF,
  P46_EV_FAULT
} p46_event_t;

typedef struct
{
  p46_state_t state;
  uint16_t    low;
  uint16_t    high;
  uint16_t    raw;
  uint8_t     raw_valid;
  uint32_t    seq;          /* 本轮 START 以来的有效样本数 */
  uint32_t    per_min;      /* 相邻有效样本间隔（ms，软件时间戳） */
  uint32_t    per_max;
  uint32_t    miss;         /* 调度落后而跳过的周期数（不补采） */
  uint32_t    faults;       /* 累计进入 FAULT 的次数，STOP 不清 */
  p46_fault_t fault;        /* 最近一次故障原因，STOP 不清 */
  uint32_t    fault_seq;
  p46_event_t ev;           /* 最近事件，只存 RAM */
  uint32_t    ev_seq;
  uint8_t     ev_time_valid;
} p46_app_t;

/* Private define ------------------------------------------------------------*/
#if defined(P46_TEST) && (P46_TEST != 0)
#define P46_FW_NAME   "c5-46 v1 TEST"
#else
#define P46_FW_NAME   "c5-46 v1"
#endif /* P46_TEST */
#define P46_PER_CAP   9999U     /* STATUS 里间隔最多显示 4 位，保证整行 <= 256 字节 */

/* Private variables ---------------------------------------------------------*/
p46_app_t g_app;                /* 全局：调试器可直接查看 */

static p46_alarm_t s_alarm;
static uint32_t    s_next_due;
static uint32_t    s_last_valid_ms;
static uint32_t    s_blink_ms;
static uint8_t     s_time_ok;
static char        s_line[P46_LINE_MAX + 1U];
static char        s_out[P46_STATUS_MAX];
#if defined(P46_TEST) && (P46_TEST != 0)
static uint8_t     s_test_noreq;
#endif /* P46_TEST */

static const char *const s_state_name[] = {"STOP", "RUN", "ALARM", "FAULT"};
static const char *const s_fault_name[] = {"NONE", "ADC_ERR", "TIMEOUT", "STALE"};
static const char *const s_event_name[] = {"NONE", "BOOT", "START", "STOP", "ALARM_ON", "ALARM_OFF", "FAULT"};

/* Private functions prototype -----------------------------------------------*/
static void app_set_state(p46_state_t state, uint32_t now);
static void app_event(p46_event_t ev);
static void app_stop(uint32_t now);
static void app_fault(p46_fault_t fault, uint32_t now);
static void app_run(void);
static void app_command(const char *p_line, uint32_t now);
static void app_reply(const char *p_text);

app_status_t app_init(void)
{
  int32_t cfg;

  health_capture_reset();         /* 先快照复位原因，再做别的 */
  led_off(MX_STATUS_LED);

  if ((term_init() != 0) || (sampler_init() != 0))
  {
    return EXEC_STATUS_ERROR;
  }
  s_time_ok = (time_init() == 0) ? 1U : 0U;   /* LSE 起不来也照样采集，只是没有时间 */
  cfg = cfg_init(&g_app.low, &g_app.high);
  if (cfg < 0)
  {
    return EXEC_STATUS_ERROR;
  }

  g_app.state = P46_ST_STOP;
  app_event(P46_EV_BOOT);
  (void)snprintf(s_out, sizeof(s_out),
                 "BOOT %s reset=%s cfg=%s th=%u/%u used=%" PRIu32 "/%u foreign=%" PRIu32 " bad=%" PRIu32
                 " ecc=%" PRIu32 " time=%s\r\n",
                 P46_FW_NAME, health_reset_name(), (cfg > 0) ? "LOADED" : "DEFAULT",
                 (unsigned int)g_app.low, (unsigned int)g_app.high, g_cfg_scan.used, (unsigned int)P46_CFG_SLOTS,
                 g_cfg_scan.foreign, g_cfg_scan.bad, g_cfg_scan.ecc,
                 (s_time_ok == 0U) ? "NORTC" : ((time_valid() != 0U) ? "VALID" : "UNSET"));
  app_reply(s_out);

  if (health_start() != 0)        /* 最后才启动看门狗：初始化期间不会被它打断 */
  {
    return EXEC_STATUS_ERROR;
  }
  return EXEC_STATUS_INIT_OK;
} /* end app_init */

app_status_t app_process(void)
{
  uint32_t now = HAL_GetTick();
  p46_line_t line = term_poll_line(s_line);

  health_progress(P46_PROGRESS_TERM);
  if (line == P46_LINE_READY)
  {
    app_command(s_line, now);     /* 先处理控制命令，再处理样本：STOP 之后迟到的样本不会再点灯 */
  }
  else if (line == P46_LINE_TOO_LONG)
  {
    app_reply("ERR LONG\r\n");
  }

  app_run();

  now = HAL_GetTick();
  if ((g_app.state == P46_ST_FAULT) && ((now - s_blink_ms) >= P46_FAULT_BLINK_MS))
  {
    s_blink_ms += P46_FAULT_BLINK_MS;
    led_toggle(MX_STATUS_LED);
  }

  health_progress(P46_PROGRESS_LOOP);
  health_service();
  return EXEC_STATUS_OK;
} /* end app_process */

/** In this project, app_deinit is never called and it is provided as a reference only. */
app_status_t app_deinit(void)
{
  sampler_stop();
  mx_example_adc_deinit();
  return EXEC_STATUS_OK;
} /* end app_deinit */

static void app_reply(const char *p_text)
{
  term_write(p_text);
}

static void app_set_state(p46_state_t state, uint32_t now)
{
  g_app.state = state;
  if (state == P46_ST_ALARM)
  {
    led_on(MX_STATUS_LED);        /* PA5/LD1 高有效 */
  }
  else if (state == P46_ST_FAULT)
  {
    led_on(MX_STATUS_LED);        /* 之后每 250 ms 翻转一次 */
    s_blink_ms = now;
  }
  else
  {
    led_off(MX_STATUS_LED);
  }
}

static void app_event(p46_event_t ev)
{
  p46_datetime_t dt;

  g_app.ev = ev;
  g_app.ev_seq = g_app.seq;
  g_app.ev_time_valid = (time_get(&dt) == 0) ? 1U : 0U;   /* 时间值本身不存，只记当时是否有效 */
}

static void app_stop(uint32_t now)
{
  sampler_stop();
#if defined(P46_TEST) && (P46_TEST != 0)
  sampler_test_mask_complete(0U);
  s_test_noreq = 0U;
#endif /* P46_TEST */
  alarm_reset(&s_alarm);
  g_app.raw_valid = 0U;
  app_set_state(P46_ST_STOP, now);
  app_event(P46_EV_STOP);
}

static void app_fault(p46_fault_t fault, uint32_t now)
{
  sampler_stop();
  alarm_reset(&s_alarm);
  g_app.raw_valid = 0U;
  g_app.fault = fault;
  g_app.fault_seq = g_app.seq;
  g_app.faults++;
  app_set_state(P46_ST_FAULT, now);
  app_event(P46_EV_FAULT);
  (void)snprintf(s_out, sizeof(s_out), "EV FAULT %s seq=%" PRIu32 " tv=%u\r\n",
                 s_fault_name[fault], g_app.seq, (unsigned int)g_app.ev_time_valid);
  app_reply(s_out);
}

/* RUN/ALARM 下的采样流水：错误 → 取样本 → 超时 → 过期 → 调度 */
static void app_run(void)
{
  p46_sample_t smp;
  uint32_t since;
  uint32_t now;

  if ((g_app.state != P46_ST_RUN) && (g_app.state != P46_ST_ALARM))
  {
    return;
  }
  if (sampler_take_error() != 0U)
  {
    app_fault(P46_FAULT_ADC_ERR, HAL_GetTick());
    return;
  }
  if (sampler_take(&smp) != 0U)
  {
    p46_state_t next;

    if (g_app.seq != 0U)
    {
      uint32_t per = smp.tick - s_last_valid_ms;

      per = (per > P46_PER_CAP) ? P46_PER_CAP : per;
      g_app.per_min = ((g_app.seq == 1U) || (per < g_app.per_min)) ? per : g_app.per_min;
      g_app.per_max = (per > g_app.per_max) ? per : g_app.per_max;
    }
    g_app.seq++;
    g_app.raw = smp.raw;
    g_app.raw_valid = 1U;
    s_last_valid_ms = smp.tick;

    next = alarm_step(&s_alarm, g_app.state, smp.raw, g_app.low, g_app.high);
    if (next != g_app.state)
    {
      app_set_state(next, smp.tick);
      app_event((next == P46_ST_ALARM) ? P46_EV_ALARM_ON : P46_EV_ALARM_OFF);
      (void)snprintf(s_out, sizeof(s_out), "EV %s seq=%" PRIu32 " raw=%u tv=%u\r\n", s_event_name[g_app.ev],
                     g_app.seq, (unsigned int)smp.raw, (unsigned int)g_app.ev_time_valid);
      app_reply(s_out);
    }
  }

  now = HAL_GetTick();            /* 样本时间戳来自中断，必须在取样本之后再读当前时间 */
  if ((sampler_pending(&since) != 0U) && ((now - since) >= P46_ADC_TIMEOUT_MS))
  {
    app_fault(P46_FAULT_TIMEOUT, now);
    return;
  }
  if ((now - s_last_valid_ms) >= P46_STALE_MS)
  {
    app_fault(P46_FAULT_STALE, now);
    return;
  }
#if defined(P46_TEST) && (P46_TEST != 0)
  if (s_test_noreq != 0U)
  {
    return;                       /* 测试钩子：不再发起转换，用来验证 300 ms 过期 */
  }
#endif /* P46_TEST */
  if ((sampler_pending(&since) == 0U) && ((int32_t)(now - s_next_due) >= 0))
  {
    uint32_t skipped = (now - s_next_due) / P46_SAMPLE_PERIOD_MS;

    g_app.miss += skipped;        /* 落后整周期就跳过，不追赶补采 */
    s_next_due += (skipped + 1U) * P46_SAMPLE_PERIOD_MS;
    if (sampler_request(now) != 0)
    {
      app_fault(P46_FAULT_ADC_ERR, now);
    }
  }
}

/* ---- 命令解析：ASCII、区分大小写、十进制整数 ---- */

/* 跳过至少一个空格后读一个可带负号的十进制整数（最多 5 位）；格式不对返回 0 */
static uint8_t parse_int(const char **pp, int32_t *p_value)
{
  const char *p = *pp;
  int32_t sign = 1;
  int32_t value = 0;
  uint32_t digits = 0U;

  if (*p != ' ')
  {
    return 0U;
  }
  while (*p == ' ')
  {
    p++;
  }
  if (*p == '-')
  {
    sign = -1;
    p++;
  }
  while ((*p >= '0') && (*p <= '9'))
  {
    if (digits >= 5U)
    {
      return 0U;
    }
    value = (value * 10) + (int32_t)(*p - '0');
    digits++;
    p++;
  }
  if (digits == 0U)
  {
    return 0U;
  }
  *p_value = sign * value;
  *pp = p;
  return 1U;
}

/* 读固定位数的数字，用于 TIME YYYY-MM-DD HH:MM:SS */
static uint8_t parse_fixed(const char *p, uint32_t width, uint32_t *p_value)
{
  uint32_t value = 0U;

  for (uint32_t i = 0U; i < width; i++)
  {
    if ((p[i] < '0') || (p[i] > '9'))
    {
      return 0U;
    }
    value = (value * 10U) + (uint32_t)(p[i] - '0');
  }
  *p_value = value;
  return 1U;
}

static void cmd_status(void)
{
  char raw[8];
  char tim[32];   /* 合法时间 19 字节；按 uint8_t 字段最坏 26 字节留够，免得截断告警 */
  p46_datetime_t dt;
  int len;

  if (g_app.raw_valid != 0U)
  {
    (void)snprintf(raw, sizeof(raw), "%u", (unsigned int)g_app.raw);
  }
  else
  {
    (void)strcpy(raw, "NA");
  }
  if (time_get(&dt) == 0)
  {
    (void)snprintf(tim, sizeof(tim), "%04u-%02u-%02uT%02u:%02u:%02u", (unsigned int)dt.year, (unsigned int)dt.mon,
                   (unsigned int)dt.mday, (unsigned int)dt.hour, (unsigned int)dt.min, (unsigned int)dt.sec);
  }
  else
  {
    (void)strcpy(tim, "UNSET");
  }
  len = snprintf(s_out, sizeof(s_out),
                 "STATUS state=%s raw=%s seq=%" PRIu32 " th=%u/%u saved=%u time=%s per=%" PRIu32 "/%" PRIu32
                 " miss=%" PRIu32 " flt=%" PRIu32 " last=%s@%" PRIu32 " rxdrop=%" PRIu32 " txdrop=%" PRIu32
                 " long=%" PRIu32 " reset=%s ev=%s@%" PRIu32 " tv=%u\r\n",
                 s_state_name[g_app.state], raw, g_app.seq, (unsigned int)g_app.low, (unsigned int)g_app.high,
                 (unsigned int)cfg_is_saved(g_app.low, g_app.high), tim, g_app.per_min, g_app.per_max,
                 g_app.miss, g_app.faults, s_fault_name[g_app.fault], g_app.fault_seq, g_term.rx_dropped,
                 g_term.tx_dropped, g_term.long_lines, health_reset_name(), s_event_name[g_app.ev], g_app.ev_seq,
                 (unsigned int)g_app.ev_time_valid);
  if ((len < 0) || ((uint32_t)len >= sizeof(s_out)))
  {
    app_reply("ERR LEN\r\n");    /* 按最坏情况设计不会走到这里；万一走到也不发半行 */
    return;
  }
  app_reply(s_out);
}

static void cmd_start(uint32_t now)
{
  if (g_app.state == P46_ST_FAULT)
  {
    app_reply("ERR BUSY\r\n");   /* 先 STOP 再 START，不自动重启 */
    return;
  }
  if (g_app.state != P46_ST_STOP)
  {
    app_reply("OK RUNNING\r\n"); /* 重复 START 不清零 */
    return;
  }
  alarm_reset(&s_alarm);
  g_app.seq = 0U;
  g_app.per_min = 0U;
  g_app.per_max = 0U;
  g_app.raw_valid = 0U;
  s_next_due = now;               /* 第一次转换立即发起 */
  s_last_valid_ms = now;          /* 过期计时从 START 开始 */
  app_set_state(P46_ST_RUN, now);
  app_event(P46_EV_START);
  app_reply("OK START\r\n");
}

static void cmd_th(const char *p_args)
{
  int32_t low;
  int32_t high;

  if ((parse_int(&p_args, &low) == 0U) || (parse_int(&p_args, &high) == 0U) || (*p_args != '\0'))
  {
    app_reply("ERR FORMAT\r\n");
    return;
  }
  if ((low < 0) || (high > 4095) || (low >= high))
  {
    app_reply("ERR RANGE\r\n");
    return;
  }
  g_app.low = (uint16_t)low;
  g_app.high = (uint16_t)high;
  (void)snprintf(s_out, sizeof(s_out), "OK TH %u %u\r\n", (unsigned int)g_app.low, (unsigned int)g_app.high);
  app_reply(s_out);
}

static void cmd_save(void)
{
  switch (cfg_save(g_app.low, g_app.high))
  {
    case P46_SAVE_OK:
      (void)snprintf(s_out, sizeof(s_out), "OK SAVE seq=%" PRIu32 " slot=%" PRIu32 "\r\n", g_cfg_scan.last_seq,
                     g_cfg_scan.next_slot - 1U);
      app_reply(s_out);
      break;
    case P46_SAVE_SAME:
      app_reply("OK SAME\r\n");
      break;
    case P46_SAVE_FULL:
      app_reply("ERR FULL\r\n");
      break;
    case P46_SAVE_ERR_FLASH:
      app_reply("ERR FLASH\r\n");
      break;
    default:
      app_reply("ERR VERIFY\r\n");
      break;
  }
}

static void cmd_time(const char *p_args)
{
  p46_datetime_t dt;
  uint32_t y;
  uint32_t mo;
  uint32_t d;
  uint32_t h;
  uint32_t mi;
  uint32_t s;

  /* 恰好 " YYYY-MM-DD HH:MM:SS" 共 20 个字符 */
  if ((strlen(p_args) != 20U) || (p_args[0] != ' ') || (p_args[5] != '-') || (p_args[8] != '-')
      || (p_args[11] != ' ') || (p_args[14] != ':') || (p_args[17] != ':')
      || (parse_fixed(&p_args[1], 4U, &y) == 0U) || (parse_fixed(&p_args[6], 2U, &mo) == 0U)
      || (parse_fixed(&p_args[9], 2U, &d) == 0U) || (parse_fixed(&p_args[12], 2U, &h) == 0U)
      || (parse_fixed(&p_args[15], 2U, &mi) == 0U) || (parse_fixed(&p_args[18], 2U, &s) == 0U))
  {
    app_reply("ERR FORMAT\r\n");
    return;
  }
  dt.year = (uint16_t)y;
  dt.mon = (uint8_t)mo;
  dt.mday = (uint8_t)d;
  dt.hour = (uint8_t)h;
  dt.min = (uint8_t)mi;
  dt.sec = (uint8_t)s;
  if (time_is_legal(&dt) == 0U)
  {
    app_reply("ERR RANGE\r\n");  /* 例如 2026-02-30：不写 RTC */
    return;
  }
  app_reply((time_set(&dt) == 0) ? "OK TIME\r\n" : "ERR RTC\r\n");
}

#if defined(P46_TEST) && (P46_TEST != 0)
/* 测试变体专用命令，正式固件里不存在 */
static void cmd_test(const char *p_args)
{
  if (strcmp(p_args, "MASK") == 0)
  {
    sampler_test_mask_complete(1U);
    app_reply("OK T MASK\r\n");
  }
  else if (strcmp(p_args, "NOREQ") == 0)
  {
    s_test_noreq = 1U;
    app_reply("OK T NOREQ\r\n");
  }
  else if (strcmp(p_args, "ERR") == 0)
  {
    sampler_test_inject_error();
    app_reply("OK T ERR\r\n");
  }
  else if (strncmp(p_args, "SEQ", 3U) == 0)
  {
    const char *p = p_args + 3;
    uint16_t values[4];
    uint32_t n = 0U;
    int32_t v;

    while ((n < 4U) && (*p != '\0'))
    {
      if ((parse_int(&p, &v) == 0U) || (v < 0) || (v > 4095))
      {
        app_reply("ERR FORMAT\r\n");
        return;
      }
      values[n] = (uint16_t)v;
      n++;
    }
    if ((n == 0U) || (*p != '\0') || (sampler_test_queue(values, n) == 0U))
    {
      app_reply("ERR FORMAT\r\n");
      return;
    }
    app_reply("OK T SEQ\r\n");
  }
  else if (strcmp(p_args, "HANG") == 0)
  {
    app_reply("OK T HANG\r\n");   /* 回复靠 TX 中断继续发完 */
    while (1)
    {
      /* 主循环停止推进，不再喂狗，等 IWDG 复位 */
    }
  }
  else
  {
    app_reply("ERR CMD\r\n");
  }
}
#endif /* P46_TEST */

static void app_command(const char *p_line, uint32_t now)
{
  if (strcmp(p_line, "HELP") == 0)
  {
    app_reply("OK HELP STATUS START STOP TH SAVE TIME\r\n");
  }
  else if (strcmp(p_line, "STATUS") == 0)
  {
    cmd_status();
  }
  else if (strcmp(p_line, "START") == 0)
  {
    cmd_start(now);
  }
  else if (strcmp(p_line, "STOP") == 0)
  {
    app_stop(now);
    app_reply("OK STOP\r\n");
  }
  else if ((strncmp(p_line, "TH", 2U) == 0) && ((p_line[2] == ' ') || (p_line[2] == '\0')))
  {
    if (g_app.state != P46_ST_STOP)
    {
      app_reply("ERR BUSY\r\n");
    }
    else
    {
      cmd_th(&p_line[2]);
    }
  }
  else if (strcmp(p_line, "SAVE") == 0)
  {
    if (g_app.state != P46_ST_STOP)
    {
      app_reply("ERR BUSY\r\n");
    }
    else
    {
      cmd_save();
    }
  }
  else if ((strncmp(p_line, "TIME", 4U) == 0) && ((p_line[4] == ' ') || (p_line[4] == '\0')))
  {
    if (g_app.state != P46_ST_STOP)
    {
      app_reply("ERR BUSY\r\n");
    }
    else
    {
      cmd_time(&p_line[4]);
    }
  }
#if defined(P46_TEST) && (P46_TEST != 0)
  else if (strncmp(p_line, "T ", 2U) == 0)
  {
    cmd_test(&p_line[2]);
  }
#endif /* P46_TEST */
  else
  {
    app_reply("ERR CMD\r\n");
  }
}
