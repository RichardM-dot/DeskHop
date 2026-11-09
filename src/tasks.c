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

/*
 * ============================================================================
 * 【中文导读】tasks.c —— 协作式任务调度器 + 系统级任务集
 * ----------------------------------------------------------------------------
 * 本文件做什么：
 *   1. task_scheduler(): 整个固件的"心脏"——比较当前时间与任务下次执行时间，
 *      到点就执行。没有 RTOS、没有抢占，靠"每个任务都不阻塞"实现并发。
 *   2. 一组横切任务：喂狗、USB 轮询、屏保、心跳包、固件升级搬运、UART 收包。
 *
 * 精读建议：
 *   - kick_watchdog_task: 双核互检的经典写法（复制时间戳→再读当前时间→比较）
 *   - firmware_upgrade_task: 从对端逐字节拉固件写 Flash 的状态机
 *   - packet_receiver_task: DMA 环形缓冲找包头的零拷贝收包手法
 * ============================================================================
 */

#include "main.h"

/*
 * 极简协作式调度器（main.c 的 while 循环每圈对每个任务调一次）。
 * 未到执行时间立即返回（无阻塞）；到点则更新下次执行时间并调用任务函数。
 * uint64_t 微秒时间戳避免 49 天溢出问题。
 */
void task_scheduler(device_t *state, task_t *task) {
    uint64_t current_time = time_us_64();

    if (current_time < task->next_run)   /* 还没到点，直接返回，不空转占用 CPU */
        return;

    task->next_run = current_time + task->frequency;   /* 用"当前时间+周期"而不是"+=周期"，避免落后时连环补跑 */
    task->exec(state);   /* 执行任务本体（任务必须快速返回，绝不能阻塞） */
}

/* ================================================== *
 * ==============  Watchdog Functions  ============== *
 * ================================================== */

/*
 * 喂狗任务（core0，30Hz）：检查 core1 是否活着，活着才喂狗。
 * 若 core1 卡死 → 时间戳停止更新 → 这里停止喂狗 → 看门狗超时整机重启。
 */
void kick_watchdog_task(device_t *state) {
    /* Read the timer AFTER duplicating the core1 timestamp,
       so it doesn't get updated in the meantime. */
    uint32_t core1_last_loop_pass = state->core1_last_loop_pass;
    uint32_t current_time         = time_us_32();

    /* If a reboot is requested, we'll stop updating watchdog */
    if (state->reboot_requested)
        return;

    /* If core1 stops updating the timestamp, we'll stop kicking the watchog and reboot */
    if ((uint32_t)(current_time - core1_last_loop_pass) < CORE1_HANG_TIMEOUT_US)
        watchdog_update();
}

/* ================================================== *
 * ===============  USB Device / Host  ============== *
 * ================================================== */

/* USB 设备端轮询（TinyUSB 设备栈）：枚举为键鼠/vendor 设备并维护端点 */
void usb_device_task(device_t *state) {
    tud_task();
}

/* USB 主机端轮询（TinyUSB 主机栈 + PIO-USB）：读真实键鼠的 HID 报告。
 * tuh_inited() 检查是必须的——PIO-USB 初始化可能尚未完成 */
void usb_host_task(device_t *state) {
    if (tuh_inited())
        tuh_task();
}

/* 屏保·弹球模式：光标在屏幕边界间弹跳（静态变量记忆位置与速度向量） */
mouse_report_t *screensaver_pong(device_t *state) {
    static mouse_report_t report = {0};
    static int dx = 20, dy = 25;

    /* Check if we are bouncing off the walls and reverse direction in that case. */
    if (report.x + dx < MIN_SCREEN_COORD || report.x + dx > MAX_SCREEN_COORD)
        dx = -dx;

    if (report.y + dy < MIN_SCREEN_COORD || report.y + dy > MAX_SCREEN_COORD)
        dy = -dy;

    report.x += dx;
    report.y += dy;

    return &report;
}

/* 屏保·抖动模式：光标上下往返 1 像素（每次调用取反方向），防止屏保烧屏/休眠 */
mouse_report_t *screensaver_jitter(device_t *state) {
    static mouse_report_t report = {
        .y = JITTER_DISTANCE,
        .mode = RELATIVE,
    };
    report.y = -report.y;

    return &report;
}

/*
 * 屏保主任务（core1，120Hz 轮询、实际按模式限速）：系统空闲超时后模拟鼠标活动。
 * 三道闸门依次检查：是否启用/空闲够久/未超最大时长/允许在活跃输出上运行。
 */
/* Have something fun and entertaining when idle. */
void screensaver_task(device_t *state) {
    const uint32_t delays[] = {
        0,        /* DISABLED, unused index 0 */
        5000,     /* PONG, move mouse every 5 ms for a high framerate */
        10000000, /* JITTER, once every 10 sec is more than enough */
    };
    static uint32_t last_pointer_move = 0;
    screensaver_t *screensaver = &state->config.output[BOARD_ROLE].screensaver;
    uint64_t inactivity_period = time_us_64() - state->last_activity[BOARD_ROLE];

    /* If we're not enabled, nothing to do here. */
    if (screensaver->mode == DISABLED)   /* 闸门1：用户没开启屏保 */
        return;

    /* System is still not idle for long enough to activate or screensaver mode is not supported */
    if (inactivity_period < screensaver->idle_time_us || screensaver->mode > MAX_SS_VAL)
        return;   /* 闸门2：空闲时间还不够长（或模式值非法） */

    /* We exceeded the maximum permitted screensaver runtime */
    if (screensaver->max_time_us
        && inactivity_period > (screensaver->max_time_us + screensaver->idle_time_us))
        return;   /* 闸门3：屏保运行超过用户设定的最大时长，不再动鼠标 */

    /* If we're the selected output and we can only run on inactive output, nothing to do here. */
    if (screensaver->only_if_inactive && CURRENT_BOARD_IS_ACTIVE_OUTPUT)
        return;   /* 闸门4：设置为"只在非活跃输出跑"且本机正是活跃输出 */

    /* We're active! Now check if it's time to move the cursor yet. */
    if (time_us_32() - last_pointer_move < delays[screensaver->mode])
        return;   /* 按模式限速：PONG 每 5ms 一步（流畅弹球），JITTER 每 10s 一步 */

    /* Return, if we're not connected or the host is suspended */
    if(!tud_ready()) {   /* USB 端没准备好（未枚举/主机休眠）就不发 */
        return;
    }

    mouse_report_t *report;
    switch (screensaver->mode) {
        case PONG:
            report = screensaver_pong(state);
            break;

        case JITTER:
            report = screensaver_jitter(state);
            break;

        default:
            return;
    }

    /* Move mouse pointer */
    queue_mouse_report(report, state);   /* 把模拟移动塞进鼠标队列，后续由 core0 上报给电脑 */

    /* Update timer of the last pointer move */
    last_pointer_move = time_us_32();
}

/*
 * 心跳包任务（1Hz）：每秒向对端广播固件版本与当前活跃输出。
 * 对端借此得知"另一块板子还活着、跑的什么版本、当前切到了哪台电脑"。
 */
/* Periodically emit heartbeat packets */
void heartbeat_output_task(device_t *state) {
    /* If firmware upgrade is in progress, don't touch flash_cs */
    if (state->fw.upgrade_in_progress)
        return;

    if (state->config_mode_active) {   /* 配置模式期间借心跳节拍顺带处理两件事 */
        /* Leave config mode if timeout expired and user didn't click exit */
        if (time_us_64() > state->config_mode_timer)
            reboot();   /* 配置模式有超时：超时未退出就重启回到正常模式 */

        /* Keep notifying the user we're still in config mode */
        blink_led(state);
    }

#ifdef DH_DEBUG
    /* Holding the button invokes bootsel firmware upgrade */
    if (is_bootsel_pressed())
        reset_usb_boot(1 << PICO_DEFAULT_LED_PIN, 0);
#endif

    /* 组心跳包：版本号放 data16[0]、活跃输出放 data16[2]（协议见 protocol.c） */
    uart_packet_t packet = {
        .type = HEARTBEAT_MSG,
        .data16 = {
            [0] = state->_running_fw.version,
            [2] = state->active_output,
        },
    };

    queue_try_add(&global_state.uart_tx_queue, &packet);   /* 非阻塞入发送队列，满了就丢（下秒再发，心跳丢一拍无妨） */
}


/*
 * vendor 通道 HID 发送任务：Web 配置器等工具通过自定义 HID 接口与固件通信，
 * 回包都从 hid_queue_out 取出经 tud_hid_n_report 发给电脑。
 * peek→send→remove 三段式：发送成功才出队，失败下轮重试。
 */
/* Process other outgoing hid report messages. */
void process_hid_queue_task(device_t *state) {
    hid_generic_pkt_t packet;

    if (!queue_try_peek(&state->hid_queue_out, &packet))
        return;

    if (!tud_hid_n_ready(packet.instance))
        return;

    /* Only the keyboard interface can be in boot protocol, so discard other reports. */
    if (tud_hid_n_get_protocol(packet.instance) == HID_PROTOCOL_BOOT) {
        queue_try_remove(&state->hid_queue_out, &packet);
        return;   /* BIOS 启动协议模式下不支持 report id，这类包只能丢弃 */
    }

    /* ... try sending it to the host, if it's successful */
    bool succeeded = tud_hid_n_report(packet.instance, packet.report_id, packet.data, packet.len);

    /* ... then we can remove it from the queue. Race conditions shouldn't happen [tm] */
    if (succeeded)
        queue_try_remove(&state->hid_queue_out, &packet);
}

/*
 * 固件升级搬运任务（core1，4kHz）：A 板从电脑收到新固件后，B 板通过本任务
 * 逐字节向 A 板请求（request_byte），攒满一 Flash 页（256B）写一次。
 * 校验失败则擦掉 stage2 退回 ROM USB 恢复模式——升级失败的兜底路径。
 */
/* Task that handles copying firmware from the other device to ours */
void firmware_upgrade_task(device_t *state) {
    if (!state->fw.upgrade_in_progress || !state->fw.byte_done)
        return;

    if (queue_is_full(&state->uart_tx_queue))
        return;

    /* If we're on the last element of the current page, page is done - write it.
       Address zero is not the end of a page: nothing has arrived yet */
    /* 地址低字节为 0 说明刚跨过一页边界（0 不是页首：还没收到任何字节），
     * 把攒满的 256 字节页缓冲写进 Flash */
    if (TU_U32_BYTE0(state->fw.address) == 0x00 && state->fw.address != 0) {

        uint32_t page_start_addr = (state->fw.address - 1) & 0xFFFFFF00;
        write_flash_page((uint32_t)ADDR_FW_RUNNING + page_start_addr - XIP_BASE, state->page_buffer);
    }

    /* End condition, when reached the process is completed. */
    if (state->fw.address >= STAGING_IMAGE_SIZE) {   /* 收满整个镜像，收尾 */
        state->fw.upgrade_in_progress = 0;
        state->fw.checksum = ~state->fw.checksum;   /* CRC32 收尾取反（规范如此） */

        /* Checksum mismatch, we wipe the stage 2 bootloader and rely on ROM recovery */
        if(calculate_firmware_crc32() != state->fw.checksum) {
            /* 校验失败：擦掉固件头，重启进 RP2040 ROM 的 USB Boot模式（BOOTSEL 恢复） */
            flash_range_erase((uint32_t)ADDR_FW_RUNNING - XIP_BASE, FLASH_SECTOR_SIZE);
            reset_usb_boot(1 << PICO_DEFAULT_LED_PIN, 0);
        }

        else {
            state->_running_fw = _firmware_metadata;
            global_state.reboot_requested = true;
        }

        return;
    }

    request_byte(state, state->fw.address);
}

/*
 * UART 收包任务（core1，每圈都跑）：UART 接收由 DMA 持续写入环形缓冲，
 * 本任务不搬运数据，只沿着环形指针扫描，找同步字→组包→交给协议处理。
 * 关键技巧：DMA 的 transfer_count 寄存器就是"写指针"，零拷贝读取。
 */
void packet_receiver_task(device_t *state) {
    /* 读 DMA 剩余计数换算出当前写入位置（环形缓冲里的下标） */
    uint32_t current_pointer
        = (uint32_t)DMA_RX_BUFFER_SIZE - dma_channel_hw_addr(state->dma_rx_channel)->transfer_count;
    uint32_t delta = get_ptr_delta(current_pointer, state);   /* 自上次处理位置到写入位置的新字节数 */

    /* If we don't have enough characters for a packet, skip loop and return immediately */
    while (delta >= RAW_PACKET_LENGTH) {
        if (is_start_of_packet(state)) {
            fetch_packet(state);
            process_packet(&state->in_packet, state);
            return;
        }

        /* No packet found, advance to next position and decrement delta */
        state->dma_ptr = NEXT_RING_IDX(state->dma_ptr);
        delta--;
    }
}
