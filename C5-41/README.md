# #41 A/B双板教学工程

2026-10-20。适用NUCLEO-C542RC，硬件依据C542RC B02原理图；SDK为STM32C5 MCU HAL2.1.0，GCC14.3.1+st.2。两端代码构建通过，未上板。

## 先认角色与文件

- A_controller：请求发起端，TX101、RX102；NODE_CONTROLLER=1。
- B_responder：回显端，TX102、RX101；NODE_CONTROLLER=0。
- 每个角色目录含example.c、main.c、flags.cmake和三个相对官方的diff。两个example.c内容相同，flags宏不同；main保留各自原版结构，只修改成功LED条件。
- 固件目录按A/B、Debug/Release分开，有4个真实教学ELF；完整身份见固件构建记录.json。原版两端也构建了Debug/Release，共8种配置。
- 各角色LICENSE.md保留例程SLA0044及版权；本包不包含整个官方SDK。

## 创建独立教学工程

Python3.12已安装时，在本目录的PowerShell执行：

```powershell
python ./代码/准备工程.py --sdk-zip '<SDK 下载目录>/STM32C5-MCU-HAL2.1.0_12-08-2026_06-37-21.zip' --out 'D:/STM32C5_41'
```

SDK路径换成你自己的存放位置，输出目录必须不存在。脚本核对ZIP和六个原版/教学文件的SHA256，提取完整相对依赖并覆盖六处；不删除、不覆盖已有工程。整个ZIP哈希不同则停下，先核对版本，不自动改用新版。`--check-only`只核对、不创建目录。

本轮实际运行了准备脚本，并将输出6230个文件与构建输入sdk_mod逐个字节比对。没有在读者电脑安装或运行。

也可手动解压同一SDK，在独立副本内覆盖以下文件：

| 包内 | 独立SDK目标 |
|---|---|
| A_controller/example.c、main.c | examples/hal/fdcan/two_boards_com_polling_controller/application/ |
| A_controller/flags.cmake | 上述controller/NUCLEO-C542RC/cmake/cmake/ |
| B_responder/example.c、main.c | examples/hal/fdcan/two_boards_com_polling_responder/application/ |
| B_responder/flags.cmake | 上述responder/NUCLEO-C542RC/cmake/cmake/ |

生成层、HAL和files.cmake不改，无新增C编译单元。两个生成层接收ID不同，不要把A整目录当B工程。

## 构建

先按#02将已安装GCC/CMake/Ninja放入当前终端PATH。A：

```powershell
Set-Location 'D:/STM32C5_41/examples/hal/fdcan/two_boards_com_polling_controller/NUCLEO-C542RC/cmake'
cmake --preset debug_GCC_NUCLEO-C542RC
cmake --build --preset debug_GCC_NUCLEO-C542RC
```

B进入two_boards_com_polling_responder的对应cmake目录，执行同样两条命令。Release把两处debug换release。产物分别是build/<preset>/hal_fdcan_two_boards_com_polling_controller.elf与responder.elf。program/debug配置要指向各自ELF，不能指向上一篇。

## 启动和成功目标

烧录前标A/B并记录探针序列号。各自ST-LINK USB供电，CN18的1/2/3分别同名相接为CANH/CANL/GND，两边JP9接通；USB断电时核约60Ω。先启动B等待，再复位A。

两端VCP115200 8N1无流控，只TX不收命令。A先打印TX_OK，收到匹配回包后双方有EXCHANGE_OK，序号递增；正常后A暂停1000ms再发。至少保存十轮日志，不能把LED常亮单独当在线证明。

PE2显式输出低，B02的STBY还有R26下拉。g_pe2读回只是MCU脚电平信息，不能替代对实际STBY测量。FAULT时PSR/ECR等仅抓一份，后续Stop会改变现场；看保存的g_变量。

## 错误和恢复

发送100ms等待，回复1000ms等待；软件超时不是CAN规范时限。A code10=发送未完成/失败，20=回复等待或头检查失败，21=数据不同；B code30=请求头/接收失败，31=回送失败。保存HAL十六进制状态、TEC/REC、LEC/DLEC、EW/EP/BO及CCCR/TXBRP。

错误后请求取消挂起帧、Stop并锁定；g_abort_status、g_stop_status保存返回值。不自动恢复bus-off；排除配置与总线原因后复位，先B后A。B单纯没请求时每秒WAIT，继续等待，不报致命错误。

单项练习只把B生成mx_fdcan1.c的filter_id1=101改成100：健康总线正常模式下，预期A仍TX_OK但之后RX_FAIL，B仍WAIT。该现象尚未实测。恢复101后重编B，再复位A。不要将练习固件覆盖交付ELF及其哈希。

上板欠账见上板验收计划.md。当前Programmer只读枚举无ST-LINK；没有测总线、电阻、波形或跑过两板互发。
