/*
 * ps5-native-app-boilerplate - ProsperoLight component.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RADIO_INPUT_HPP
#define RADIO_INPUT_HPP

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RADIO_INPUT_CROSS,
    RADIO_INPUT_CIRCLE,
    RADIO_INPUT_SQUARE,
    RADIO_INPUT_TRIANGLE,
    RADIO_INPUT_OPTIONS,
    RADIO_INPUT_L1,
    RADIO_INPUT_R1,
    RADIO_INPUT_UP,
    RADIO_INPUT_DOWN,
    RADIO_INPUT_LEFT,
    RADIO_INPUT_RIGHT,
    RADIO_INPUT_COUNT
} radio_input_key_t;

typedef struct {
    radio_input_key_t key;
    bool pressed;
} radio_input_event_t;

#define RADIO_INPUT_TOUCH_CAPACITY 2U

typedef struct {
    uint16_t x;
    uint16_t y;
    int8_t id;
} radio_input_touch_t;

typedef struct {
    uint32_t buttons;
    uint8_t left_x, left_y, right_x, right_y;
    uint8_t left_trigger, right_trigger;
    uint8_t touch_count;
    radio_input_touch_t touches[RADIO_INPUT_TOUCH_CAPACITY];
    float orientation_x, orientation_y, orientation_z, orientation_w;
    float acceleration_x, acceleration_y, acceleration_z;
    float angular_velocity_x, angular_velocity_y, angular_velocity_z;
    uint64_t timestamp;
    bool connected;
} radio_input_pad_state_t;

bool radio_input_init(void);
void radio_input_poll(void);
bool radio_input_next(radio_input_event_t * event);
bool radio_input_pressed(radio_input_key_t key);
bool radio_input_pad_state(radio_input_pad_state_t *state);
bool radio_input_reset_orientation(void);
bool radio_input_set_vibration(uint8_t low_frequency, uint8_t high_frequency);
bool radio_input_set_haptic_audio(const int16_t *samples, size_t frame_count,
                                  uint8_t intensity);
bool radio_input_set_trigger_effects(uint8_t left_type, const uint8_t left[10],
                                     uint8_t right_type, const uint8_t right[10],
                                     uint8_t intensity);
bool radio_input_clear_trigger_effects(void);
void radio_input_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
