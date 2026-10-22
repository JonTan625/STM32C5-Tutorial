/**
  ******************************************************************************
  * file           : example.c
  * brief          : How to calculate sines and cosines array in polling mode using
                     the CORDIC HAL APIs
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
#include <stdlib.h> /* Required for function abs() */
#include <math.h>   /* C5 #27：sinf/cosf 对比与双精度真值 */
#include <stdint.h> /* C5 #27：INT32_MAX */
#include <arm_math.h> /* Required for conversion function arm_float_to_q31() and PI definition */
#include "example.h"

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/
/* Size of data array */
#define ARRAY_SIZE        64U
#define TIMEOUT 1000U

/** Reference values in Q1.31 format
  * Max residual error for sines and cosines, with 6 cycles precision:
  * 2^-19 max residual error, ie 31-19=12 LSB, ie 0x1000
  */
#define MAX_RESIDUAL_ERROR (int32_t)0x1000

/* Private variables ---------------------------------------------------------*/
hal_cordic_handle_t *pCORDIC;  /* pointer referencing the CORDIC handle from the generated code */

/* Input angles in radians in range [-pi, pi[ regularly incremented */
static const float32_t Angles[ARRAY_SIZE] =
{
  -3.141592653589793,      -3.043417883165112,     -2.945243112740431,     -2.84706834231575,
  -2.748893571891069,      -2.650718801466388,     -2.552544031041707,     -2.454369260617026,
  -2.356194490192345,      -2.2580197197676637,    -2.1598449493429825,    -2.061670178918302,
  -1.9634954084936207,     -1.8653206380689396,    -1.7671458676442586,    -1.6689710972195777,
  -1.5707963267948966,     -1.4726215563702154,    -1.3744467859455345,    -1.2762720155208536,
  -1.1780972450961724,     -1.0799224746714913,    -0.9817477042468106,    -0.8835729338221294,
  -0.7853981633974483,     -0.6872233929727671,    -0.589048622548086,     -0.4908738521234053,
  -0.39269908169872414,    -0.294524311274043,     -0.1963495408493623,    -0.09817477042468115,
  0.0,                     0.09817477042468115,    0.1963495408493623,     0.294524311274043,
  0.39269908169872414,     0.4908738521234053,     0.589048622548086,      0.6872233929727671,
  0.7853981633974483,      0.883572933822129,      0.9817477042468106,     1.0799224746714913,
  1.178097245096172,       1.2762720155208536,     1.3744467859455343,     1.4726215563702159,
  1.5707963267948966,      1.6689710972195773,     1.7671458676442588,     1.8653206380689396,
  1.9634954084936211,      2.061670178918302,      2.1598449493429825,     2.258019719767664,
  2.356194490192345,       2.4543692606170255,     2.552544031041707,      2.650718801466388,
  2.7488935718910685,      2.84706834231575,       2.945243112740431,      3.0434178831651124
};

/* Input angles in radians divided by pi */
static float32_t AnglesDivPi[ARRAY_SIZE];

/* Q1.31 format representation of the angles divided by pi, buffer used by CPU */
/* C5 #27：两个数组都多留保护空间。HAL 2.1.0 的 HAL_CORDIC_CalculateZeroOverhead() 循环前没有像
 * HAL_CORDIC_Calculate() 那样先减 1（stm32c5xx_hal_cordic.c 第 1103 行 vs 第 1211～1237 行），
 * 会多写 1 个参数（读 Q1_31[64]）、多读 1 组结果（写 CalculatedSinCos[128..129]），2026-10-14 上板发现 */
static q31_t Q1_31[ARRAY_SIZE + 1U];

/* Output array of the CORDIC calculated sines (even indexes) and cosines (odd indexes) in Q1.31 format, used by CPU */

static int32_t CalculatedSinCos[(2U * ARRAY_SIZE) + 2U];

/** HAL CORDIC data buffer descriptors
  * Size of destination buffer is doubled to gather sines and cosines
  */
hal_cordic_buffer_desc_t SrcBuffer =
{
  Q1_31,
  ARRAY_SIZE
};

hal_cordic_buffer_desc_t DstBuffer =
{
  CalculatedSinCos,
  2 * ARRAY_SIZE
};

/* Array of reference sines in Q1.31 format. Used for check purpose only. */
static const int32_t RefSin[ARRAY_SIZE] =
{
  0x00000000,    0xF3742CA2,    0xE70747C4,    0xDAD7F3A2,
  0xCF043AB3,    0xC3A94590,    0xB8E31319,    0xAECC336C,
  0xA57D8666,    0x9D0DFE54,    0x9592675C,    0x8F1D343A,
  0x89BE50C3,    0x8582FAA5,    0x8275A0C0,    0x809DC971,
  0x80000000,    0x809DC971,    0x8275A0C0,    0x8582FAA5,
  0x89BE50C3,    0x8F1D343A,    0x9592675C,    0x9D0DFE54,
  0xA57D8666,    0xAECC336C,    0xB8E31319,    0xC3A94590,
  0xCF043AB3,    0xDAD7F3A2,    0xE70747C4,    0xF3742CA2,
  0x00000000,    0x0C8BD35E,    0x18F8B83C,    0x25280C5E,
  0x30FBC54D,    0x3C56BA70,    0x471CECE7,    0x5133CC94,
  0x5A82799A,    0x62F201AC,    0x6A6D98A4,    0x70E2CBC6,
  0x7641AF3D,    0x7A7D055B,    0x7D8A5F40,    0x7F62368F,
  0x80000000,    0x7F62368F,    0x7D8A5F40,    0x7A7D055B,
  0x7641AF3D,    0x70E2CBC6,    0x6A6D98A4,    0x62F201AC,
  0x5A82799A,    0x5133CC94,    0x471CECE7,    0x3C56BA70,
  0x30FBC54D,    0x25280C5E,    0x18F8B83C,    0x0C8BD35E
};

/* Array of reference cosines in Q1.31 format. Used for check purpose only. */
static const int32_t RefCos[ARRAY_SIZE] =
{
  0x80000000,    0x809DC971,    0x8275A0C0,    0x8582FAA5,
  0x89BE50C3,    0x8F1D343A,    0x9592675C,    0x9D0DFE54,
  0xA57D8666,    0xAECC336C,    0xB8E31319,    0xC3A94590,
  0xCF043AB3,    0xDAD7F3A2,    0xE70747C4,    0xF3742CA2,
  0x00000000,    0x0C8BD35E,    0x18F8B83C,    0x25280C5E,
  0x30FBC54D,    0x3C56BA70,    0x471CECE7,    0x5133CC94,
  0x5A82799A,    0x62F201AC,    0x6A6D98A4,    0x70E2CBC6,
  0x7641AF3D,    0x7A7D055B,    0x7D8A5F40,    0x7F62368F,
  0x80000000,    0x7F62368F,    0x7D8A5F40,    0x7A7D055B,
  0x7641AF3D,    0x70E2CBC6,    0x6A6D98A4,    0x62F201AC,
  0x5A82799A,    0x5133CC94,    0x471CECE7,    0x3C56BA70,
  0x30FBC54D,    0x25280C5E,    0x18F8B83C,    0x0C8BD35E,
  0x00000000,    0xF3742CA2,    0xE70747C4,    0xDAD7F3A2,
  0xCF043AB3,    0xC3A94590,    0xB8E31319,    0xAECC336C,
  0xA57D8666,    0x9D0DFE54,    0x9592675C,    0x8F1D343A,
  0x89BE50C3,    0x8582FAA5,    0x8275A0C0,    0x809DC971
};

/* C5 #27 改写：在官方流程上加四件事，结果全部放在下面的全局变量里，用调试器 Watch 查看（本工程没有串口）。
 * 1. 用 64 位差值重新核对官方参考表 RefSin/RefCos，找出与 CORDIC 结果相差超过阈值的项；
 * 2. 在 CPU 上用双精度 sin()/cos() 算真值，按 PRECISION = 1..6 各算一遍，记最大误差，对照 RM 表 123；
 * 3. 用 DWT 比较四种做法算 64 组 sin/cos 的耗时：HAL 轮询、HAL 零开销、直接读写寄存器、软件 sinf/cosf；
 * 4. 记下 sinf/cosf 的最大误差，和 CORDIC 放在一起看。
 */
#define ERR_LIMIT_6_CYCLE  6144U   /* 2^-19 + 2^-20（RM 表 123 及注 1），单位 Q1.31 LSB */
#define PI_DOUBLE          3.14159265358979323846
#define Q31_SCALE          2147483648.0

volatile uint32_t g_ref_bad_count;      /* 官方参考表中超过阈值的项数 */
volatile uint32_t g_ref_bad_index[4];   /* 超过阈值的项在 CalculatedSinCos[] 中的下标：偶数为正弦，奇数为余弦 */
volatile uint32_t g_max_err_lsb[6];     /* PRECISION = 1..6 时 128 个结果相对双精度真值的最大误差，Q1.31 LSB */
volatile uint32_t g_cycles_hal_poll;    /* 64 组 sin/cos 的 HCLK 周期：HAL_CORDIC_Calculate（轮询 RRDY） */
volatile uint32_t g_cycles_hal_zo;      /* HAL_CORDIC_CalculateZeroOverhead */
volatile uint32_t g_cycles_reg;         /* 直接写 CORDIC_WDATA、读两次 CORDIC_RDATA */
volatile uint32_t g_cycles_sw;          /* 软件 sinf() + cosf() */
volatile uint32_t g_sw_max_err_lsb;     /* sinf/cosf 相对双精度真值的最大误差，换算成 Q1.31 LSB */
volatile uint32_t g_csr;                /* 计算后的 CORDIC_CSR */
static int32_t RefQ31[2 * ARRAY_SIZE];  /* 双精度真值：按 CORDIC 实际收到的 Q1.31 角度，乘 ARG2 的复位模长 */
static float32_t SwSinCos[2 * ARRAY_SIZE];

/* Private functions prototype -----------------------------------------------*/
static int32_t to_q31_sat(double x);
static uint32_t abs_diff(int32_t a, int32_t b);
static uint32_t max_error_lsb(void);


/** ########## Step 1 ##########
  * The applicative code initializes the CORDIC instance.
  */
app_status_t app_init(void)
{
  app_status_t return_status = EXEC_STATUS_ERROR;

  pCORDIC = mx_example_cordic_init();
  if (pCORDIC != NULL)
  {
    DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;  /* DWT 周期计数器，用来比耗时（#14） */
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    return_status = EXEC_STATUS_INIT_OK;
  }

  return return_status;
} /* end app_init */


app_status_t app_process(void)
{
  app_status_t return_status = EXEC_STATUS_ERROR;
  const double modulus = 2147483647.0 / Q31_SCALE;  /* ARG2 复位值 0x7FFFFFFF（RM0522 第 490 页） */
  uint32_t t0;
  uint32_t i;
  uint32_t p;

  /** ########## Step 2 ##########
    * Prepares the input values for CORDIC: angles must be in radians divided by pi and presented in Q1.31 format.
    */
  for (i = 0; i < ARRAY_SIZE; i++)
  {
    AnglesDivPi[i] = Angles[i] / PI;
  }
  arm_float_to_q31(AnglesDivPi, Q1_31, ARRAY_SIZE);

  /* 双精度真值：角度取 CORDIC 实际收到的 Q1.31 值，避免把 float32 量化误差算到 CORDIC 头上 */
  for (i = 0; i < ARRAY_SIZE; i++)
  {
    double theta = (double)Q1_31[i] * (PI_DOUBLE / Q31_SCALE);
    RefQ31[2U * i]        = to_q31_sat(modulus * sin(theta));
    RefQ31[(2U * i) + 1U] = to_q31_sat(modulus * cos(theta));
  }

  /** ########## Step 3 ##########
    * ① 官方配置（6 个周期）、HAL 轮询方式，计时。
    */
  t0 = DWT->CYCCNT;
  if (HAL_CORDIC_Calculate(pCORDIC, &SrcBuffer, &DstBuffer, TIMEOUT) != HAL_OK)
  {
    goto _app_process_exit;
  }
  g_cycles_hal_poll = DWT->CYCCNT - t0;
  g_csr = CORDIC->CSR;

  /* 用 64 位差值核对官方参考表：官方的 abs(int32 - int32) 在 +1 与 -1 之间会有符号溢出 */
  for (i = 0; i < ARRAY_SIZE; i++)
  {
    if (abs_diff(CalculatedSinCos[2U * i], RefSin[i]) > (uint32_t)MAX_RESIDUAL_ERROR)
    {
      if (g_ref_bad_count < 4U)
      {
        g_ref_bad_index[g_ref_bad_count] = 2U * i;
      }
      g_ref_bad_count++;
    }
    if (abs_diff(CalculatedSinCos[(2U * i) + 1U], RefCos[i]) > (uint32_t)MAX_RESIDUAL_ERROR)
    {
      if (g_ref_bad_count < 4U)
      {
        g_ref_bad_index[g_ref_bad_count] = (2U * i) + 1U;
      }
      g_ref_bad_count++;
    }
  }

  /* ② HAL 零开销方式：不查 RRDY，直接读，靠总线等待（RM0522 §18.3.6） */
  t0 = DWT->CYCCNT;
  if (HAL_CORDIC_CalculateZeroOverhead(pCORDIC, &SrcBuffer, &DstBuffer, TIMEOUT) != HAL_OK)
  {
    goto _app_process_exit;
  }
  g_cycles_hal_zo = DWT->CYCCNT - t0;

  /* ③ 直接读写寄存器：写一个参数，读两个结果（NRES = 1 时正弦在前、余弦在后） */
  t0 = DWT->CYCCNT;
  for (i = 0; i < ARRAY_SIZE; i++)
  {
    LL_CORDIC_WriteData(CORDIC, (uint32_t)Q1_31[i]);
    CalculatedSinCos[2U * i]        = (int32_t)LL_CORDIC_ReadData(CORDIC);
    CalculatedSinCos[(2U * i) + 1U] = (int32_t)LL_CORDIC_ReadData(CORDIC);
  }
  g_cycles_reg = DWT->CYCCNT - t0;

  /* ④ 软件 sinf()、cosf()：角度经 volatile 读出，防止编译器在编译期把结果算好 */
  t0 = DWT->CYCCNT;
  for (i = 0; i < ARRAY_SIZE; i++)
  {
    float32_t a = *(volatile const float32_t *)&Angles[i];
    SwSinCos[2U * i]        = sinf(a);
    SwSinCos[(2U * i) + 1U] = cosf(a);
  }
  g_cycles_sw = DWT->CYCCNT - t0;
  for (i = 0; i < ARRAY_SIZE; i++)
  {
    double a = (double)Angles[i];
    double es = fabs((double)SwSinCos[2U * i] - sin(a)) * Q31_SCALE;
    double ec = fabs((double)SwSinCos[(2U * i) + 1U] - cos(a)) * Q31_SCALE;
    uint32_t e = (uint32_t)((es > ec) ? es : ec) + 1U;
    g_sw_max_err_lsb = (e > g_sw_max_err_lsb) ? e : g_sw_max_err_lsb;
  }

  /* ⑤ PRECISION = 1..6 各算一遍，对照 RM 表 123 */
  for (p = 1U; p <= 6U; p++)
  {
    if ((HAL_CORDIC_SetPrecision(pCORDIC, (hal_cordic_precision_t)(LL_CORDIC_PRECISION_1_CYCLE * p))  /* PRECISION 在 CSR 位 7:4 */ != HAL_OK)
        || (HAL_CORDIC_Calculate(pCORDIC, &SrcBuffer, &DstBuffer, TIMEOUT) != HAL_OK))
    {
      goto _app_process_exit;
    }
    g_max_err_lsb[p - 1U] = max_error_lsb();
  }

  /* 判据：6 个周期的误差不超过表 123 的 2^-19 再加注 1 的舍入误差 2^-20 */
  if (g_max_err_lsb[5] <= ERR_LIMIT_6_CYCLE)
  {
    return_status = EXEC_STATUS_OK;
  }

_app_process_exit:
  return return_status;
} /* end app_process */

/** 双精度值换成 Q1.31，越界饱和（+1 只能表示成 0x7FFFFFFF） */
static int32_t to_q31_sat(double x)
{
  double v = x * Q31_SCALE;

  if (v >= 2147483647.0)
  {
    return INT32_MAX;
  }
  if (v <= -Q31_SCALE)
  {
    return INT32_MIN;
  }
  return (int32_t)lround(v);
}

/** 两个 Q1.31 之差的绝对值，用 64 位算，不会溢出 */
static uint32_t abs_diff(int32_t a, int32_t b)
{
  int64_t d = (int64_t)a - (int64_t)b;

  return (uint32_t)((d < 0) ? -d : d);
}

/** CalculatedSinCos[] 相对 RefQ31[] 的最大误差 */
static uint32_t max_error_lsb(void)
{
  uint32_t worst = 0U;

  for (uint32_t i = 0U; i < (2U * ARRAY_SIZE); i++)
  {
    uint32_t e = abs_diff(CalculatedSinCos[i], RefQ31[i]);
    worst = (e > worst) ? e : worst;
  }
  return worst;
}

/** ########## Step 5 ##########
  * Deinitializes the CORDIC instance before leaving the scenario.
  */
app_status_t app_deinit(void)
{
  mx_example_cordic_deinit();

  return EXEC_STATUS_OK;
} /* end app_deinit */
