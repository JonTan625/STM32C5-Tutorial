/**
  * ADC timer-triggered acquisition for NUCLEO-C542RC, C5 article #33.
  * Based on STMicroelectronics 2026 ll/adc/multi_channels_dma example.
  * Original component license applies; see SDK LICENSE.md.
  */
#include "example.h"
#include "ll_example.h"
#include "stm32c5xx_ll_tim.h"
#include "stm32c5xx_ll_dbgmcu.h"

#define TIMER_PSC         143U
#define ADC_TIMEOUT_MS    100U
#define ADC_STOP_MS       2U
#define PRETRIGGER_US     200U

/* 本篇只支持当前生成层的144MHz时钟；改变时钟树必须重新推导。 */
static const uint32_t s_rates[3] = {500U, 1000U, 2000U};
typedef struct
{
  uint32_t hz;
  uint32_t psc;
  uint32_t arr;
  uint32_t pretrigger_written;
  uint32_t run_cycles;
  uint32_t run_us;
  uint32_t dma_remain;
  uint32_t ovr;
  uint32_t unwritten;
  uint32_t valid; /* 最后写1；失败或采集中为0 */
} sample_result_t;
volatile sample_result_t g_results[3];
uint32_t g_rounds;
volatile uint32_t g_error; /* 1超时，2OVR，3DMA错误，4停ADC失败，5未开表已采样，6数据不完整 */
uint32_t g_cfgr1;
uint32_t g_tim_cr2;
uint32_t g_dma_csr;
uint32_t g_adstart_after;
uint32_t g_unwritten;
uint16_t g_raw_avg[ADC_RANKS];
uint16_t g_raw_min[ADC_RANKS];
uint16_t g_raw_max[ADC_RANKS];
uint32_t g_vrefplus_mv;
uint32_t g_pa4_mv;
uint32_t g_pa4_mv_assumed;
int32_t g_temp_degc;
int32_t g_temp_degc_assumed;
volatile uint8_t ADCConversionComplete;
static volatile uint32_t s_tc_stamp;

static uint32_t CountWritten(void)
{
  uint32_t n = 0U;
  for (uint32_t i = 0U; i < ADC_BUFFER_SIZE; i++)
  {
    if (ADCRawData[i] != ADC_SENTINEL) { n++; }
  }
  return n;
}

static uint32_t StopADC(void)
{
  uint32_t start = DWT->CYCCNT;
  if (LL_ADC_REG_IsConversionOngoing(MX_ADCx) != 0U)
  {
    LL_ADC_REG_StopConversion(MX_ADCx);
  }
  while (LL_ADC_REG_IsStopConversionOngoing(MX_ADCx) != 0U)
  {
    if ((DWT->CYCCNT - start) > (SystemCoreClock / 1000U) * ADC_STOP_MS)
    {
      return 0U;
    }
  }
  return (LL_ADC_REG_IsConversionOngoing(MX_ADCx) == 0U) ? 1U : 0U;
}

app_status_t app_init(void)
{
  if (SystemCoreClock != 144000000U || mx_adcx_init() == NULL)
  {
    return EXEC_STATUS_ERROR;
  }
  /* ADC使能前修改公共时钟；沿用 #32 的内部通道采样时间修正。 */
  LL_RCC_SetADCDACPrescaler(LL_RCC_ADCDAC_PRESCALER_8);
  LL_ADC_REG_SetOverrun(MX_ADCx, LL_ADC_REG_OVR_DATA_PRESERVED);
  LL_ADC_REG_SetContinuousMode(MX_ADCx, LL_ADC_REG_CONV_SINGLE);
  LL_ADC_REG_SetTriggerSource(MX_ADCx, LL_ADC_REG_TRIG_TIM6_TRGO);
  LL_ADC_REG_SetTriggerEdge(MX_ADCx, LL_ADC_REG_TRIG_RISING);
  DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0U;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM6);
  LL_APB1_GRP1_ForceReset(LL_APB1_GRP1_PERIPH_TIM6);
  LL_APB1_GRP1_ReleaseReset(LL_APB1_GRP1_PERIPH_TIM6);
  LL_TIM_EnableARRPreload(TIM6);
  LL_DBGMCU_APB1_GRP1_FreezePeriph(LL_DBGMCU_TIM6_STOP);
  /* TIM6不使能UIE/UDE/NVIC；TRGO直接送ADC，不经过定时器ISR。 */
  /* 校准函数先退出深度掉电、启动稳压器，最后才激活ADC，禁止提前写ADEN。 */
  if (ADC_Calibrate() != SYSTEM_OK)
  {
    return EXEC_STATUS_ERROR;
  }
  LL_Delay_NoISR(1U); /* 内部温度/VrefInt通路稳定等待；DS15135温度启动上限25.2us。 */
  return EXEC_STATUS_INIT_OK;
}

app_status_t app_process(void)
{
  uint32_t index = g_rounds % 3U;
  volatile sample_result_t *r = &g_results[index];
  uint32_t t0;
  uint32_t sum[ADC_RANKS] = {0U};
  uint32_t scan;
  uint32_t rank;
  uint32_t timeout = (SystemCoreClock / 1000U) * ADC_TIMEOUT_MS;

  r->valid = 0U;
  r->run_cycles = 0U;
  r->run_us = 0U;
  r->hz = s_rates[index];
  r->psc = TIMER_PSC;
  r->arr = 1000000U / r->hz - 1U;
  g_error = 0U;
  LL_TIM_DisableCounter(TIM6);
  if (StopADC() == 0U) { g_error = 4U; return EXEC_STATUS_ERROR; }

  /* UG也可形成更新：必须在ADC未armed时装载PSC/ARR。MMS=RESET并非无触发。 */
  LL_TIM_SetPrescaler(TIM6, r->psc);
  LL_TIM_SetAutoReload(TIM6, r->arr);
  LL_TIM_GenerateEvent_UPDATE(TIM6);
  LL_TIM_ClearFlag_UPDATE(TIM6);
  LL_TIM_SetCounter(TIM6, 0U);
  LL_TIM_SetTriggerOutput(TIM6, LL_TIM_TRGO_UPDATE);
  LL_ADC_ClearFlag_OVR(MX_ADCx);
  ADCConversionComplete = 0U;
  ADC_StartConversion_DMA();

  /* 验证ADSTART只是待触发。此时不应有任何数据；该200us不计入采集时间。 */
  t0 = DWT->CYCCNT;
  while ((DWT->CYCCNT - t0) < (SystemCoreClock / 1000000U) * PRETRIGGER_US) {}
  r->pretrigger_written = CountWritten();
  if (r->pretrigger_written != 0U) { g_error = 5U; goto finish; }

  g_cfgr1 = MX_ADCx->CFGR1;
  g_tim_cr2 = TIM6->CR2;
  t0 = DWT->CYCCNT;
  LL_TIM_EnableCounter(TIM6); /* CEN最后打开，首轮在一个完整周期后开始 */
  while (ADCConversionComplete == 0U)
  {
    if (g_error != 0U) { break; }
    if (LL_ADC_IsActiveFlag_OVR(MX_ADCx) != 0U) { g_error = 2U; break; }
    if ((LL_DMA_READ_REG(MX_LPDMAX_CHY_CH, CSR) & (LL_DMA_FLAG_DTE | LL_DMA_FLAG_USE)) != 0U)
    { g_error = 3U; break; }
    if ((DWT->CYCCNT - t0) >= timeout) { g_error = 1U; break; }
  }
  r->run_cycles = (ADCConversionComplete != 0U) ? s_tc_stamp - t0 : DWT->CYCCNT - t0;
  r->run_us = r->run_cycles / (SystemCoreClock / 1000000U);

finish:
  LL_TIM_DisableCounter(TIM6);
  r->dma_remain = LL_DMA_GetBlkDataLength(MX_LPDMAX_CHY_CH);
  r->ovr = LL_ADC_IsActiveFlag_OVR(MX_ADCx);
  g_dma_csr = LL_DMA_READ_REG(MX_LPDMAX_CHY_CH, CSR);
  g_adstart_after = LL_ADC_REG_IsConversionOngoing(MX_ADCx);
  if (StopADC() == 0U) { g_error = 4U; }
  if ((g_dma_csr & (LL_DMA_FLAG_DTE | LL_DMA_FLAG_USE)) != 0U) { g_error = 3U; }
  /* 失败不重启、不读取半块数据；主程序进入错误灯。 */
  if (g_error != 0U)
  {
    LL_DMA_DisableChannel(MX_LPDMAX_CHY_CH);
    return EXEC_STATUS_ERROR;
  }
  __DMB();
  if (r->ovr != 0U || r->dma_remain != 0U || CountWritten() != ADC_BUFFER_SIZE)
  { g_error = 6U; return EXEC_STATUS_ERROR; }

  g_unwritten = ADC_BUFFER_SIZE - CountWritten();
  for (rank = 0U; rank < ADC_RANKS; rank++)
  {
    g_raw_min[rank] = ADC_SENTINEL;
    g_raw_max[rank] = 0U;
  }
  for (scan = 0U; scan < ADC_SCANS; scan++)
  {
    for (rank = 0U; rank < ADC_RANKS; rank++)
    {
      uint16_t v = ADCRawData[(scan * ADC_RANKS) + rank];
      sum[rank] += v;
      if (v < g_raw_min[rank])
      {
        g_raw_min[rank] = v;
      }
      if (v > g_raw_max[rank])
      {
        g_raw_max[rank] = v;
      }
    }
  }
  for (rank = 0U; rank < ADC_RANKS; rank++)
  {
    g_raw_avg[rank] = (uint16_t)((sum[rank] + (ADC_SCANS / 2U)) / ADC_SCANS);
  }

  if (g_raw_avg[1] == 0U) { g_error = 6U; return EXEC_STATUS_ERROR; }

  g_vrefplus_mv       = LL_ADC_CALC_VREFANALOG_VOLTAGE(g_raw_avg[1], LL_ADC_RESOLUTION_12B);
  g_pa4_mv            = LL_ADC_CALC_DATA_TO_VOLTAGE(g_vrefplus_mv, g_raw_avg[0], LL_ADC_RESOLUTION_12B);
  g_pa4_mv_assumed    = LL_ADC_CALC_DATA_TO_VOLTAGE(VDD_VALUE, g_raw_avg[0], LL_ADC_RESOLUTION_12B);
  g_temp_degc         = LL_ADC_CALC_TEMPERATURE(g_vrefplus_mv, g_raw_avg[2], LL_ADC_RESOLUTION_12B);
  g_temp_degc_assumed = LL_ADC_CALC_TEMPERATURE(VDD_VALUE, g_raw_avg[2], LL_ADC_RESOLUTION_12B);

  r->unwritten = g_unwritten;
  __DMB();
  r->valid = 1U;
  g_rounds++;
  return EXEC_STATUS_OK;
}

void ADC_ConversionCompleteCallback(void)
{
  s_tc_stamp = DWT->CYCCNT;
  LL_TIM_DisableCounter(TIM6);
  __DMB();
  ADCConversionComplete = 1U;
}

#if defined(USE_LL_APP_ERROR) && (USE_LL_APP_ERROR == 1)
void ADC_ConversionErrorCallback(void)
{
  LL_TIM_DisableCounter(TIM6);
  g_error = 3U;
}
#endif

app_status_t app_deinit(void)
{
  LL_TIM_DisableCounter(TIM6);
  if (StopADC() == 0U) { return EXEC_STATUS_ERROR; }
  mx_adcx_deinit();
  return EXEC_STATUS_OK;
}
