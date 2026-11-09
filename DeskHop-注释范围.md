# DeskHop 注释范围

- A级（逐函数/逐逻辑中文深注，17 个 .c 全覆盖）：
  src/main.c core1调度、src/hid_parser.c、src/hid_report.c、src/mouse.c、
  src/keyboard.c、src/handlers.c、src/usb.c、src/setup.c、src/tasks.c、
  src/protocol.c、src/uart.c、src/usb_descriptors.c、src/ramdisk.c、
  src/led.c、src/utils.c、src/constants.c、src/defaults.c
- B级：include/*.h 头文件不加注释（结构体/宏的中文说明穿插在 .c 注释里讲）
- 说明：DeskHop 是"单文件单职责"的典范，全部 17 个 .c 都是 A 级。
