# Copyright (c) 2024 The ZMK Contributors
# SPDX-License-Identifier: MIT

target_sources_ifdef(
    CONFIG_ZMK_INPUT_PROCESSOR_VOLUME
    app
    PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src/input_processor_volume.c
)
