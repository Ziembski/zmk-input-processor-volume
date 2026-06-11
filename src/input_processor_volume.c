/*
 * Copyright (c) 2024 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * input_processor_volume.c
 *
 * Input processor that converts continuous REL_Y (or REL_X) trackball motion
 * into C_VOL_UP / C_VOL_DN key presses.
 *
 * Design mirrors input_processor_scaler.c (remainder accumulation) combined
 * with input_processor_behaviors.c (zmk_behavior_invoke_binding dispatch).
 *
 * A signed accumulator absorbs every incoming delta.  Each time it crosses
 * ±tick the processor fires the appropriate binding and subtracts the
 * threshold, preserving the remainder for the next event.
 */

#define DT_DRV_COMPAT zmk_input_processor_volume

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <drivers/input_processor.h>
#include <zmk/behavior.h>
#include <zmk/keymap.h>
#include <zmk/virtual_key_position.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* ── Config (from DT, set at build time) ─────────────────────────────────── */
struct volume_config {
    uint8_t  index;           /* processor instance index for virtual key pos  */
    uint16_t type;            /* INPUT_EV_REL                                   */
    uint16_t y_input_code;    /* e.g. INPUT_REL_Y                               */
    int32_t  tick;            /* counts to accumulate before one vol step       */
    bool     y_invert;        /* flip axis direction                             */

    /* bindings[0] = vol-up binding                                              */
    /* bindings[1] = vol-dn binding                                              */
    const struct zmk_behavior_binding *bindings;
};

/* ── Runtime data (mutable per instance) ─────────────────────────────────── */
struct volume_data {
    int32_t remainder;        /* leftover counts after last threshold crossing  */
};

/* ── Helper: invoke a binding as a momentary press + release ────────────── */
static int fire_binding(const struct zmk_behavior_binding *binding,
                        const struct volume_config *cfg,
                        struct zmk_input_processor_state *state) {
    struct zmk_behavior_binding_event ev = {
        .position  = ZMK_VIRTUAL_KEY_POSITION_BEHAVIOR_INPUT_PROCESSOR(
                         state->input_device_index, cfg->index),
        .timestamp = k_uptime_get(),
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source    = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };

    /* zmk_behavior_invoke_binding third arg is bool pressed */
    int ret = zmk_behavior_invoke_binding(binding, ev, true);
    if (ret < 0) {
        return ret;
    }
    return zmk_behavior_invoke_binding(binding, ev, false);
}

/* ── Main event handler ──────────────────────────────────────────────────── */
static int volume_handle_event(const struct device *dev,
                               struct input_event *event,
                               uint32_t param1,    /* unused */
                               uint32_t param2,    /* unused */
                               struct zmk_input_processor_state *state) {
    const struct volume_config *cfg  = dev->config;
    struct volume_data         *data = dev->data;

    /* Only handle the configured event type and axis code */
    if (event->type != cfg->type || event->code != cfg->y_input_code) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    int32_t delta = cfg->y_invert ? -(int32_t)event->value : (int32_t)event->value;
    data->remainder += delta;

    /* Fire as many steps as the accumulator contains */
    while (data->remainder >= cfg->tick) {
        data->remainder -= cfg->tick;
        int ret = fire_binding(&cfg->bindings[0], cfg, state); /* vol up */
        if (ret < 0) {
            LOG_WRN("vol-up binding failed: %d", ret);
        }
    }
    while (data->remainder <= -cfg->tick) {
        data->remainder += cfg->tick;
        int ret = fire_binding(&cfg->bindings[1], cfg, state); /* vol dn */
        if (ret < 0) {
            LOG_WRN("vol-dn binding failed: %d", ret);
        }
    }

    /*
     * Stop propagation — prevents the raw REL_Y event from also moving
     * the cursor while the volume layer is active.
     */
    return ZMK_INPUT_PROC_STOP;
}

/* ── Driver API ──────────────────────────────────────────────────────────── */
static const struct zmk_input_processor_driver_api volume_driver_api = {
    .handle_event = volume_handle_event,
};

static int volume_init(const struct device *dev) {
    struct volume_data *data = dev->data;
    data->remainder = 0;
    return 0;
}

/* ── Per-instance instantiation macro ───────────────────────────────────── */
#define VOLUME_INST(n)                                                          \
    static struct volume_data volume_data_##n = {};                             \
                                                                                \
    static const struct zmk_behavior_binding volume_bindings_##n[] = {         \
        ZMK_KEYMAP_EXTRACT_BINDING(0, DT_DRV_INST(n)),  /* vol up */           \
        ZMK_KEYMAP_EXTRACT_BINDING(1, DT_DRV_INST(n)),  /* vol dn */           \
    };                                                                          \
                                                                                \
    static const struct volume_config volume_config_##n = {                    \
        .index        = n,                                                      \
        .type         = DT_INST_PROP_OR(n, type, INPUT_EV_REL),                \
        .y_input_code = DT_INST_PROP_OR(n, y_input_code, INPUT_REL_Y),         \
        .tick         = DT_INST_PROP_OR(n, tick, 30),                          \
        .y_invert     = DT_INST_PROP(n, y_invert),                             \
        .bindings     = volume_bindings_##n,                                    \
    };                                                                          \
                                                                                \
    DEVICE_DT_INST_DEFINE(n, volume_init, NULL,                                 \
                          &volume_data_##n, &volume_config_##n,                 \
                          POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,     \
                          &volume_driver_api);

DT_INST_FOREACH_STATUS_OKAY(VOLUME_INST)
