# #34 代码使用说明

状态：以文章开头的“本篇状态”为准。

本示例针对 NUCLEO-C542RC / STM32C542RCT6，SDK STM32C5 MCU HAL2.1.0。
基线为官方 `examples/ll/dac/simple_conversion`。仅改写下列三个既有文件，生成层和构建源列表沿用官方；LL ADC/DAC 为头文件内联接口。

| 本目录文件 | 放回完整 SDK 的相对路径 |
|---|---|
| `example.c` | `examples/ll/dac/simple_conversion/application/example.c` |
| `ll_example.h` | `examples/ll/dac/simple_conversion/application/ll_example.h` |
| `stm32c5xx_ll_example.c` | `examples/ll/dac/simple_conversion/application/stm32c5xx_ll_example.c` |

在完整 SDK 的 `examples/ll/dac/simple_conversion/NUCLEO-C542RC/cmake` 运行：

```text
cmake --preset debug_GCC_NUCLEO-C542RC
cmake --build --preset debug_GCC_NUCLEO-C542RC
```

Release 将两处 debug 改为 release。工具链环境按 #02。
`固件构建记录.json` 保存 ELF、日志和包内 BIN 的 SHA256；编译源清单位于工作目录 logs。

## 固件与运行

`固件/C5_34_debug.elf` 和 `C5_34_release.elf` 是本轮构建产物，供调试下载；同名 BIN 为 Flash 镜像。
上板前先识别板卡、备份原程序，按 #02 下载/校验流程操作。当前未烧录，无实板测量记录。

没有串口。main.c 每秒调用一次 app_process，七档码值 0、400、1024、2048、3072、3700、4095 轮流输出到 PA4（CN8 A2），
ADC1 在同一脚回读 16 次平均。结果在 `g_points[0..6]`；`valid=1` 才接受。
中间五档预期 |diff_lsb| ≤ 29（DS15135 表 58 TUECal 23 LSB + 表 54 ADC ET 5.7 LSB，推算，未实测）；0 与 4095 两档只观察。
`g_error=0` 为无已记录错误；1 时钟、2 ADC 校准/使能、3 DAC1RDY、4 ADC 超时、5 OVR、6 DOR 不符、7 VrefInt 为 0。

## 相对官方的改动

- ADC/DAC 共用内核时钟：在 DAC、ADC 使能前设 ADCDACPRE = ÷8（PSIS 144 MHz → 18 MHz），快照存 `g_ccipr2`。
- 新增 ADC1 两位次（PA4、VrefInt）软件触发轮询读数，配置沿用 #32 官方生成层写法；OVR 保留旧数据并按错误处理。
- 12 位右对齐写 DHR12R1（官方为 8 位 + 3300 mV 假设）；写后读回 DOR1 核对。
- DelayUs 由 volatile 空循环改为 DWT 计时，保证 tTRIM ≥ 50 us；校准算法保持官方。
- 使能后先等 DAC1RDY（RM0522 §22.4.4），再沿用官方 15 us 等待。
- 记录校准前后的 OTRIM1、DAC_MCR、VrefInt 平均值与 VREF+。

官方波形例程 `ll/dac/signal_generation_dma_silent_tim` 本篇只做解读与构建，未改代码。
本包保留相对官方基线的三个 diff。官方例程许可证见 `LICENSE_官方例程.md`，上游版权声明继续适用。
