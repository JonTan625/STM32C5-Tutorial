# #40 教学代码与复现

适用：NUCLEO-C542RC、STM32C542RCT6，STM32C5 MCU HAL2.1.0，GCC 14.3.1+st.2。源码已构建，尚未上板。

## 文件与替换位置

- `example.c` → `examples/hal/fdcan/loopback/application/example.c`
- `flags.cmake` → `examples/hal/fdcan/loopback/NUCLEO-C542RC/cmake/cmake/flags.cmake`
- 对应 `.diff` 是相对官方归档的变化，`改动清单.json` 给出原版/教学版哈希。
- `固件/Debug/hal_fdcan_loopback.elf`、`固件/Release/hal_fdcan_loopback.elf` 是本轮真实构建产物；身份与大小见 `固件构建记录.json`。
- `LICENSE.md` 保留本例程的 SLA0044 Rev6 条款及版权。官方 SDK 本身另行从 ST 获取；不要把本篇两个文件当成完整 SDK。

## 方法一：手动覆盖独立副本

将与 #02 相同的 STM32C5 MCU HAL2.1.0 ZIP 解压到新目录 `D:/STM32C5_40/`，在这个副本内替换上述两个文件。原官方包不要覆盖。生成层、HAL 驱动和 main.c 都不改。

## 方法二：Python 准备脚本

已安装 Python 3.12 时，在本目录的 PowerShell 中运行（SDK 路径按自己的存放位置修改）：

```powershell
python ./代码/准备工程.py --sdk-zip '<SDK 下载目录>/STM32C5-MCU-HAL2.1.0_12-08-2026_06-37-21.zip' --out 'D:/STM32C5_40'
```

脚本先核对整个 ZIP 与两个被替换文件的哈希，再提取本例的完整相对依赖并覆盖两处。输出目录必须不存在；不会删除或覆盖已有工程。若 ZIP 哈希不同，停止并核对下载包，不自动适配新版。`--check-only` 可只校验、不提取。

本轮实际执行过 `--check-only`，验证 ZIP 与两个教学文件身份；也实际执行了工程创建并核对输出集合。

## 构建与调试

先按 #02 将已安装 GCC/CMake/Ninja 放入当前终端 PATH，然后：

```powershell
Set-Location 'D:/STM32C5_40/examples/hal/fdcan/loopback/NUCLEO-C542RC/cmake'
cmake --preset debug_GCC_NUCLEO-C542RC
cmake --build --preset debug_GCC_NUCLEO-C542RC
```

Release 将两处 `debug` 改为 `release`。ELF 在 `build/<preset>/hal_fdcan_loopback.elf`。VS Code 打开此 cmake 目录可查看源码与构建配置；若使用既有调试配置，program 必须指向这个 ELF，不能仍指向前一篇固件。

教学版为内部回环，不接 CN18，不短接 PB8/PB9。USART2 只TX，ST-LINK VCP 115200 8N1，无流控。先开终端再复位。成功目标为 `[40] DONE pass=15 fail=0`，LED常亮。原版 USE_TRACE=0 不打印，不能拿本篇日志当原版行为。

## 当前证据与限制

官方 Debug/Release text=15688/8480；教学版 text=23488/14448，data=104、bss=2304，均0编译警告。GNU size 的 text 同时包括代码与只读数据，不等于 Keil map 的纯Code列。

当前未枚举到ST-LINK，未烧录本篇ELF。三档位速率为按配置推算；没有真实总线测量。接收缓冲64字节，先检查目标FIFO非空；Tx/Rx100 ms、拒收观察20 ms。最后配置与接收保存在 `g_` 变量，成功后会DeInit，现场寄存器可能已改变。

上板清单在 `上板验收计划.md`；错误码按正文07节及当前 `example.c` 的 `Fail()` 调用核对。
