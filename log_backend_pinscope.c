/* SPDX-License-Identifier: LicenseRef-Pinscope-Device-1.0
 * Copyright (c) 2026 Pinscope. See LICENSE: for use with the Pinscope service only. */
/*
 * A Zephyr logging backend that sends each formatted line to Pinscope, so the
 * dashboard shows the device's log like ESP_LOG output on an ESP32. Runs in
 * the deferred log thread; nothing here logs.
 */
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_backend_std.h>
#include <zephyr/logging/log_output.h>

#include "pinscope.h"

bool ps_zephyr_log_capture(void);

static char s_line[160];
static size_t s_len;

static int out(uint8_t *data, size_t length, void *ctx)
{
    ARG_UNUSED(ctx);
    for (size_t i = 0; i < length; i++) {
        char c = (char)data[i];
        if (c == '\r') {
            continue;
        }
        if (s_len < sizeof(s_line) - 1) {
            s_line[s_len++] = c;
        }
        if (c == '\n' || s_len == sizeof(s_line) - 1) {
            if (s_line[s_len - 1] != '\n') {
                s_line[s_len++] = '\n';
            }
            if (ps_zephyr_log_capture()) {
                pinscope_log(s_line, s_len);
            }
            s_len = 0;
        }
    }
    return (int)length;
}

static uint8_t s_buf[64];
LOG_OUTPUT_DEFINE(s_output, out, s_buf, sizeof(s_buf));

static void process(const struct log_backend *const backend, union log_msg_generic *msg)
{
    ARG_UNUSED(backend);
    log_output_msg_process(&s_output, &msg->log, LOG_OUTPUT_FLAG_LEVEL);
}

static void dropped(const struct log_backend *const backend, uint32_t cnt)
{
    ARG_UNUSED(backend);
    log_output_dropped_process(&s_output, cnt);
}

static void panic(const struct log_backend *const backend)
{
    ARG_UNUSED(backend);
}

static const struct log_backend_api api = {
    .process = process,
    .dropped = dropped,
    .panic = panic,
};

LOG_BACKEND_DEFINE(log_backend_pinscope, api, true);
