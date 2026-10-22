# #38 代码使用说明

状态：以文章开头的“本篇状态”为准。

本示例针对 NUCLEO-C542RC / STM32C542RCT6，SDK STM32C5 MCU HAL2.1.0，外接 24C128/24C256 一类 I²C EEPROM（16 位存储地址、64 字节页）。
基线为官方 `examples/hal/i2c/eeprom_dma`。只改下列两个文件；生成层（`NUCLEO-C542RC/generated`）与 HAL 驱动与官方逐字节一致。

| 本目录文件 | 放回完整 SDK 的相对路径 |
|---|---|
| `example.c` | `examples/hal/i2c/eeprom_dma/application/example.c` |
| `flags.cmake` | `examples/hal/i2c/eeprom_dma/NUCLEO-C542RC/cmake/cmake/flags.cmake` |

在完整 SDK 的 `examples/hal/i2c/eeprom_dma/NUCLEO-C542RC/cmake` 运行：

```text
cmake --preset debug_GCC_NUCLEO-C542RC
cmake --build --preset debug_GCC_NUCLEO-C542RC
```

Release 将两处 debug 改为 release。工具链环境按 #02。
`固件构建记录.json` 保存 ELF、日志和包内 BIN 的 SHA256；编译源清单位于工作目录 logs。

## 固件与运行

`固件/C5_38_debug.elf` 和 `C5_38_release.elf` 是本轮构建产物，供调试下载；同名 BIN 为 Flash 镜像。
上板前先识别板卡、备份原程序，按 #02 下载/校验流程操作。当前未烧录，无实板测量记录。

接线：模块 VCC、GND 接 CN6 pin 4（3V3）、pin 6（GND）；SCL、SDA 接 CN5 D15（PB6）、D14（PB7）。板上没有 I²C 上拉，模块没带上拉时各接 4.7 kΩ 到 3V3。
串口：ST-LINK 虚拟串口 115200 8N1。**程序会覆盖 EEPROM 前 256 字节。**第 6 步有 30 秒用于拔插 SDA。
结果：串口逐步打印；LD1 常亮表示成功，每 2 秒闪一下表示失败（官方 main.c）。

## 相对官方的改动

- `flags.cmake`：Debug、Release 两处 `USE_TRACE=0` 改为 1。
- `example.c`：
  - 地址：扫描 0x08～0x77，取 0x50～0x57 中第一个应答者（官方写死 0xA2，即 0x51 左移）。
  - 等待：ACK 轮询最多 10 ms（`WaitReady`），DMA 完成最多 100 ms（`MemTransferDMA`）；官方两处都无出口。
  - 错误：`ReportError` 拆解 NACK/BERR/ARLO/TIMEOUT，hal_status 按十六进制打印；只报 BERR 时整笔重试一次（ES0676 §2.8.2）。
  - 恢复：BUSY 不退或超时后 `BusRecover`：关 I2C1，PB6 开漏 GPIO 最多 9 个脉冲，发 STOP，重新初始化并重新注册回调。
  - 新增第 5 步页回卷实验、第 6 步 30 秒热插拔监测；BUFFER_SIZE 128 → 256。

本包保留相对官方基线的两个 diff。官方例程许可证见 `LICENSE_官方例程.md`，上游版权声明继续适用。
