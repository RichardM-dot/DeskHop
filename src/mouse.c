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
 * 【中文导读】mouse.c —— 鼠标报文处理：绝对坐标魔法 + 跨机切换（本项目精华）
 * ----------------------------------------------------------------------------
 * 本文件做什么：
 *   真实鼠标上报的是"相对位移"（左移 -3）。DeskHop 在它对电脑的 HID 描述符里
 *   把自己声明成【绝对坐标】鼠标，于是：
 *     相对位移 → 累加进全局虚拟光标位置 (pointer_x/y) → 作为绝对坐标上报。
 *   这样固件完全掌握光标位置，把光标"推过屏幕边界"就成了普通的坐标赋值，
 *   也就实现了"鼠标划到边上就切电脑"的核心体验。
 *
 * 主要链路（process_mouse_report）：
 *   抠原始值 → 加速度/速度缩放 → 判断是否越界(该切换了吗) → 更新虚拟光标
 *   → 组绝对坐标报文 → 发给本机或经 UART 发给对端 → 必要时执行切换。
 *
 * 精读点：
 *   - do_screen_switch 的边界/虚拟桌面决策树
 *   - calculate_mouse_acceleration_factor 的折线插值加速曲线
 *   - switch_virtual_desktop_macos 对 macOS 的兼容性 workaround
 * ============================================================================
 */

#include "main.h"
#include <math.h>

#define MACOS_SWITCH_MOVE_X 10
#define MACOS_SWITCH_MOVE_COUNT 5
#define ACCEL_POINTS 7

/* 计算当前方向的跳转阈值：多屏布局下"本地屏间切换"不设阈值（阈值=跨机跳转的越界余量） */
uint16_t get_jump_threshold(output_t *output, enum screen_pos_e direction) {
    const uint16_t NO_JUMP_THRESHOLD = 0;

    /* If on non-main local screen, every possible switch is local */
    if (output->screen_index > 1)
        return NO_JUMP_THRESHOLD;

    /* If on main screen but going away from the border, switch is local */
    if (output->pos == direction && output->screen_index == 1)
        return NO_JUMP_THRESHOLD;

    /* ... in all other cases, switch is non-local (jump to another pc) */
    return global_state.config.jump_threshold;
}

/*
 * 判断本次移动是否会越出"虚拟屏幕"（坐标系是 0~32767 的绝对坐标空间）。
 * 越界且超过 threshold 余量 → 返回方向（该切了）；否则 NONE。
 * threshold 的作用：防止"慢速挪到边缘"被误判成切换意图，要"用力撞边"才切。
 */
/* Check if our upcoming mouse movement would result in having to switch outputs */
enum screen_pos_e is_screen_switch_needed(output_t *output, int position, int offset) {
    enum screen_pos_e direction = (offset < 0) ? LEFT : RIGHT;

    /* No position offset implies no switch needed. */
    if (offset == 0)
        return NONE;

    /* Local switches (virtual desktop changes) have no gap, only cross-output jumps use threshold */
    uint16_t threshold = get_jump_threshold(output, direction);

    if (position + offset < MIN_SCREEN_COORD - threshold)
        return LEFT;

    if (position + offset > MAX_SCREEN_COORD + threshold)
        return RIGHT;

    return NONE;
}

/* 虚拟光标移动并夹紧在 [0, 32767] 屏幕范围内（绝对坐标空间不许出界） */
/* Move mouse coordinate 'position' by 'offset', but don't fall off the screen */
int32_t move_and_keep_on_screen(int position, int offset) {
    /* Lowest we can go is 0 */
    if (position + offset < MIN_SCREEN_COORD)
        return MIN_SCREEN_COORD;

    /* Highest we can go is MAX_SCREEN_COORD */
    else if (position + offset > MAX_SCREEN_COORD)
        return MAX_SCREEN_COORD;

    /* We're still on screen, all good */
    return position + offset;
}

/* Implement basic mouse acceleration based on actual 2D movement magnitude.
   Returns the acceleration factor to apply to both x and y components. */
/*
 * 鼠标加速度：按 2D 合位移大小查折线表，线性插值出放大倍数（1.0~4.0）。
 * 慢速微调时 1:1 精确移动，快速甩动时最高 4 倍——和 OS 鼠标加速同理。
 */
float calculate_mouse_acceleration_factor(int32_t offset_x, int32_t offset_y) {
    const struct curve {
        int value;
        float factor;
    } acceleration[ACCEL_POINTS] = {
                   // 4 |                                        *
        {2, 1},    //   |                                  *
        {5, 1.1},  // 3 |
        {15, 1.4}, //   |                       *
        {30, 1.9}, // 2 |                *
        {45, 2.6}, //   |        *
        {60, 3.4}, // 1 |  *
        {70, 4.0}, //    -------------------------------------------
    };             //        10    20    30    40    50    60    70

    if (offset_x == 0 && offset_y == 0)
        return 1.0;

    if (!global_state.config.enable_acceleration)
        return 1.0;

    // Calculate the 2D movement magnitude
    /* 合位移 = √(dx²+dy²)，只看幅度不看方向 */
    const float movement_magnitude = sqrtf((float)(offset_x * offset_x) + (float)(offset_y * offset_y));

    if (movement_magnitude <= acceleration[0].value)
        return acceleration[0].factor;

    if (movement_magnitude >= acceleration[ACCEL_POINTS-1].value)
        return acceleration[ACCEL_POINTS-1].factor;

    const struct curve *lower = NULL;
    const struct curve *upper = NULL;

    for (int i = 0; i < ACCEL_POINTS-1; i++) {
        if (movement_magnitude < acceleration[i + 1].value) {
            lower = &acceleration[i];
            upper = &acceleration[i + 1];
            break;
        }
    }

    // Should never happen, but just in case
    if (lower == NULL || upper == NULL)
        return 1.0;

    const float interpolation_pos = (movement_magnitude - lower->value) /
                                  (upper->value - lower->value);

    return lower->factor + interpolation_pos * (upper->factor - lower->factor);   /* 折线两点间线性插值 */
}

/*
 * 把相对位移换算成绝对坐标的主逻辑：
 * 原始位移 × 加速度 × 用户速度档(>>) → 虚拟光标新位置；同时判断切换方向。
 * mouse_zoom（慢速模式）用 >> 右移 2 位实现 ÷4 的精细移动。
 */
/* Returns LEFT if need to jump left, RIGHT if right, NONE otherwise */
enum screen_pos_e update_mouse_position(device_t *state, mouse_values_t *values) {
    output_t *current    = &state->config.output[state->active_output];
    uint8_t reduce_speed = 0;

    /* Check if we are configured to move slowly */
    if (state->mouse_zoom)
        reduce_speed = MOUSE_ZOOM_SCALING_FACTOR;

    /* Calculate movement */
    float acceleration_factor = calculate_mouse_acceleration_factor(values->move_x, values->move_y);
    int offset_x = round(values->move_x * acceleration_factor * (current->speed_x >> reduce_speed));
    int offset_y = round(values->move_y * acceleration_factor * (current->speed_y >> reduce_speed));

    /* Determine if our upcoming movement would stay within the screen */
    enum screen_pos_e switch_direction = is_screen_switch_needed(current, state->pointer_x, offset_x);

    /* Update movement */
    state->pointer_x = move_and_keep_on_screen(state->pointer_x, offset_x);
    state->pointer_y = move_and_keep_on_screen(state->pointer_y, offset_y);

    /* Update buttons state */
    state->mouse_buttons = values->buttons;

    return switch_direction;
}

/* 鼠标报文出口：本机是活跃输出→入本地 USB 队列；否则打包经 UART 发给对端板 */
/* If we are active output, queue packet to mouse queue, else send them through UART */
void output_mouse_report(mouse_report_t *report, device_t *state) {
    if (CURRENT_BOARD_IS_ACTIVE_OUTPUT) {
        queue_mouse_report(report, state);
        state->last_activity[BOARD_ROLE] = time_us_64();
    } else {
        queue_packet((uint8_t *)report, MOUSE_REPORT_MSG, MOUSE_REPORT_LENGTH);
    }
}

/*
 * 两台电脑屏幕高度不同时，跨越后按比例映射 Y 坐标（注释里的两个公式），
 * 保证光标在视觉上"等比例"落在对侧屏幕相同相对高度处。
 */
/* Calculate and return Y coordinate when moving from screen out_from to screen out_to */
int16_t scale_y_coordinate(int screen_from, int screen_to, device_t *state) {
    output_t *from = &state->config.output[screen_from];
    output_t *to   = &state->config.output[screen_to];

    int size_to   = to->border.bottom - to->border.top;
    int size_from = from->border.bottom - from->border.top;

    /* If sizes match, there is nothing to do */
    if (size_from == size_to)
        return state->pointer_y;

    /* Moving from smaller ==> bigger screen
       y_a = top + (((bottom - top) * y_b) / HEIGHT) */

    if (size_from > size_to) {
        return to->border.top + ((size_to * state->pointer_y) / MAX_SCREEN_COORD);
    }

    /* Moving from bigger ==> smaller screen
       y_b = ((y_a - top) * HEIGHT) / (bottom - top) */

    if (state->pointer_y < from->border.top)
        return MIN_SCREEN_COORD;

    if (state->pointer_y > from->border.bottom)
        return MAX_SCREEN_COORD;

    return ((state->pointer_y - from->border.top) * MAX_SCREEN_COORD) / size_from;
}

/*
 * 切换到另一台电脑的动作序列：
 * 1) 把光标"藏"到对侧边缘（避免切换瞬间在旧屏幕闪现）
 * 2) 切换活跃输出（set_active_output 会通知对端板）
 * 3) 新机器上的虚拟光标从对应边缘滑入，Y 按屏幕比例缩放
 */
void switch_to_another_pc(
    device_t *state, output_t *output, int output_to, int direction) {
    uint8_t *mouse_park_pos = &state->config.output[state->active_output].mouse_park_pos;

    int16_t mouse_y = (*mouse_park_pos == 0) ? MIN_SCREEN_COORD : /* Top */
                      (*mouse_park_pos == 1) ? MAX_SCREEN_COORD : /* Bottom */
                                               state->pointer_y;  /* Previous */

    mouse_report_t hidden_pointer = {.y = mouse_y, .x = MAX_SCREEN_COORD};   /* 先把光标停到边缘停泊位 */

    output_mouse_report(&hidden_pointer, state);
    set_active_output(state, output_to);   /* 真正的切换点：此后键鼠数据流向另一台电脑 */
    state->pointer_x = (direction == LEFT) ? MAX_SCREEN_COORD : MIN_SCREEN_COORD;
    state->pointer_y = scale_y_coordinate(output->number, 1 - output->number, state);
}

/* macOS 虚拟桌面切换 workaround：macOS 不认绝对坐标跳屏，要用"贴边+相对位移"哄过去 */
void switch_virtual_desktop_macos(device_t *state, int direction) {
    /*
     * Fix for MACOS: Before sending new absolute report setting X to 0:
     * 1. Move the cursor to the edge of the screen directly in the middle to handle screens
     *    of different heights
     * 2. Send relative mouse movement one or two pixels in the direction of movement to get
     *    the cursor onto the next screen
     */
    mouse_report_t edge_position = {
        .x = (direction == LEFT) ? MIN_SCREEN_COORD : MAX_SCREEN_COORD,
        .y = MAX_SCREEN_COORD / 2,
        .mode = ABSOLUTE,
        .buttons = state->mouse_buttons,
    };

    uint16_t move = (direction == LEFT) ? -MACOS_SWITCH_MOVE_X : MACOS_SWITCH_MOVE_X;
    mouse_report_t move_relative_one = {
        .x = move,
        .mode = RELATIVE,
        /* Force buttons to 0 for relative movement to avoid duplicating the button 
           press state, which would leave the relative HID mouse permanently stuck 
           down if the user is dragging an item while switching desktops. */
        .buttons = 0,
    };

    output_mouse_report(&edge_position, state);

    /* Once doesn't seem reliable enough, do it a few times */
    for (int i = 0; i < MACOS_SWITCH_MOVE_COUNT; i++)
        output_mouse_report(&move_relative_one, state);
}

/* 本机多屏（虚拟桌面）时的切换：按 OS 分派，最后把虚拟光标放到新桌面入口边缘 */
void switch_virtual_desktop(device_t *state, output_t *output, int new_index, int direction) {
    switch (output->os) {
        case MACOS:
            switch_virtual_desktop_macos(state, direction);
            break;

        case WINDOWS:
            /* TODO: Switch to relative-only if index > 1, but keep tabs to switch back */
            state->relative_mouse = (new_index > 1);
            break;

        case LINUX:
        case ANDROID:
        case OTHER:
            /* Linux should treat all desktops as a single virtual screen, so you should leave
            screen_count at 1 and it should just work */
            break;
    }

    state->pointer_x       = (direction == RIGHT) ? MIN_SCREEN_COORD : MAX_SCREEN_COORD;
    output->screen_index = new_index;
}

/*                               BORDER
                                   |
       .---------.    .---------.  |  .---------.    .---------.    .---------.
      ||    B2   ||  ||    B1   || | ||    A1   ||  ||    A2   ||  ||    A3   ||   (output, index)
      ||  extra  ||  ||   main  || | ||   main  ||  ||  extra  ||  ||  extra  ||   (main or extra)
       '---------'    '---------'  |  '---------'    '---------'    '---------'
          )___(          )___(     |     )___(          )___(          )___(
*/
/*
 * 切换决策树（精读）：
 *   向对侧方向移动？
 *     ├─ 在主屏(screen_index=1) → 跨机切换（按住鼠标键时不切，防拖拽误触）
 *     └─ 在副屏 → 先切回上一虚拟桌面（一步步退回主屏）
 *   向背离对侧方向移动？→ 只要还有副屏就前进到下一个虚拟桌面
 * switch_lock/gaming_mode 直接冻结一切切换。
 */
void do_screen_switch(device_t *state, int direction) {
    output_t *output = &state->config.output[state->active_output];

    /* No switching allowed if explicitly disabled or in gaming mode */
    if (state->switch_lock || state->gaming_mode)
        return;

    /* We want to jump in the direction of the other computer */
    if (output->pos != direction) {
        if (output->screen_index == 1) { /* We are at the border -> switch outputs */
            /* No switching allowed if mouse button is held. Should only apply to the border! */
            if (state->mouse_buttons)
                return;

            switch_to_another_pc(state, output, 1 - state->active_output, direction);
        }
        /* If here, this output has multiple desktops and we are not on the main one */
        else
            switch_virtual_desktop(state, output, output->screen_index - 1, direction);
    }

    /* We want to jump away from the other computer, only possible if there is another screen to jump to */
    else if (output->screen_index < output->screen_count)
        switch_virtual_desktop(state, output, output->screen_index + 1, direction);
}

/* 从原始报文抠一个字段（处理 report_id 偏移），失败返回 false */
static inline bool extract_value(bool uses_id, int32_t *dst, report_val_t *src, uint8_t *raw_report, int len) {
    /* The report must contain at least one payload byte. */
    if (len <= uses_id)
        return false;

    /* If the report uses an ID, the first byte must match the expected report ID */
    if (uses_id && *raw_report != src->report_id)
        return false;

    /* Extract the value, adjust pointer and length if report ID is used */
    *dst = get_report_value(raw_report + uses_id, len - uses_id, src);
    return true;
}

/* 抠出鼠标五元组 [x,y,wheel,pan,buttons]：boot 协议按固定布局，report 协议按描述符偏移 */
void extract_report_values(uint8_t *raw_report, int len, device_t *state, mouse_values_t *values, hid_interface_t *iface) {
    /* Interpret values depending on the current protocol used. */
    if (iface->protocol == HID_PROTOCOL_BOOT) {
        hid_mouse_report_t *mouse_report = (hid_mouse_report_t *)raw_report;

        values->move_x  = mouse_report->x;
        values->move_y  = mouse_report->y;
        values->wheel   = mouse_report->wheel;
        values->pan     = mouse_report->pan;
        values->buttons = mouse_report->buttons;
        return;
    }
    mouse_t *mouse = &iface->mouse;
    bool uses_id = iface->uses_report_id;

    extract_value(uses_id, &values->move_x, &mouse->move_x, raw_report, len);
    extract_value(uses_id, &values->move_y, &mouse->move_y, raw_report, len);
    extract_value(uses_id, &values->wheel, &mouse->wheel, raw_report, len);
    extract_value(uses_id, &values->pan, &mouse->pan, raw_report, len);

    if (!extract_value(uses_id, &values->buttons, &mouse->buttons, raw_report, len)) {
        values->buttons = state->mouse_buttons;
    }
}

/* 组上报报文：默认绝对坐标模式；Windows 多桌面/游戏模式下退回相对模式（兼容性更好） */
mouse_report_t create_mouse_report(device_t *state, mouse_values_t *values) {
    mouse_report_t mouse_report = {
        .buttons = values->buttons,
        .x       = state->pointer_x,
        .y       = state->pointer_y,
        .wheel   = values->wheel,
        .pan     = values->pan,
        .mode    = ABSOLUTE,
    };

    /* Workaround for Windows multiple desktops */
    if (state->relative_mouse || state->gaming_mode) {
        mouse_report.x = values->move_x;
        mouse_report.y = values->move_y;
        mouse_report.mode = RELATIVE;
    }

    return mouse_report;
}

/*
 * 鼠标报文处理主入口（由 hid_report.c 的分发表调用）。
 * 全零报文直接丢弃——防止某些键盘（QMK）附带鼠标接口时发出干扰性绝对坐标。
 */
void process_mouse_report(uint8_t *raw_report, int len, uint8_t itf, hid_interface_t *iface) {
    mouse_values_t values = {0};
    device_t *state = &global_state;

    /* Interpret the mouse HID report, extract and save values we need. */
    extract_report_values(raw_report, len, state, &values, iface);

    /* If nothing changed, don't send a report. This prevents composite keyboards
       (e.g. QMK) that expose a mouse HID interface from generating spurious
       absolute position reports when they send zero-movement mouse reports during
       keyboard events. */
    if (values.move_x == 0 && values.move_y == 0 &&
        values.wheel == 0 && values.pan == 0 &&
        values.buttons == state->mouse_buttons) {
        return;
    }

    /* Calculate and update mouse pointer movement. */
    enum screen_pos_e switch_direction = update_mouse_position(state, &values);

    /* Create the report for the output PC based on the updated values */
    mouse_report_t report = create_mouse_report(state, &values);

    /* Move the mouse, depending where the output is supposed to go */
    output_mouse_report(&report, state);

    /* We use the mouse to switch outputs, if switch_direction is LEFT or RIGHT */
    if (switch_direction != NONE)
        do_screen_switch(state, switch_direction);
}

/* ==================================================== *
 * Mouse Queue Section
 * ==================================================== */

/* 鼠标队列→USB 上报任务（core0，2kHz）：peek→ready 检查→发送→成功才出队 */
void process_mouse_queue_task(device_t *state) {
    mouse_report_t report = {0};

    /* We need to be connected to the host to send messages */
    if (!state->tud_connected)
        return;

    /* Peek first, if there is anything there... */
    if (!queue_try_peek(&state->mouse_queue, &report))
        return;

    /* If we are suspended, let's wake the host up */
    if (tud_suspended())
        tud_remote_wakeup();   /* 电脑休眠时来鼠标数据 = 远程唤醒信号 */

    /* If it's not ready, we'll try on the next pass */
    if (!tud_hid_n_ready(ITF_NUM_HID))
        return;

    /* If the interface is configured as a keyboard in boot protocol, discard mouse data. */
    if (report.mode == ABSOLUTE && tud_hid_n_get_protocol(ITF_NUM_HID) == HID_PROTOCOL_BOOT) {
        queue_try_remove(&state->mouse_queue, &report);
        return;
    }

    /* Try sending it to the host, if it's successful */
    bool succeeded
        = tud_mouse_report(report.mode, report.buttons, report.x, report.y, report.wheel, report.pan);

    /* ... then we can remove it from the queue */
    if (succeeded)
        queue_try_remove(&state->mouse_queue, &report);
}

/* 入队接口（屏保/切换逻辑都经它上报）；未连接电脑时直接丢弃，防队列积压 */
void queue_mouse_report(mouse_report_t *report, device_t *state) {
    /* It wouldn't be fun to queue up a bunch of messages and then dump them all on host */
    if (!state->tud_connected)
        return;

    queue_try_add(&state->mouse_queue, report);
}
