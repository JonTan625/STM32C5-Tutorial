/**
  ******************************************************************************
  * file           : p46_terminal.c
  * brief          : C5 #46 终端模块：USART2（ST-LINK VCP，PA2 TX / PA3 RX，AF7）115200 8N1。
  *                  中断只搬字节（RXNE 收、TXE 发），主循环组行、解析、排队回复。
  *                  环形缓冲写法沿用 #36；打开接收方向的两处补丁沿用 #18（2026-10-14 上板发现）。
  ******************************************************************************
  */
#include "p46.h"
#include "mx_usart2.h"

p46_term_stats_t g_term;

static uint8_t s_rx_ring[P46_RX_RING];
static volatile uint32_t s_rx_head;   /* 只由中断写 */
static volatile uint32_t s_rx_tail;   /* 只由主循环写 */
static uint8_t s_tx_ring[P46_TX_RING];
static volatile uint32_t s_tx_head;   /* 只由主循环写 */
static volatile uint32_t s_tx_tail;   /* 只由中断写 */

/* 组行状态（只在主循环里用） */
static char     s_line[P46_LINE_MAX + 1U];
static uint32_t s_line_len;
static uint8_t  s_discarding;   /* 本行已超长，丢到行尾 */
static uint8_t  s_last_cr;      /* 上一个字节是 CR：紧跟的 LF 不再算一次结束 */

void USART2_IRQHandler(void);

int32_t term_init(void)
{
  hal_uart_handle_t *p_uart = mx_usart2_uart_gethandle();
  hal_gpio_config_t rx_pin;

  /* 生成代码只开了发送：PA3 没配、CR1.RE=0（同 #18）。 */
  rx_pin.mode        = HAL_GPIO_MODE_ALTERNATE;
  rx_pin.pull        = HAL_GPIO_PULL_NO;
  rx_pin.speed       = HAL_GPIO_SPEED_FREQ_LOW;
  rx_pin.output_type = HAL_GPIO_OUTPUT_PUSHPULL;
  rx_pin.alternate   = HAL_GPIO_AF_7;   /* PA3 = USART2_RX */
  rx_pin.init_state  = HAL_GPIO_PIN_RESET;
  if (HAL_GPIO_Init(HAL_GPIOA, HAL_GPIO_PIN_3, &rx_pin) != HAL_OK)
  {
    return -1;
  }
  if (HAL_UART_SetXferDirection(p_uart, HAL_UART_DIRECTION_TX_RX) != HAL_OK)
  {
    return -1;
  }
  /* HAL2 只在首次调用收发函数时才置 UE（UART_CheckEnabledState），本模块不用 HAL 收发，要自己使能。
     2026-10-22 在 #48 上板时发现漏了这一步（两篇共用本段）：CR1.UE=0，串口收发全无。 */
  LL_USART_Enable(USART2);

  /* 之后不再调用 HAL UART 收发函数，中断入口由本文件直接处理寄存器（同 #36）。
     生成代码已关闭溢出检测（OVRDIS=1），ORE 分支只是保底。 */
  LL_USART_EnableIT_RXNE_RXFNE(USART2);
  HAL_CORTEX_NVIC_SetPriority(USART2_IRQn, HAL_CORTEX_NVIC_PREEMP_PRIORITY_1, HAL_CORTEX_NVIC_SUB_PRIORITY_0);
  HAL_CORTEX_NVIC_EnableIRQ(USART2_IRQn);
  return 0;
}

static uint8_t term_get_byte(uint8_t *p_byte)
{
  uint32_t tail = s_rx_tail;

  if (tail == s_rx_head)
  {
    return 0U;
  }
  *p_byte = s_rx_ring[tail];
  s_rx_tail = (tail + 1U) & (P46_RX_RING - 1U);
  return 1U;
}

p46_line_t term_poll_line(char *p_line)
{
  uint8_t c;

  while (term_get_byte(&c) != 0U)
  {
    if ((c == (uint8_t)'\r') || (c == (uint8_t)'\n'))
    {
      uint8_t lf_after_cr = ((c == (uint8_t)'\n') && (s_last_cr != 0U)) ? 1U : 0U;

      s_last_cr = (c == (uint8_t)'\r') ? 1U : 0U;
      if (lf_after_cr != 0U)
      {
        continue;          /* CRLF 只算一次结束 */
      }
      if (s_discarding != 0U)
      {
        s_discarding = 0U;
        s_line_len = 0U;
        g_term.long_lines++;
        return P46_LINE_TOO_LONG;
      }
      if (s_line_len == 0U)
      {
        continue;          /* 空行忽略、不回复 */
      }
      s_line[s_line_len] = '\0';
      for (uint32_t i = 0U; i <= s_line_len; i++)
      {
        p_line[i] = s_line[i];
      }
      s_line_len = 0U;
      return P46_LINE_READY;
    }

    s_last_cr = 0U;
    if (s_discarding != 0U)
    {
      continue;
    }
    if (s_line_len >= P46_LINE_MAX)
    {
      s_discarding = 1U;   /* 第 33 个字节：本行作废，等行尾只回一次 ERR LONG */
      continue;
    }
    s_line[s_line_len] = (char)c;
    s_line_len++;
  }
  return P46_LINE_NONE;
}

void term_write(const char *p_text)
{
  uint32_t len = 0U;
  uint32_t head = s_tx_head;
  uint32_t used = (head - s_tx_tail) & (P46_TX_RING - 1U);

  while (p_text[len] != '\0')
  {
    len++;
  }
  if (len > (P46_TX_RING - 1U - used))
  {
    g_term.tx_dropped++;   /* 整行放不下：整行丢弃，不截半行 */
    return;
  }
  for (uint32_t i = 0U; i < len; i++)
  {
    s_tx_ring[head] = (uint8_t)p_text[i];
    head = (head + 1U) & (P46_TX_RING - 1U);
  }
  s_tx_head = head;
  LL_USART_EnableIT_TXE_TXFNF(USART2);
}

/**
  * RXNEIE 置位时 ORE 也会进中断，必须写 ORECF 清掉（RM0522 §43.8.2 USART_CR1 p1856、ORECF p1884，#36 已核）。
  */
void USART2_IRQHandler(void)
{
  uint8_t c;
  uint32_t next;

  if (LL_USART_IsActiveFlag_ORE(USART2) != 0U)
  {
    LL_USART_ClearFlag_ORE(USART2);
    g_term.rx_errors++;
  }
  if ((LL_USART_IsActiveFlag_FE(USART2) != 0U) || (LL_USART_IsActiveFlag_NE(USART2) != 0U))
  {
    LL_USART_ClearFlag_FE(USART2);
    LL_USART_ClearFlag_NE(USART2);
    g_term.rx_errors++;
  }
  if (LL_USART_IsActiveFlag_RXNE_RXFNE(USART2) != 0U)
  {
    c = LL_USART_ReceiveData8(USART2);   /* 读 RDR 清 RXNE */
    g_term.rx_bytes++;
    next = (s_rx_head + 1U) & (P46_RX_RING - 1U);
    if (next != s_rx_tail)
    {
      s_rx_ring[s_rx_head] = c;
      s_rx_head = next;
    }
    else
    {
      g_term.rx_dropped++;
    }
  }
  if ((LL_USART_IsEnabledIT_TXE_TXFNF(USART2) != 0U) && (LL_USART_IsActiveFlag_TXE_TXFNF(USART2) != 0U))
  {
    if (s_tx_tail != s_tx_head)
    {
      LL_USART_TransmitData8(USART2, s_tx_ring[s_tx_tail]);
      s_tx_tail = (s_tx_tail + 1U) & (P46_TX_RING - 1U);
    }
    else
    {
      LL_USART_DisableIT_TXE_TXFNF(USART2);
    }
  }
}
