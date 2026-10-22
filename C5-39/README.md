# #39 代码使用说明

状态：以文章开头的“本篇状态”为准。

本示例针对 NUCLEO-C542RC / STM32C542RCT6，SDK STM32C5 MCU HAL2.1.0。I3C1 作控制器，混合总线上只挂一颗 24C128/24C256 类 I²C EEPROM（与 #38 同一模块）。
基线为官方 `examples/hal/i3c/private_i2c_dma_controller`。改下列三个文件；生成层与 HAL、utils 源码与官方逐字节一致。

| 本目录文件 | 放回完整 SDK 的相对路径 |
|---|---|
| `example.c` | `examples/hal/i3c/private_i2c_dma_controller/application/example.c` |
| `files.cmake` | `examples/hal/i3c/private_i2c_dma_controller/NUCLEO-C542RC/cmake/cmake/files.cmake` |
| `flags.cmake` | `examples/hal/i3c/private_i2c_dma_controller/NUCLEO-C542RC/cmake/cmake/flags.cmake` |

在完整 SDK 的 `examples/hal/i3c/private_i2c_dma_controller/NUCLEO-C542RC/cmake` 运行：

```text
cmake --preset debug_GCC_NUCLEO-C542RC
cmake --build --preset debug_GCC_NUCLEO-C542RC
```

Release 将两处 debug 改为 release。工具链环境按 #02。

## 固件与运行

`固件/C5_39_debug.elf` 和 `C5_39_release.elf` 是本轮构建产物；同名 BIN 为 Flash 镜像。当前未烧录，无实板测量记录。

接线与 #38 相同：VCC、GND 接 CN6 pin 4、pin 6；SCL、SDA 接 CN5 D15（PB6）、D14（PB7）。串口 115200 8N1。程序写 EEPROM 0x0100 起 64 字节，之后每秒只读 1 字节。

## 相对官方的改动

- `files.cmake`：Debug、Release 两处加入 `stm32c5xx_drivers/utils/i3c_timing/stm32_utils_i3c.c`。
- `flags.cmake`：`USE_TRACE=0` 改为 1；include 路径加入 `stm32c5xx_drivers/utils/i3c_timing`。
- `example.c`：
  - 第 1 步：打印生成的 TIMINGR0/1；`STM32_UTILS_I3C_CTRL_ComputeTiming` 算 400 kHz 混合总线时序；FREE 超过 7 位时钳到 127 并警告；写入并读回打印。
  - 第 2 步：`HAL_I3C_CTRL_PoolForDeviceI2cReady` 在 0x50～0x57 找 EEPROM（7 位地址，不左移）。
  - 第 3 步：一条老式 I²C 写消息（2 字节地址 + 64 字节，`HAL_I2C_PRIVATE_WITHOUT_ARB_STOP`），10 ms 上限等写周期。
  - 第 4 步：写地址 + 重复起始 + 读 64 字节（`HAL_I2C_PRIVATE_WITHOUT_ARB_RESTART`），比对。
  - 第 5 步：打开 0x7E 仲裁头再读一次，预期 CE2。
  - 第 6 步：之后每秒读 1 字节，失败打印错误码并重新初始化。
  - 传输改为阻塞 `HAL_I3C_CTRL_Transfer`（100 ms 超时）；错误码拆为 ANACK/DNACK/CEx。

本包保留相对官方基线的三个 diff。官方例程许可证见 `LICENSE_官方例程.md`，上游版权声明继续适用。
