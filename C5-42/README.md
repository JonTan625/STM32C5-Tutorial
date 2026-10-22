# #42 USB HID鼠标教学工程

2026-10-20。NUCLEO-C542RC；硬件依据C542RC B02图纸，HAL2.1.0归档、官方例程2.0.3、GCC14.3.1+st.2。两种构建通过，未上板。

## 创建工程

在本目录的PowerShell执行（已安装Python3.12时）：

```powershell
python ./代码/准备工程.py --sdk-zip '<SDK 下载目录>/STM32C5-MCU-HAL2.1.0_12-08-2026_06-37-21.zip' --out 'D:/STM32C5_42'
```

SDK路径按实际替换。输出目录必须不存在，脚本不覆盖已有目录；先核ZIP和两个原/新文件SHA256，再提取6457文件依赖子集并覆盖两处。--check-only只核对。本轮实际运行并逐字节比较输出6457文件与sdk_mod一致；未在读者电脑复现。

也可手动解压同版SDK，把本目录application/example.c，以及NUCLEO-C542RC/generated/middleware/usbx/mx_usbx_device_hid_mouse.c分别放回examples/middleware/usbx/device_hid_mouse_no_os/下同名路径。两个diff对照官方原版；本篇明确改了一个生成文件，重新生成会覆盖教学改动。HAL、USBX库、描述符字节和构建源列表不改，无新增C单元。

## 构建与固件

在#02配置过当前终端PATH的环境中：

```powershell
Set-Location 'D:/STM32C5_42/examples/middleware/usbx/device_hid_mouse_no_os/NUCLEO-C542RC/cmake'
cmake --preset debug_GCC_NUCLEO-C542RC
cmake --build --preset debug_GCC_NUCLEO-C542RC
```

Release把两处debug换release。ELF为build/<preset>/usbx_device_hid_mouse_no_os.elf。固件/Debug与Release提供两个实际教学ELF，SHA、size与日志索引在固件构建记录.json；不要仍选择#41或别板工程。

## 接口与观测

保持JP1 STLK、1–2，CN1接ST-LINK供电/下载/调试；CN13另一根USB-C数据线接电脑主机。无需改焊桥或外接线。PC13为输入按键，不能配输出低。用普通桌面空白区域验鼠标，每次完整按放、间隔≥200ms。

本篇不初始化USART、没有串口日志。主机核VID0483/PID5710和产品字符串STM32 USBD Device（官方演示身份）；调试器观察g_usb_state=3、g_configured=1是配置后的目标，不能独立代替主机证据。LD1反映类激活。PC4 VBUS_DETECT未接入软件，拔掉CN13但CN1继续供电时，不保证LED/类状态立即清零。

g_edges/g_press/g_bounce：原始边沿、50ms窗口接受、抑制计数；ButtonState为单标志，会合并快速多按。g_queued/g_queue_fail只统计事件入队，不能冒充总线已成功传输或主机已处理。g_step是下一步索引，0–15；每四步换方向，16次成功后回0。g_last_report是最近准备的四字节；X+/Y+/X−/Y−分别为0A/F6，按键与滚轮均0。

CRS在app_init显式配置USB SOF同步与自动微调。g_crs_cr/cfgr/isr记录当前寄存器，RELOAD47999；FELIM/TRIM沿用本版HAL默认值，不声称最优或已校准到某精度。SOF的CPU中断仍关闭，不影响选择内部SOF同步。

g_fault1=句柄、2=CRS、3=PCD、4=USBX、5=DCD/PCD状态、6=EXTI。g_init_status为USBX状态；g_tasks_status是USBX任务状态值，不能当HAL状态码解释。如果ExecStatus错误且g_fault仍0，先查main里的mx_system_init，因为还没运行到应用故障分支。

## 验证与边界

上板验收计划.md列六项操作、判据与当前源码行号。步长练习只改生成HID文件CURSOR_STEP=10为5，重编后字节05/FB；不要把练习ELF混同归档基线哈希。

本轮Programmer2.23.0只读枚举No ST-Link detected!，未连接/烧录/按键/枚举。USB OUT缓冲时序勘误ES0676§2.13.1尚未测IRQ裕量，Debug与Release各自验收；HAL保留原版，没有宣称已修复。物理拔线检测、远程唤醒、BIOS Boot Protocol、PD/Host、认证均不属于已验证功能。
