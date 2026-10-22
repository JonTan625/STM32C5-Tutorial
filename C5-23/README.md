# #23 代码使用说明

状态：以文章开头的“本篇状态”为准。

## 恢复工程

1. 从 ST Package Creator 获取本文基线 STM32C5 MCU HAL2.1.0 SDK。使用 `examples/hal/pwr/stop1` 的 NUCLEO-C542RC 工程。
2. 备份原文件，按下表的官方相对路径覆盖全部改动文件。`代码/` 里应用文件放在根目录；配置文件保留目录层级。
3. 进入 `NUCLEO-C542RC/cmake`，执行 `cmake --preset debug_GCC_NUCLEO-C542RC`，再执行 `cmake --build --preset debug_GCC_NUCLEO-C542RC`。Release 用 `release_GCC_NUCLEO-C542RC`。
4. 构建产物为 `build/<preset>/*.elf`。本机 GCC 14.3.1、CMake 4.4.0、Ninja 1.13.2；实际尺寸、哈希与日志位置见上一级 `固件构建记录.json`。

| 本目录文件 | 覆盖官方文件 | 差异文件 |
|---|---|---|
| `example.c` | `application/example.c` | `example_c_改动.diff` |


## 上板复现

先核对 `上板验收计划.json` 与正文验收节，按作者手动操作执行。这里没有自动烧录、自动擦除脚本。中断/DMA例程读寄存器使用 ST-LINK GDB server 的共享模式 `-m 1`，再用对应 ELF 附着；CubeProgrammer HOTPLUG 可能清掉 NVIC 使能，不能拿这种读数判断中断程序。

Flash 类文章 #22/#25 会写主 Flash 的最后一页；B1 启动清空及页满自动擦除都丢失该页历史。链接器已预留最后 8 KB，烧录工具的整片擦除仍会清掉记录。不要修改选项字或执行整片擦除来排障。

源码中的 ST 版权声明保留；许可证以官方 SDK 为准。构建通过不能推出硬件通过。
