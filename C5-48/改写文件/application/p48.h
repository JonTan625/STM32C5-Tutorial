/**
  ******************************************************************************
  * file           : p48.h
  * brief          : C5 #48 项目三：低功耗 RTC 记录仪，各模块接口。
  *                  参数单一来源：#47 项目资料/01_参考方案.json（教学约定，不是芯片保证）。
  ******************************************************************************
  */
#ifndef P48_H
#define P48_H

#include <stdint.h>
#include "example.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* ---- 参数（#47 01_参考方案.json "parameters"） ---- */
#define P48_PERIOD_DEFAULT_S    60U
#if defined(P48_TEST) && (P48_TEST != 0)
#define P48_PERIOD_MIN_S        1U       /* 测试变体：1 s 周期，几十分钟转完一圈环形日志 */
#else
#define P48_PERIOD_MIN_S        10U
#endif /* P48_TEST */
#define P48_PERIOD_MAX_S        60U      /* IWDG 在 Stop 中照样计数（选项字 IWDG_STOP=1），最早 114 s 复位 */
#define P48_IWDG_MS             120000U
#define P48_CONSOLE_IDLE_MS     60000U
#define P48_LOG_ADDR            0x08020000UL   /* bank 2 起点；程序只在 bank 1（链接器 ROM 0x20000） */
#ifndef P48_LOG_PAGES
#define P48_LOG_PAGES           16U      /* 测试变体用 -DP48_LOG_PAGES=2 */
#endif /* P48_LOG_PAGES */
#define P48_PAGE_BYTES          8192U
#define P48_SLOTS_PER_PAGE      (P48_PAGE_BYTES / 16U)
#define P48_LOG_SLOTS           (P48_LOG_PAGES * P48_SLOTS_PER_PAGE)
#define P48_REC_MAGIC           0xC547U
#define P48_TIME_INVALID        0xFFFFFFFFUL
#define P48_FLAG_TIME_VALID     (1UL << 24)
#define P48_FLAG_CONSOLE        (1UL << 25)
#define P48_FLAG_ADC_ERR        (1UL << 26)
#define P48_LINE_MAX            32U
#define P48_RX_RING             128U
#define P48_TX_RING             512U
#define P48_OUT_MAX             256U

/* ---- 记录：16 字节，一次 quad-word 编程 ---- */
typedef struct
{
  uint32_t head;   /* bit31:16 = 0xC547，bit15:0 = CRC-16/CCITT-FALSE(后 12 字节) */
  uint32_t seq;    /* 单调加 1，跨页连续 */
  uint32_t time;   /* 2000-01-01 起的秒数；未校时为 0xFFFFFFFF */
  uint32_t data;   /* bit11:0 温度原始码，bit23:12 VREFINT 原始码，bit26:24 标志 */
} p48_rec_t;

/* ---- sense：内部温度 + VREFINT，每次上电、校准、采样、深度掉电 ---- */
int32_t sense_init(void);
int32_t sense_sample(uint16_t *p_temp_raw, uint16_t *p_vref_raw);   /* 0 成功 */
int32_t sense_temp_x10(uint16_t temp_raw, uint16_t vref_raw);         /* 0.1 °C */
uint32_t sense_vdda_mv(uint16_t vref_raw);

/* ---- log：bank 2 环形日志 ---- */
typedef struct
{
  uint32_t valid;        /* 有效记录数 */
  uint32_t bad;          /* 非空但 magic/CRC 不对 */
  uint32_t ecc;          /* 读时触发双 ECC 错误 */
  uint32_t last_seq;     /* 最新有效序号；无记录为 0 */
  uint32_t next_slot;    /* 下一次写入的槽号 */
  uint32_t erases;       /* 本次上电以来擦页次数 */
  uint32_t write_errors; /* 编程或读回失败次数 */
} p48_log_stat_t;

extern p48_log_stat_t g_log;

uint16_t log_crc16(const uint8_t *p_data, uint32_t len);
int32_t  log_init(void);
int32_t  log_append(uint32_t time, uint32_t data);   /* 0 成功 */
int32_t  log_clear(void);
void     log_iter_begin(uint32_t skip);              /* 从最旧开始，先跳过 skip 条有效记录 */
uint8_t  log_iter_next(p48_rec_t *p_rec);            /* 1 = 取到一条 */
#if defined(P48_TEST) && (P48_TEST != 0)
int32_t  log_test_corrupt(void);                     /* 测试钩子：写一条 CRC 错的记录 */
#endif /* P48_TEST */

/* ---- time：RTC + LSE，唤醒定时器，备份寄存器 ---- */
typedef struct
{
  uint16_t year;
  uint8_t  mon;
  uint8_t  mday;
  uint8_t  hour;
  uint8_t  min;
  uint8_t  sec;
} p48_datetime_t;

extern volatile uint8_t g_rtc_wake;

int32_t  time_init(void);
uint8_t  time_valid(void);
int32_t  time_get(p48_datetime_t *p_dt);
uint8_t  time_is_legal(const p48_datetime_t *p_dt);
int32_t  time_set(const p48_datetime_t *p_dt);
uint32_t time_to_secs(const p48_datetime_t *p_dt);
void     time_from_secs(uint32_t secs, p48_datetime_t *p_dt);
int32_t  time_wakeup_start(uint32_t period_s);
uint32_t time_period_load(void);
void     time_period_store(uint32_t period_s);

/* ---- terminal：USART2，只在命令模式工作 ---- */
typedef struct
{
  uint32_t rx_bytes;
  uint32_t rx_dropped;
  uint32_t rx_errors;
  uint32_t tx_dropped;
  uint32_t long_lines;
} p48_term_stats_t;

typedef enum
{
  P48_LINE_NONE = 0,
  P48_LINE_READY,
  P48_LINE_TOO_LONG
} p48_line_t;

extern p48_term_stats_t g_term;

int32_t    term_init(void);
p48_line_t term_poll_line(char *p_line);
void       term_write(const char *p_text);
uint32_t   term_tx_free(void);
void       term_flush(void);          /* 等发送环清空且最后一位发完 */
void       term_rx_disable(void);     /* 睡前：关接收，PA3 交给 EXTI */
void       term_rx_enable(void);      /* 醒后：PA3 回 AF7，清错误，开接收 */

/* ---- power：Stop1 进出与串口唤醒 ---- */
extern volatile uint8_t g_console_wake;
extern uint32_t g_wakes;
extern uint32_t g_awake_us_last;
extern uint32_t g_awake_us_max;

void    power_init(void);
void    power_sleep(void);            /* 有待处理标志时直接返回；醒来后 SystemCoreClock = 48 MHz */
int32_t power_console_clock(void);    /* 恢复 HSE → PSIS 144 MHz */
void    power_mark_awake_end(void);   /* 记下本次醒着的时长（只统计记录唤醒） */
void    power_cancel_awake(void);     /* 转入命令模式时调用 */

/* ---- health：复位原因、IWDG、栈水位 ---- */
void        health_capture_reset(void);
int32_t     health_start(void);
void        health_feed(void);
const char *health_reset_name(void);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* P48_H */
