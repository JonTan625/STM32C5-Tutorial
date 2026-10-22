# #35 代码使用说明

状态：以文章开头的“本篇状态”为准。

本示例针对 NUCLEO-C542RC / STM32C542RCT6，SDK STM32C5 MCU HAL2.1.0。
基线为官方 `examples/ll/opamp/programmable_gain_amplifier`。仅改写下列三个既有文件，生成层和构建源列表沿用官方；LL DAC/COMP 为头文件内联接口。

| 本目录文件 | 放回完整 SDK 的相对路径 |
|---|---|
| `example.c` | `examples/ll/opamp/programmable_gain_amplifier/application/example.c` |
| `ll_example.h` | `examples/ll/opamp/programmable_gain_amplifier/application/ll_example.h` |
| `stm32c5xx_ll_example.c` | `examples/ll/opamp/programmable_gain_amplifier/application/stm32c5xx_ll_example.c` |

在完整 SDK 的 `examples/ll/opamp/programmable_gain_amplifier/NUCLEO-C542RC/cmake` 运行：

```text
cmake --preset debug_GCC_NUCLEO-C542RC
cmake --build --preset debug_GCC_NUCLEO-C542RC
```

Release 将两处 debug 改为 release。工具链环境按 #02。
`固件构建记录.json` 保存 ELF、日志和包内 BIN 的 SHA256；编译源清单位于工作目录 logs。

## 固件与运行

`固件/C5_35_debug.elf` 和 `C5_35_release.elf` 是本轮构建产物，供调试下载；同名 BIN 为 Flash 镜像。
上板前先识别板卡、备份原程序，按 #02 下载/校验流程操作。当前未烧录，无实板测量记录。

不需要接线，没有串口。每轮：DAC 通道 1 置 0/4095 各读一次 VrefInt（VIN13 检查）→ COMP1 七项扫描 → OPAMP1 五项两点测量；
main.c 每轮之间等 1 秒。结果在 `g_comp[0..6]`、`g_opamp[0..4]`，`valid=1` 才接受；`g_rounds` 为已完成轮数。
COMP 前四项预期 rise 与期望码相差不超过 55 LSB；迟滞三项只观察。OPAMP 两点增益误差预算 17/30/36/47/70‰。均为推算，未实测。
`g_dac2_ready=0` 表示 DAC2RDY 未置位，OPAMP 项跳过。
`g_error=0` 为无已记录错误；1 时钟、2 ADC 校准/使能、3 DAC1RDY、4 ADC 超时、5 OVR、6 VrefInt 为 0、7 DOR 不符。

## 相对官方的改动

- DAC1 两通道 MODE=011（缓冲关、只接片内），通道 1 接 COMP1 同相端，通道 2 接 OPAMP1 同相端；PA4、PA5 不被 DAC 占用。
- COMP1：反相端 VrefInt 1/4～全值、迟滞四档，先关后改再开、DWT 等 500 us；码值上扫/下扫读 C1VAL。COMP2 不使能（ES0676 §2.4.1）。
- OPAMP1：初始化后不立即使能；每项关闭后改为跟随或 PGA ×2/4/8/16，同相端改 DAC，输出仍到 PA6。
- ADC1：生成层 rank 1 = PA6 之外补 rank 2 = VrefInt，OVR 保留旧数据，软件触发轮询读 16 次平均；校准/使能顺序同 #34。
- 删除官方 ADC 中断读数路径（ADC_Activate/ADC_Calibrate/ADC_StartConversion_IT/ADC_GetDataGPIOmV/中断回调）与 OPAMP_Start，改为上述函数。
- DelayUs 由 volatile 空循环改为 DWT 计时。
- 记录 CCIPR2、DAC_MCR、最后一项 COMP_CFGR1 与 OPAMP_CSR、出厂 TRIMOFFSETN/P。

本包保留相对官方基线的三个 diff。官方例程许可证见 `LICENSE_官方例程.md`，上游版权声明继续适用。
