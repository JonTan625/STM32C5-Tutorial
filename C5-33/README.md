# #33 代码使用说明

状态：以文章开头的“本篇状态”为准。

本示例针对 NUCLEO-C542RC / STM32C542RCT6，SDK STM32C5 MCU HAL2.1.0。
仅改写下列三个既有文件，生成层和构建源列表沿用官方；TIM/DBGMCU LL为内联接口。

| 本目录文件 | 放回完整 SDK 的相对路径 |
|---|---|
| `example.c` | `examples/ll/adc/multi_channels_dma/application/example.c` |
| `ll_example.h` | `examples/ll/adc/multi_channels_dma/application/ll_example.h` |
| `stm32c5xx_ll_example.c` | `examples/ll/adc/multi_channels_dma/application/stm32c5xx_ll_example.c` |

在完整SDK的 `examples/ll/adc/multi_channels_dma/NUCLEO-C542RC/cmake` 运行：

```text
cmake --preset debug_GCC_NUCLEO-C542RC
cmake --build --preset debug_GCC_NUCLEO-C542RC
```

Release 将两处 debug 改为 release。工具链环境按 #02。
构建必须检查日志与链接 map。`固件构建记录.json` 保存 ELF、日志和包内 BIN 的 SHA256，实际源清单位于工作目录 logs。

## 固件与运行

`固件/C5_33_debug.elf` 和 `C5_33_release.elf` 是本轮构建产物，供调试下载；同名 BIN 为Flash镜像。
上板前先识别板卡、备份原程序，按 #02 下载/校验流程操作。当前未烧录，无实板测量记录。

没有串口。main.c 约每秒调用一次 app_process，三档500/1000/2000Hz轮换。
每块16轮、三通道、48半字（96字节）。PA4悬空值不用于验收电压。
`g_results[0..2]` 是三档最近一次计时；`valid=1` 才接受。
预期 `pretrigger_written=0`、`dma_remain=0`、`ovr=0`、`unwritten=0`。
`run_us` 理论约32050/16050/8050，另有同步、DMA与IRQ开销，以上未实测。
`g_error=0` 为无已记录错误；1超时、2OVR、3DMA错误、4停止失败、5提前采样、6数据/参考值异常。

## 与 #32 的主要区别

- CONT=0、TIM6 TRGO上升沿（EXTSEL12），三档定时；UG在ADC未armed时完成。
- ADSTART后先停表等待200us并检查未写入，随后才开表，DWT从开表前计时。
- DMA TC回调停TIM6；主循环确认ADC停止，检查DMA错误/OVR/长度后才统计。
- 采集等待100ms超时，ADC停止2ms超时；失败关闭DMA，不使用半块数据。
- 开启原例程的初始化超时，禁用失败立即返回，校准后的ADC激活错误向上传递；校准后延时比例与18MHz匹配。
- 初次启动按退出深度掉电→开稳压器并等待→校准→激活执行，去掉稳压器准备前的首次激活；稳压器与校准后等待使用已启用的DWT。
- 缓冲区沿用 #32 的volatile、哨兵和普通.bss；温度保留int32_t；VrefInt平均值为0时停止换算。

本包同时保留相对官方基线的三个diff。官方例程许可证见 `LICENSE_官方例程.md`，上游版权声明继续适用。
