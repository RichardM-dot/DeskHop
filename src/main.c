/*
 * ============================================================================
 * 【中文导读】main.c —— DeskHop 程序入口：双核协作的起点
 * ----------------------------------------------------------------------------
 * 本文件做什么：
 *   定义 core0 / core1 两个"任务表"，各自是一个永远不会退出的调度循环。
 *   DeskHop 没有用 RTOS，而是自写了极简协作式调度器（task_scheduler，见
 *   tasks.c）：每个任务带一个执行频率（_HZ 每秒几次 / _TOP 越快越好），
 *   到点了就跑一次，跑完立刻返回——没有抢占，全靠每个函数都不阻塞。
 *
 * 双核分工（RP2040 特性）：
 *   core0：USB 设备端（对电脑假装自己是键鼠）+ 键鼠队列转发 + UART 发送
 *   core1：USB 主机端（通过 PIO 模拟的 USB 收真实键鼠）+ 收对端 UART 包 +
 *          LED/屏保/固件升级
 *   两核通过 device 结构体共享状态（全局单例 device），靠时间戳互相监测死活。
 *
 * 值得注意的机制：
 *   - _firmware_metadata 放在专用段 .section_metadata，供固件升级流程校验版本
 *   - watchdog：core0 喂狗，core1 每圈更新时间戳；core1 死了 core0 就不喂狗 →
 *     整机重启，这是双核互检的经典手法
 * ============================================================================
 */
/*
 * This file is part of DeskHop (https://github.com/hrvach/deskhop).
 * Copyright (c) 2025 Hrvoje Cavrak
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * See the file LICENSE for the full license text.
 */
#include "main.h"

/*********  Global Variables  **********/
device_t global_state     = {0};     /* 全局唯一状态单例：两核共享的所有状态都在这里 */
device_t *device          = &global_state;

/* 固件元数据放在链接脚本指定的独立段里，升级固件时按段定位、按版本兼容 */
firmware_metadata_t _firmware_metadata __attribute__((section(".section_metadata"))) = {
    .version = 0x0001,
};

/* ================================================== *
 * ==============  Main Program Loops  ============== *
 * ================================================== */

int main(void) {
    /* core0 任务表：frequency 用 _HZ(n)=每秒跑 n 次、_TOP()=每圈都跑。
     * 阅读提示：从上到下按优先概念排布——USB 设备端时序最敏感放最前 */
    static task_t tasks_core0[] = {
        [0] = {.exec = &usb_device_task,          .frequency = _TOP()},      // .-> USB device task, needs to run as often as possible   /* USB设备端（TinyUSB）：维护与电脑A/B的键鼠枚举与上报 */
        [1] = {.exec = &kick_watchdog_task,       .frequency = _HZ(30)},     // | Verify core1 is still running and if so, reset watchdog timer  /* 喂狗前先检查 core1 心跳时间戳 */
        [2] = {.exec = &process_kbd_queue_task,   .frequency = _HZ(2000)},   // | Check if there were any keypresses and send them  /* 从键盘队列取按键，经当前活跃 USB 端口上报 */
        [3] = {.exec = &process_mouse_queue_task, .frequency = _HZ(2000)},   // | Check if there were any mouse movements and send them  /* 同上，鼠标移动 */
        [4] = {.exec = &process_hid_queue_task,   .frequency = _HZ(1000)},   // | Check if there are any packets to send over vendor link  /* vendor 自定义通道：给 Web 配置器回数据 */
        [5] = {.exec = &process_uart_tx_task,     .frequency = _TOP()},      // | Check if there are any packets to send over UART  /* 把要发给另一块板子的包排队发出 */
    };                                                                       // `----- then go back and repeat forever
    const int NUM_TASKS = ARRAY_SIZE(tasks_core0);

    // Wait for the board to settle
    sleep_ms(10);      /* 等电源/时钟稳定，避免上电瞬间外设异常 */

    // Initial board setup
    initial_setup(device);   /* GPIO/UART/PIO-USB/共享状态初始化（setup.c） */

    // Initial state, A is the default output
    set_active_output(device, OUTPUT_A);   /* 上电默认输出到 A 电脑 */

    // Start core 1 after hardware and shared state initialization is complete
    multicore_launch_core1(core1_main);   /* 共享状态就绪后才启动 core1，避免竞态 */

    // Setup the watchdog so we reboot and recover from a crash
    watchdog_enable(WATCHDOG_TIMEOUT, WATCHDOG_PAUSE_ON_DEBUG);   /* 开看门狗：调试器暂停时暂停计数 */

    while (true) {
        for (int i = 0; i < NUM_TASKS; i++)
            task_scheduler(device, &tasks_core0[i]);   /* 永不退出的协作式调度循环 */
    }
}

/* core1 入口：负责"对内"——收真实键鼠（PIO-USB 主机）、收对端数据、外设指示 */
void core1_main() {
    static task_t tasks_core1[] = {
        [0] = {.exec = &usb_host_task,           .frequency = _TOP()},       // .-> USB host task, needs to run as often as possible  /* PIO-USB 主机轮询：读键鼠 HID 报告的入口 */
        [1] = {.exec = &packet_receiver_task,    .frequency = _TOP()},       // | Receive data over serial from the other board  /* 解析另一块板子发来的 UART 包 */
        [2] = {.exec = &led_blinking_task,       .frequency = _HZ(30)},      // | Check if LED needs blinking  /* 状态灯闪烁逻辑 */
        [3] = {.exec = &led_sync_task,           .frequency = _HZ(30)},      // | Sync LED state if needed  /* 两板 LED 状态同步 */
        [4] = {.exec = &screensaver_task,        .frequency = _HZ(120)},     // | Handle "screensaver" movements  /* 屏保模式下模拟鼠标移动 */
        [5] = {.exec = &firmware_upgrade_task,   .frequency = _HZ(4000)},    // | Send firmware to the other board if needed  /* 固件升级：把收到的固件块转发/写入 */
        [6] = {.exec = &heartbeat_output_task,   .frequency = _HZ(1)},       // | Output periodic heartbeats  /* 每秒心跳包，供对端判断本板存活 */
    };                                                                       // `----- then go back and repeat forever
    const int NUM_TASKS = ARRAY_SIZE(tasks_core1);

    while (true) {
        // Update the timestamp, so core0 can figure out if we're dead
        device->core1_last_loop_pass = time_us_32();   /* 心跳时间戳：core0 喂狗前会检查它是否足够新 */

        for (int i = 0; i < NUM_TASKS; i++)
            task_scheduler(device, &tasks_core1[i]);
    }
}
/* =======  End of Main Program Loops  ======= */
