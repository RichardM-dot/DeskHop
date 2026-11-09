/*
 * This file is part of DeskHop (https://github.com/hrvach/deskhop).
 * Copyright (c) 2025 Hrvoje Cavrak
 *
 * Based on the TinyUSB HID parser routine and the amazing USB2N64
 * adapter (https://github.com/pdaxrom/usb2n64-adapter)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * See the file LICENSE for the full license text.
 */

/*
 * ============================================================================
 * 【中文导读】hid_parser.c —— HID 报表描述符解析器（逐字节状态机）
 * ----------------------------------------------------------------------------
 * 本文件做什么：
 *   USB 设备的"报表描述符"是一串短指令（字节流），声明报文里每个字段的
 *   位置和含义。本文件按 HID 规范逐条取出描述符条目（item），维护三张表：
 *     - GLOBAL：全局状态（report_id、usage_page、report_size、report_count…），
 *       对后续所有条目持续生效（类似"上下文变量"）
 *     - LOCAL：局部状态（usage 列表），只在下一个 MAIN 条目前有效
 *     - MAIN：真正产生动作的条目（Input/Output/Collection）——到这里才把
 *       "全局+局部"合成一个具体字段，回调 extract_data() 登记（hid_report.c）
 *
 * 描述符字节格式：[1字节头][0/1/2/4字节数据]。
 * 头部 = tag(4b) + type(2b) + size(2b)。size 编码 0/1/2/4 字节。
 * ============================================================================
 */
#include "main.h"

#define IS_BLOCK_END (parser->collection.start == parser->collection.end)

/* 头部 size 字段的 4 种编码 → 实际数据字节数（0/1/2/4） */
enum { SIZE_0_BIT = 0, SIZE_8_BIT = 1, SIZE_16_BIT = 2, SIZE_32_BIT = 3 };
const uint8_t SIZE_LOOKUP[4] = {0, 1, 2, 4};

/* Size is 0, 1, 2, or 3, describing cases of no data, 8-bit, 16-bit,
  or 32-bit data. */
/* 按头部声明的宽度取条目数据值（小端序） */
uint32_t get_descriptor_value(uint8_t const *report, int size) {
    switch (size) {
        case SIZE_8_BIT:
            return report[0];
        case SIZE_16_BIT:
            return tu_u16(report[1], report[0]);
        case SIZE_32_BIT:
            return tu_u32(report[3], report[2], report[1], report[0]);
        default:
            return 0;
    }
}

/* 每个 report_id 一本"位偏移账本"：找不到就开新账页（报文内当前写到第几位） */
uint32_t *get_or_create_report_offset(parser_state_t *parser, uint8_t report_id) {
    for (int i = 0; i < parser->num_report_offsets; i++) {
        if (parser->report_offsets[i].report_id == report_id) {
            return &parser->report_offsets[i].offset_in_bits;
        }
    }

    if (parser->num_report_offsets < MAX_REPORTS_PER_IFACE) {
        parser->report_offsets[parser->num_report_offsets].report_id = report_id;
        parser->report_offsets[parser->num_report_offsets].offset_in_bits = 0;
        return &parser->report_offsets[parser->num_report_offsets++].offset_in_bits;
    }

    return NULL;
}

uint32_t get_current_offset(parser_state_t *parser) {
    uint32_t *offset = get_or_create_report_offset(parser, parser->report_id);
    return offset ? *offset : 0;
}

/* Return the usage for element i. Once the local usage list is exhausted
   the last declared usage repeats (HID spec). */
uint16_t get_usage(parser_state_t *parser, int i) {
    int idx;

    if (parser->usage_count == 0) {
        idx = 0;                      /* use carried/default usage */
    } else if (i >= parser->usage_count) {
        idx = parser->usage_count - 1;
    } else {
        idx = i;
    }

    uint16_t *slot = parser->p_usage + idx;
    if (slot >= parser->usages + HID_MAX_USAGES)
        slot = parser->usages + HID_MAX_USAGES - 1;

    return *slot;
}

/* 把"当前全局+局部状态+位偏移"打包成一个字段描述 report_val_t */
void store_element(parser_state_t *parser, report_val_t *val, uint16_t usage, uint32_t data, uint16_t size, hid_interface_t *iface) {
    uint32_t current_offset = get_current_offset(parser);

    *val = (report_val_t){
        .offset     = current_offset,
        .offset_idx = current_offset >> 3,
        .size       = size,

        .usage_max = parser->locals[RI_LOCAL_USAGE_MAX].val,
        .usage_min = parser->locals[RI_LOCAL_USAGE_MIN].val,

        .item_type   = (data & 0x01) ? CONSTANT : DATA,
        .data_type   = (data & 0x02) ? VARIABLE : ARRAY,

        .usage        = usage,
        .usage_page   = parser->globals[RI_GLOBAL_USAGE_PAGE].val,
        .global_usage = parser->global_usage,
        .report_id    = parser->report_id
    };

    iface->uses_report_id |= (parser->report_id != 0);
}

void handle_global_item(parser_state_t *parser, item_t *item) {
    if (item->hdr.tag == RI_GLOBAL_REPORT_ID) {
        parser->report_id = item->val;
    }

    parser->globals[item->hdr.tag] = *item;
}

void handle_local_item(parser_state_t *parser, item_t *item) {
    /* There are just 16 possible tags, store any one that comes along to an array
        instead of doing switch and 16 cases */
    parser->locals[item->hdr.tag] = *item;

    if (item->hdr.tag == RI_LOCAL_USAGE) {
        if(IS_BLOCK_END)
            parser->global_usage = item->val;

        else if (parser->p_usage + parser->usage_count + 1 < parser->usages + HID_MAX_USAGES)
            *(parser->p_usage + parser->usage_count++) = item->val;
    }
}

/*
 * MAIN·Input 条目处理（解析器核心）：描述符在这里声明"报文里有一块数据"。
 * 字段总位宽 = report_size × report_count；逐字段登记并推进位偏移。
 * size=1 且无 usage 队列时交换 size/count——240 个 1 位键（NKRO）一次处理。
 */
void handle_main_input(parser_state_t *parser, item_t *item, hid_interface_t *iface) {
    uint32_t size  = parser->globals[RI_GLOBAL_REPORT_SIZE].val;
    uint32_t count = parser->globals[RI_GLOBAL_REPORT_COUNT].val;
    report_val_t val = {0};

    /* Swap count and size for 1-bit variables, it makes sense to process e.g. NKRO with
       size = 1 and count = 240 in one go instead of doing 240 iterations
       Don't do this if there are usages in the queue, though.
       */
    if (size == 1 && parser->usage_count <= 1) {
        size  = count;
        count = 1;
    }

    uint32_t *current_offset = get_or_create_report_offset(parser, parser->report_id);
    if (!current_offset)
        return;

    for (int i = 0; i < count; i++) {
        store_element(parser, &val, get_usage(parser, i), item->val, size, iface);

        /* Use the parsed data to populate internal device structures */
        extract_data(iface, &val);

        /* Iterate <count> times and increase offset by <size> amount, moving by <count> x <size> bits */
        *current_offset += size;
    }

    /* If no usages were declared for this main item, nothing to carry. */
    if (parser->usage_count == 0)
        return;   /* HID 规范：usage 用完后最后一个继续沿用（游标搬运实现它） */

    /* Advance the usage cursor and carry the last usage of this block.
       Pin to the last slot if the array is full. */
    if (parser->p_usage + parser->usage_count < parser->usages + HID_MAX_USAGES) {
        parser->p_usage += parser->usage_count;

        /* Carry the last usage of this block to the new location */
        *parser->p_usage = *(parser->p_usage - 1);
    } else {
        parser->p_usage = parser->usages + HID_MAX_USAGES - 1;
    }
}

/* MAIN 条目分发：Collection 只数嵌套深度；Input 才是数据字段；处理完清 LOCAL 态 */
void handle_main_item(parser_state_t *parser, item_t *item, hid_interface_t *iface) {
    switch (item->hdr.tag) {
        case RI_MAIN_COLLECTION:
            parser->collection.start++;
            break;

        case RI_MAIN_COLLECTION_END:
            parser->collection.end++;
            break;

        case RI_MAIN_INPUT:
            handle_main_input(parser, item, iface);
            break;
    }

    parser->usage_count = 0;

    /* Local items do not carry over to the next Main item (HID spec v1.11, section 6.2.2.8) */
    memset(parser->locals, 0, sizeof(parser->locals));
}


/* This method is sub-optimal and far from a generalized HID descriptor parsing, but should
 * hopefully work well enough to find the basic values we care about to move the mouse around.
 * Your descriptor for a mouse with 2 wheels and 264 buttons might not parse correctly.
 * */
/* 解析器工作状态太大（数 KB），放静态区而不是栈上 */
parser_state_t parser_state = {0};  // Avoid placing it on the stack, it's large

/*
 * 描述符解析主循环：逐条目 [头][数据] 消费字节流，按类型分发给
 * MAIN/GLOBAL/LOCAL 三个处理器。解析结果全部经 extract_data() 落到 iface。
 */
void parse_report_descriptor(hid_interface_t *iface,
                            uint8_t const *report,
                            int desc_len
                            ) {
    item_t item = {0};

    /* Wipe parser_state clean */
    memset(&parser_state, 0, sizeof(parser_state_t));
    parser_state.p_usage = parser_state.usages;

    while (desc_len > 0) {
        item.hdr = *(header_t *)report++;
        item.val = get_descriptor_value(report, item.hdr.size);

        switch (item.hdr.type) {
            case RI_TYPE_MAIN:
                handle_main_item(&parser_state, &item, iface);
                break;

            case RI_TYPE_GLOBAL:
                handle_global_item(&parser_state, &item);
                break;

            case RI_TYPE_LOCAL:
                handle_local_item(&parser_state, &item);
                break;
        }
        /* Move to the next position and decrement size by header length + data length */
        report += SIZE_LOOKUP[item.hdr.size];
        desc_len -= (SIZE_LOOKUP[item.hdr.size] + 1);
    }
}
