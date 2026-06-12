/*
 * Copyright (c) 2024 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * input_processor_volume.c
 *
 * Converts REL_Y trackball motion into C_VOL_UP / C_VOL_DN, while
 * suppressing cursor movement — without needing a separate zeroing processor.
 *
 * Design mirrors zip_xy_to_scroll_mapper (input_processor_code_mapper.c):
 * instead of stopping the event or zeroing it, we remap event->code to
 * INPUT_REL_MISC after consuming the value. handle_rel_code() in the input
 * listener has no case for INPUT_REL_MISC, so it silently ignores the event —
 * no cursor movement, no scroll, nothing. Clean and stateless.
 *
 * REL_X is similarly remapped to INPUT_REL_MISC so horizontal movement is
 * also suppressed on the volume layer, with no remainder bleed between layers.
 *
 * Accumulator: signed remainder absorbs every delta. One behavior press+release
 * fires each time it crosses ±tick. At most one step per input event to avoid
 * exhausting ZMK's synchronous event heap.
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

struct volume_config {
    uint8_t  index;
    uint16_t type;
    uint16_t y_input_code;
    int32_t  tick;
    bool     y_invert;
    const struct zmk_behavior_binding *bindings;
};

struct volume_data {
    int32_t remainder;
};

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

    int ret = zmk_behavior_invoke_binding(binding, ev, true);
    if (ret < 0) {
        return ret;
    }
    return zmk_behavior_invoke_binding(binding, ev, false);
}

static int volume_handle_event(const struct device *dev,
                               struct input_event *event,
                               uint32_t param1,
                               uint32_t param2,
                               struct zmk_input_processor_state *state) {
    const struct volume_config *cfg  = dev->config;
    struct volume_data         *data = dev->data;

    if (event->type != cfg->type) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    /* Remap REL_Y: consume value for volume, then remap code to REL_MISC
     * so handle_rel_code() in the listener ignores it (no cursor movement). */
    if (event->code == cfg->y_input_code) {
        int32_t delta = cfg->y_invert ? -(int32_t)event->value : (int32_t)event->value;
        data->remainder += delta;

        /* At most one step per event — ZMK's event manager is synchronous;
         * multiple raise_zmk_keycode_state_changed calls before the heap
         * drains causes a crash. Remainder carries over losslessly. */
        if (data->remainder >= cfg->tick) {
            data->remainder -= cfg->tick;
            int ret = fire_binding(&cfg->bindings[0], cfg, state); /* vol up */
            if (ret < 0) {
                LOG_WRN("vol-up binding failed: %d", ret);
            }
        } else if (data->remainder <= -cfg->tick) {
            data->remainder += cfg->tick;
            int ret = fire_binding(&cfg->bindings[1], cfg, state); /* vol dn */
            if (ret < 0) {
                LOG_WRN("vol-dn binding failed: %d", ret);
            }
        }

        /* Remap to REL_MISC — handle_rel_code has no case for it,
         * so the event is silently dropped without stopping the pipeline. */
        event->code = INPUT_REL_MISC;
        return ZMK_INPUT_PROC_CONTINUE;
    }

    /* Remap REL_X (and any other REL code) to REL_MISC as well,
     * suppressing horizontal cursor movement on the volume layer. */
    if (event->code == INPUT_REL_X) {
        event->code = INPUT_REL_MISC;
        return ZMK_INPUT_PROC_CONTINUE;
    }

    return ZMK_INPUT_PROC_CONTINUE;
}

static const struct zmk_input_processor_driver_api volume_driver_api = {
    .handle_event = volume_handle_event,
};

static int volume_init(const struct device *dev) {
    struct volume_data *data = dev->data;
    data->remainder = 0;
    return 0;
}

#define VOLUME_INST(n)                                                          \
    static struct volume_data volume_data_##n = {};                             \
                                                                                \
    static const struct zmk_behavior_binding volume_bindings_##n[] = {         \
        ZMK_KEYMAP_EXTRACT_BINDING(0, DT_DRV_INST(n)),                         \
        ZMK_KEYMAP_EXTRACT_BINDING(1, DT_DRV_INST(n)),                         \
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
