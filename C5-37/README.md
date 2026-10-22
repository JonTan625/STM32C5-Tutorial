# #37 代码使用说明

状态：以文章开头的“本篇状态”为准。

本示例针对 NUCLEO-C542RC / STM32C542RCT6，SDK STM32C5 MCU HAL2.1.0，外接 3.3 V Winbond W25Qxx SPI NOR 模块。
基线为官方 `examples/parts/w25q128j/read_write`。只改下列两个文件；生成层（`NUCLEO-C542RC/generated`）与 part driver（`part_drivers/w25q128j`）与官方逐字节一致。

| 本目录文件 | 放回完整 SDK 的相对路径 |
|---|---|
| `example.c` | `examples/parts/w25q128j/read_write/application/example.c` |
| `flags.cmake` | `examples/parts/w25q128j/read_write/NUCLEO-C542RC/cmake/cmake/flags.cmake` |

在完整 SDK 的 `examples/parts/w25q128j/read_write/NUCLEO-C542RC/cmake` 运行：

```text
cmake --preset debug_GCC_NUCLEO-C542RC
cmake --build --preset debug_GCC_NUCLEO-C542RC
```

Release 将两处 debug 改为 release。工具链环境按 #02。
`固件构建记录.json` 保存 ELF、日志和包内 BIN 的 SHA256；编译源清单位于工作目录 logs。

## 固件与运行

`固件/C5_37_debug.elf` 和 `C5_37_release.elf` 是本轮构建产物，供调试下载；同名 BIN 为 Flash 镜像。
上板前先识别板卡、备份原程序，按 #02 下载/校验流程操作。当前未烧录，无实板测量记录。

接线：模块 VCC、GND 接 CN6 pin 4（3V3）、pin 6（GND）；CLK、DO、DI、/CS 接 CN5 D13（PA5）、D12（PA6）、D11（PA7）、D9（PC6）；模块若引出 /WP、/HOLD，都接 3V3。
串口：ST-LINK 虚拟串口 115200 8N1。**程序会擦写模块地址 0 起的两个 4 KB 扇区，原有内容会丢失。**
结果：串口逐步打印；`ExecStatus` 为 2 表示全部通过，-1 表示某步失败（看最后一行打印）。

## 相对官方的改动

- `flags.cmake`：Debug、Release 两处 `USE_TRACE=0` 改为 1，打开串口打印。
- `example.c`：
  - 第 1 步：打印 SPI1 CFG1/CFG2；上电后等 10 ms（tPUW）；init 后等 1 ms（tRST）；读 90h、9Fh、三个状态寄存器；按 JEDEC ID 判厂商 EFh、算容量；ID 读成 00h/FFh 时分别提示。
  - 第 2～4 步：擦、写、读各自计时；擦后检查全 FF。
  - 第 5 步：不擦重写（F0h 上写 0Fh）。
  - 第 6 步：对比 `w25q128j_erase_sectors(0x800, 0x900)` 与新增的 `EraseRange()`。
  - 新增 `CountNot()`、`PrintSpiConfig()`、`EraseRange()`；步骤号顺延，deinit 为第 7 步。

本包保留相对官方基线的两个 diff。官方例程许可证见 `LICENSE_官方例程.md`，W25Q128J part driver 许可证见 `LICENSE_W25Q128J驱动.md`，上游版权声明继续适用。
