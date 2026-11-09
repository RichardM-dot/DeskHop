# DeskHop（RP2040 键鼠切换器）学习导读

> 目录：`04-项目实战/deskhop-RP2040键鼠切换器`
> 来源：https://github.com/hrvach/deskhop （约 7.9k star）

## 这是什么

DeskHop 用**两块树莓派 Pico（RP2040）**做了一个"KVM 切换器"：一套键鼠控制两台电脑，按快捷键或把鼠标"拖"过屏幕边界即可瞬间切换。核心魔法是 RP2040 的 **PIO-USB**（PIO 状态机实现的 USB 主机/设备），让单芯片同时当"USB 主机接键鼠"和"USB 设备模拟键鼠给电脑"。代码量小（`src/` 十几个文件）、注释友好、README 极其详细，**是 PIO-USB 和 USB HID 协议的最佳入门项目**。

## 目录导读

| 目录/文件 | 内容 |
| --- | --- |
| `src/main.c` | 双核初始化与主循环 |
| `src/usb.c` / `usb_descriptors.c` | USB 设备端（TinyUSB）：模拟键鼠给电脑 |
| `src/hid_parser.c` / `hid_report.c` | USB 主机端：解析真实键鼠的 HID 报表 |
| `src/mouse.c` / `keyboard.c` | 绝对坐标鼠标 trick、键盘转发逻辑 |
| `src/uart.c` / `protocol.c` | 两块板子间的 UART 私有协议（切换消息） |
| `src/tasks.c` | 双核任务分工（core0/core1） |
| `src/ramdisk.c` | 虚拟 U 盘（配置文件直改） |
| `Pico-PIO-USB/` | PIO-USB 库（重点学习对象） |
| `pcb/` `schematics/` `case/` | 电路板与外壳（3D 打印） |

## 学习路径（建议）

1. 先读 README 的"How it works"章节（有架构图），理解"两板 UART 互联 + 各管一台电脑"的拓扑。
2. 读 `hid_parser.c`：真实键鼠的 HID report descriptor 解析——USB 协议里最常用也最绕的部分，这里讲得很清楚。
3. 读 `mouse.c` 的绝对坐标魔法：把相对移动累积成绝对坐标上报，才能实现"拖动鼠标跨机切换"。
4. 读 `Pico-PIO-USB/` 了解 PIO 如何"软件模拟"出 USB 主机（需要一点 PIO 汇编基础，可以只读注释）。
5. 硬件组装：两块 Pico + 转接板（PCB 开源），或直接杜邦线连 UART。

## 学习要点

1. **RP2040 双核分工**：core1 跑 PIO-USB 主机（时序敏感），core0 跑设备端 + 逻辑——单芯片 USB 上下行的标准姿势。
2. **HID 协议**：report descriptor、endpoint、轮询周期；学完可以自己写"模拟鼠标"或"键鼠转发"类产品。
3. **私有 UART 协议设计**：帧头/校验/重同步，两个 MCU 间的可靠通信范本。
4. **量产思维**：固件 OTA、ramdisk 配置盘、LED 状态机——一个人做完整个产品的完整参考。

## 建议购买/准备

- **树莓派 Pico ×2**（¥25-40/块，务必买带排针的 H 版好接线）
- USB 母转母/A 型公头连接器或现成 **Pico 转接板**（作者推荐 ±¥30，也可 PCB 打样 ¥20-50）
- 杜邦线、面包板（入门实验够用）
- Micro-USB/USB-C 数据线 2 根
- 总预算：**¥60-120** 即可复刻一台自用
- 3D 打印外壳（可选，模型在 `case/`）

## 学习目标（自测）

- [ ] 讲清两块 Pico 各自的 USB 角色
- [ ] 读懂 HID report descriptor 并定位键盘按键位
- [ ] 解释绝对坐标鼠标如何实现"拖动切换"
- [ ] 组装一台并成功切换两台电脑
