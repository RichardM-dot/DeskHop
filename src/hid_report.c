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
 * 【中文导读】hid_report.c —— HID 报表描述符解析 + 报文取值（本项目最硬核文件）
 * ----------------------------------------------------------------------------
 * 本文件做什么：
 *   1. USB 键鼠设备会在"报表描述符"里声明"我的数据第几位是什么含义"
 *      （哪个 bit 是左键、X/Y 坐标占哪几 bit、按键码放在哪个字节…）。
 *      parse_report_descriptor（hid_parser.c）逐条描述符解析后，把每条
 *      回调到本文件的 handle_xxx_values / extract_data，按用途(usage)分类
 *      存进 iface->mouse / keyboard / consumer / system 的字段记录里。
 *   2. 真实键鼠上报的原始 HID 报文到达后，用之前记录的位偏移把数值抠出来：
 *      get_report_value() 按位取值、extract_kbd_data() 三路提取键盘数据。
 *
 * 核心概念（读代码前先建立）：
 *   - usage page/usage：HID 的"语义编号"，如 0x09/0x01=鼠标、键盘页=0x07
 *   - VARIABLE（位域，如 NKRO 每个 bit 一个键） vs ARRAY（字节里放键码本身）
 *   - report_id：同一接口可能复用多个报表，首字节区分类型
 *   - NKRO：全键无冲键盘，位图式报键，本文件把它"降维"成标准 6KRO 报文转发
 * ============================================================================
 */
#include "hid_report.h"
#include "main.h"

/*
 * 从 HID 报文按"位偏移+位宽"取出一个字段值（HID 是位打包的，不是字节对齐）。
 * 例：X 坐标声明在 offset=16、size=12 —— 要跨 2 个字节拼出 12 位再符号扩展。
 */
/* Given a value struct with size and offset in bits, find and return a value from the HID report */
int32_t get_report_value(uint8_t *report, int len, report_val_t *val) {
    /* Calculate the bit offset within the byte */
    uint16_t offset_in_bits = val->offset % 8;

    /* Calculate the remaining bits in the first byte */
    uint16_t remaining_bits = 8 - offset_in_bits;

    /* Calculate the byte offset in the array */
    uint16_t byte_offset = val->offset >> 3;

    if (byte_offset >= len)
        return 0;

    /* Create a mask for the specified number of bits */
    uint32_t mask = (1u << val->size) - 1;

    /* Initialize the result value with the bits from the first byte */
    int32_t result = report[byte_offset] >> offset_in_bits;

    /* Move to the next byte and continue fetching bits until the desired length is reached */
    while (val->size > remaining_bits && byte_offset + 1 < len) {
        result |= report[++byte_offset] << remaining_bits;
        remaining_bits += 8;
    }

    /* Apply the mask to retain only the desired number of bits */
    result = result & mask;   /* 只保留字段声明的位宽，多余高位清零 */

    /* Special case if our result is negative.
       Check if the most significant bit of 'val' is set */
    /* 符号扩展：字段最高位为 1 即负数（鼠标向左/上移动），高 32 位补 1 */
    if (result & ((mask >> 1) + 1)) {
        /* If it is set, sign-extend 'val' by filling the higher bits with 1s */
        result |= (0xFFFFFFFFU << val->size);
    }

    return result;
}

/* 描述符解析回调：多媒体键（音量/播放键，consumer page）字段的登记处理。
 * VARIABLE 型逐位记录 usage 到 cc_array；ARRAY 型整字段记一个 usage。 */
/* After processing the descriptor, assign the values so we can later use them to interpret reports */
void handle_consumer_control_values(report_val_t *src, report_val_t *dst, hid_interface_t *iface) {
    keyboard_t *keyboard = get_keyboard(iface, src->report_id);

    if (src->offset >= MAX_CC_BUTTONS) {
        return;
    }

    if (src->data_type == VARIABLE) {
        keyboard->cc_array[src->offset] = src->usage;
        iface->consumer.is_variable = true;
    }

    iface->consumer.is_array |= (src->data_type == ARRAY);
}

/* 描述符解析回调：系统控制键（电源/休眠键）字段的登记处理，逻辑同上 */
/* After processing the descriptor, assign the values so we can later use them to interpret reports */
void handle_system_control_values(report_val_t *src, report_val_t *dst, hid_interface_t *iface) {
    keyboard_t *keyboard = get_keyboard(iface, src->report_id);

    if (src->offset >= MAX_SYS_BUTTONS) {
        return;
    }

    if (src->data_type == VARIABLE) {
        keyboard->sys_array[src->offset] = src->usage;
        iface->system.is_variable = true;
    }

    iface->system.is_array |= (src->data_type == ARRAY);
}

/* 判断是否修饰键字段：修饰键 = 0xE0~0xE7（左Ctrl..右GUI）共 8 个 usage 的位域 */
static bool is_modifier_descriptor(const report_val_t *value) {
    const int left_ctrl_usage = 0xE0;   /* USB 键盘规范：左Ctrl 是 0xE0，8 个修饰键连排 */

    return value->size <= MODIFIER_BIT_LENGTH
           && left_ctrl_usage >= value->usage_min
           && left_ctrl_usage <= value->usage_max;
}

/* NKRO key field maps one usage to each bit. Sometimes keyboards are dumb, so allow for some slack */
static bool is_nkro_key_field(const report_val_t *value)
{
    const int64_t usage_count = (int64_t)value->usage_max - value->usage_min + 1;
    const int64_t key_bits = value->size;

    /* A NKRO key field must be some non-trivial usage range. */
    if (value->usage_max <= value->usage_min)
        return false;
    
    /* Every key bit must have a corresponding usage. */
    if (usage_count < key_bits)
        return false;

    /* The happy path - exactly one key usage per bit. */
    if (usage_count == key_bits)
        return true;

    /* Tolerate extra declared usages only for plausible NKRO fields. */
    return key_bits >= NKRO_MIN_BITS;
}

static void store_modifier(keyboard_t *keyboard, const report_val_t *value) {
    if (is_modifier_descriptor(value) && value->data_type == VARIABLE)
        keyboard->modifier = *value;
}

/* An array field contains one of several possible usages, so its report position
   contains a keycode that can be copied into the 6KRO keyboard report. */
static void store_key_field(keyboard_t *keyboard, const report_val_t *value) {
    if (value->offset_idx < MAX_KEYS)
        keyboard->key_array[value->offset_idx] = value->data_type == ARRAY;
}

static void store_nkro_block(
    keyboard_t *keyboard, const report_val_t *value, bool is_modifier) {
    /* Modifier bits are stored separately, not as ordinary NKRO keys. */
    if (is_modifier)
        return;

    /* NKRO uses variable fields: one bit represents one key. */
    if (value->data_type != VARIABLE)
        return;

    /* An NKRO bitmap must contain one consecutive usage for every bit. */
    if (!is_nkro_key_field(value))
        return;

    /* Prevent overflowing available storage for NKRO blocks. */
    if (keyboard->nkro_count >= MAX_NKRO_BLOCKS)
        return;

    keyboard->nkro[keyboard->nkro_count++] = (nkro_block_t){
        .offset_bits = value->offset,
        .size_bits   = value->size,
        .usage_min = value->usage_min,
        .usage_max = value->usage_max,
    };
    keyboard->nkro_bit_count += value->size;
    keyboard->is_nkro = keyboard->nkro_bit_count > NKRO_MIN_BITS;
}

/*
 * 键盘字段登记主入口：修饰键位域 / ARRAY 键码块 / NKRO 位图块 三种各存各处。
 * 解析阶段每碰到键盘页的描述符就调到这里，把"以后怎么解报文"的信息备齐。
 */
/* Store descriptor values so they can later be used to interpret reports. */
void handle_keyboard_descriptor_values(report_val_t *src, report_val_t *dst, hid_interface_t *iface) {
    /* Parse time: an unseen report ID claims its own keyboard_t. */
    keyboard_t *keyboard = get_or_add_keyboard(iface, src->report_id);

    /* Constants are normally used for padding, so skip'em */
    if (src->item_type == CONSTANT)
        return;

    /* Prevent overwriting more memory than we have */
    if (iface->num_keyboards >= MAX_KEYBOARDS)
        return;

    bool is_modifier = is_modifier_descriptor(src);

    store_modifier(keyboard, src);

    store_key_field(keyboard, src);
    store_nkro_block(keyboard, src, is_modifier);

    /* We found a keyboard on this interface for a specific report id. */
    if (!keyboard->is_found) {
        keyboard->is_found = true;
        iface->num_keyboards++;
    }
}

/* 鼠标按键字段登记：CONSTANT 位是填充位直接并进位宽，其他记录整个字段 */
void handle_buttons(report_val_t *src, report_val_t *dst, hid_interface_t *iface) {
    /* Constant is normally used for padding with mouse buttons, aggregate to simplify things */
    if (src->item_type == CONSTANT) {
        iface->mouse.buttons.size += src->size;
        return;
    }

    iface->mouse.buttons = *src;

    /* We found a mouse on this interface. */
    iface->mouse.is_found = true;
}

void _store(report_val_t *src, report_val_t *dst, hid_interface_t *iface) {
    if (src->item_type != CONSTANT)
        *dst = *src;
}

static uint8_t *get_mouse_id(hid_interface_t *iface) {
    return &iface->mouse.report_id;
}

static uint8_t *get_consumer_id(hid_interface_t *iface) {
    return &iface->consumer.report_id;
}

static uint8_t *get_system_id(hid_interface_t *iface) {
    return &iface->system.report_id;
}

/* 报表 ID → 处理函数 的分发表：报文到达后按此决定交给哪个 process_xxx_report */
const process_report_f report_receivers[] = {
    [REPORT_RECEIVER_NONE]     = NULL,
    [REPORT_RECEIVER_MOUSE]    = process_mouse_report,
    [REPORT_RECEIVER_KEYBOARD] = process_keyboard_report,
    [REPORT_RECEIVER_CONSUMER] = process_consumer_report,
    [REPORT_RECEIVER_SYSTEM]   = process_system_report,
};

/*
 * 描述符解析的总路由表 + 匹配循环。
 * map[] 列出本固件关心的全部 usage 组合（鼠标按键/XY/滚轮/PAN、键盘、
 * 多媒体、系统控制）。每解析出一条描述符值 val，就在表里找匹配项，
 * 命中则：记录 report_id → 调 handler 登记 → 给该 report_id 指定接收器。
 * 三个 match 条件里"任一项为 0"表示通配。
 */
void extract_data(hid_interface_t *iface, report_val_t *val) {
    const usage_map_t map[] = {
        {.usage_page   = HID_USAGE_PAGE_BUTTON,
         .global_usage = HID_USAGE_DESKTOP_MOUSE,
         .handler      = handle_buttons,
         .receiver_id  = REPORT_RECEIVER_MOUSE,
         .dst          = &iface->mouse.buttons,
         .get_id       = get_mouse_id},

        {.usage_page   = HID_USAGE_PAGE_DESKTOP,
         .global_usage = HID_USAGE_DESKTOP_MOUSE,
         .usage        = HID_USAGE_DESKTOP_X,
         .handler      = _store,
         .receiver_id  = REPORT_RECEIVER_MOUSE,
         .dst          = &iface->mouse.move_x,
         .get_id       = get_mouse_id},

        {.usage_page   = HID_USAGE_PAGE_DESKTOP,
         .global_usage = HID_USAGE_DESKTOP_MOUSE,
         .usage        = HID_USAGE_DESKTOP_Y,
         .handler      = _store,
         .receiver_id  = REPORT_RECEIVER_MOUSE,
         .dst          = &iface->mouse.move_y,
         .get_id       = get_mouse_id},

        {.usage_page   = HID_USAGE_PAGE_DESKTOP,
         .global_usage = HID_USAGE_DESKTOP_MOUSE,
         .usage        = HID_USAGE_DESKTOP_WHEEL,
         .handler      = _store,
         .receiver_id  = REPORT_RECEIVER_MOUSE,
         .dst          = &iface->mouse.wheel,
         .get_id       = get_mouse_id},

        {.usage_page   = HID_USAGE_PAGE_CONSUMER,
         .global_usage = HID_USAGE_DESKTOP_MOUSE,
         .usage        = HID_USAGE_CONSUMER_AC_PAN,
         .handler      = _store,
         .receiver_id  = REPORT_RECEIVER_MOUSE,
         .dst          = &iface->mouse.pan,
         .get_id       = get_mouse_id},

        {.usage_page   = HID_USAGE_PAGE_KEYBOARD,
         .global_usage = HID_USAGE_DESKTOP_KEYBOARD,
         .handler      = handle_keyboard_descriptor_values,
         .receiver_id  = REPORT_RECEIVER_KEYBOARD},

        {.usage_page   = HID_USAGE_PAGE_CONSUMER,
         .global_usage = HID_USAGE_CONSUMER_CONTROL,
         .handler      = handle_consumer_control_values,
         .receiver_id  = REPORT_RECEIVER_CONSUMER,
         .dst          = &iface->consumer.val,
         .get_id       = get_consumer_id},

        {.usage_page   = HID_USAGE_PAGE_DESKTOP,
         .global_usage = HID_USAGE_DESKTOP_SYSTEM_CONTROL,
         .handler      = _store,
         .receiver_id  = REPORT_RECEIVER_SYSTEM,
         .dst          = &iface->system.val,
         .get_id       = get_system_id},
    };

    /* We extracted all we could find in the descriptor to report_values, now go through them and
       match them up with the values in the table above, then store those values for later reference */

    for (const usage_map_t *hay = map; hay != &map[ARRAY_SIZE(map)]; hay++) {
        /* ---> If any condition is not defined, we consider it as matched <--- */
        bool global_usages_match = (val->global_usage == hay->global_usage) || (hay->global_usage == 0);
        bool usages_match        = (val->usage == hay->usage) || (hay->usage == 0);
        bool usage_pages_match   = (val->usage_page == hay->usage_page) || (hay->usage_page == 0);

        if (global_usages_match && usages_match && usage_pages_match) {
            /* Keyboards claim their slot in the handler, where the report ID is known. */
            if (hay->get_id != NULL)
                *(hay->get_id(iface)) = val->report_id;

            hay->handler(val, hay->dst, iface);

            iface->report_handler[val->report_id] = hay->receiver_id;
        }
    }
}

/* 从一个 NKRO 位图块提取按下的键：位为 1 → 键码 = usage_min + 位序号 */
/* Append pressed usages from one bitmap block. The report pointer excludes any report ID. */
int32_t extract_bit_variable(
    const nkro_block_t *block, const uint8_t *raw_report, int report_length, uint8_t *dst, int max_keys) {
    int key_count = 0;

    for (int block_bit = 0; block_bit < block->size_bits && key_count < max_keys; block_bit++) {
        int report_bit  = block->offset_bits + block_bit;
        int byte_index  = report_bit >> 3;
        int bit_index   = report_bit & 0b111;

        /* Report is shorter than the descriptor claims, don't read past the end of it */
        if (byte_index >= report_length)
            break;

        if (raw_report[byte_index] & (1 << bit_index)) {
            dst[key_count++] = (uint8_t)(block->usage_min + block_bit);
        }
    }

    return key_count;
}

/* 提取路径1：boot 协议固定 8 字节格式 [修饰键|保留|6×键码]，直接 memcpy */
int32_t _extract_kbd_boot(uint8_t *raw_report, int len, hid_keyboard_report_t *report) {
    uint8_t *src = raw_report;

    /* In case keyboard still uses report ID in this, just pick the last 8 bytes */
    if (len == KBD_REPORT_LENGTH + 1)
        src++;

    memcpy(report, src, KBD_REPORT_LENGTH);
    return KBD_REPORT_LENGTH;
}

/* 提取路径3：非标准格式——按描述符登记的偏移手工抠修饰键与 ARRAY 键码 */
int32_t _extract_kbd_other(uint8_t *raw_report, int len, hid_interface_t *iface, hid_keyboard_report_t *report) {
    keyboard_t *kb = get_keyboard(iface, raw_report[0]);
    uint8_t *src = raw_report;

    if (iface->uses_report_id)
        src++;

    if (kb->modifier.offset_idx >= len - 1)
        return -1;

    report->modifier = src[kb->modifier.offset_idx];

    for (int i=0, j=0; i < TU_MIN(len - 1, MAX_KEYS) && j < KEYS_IN_USB_REPORT; i++) {
        if(kb->key_array[i])
            report->keycode[j++] = src[i];
    }

    return KBD_REPORT_LENGTH;
}

/* 提取路径2：NKRO 位图键盘——逐块扫位图，把按下的键码塞进标准 6KRO 报文 */
int32_t _extract_kbd_nkro(uint8_t *raw_report, int len, hid_interface_t *iface, hid_keyboard_report_t *report) {
    keyboard_t *kb = get_keyboard(iface, raw_report[0]);
    uint8_t *ptr = raw_report;
    int key_count = 0;

    /* Skip report ID */
    if (iface->uses_report_id) {
        ptr++;
        len--;
    }

    if (kb->nkro_count == 0)
        return -1;

    /* We expect modifier to be 8 bits long, otherwise we'll fallback to boot mode */
    if (kb->modifier.size != MODIFIER_BIT_LENGTH || kb->modifier.offset_idx >= len)
        return -1;   /* 修饰键不是标准 8 位，NKRO 路径放弃（调用方会降级） */

    report->modifier = ptr[kb->modifier.offset_idx];

    /* Collect keys from every bitmap block until the outgoing 6KRO report is full */
    for (int i = 0; i < kb->nkro_count && key_count < KEYS_IN_USB_REPORT; i++) {
        key_count += extract_bit_variable(
            &kb->nkro[i], ptr, len, &report->keycode[key_count], KEYS_IN_USB_REPORT - key_count);
    }

    return key_count;
}

/*
 * 键盘报文提取总入口：按协议模式与描述符特征在三条路径间选择——
 * boot 协议 → 直接 memcpy；NKRO 键盘 → 位图提取（失败降级）；
 * 其余 → 按描述符偏移提取。输出统一为标准 8 字节 6KRO 报文。
 */
int32_t extract_kbd_data(
    uint8_t *raw_report, int len, uint8_t itf, hid_interface_t *iface, hid_keyboard_report_t *report) {
    keyboard_t *keyboard = get_keyboard(iface, raw_report[0]);

    /* Clear the report to start fresh */
    memset(report, 0, KBD_REPORT_LENGTH);

    /* If we're in boot protocol mode, then it's easy to decide. */
    if (iface->protocol == HID_PROTOCOL_BOOT)
        return _extract_kbd_boot(raw_report, len, report);

    /* NKRO is a special case. If extraction fails, fall through to other extractors. */
    if (keyboard->is_nkro) {
        int32_t ret = _extract_kbd_nkro(raw_report, len, iface, report);
        if (ret >= 0)
            return ret;
        memset(report, 0, KBD_REPORT_LENGTH);
    }

    /* If we're getting 8 bytes of report, it's safe to assume standard modifier + reserved + keys */
    if (!iface->uses_report_id && (len == KBD_REPORT_LENGTH || len == KBD_REPORT_LENGTH + 1))
        return _extract_kbd_boot(raw_report, len, report);

    /* This is something completely different, look at the report  */
    return _extract_kbd_other(raw_report, len, iface, report);
}
