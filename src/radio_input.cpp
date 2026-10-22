/*
 * ps5-native-app-boilerplate - ProsperoLight component.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "radio_input.hpp"

#include "app_log.hpp"
#include "storage_io.hpp"

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define INPUT_QUEUE_SIZE 64U
#define PAD_SAMPLE_SIZE 120U
#define PAD_SAMPLE_CAPACITY 64
#define PAD_BUTTON_INTERCEPTED UINT32_C(0x80000000)
#define STICK_LOW 64U
#define STICK_HIGH 192U
#define STICK_REPEAT_DELAY_MS UINT64_C(350)
#define STICK_REPEAT_MS UINT64_C(110)

typedef struct
{
    uint32_t button;
    radio_input_key_t key;
} button_map_t;

typedef struct
{
    float x, y, z;
} pad_vector3_t;

typedef struct
{
    float x, y, z, w;
} pad_vector4_t;

typedef struct
{
    uint16_t x, y;
    uint8_t finger;
    uint8_t padding[3];
} pad_touch_t;

typedef struct
{
    uint8_t fingers;
    uint8_t padding[3];
    uint32_t reserved;
    pad_touch_t touches[RADIO_INPUT_TOUCH_CAPACITY];
} pad_touch_data_t;

typedef struct
{
    uint32_t buttons;
    uint8_t left_x, left_y, right_x, right_y;
    uint8_t left_trigger, right_trigger;
    uint16_t padding;
    pad_vector4_t orientation;
    pad_vector3_t acceleration;
    pad_vector3_t angular_velocity;
    pad_touch_data_t touch;
    uint8_t connected;
    uint64_t timestamp;
    uint8_t extension[16];
    uint8_t count;
    uint8_t unknown[15];
} pad_sample_t;

typedef struct
{
    uint8_t low_frequency;
    uint8_t high_frequency;
} pad_vibration_t;

typedef enum
{
    PAD_TRIGGER_EFFECT_OFF = 0,
    PAD_TRIGGER_EFFECT_FEEDBACK = 1,
    PAD_TRIGGER_EFFECT_WEAPON = 2,
    PAD_TRIGGER_EFFECT_VIBRATION = 3,
    PAD_TRIGGER_EFFECT_MULTIPLE_POSITION_FEEDBACK = 4,
    PAD_TRIGGER_EFFECT_SLOPE_FEEDBACK = 5,
    PAD_TRIGGER_EFFECT_MULTIPLE_POSITION_VIBRATION = 6,
} pad_trigger_effect_mode_t;

typedef union
{
    uint8_t raw[48];
    struct
    {
        uint8_t position;
        uint8_t strength;
        uint8_t padding[46];
    } feedback;
    struct
    {
        uint8_t start_position;
        uint8_t end_position;
        uint8_t strength;
        uint8_t padding[45];
    } weapon;
    struct
    {
        uint8_t position;
        uint8_t amplitude;
        uint8_t frequency;
        uint8_t padding[45];
    } vibration;
    struct
    {
        uint8_t strength[10];
        uint8_t padding[38];
    } multiple_feedback;
    struct
    {
        uint8_t start_position;
        uint8_t end_position;
        uint8_t start_strength;
        uint8_t end_strength;
        uint8_t padding[44];
    } slope_feedback;
    struct
    {
        uint8_t frequency;
        uint8_t amplitude[10];
        uint8_t padding[37];
    } multiple_vibration;
} pad_trigger_effect_data_t;

typedef struct
{
    int32_t mode;
    uint8_t padding[4];
    pad_trigger_effect_data_t data;
} pad_trigger_effect_command_t;

typedef struct
{
    uint8_t trigger_mask;
    uint8_t padding[7];
    pad_trigger_effect_command_t command[2];
} pad_trigger_effect_param_t;

static_assert(offsetof(pad_sample_t, orientation) == 12, "unexpected pad orientation offset");
static_assert(offsetof(pad_sample_t, touch) == 52, "unexpected pad touch offset");
static_assert(offsetof(pad_sample_t, connected) == 76, "unexpected pad connection offset");
static_assert(offsetof(pad_sample_t, timestamp) == 80, "unexpected pad timestamp offset");
static_assert(sizeof(pad_sample_t) == PAD_SAMPLE_SIZE, "unexpected pad sample size");
static_assert(sizeof(pad_trigger_effect_command_t) == 56,
              "unexpected pad trigger command size");
static_assert(sizeof(pad_trigger_effect_param_t) == 120,
              "unexpected pad trigger parameter size");

extern "C"
{
    int scePadInit(void);
    extern int scePadOpen(int32_t user_id, int32_t port_type, int32_t index, const void *param);
    extern int scePadClose(int32_t handle);
    extern int scePadRead(int32_t handle, void *data, int32_t num);
    extern int scePadResetOrientation(int32_t handle);
    extern int scePadSetMotionSensorState(int32_t handle, bool enabled);
    extern int scePadSetTriggerEffect(int32_t handle,
                                      const pad_trigger_effect_param_t *effect);
    extern int scePadSetVibration(int32_t handle, const pad_vibration_t *vibration);
    /* Required before adaptive trigger effects work from homebrew titles. */
    extern int scePadSetProcessPrivilege(int32_t privilege);
    extern int sceUserServiceInitialize(void *init_params);
    extern int sceUserServiceGetInitialUser(int32_t *user_id);
    extern int sceUserServiceTerminate(void);
    uint64_t SDL_GetTicks64(void);
}

static const button_map_t buttons[] = {
    {UINT32_C(0x00004000), RADIO_INPUT_CROSS},   {UINT32_C(0x00002000), RADIO_INPUT_CIRCLE},
    {UINT32_C(0x00008000), RADIO_INPUT_SQUARE},  {UINT32_C(0x00001000), RADIO_INPUT_TRIANGLE},
    {UINT32_C(0x00000008), RADIO_INPUT_OPTIONS}, {UINT32_C(0x00000400), RADIO_INPUT_L1},
    {UINT32_C(0x00000800), RADIO_INPUT_R1},      {UINT32_C(0x00000010), RADIO_INPUT_UP},
    {UINT32_C(0x00000040), RADIO_INPUT_DOWN},    {UINT32_C(0x00000080), RADIO_INPUT_LEFT},
    {UINT32_C(0x00000020), RADIO_INPUT_RIGHT},
};

static radio_input_event_t queue[INPUT_QUEUE_SIZE];
static pad_sample_t samples[PAD_SAMPLE_CAPACITY];
static unsigned queue_read;
static unsigned queue_write;
static uint32_t button_state;
static int analog_key = -1;
static uint64_t analog_repeat_at;
static int32_t pad_handle = -1;
static bool owns_user_service;
static radio_input_pad_state_t latest_pad;
static char pad_diag[1024];
static int last_privilege_result = 1;
static int last_trigger_result = 1;
/* Classic rumble and haptic-audio share scePadSetVibration. Keep both and
 * apply the louder motor so quiet haptic frames do not cancel CHIAKI rumble.
 * Classic rumble is latched until an explicit (0,0) — cloud packets are sparse. */
static uint8_t last_rumble_low;
static uint8_t last_rumble_high;
static uint8_t last_haptic_low;
static uint8_t last_haptic_high;
static uint64_t haptic_hold_until_ms;

/* Durable diagnostics — debug.log is circular and trims early pad events. */
static void pad_diag_write(void)
{
#if CLOUDPLAY_PS5
    const char *paths[] = {
        "/download0/pad_diag.txt",
        "/data/cloudplay-pad_diag.txt",
    };
    for (const char *path : paths)
        (void)cloudplay::storage_write_atomic(path, pad_diag);
#endif
}

static void pad_diag_append(const char *key, int64_t value)
{
    const size_t used = strnlen(pad_diag, sizeof(pad_diag));
    if (used + 48 >= sizeof(pad_diag))
        return;
    (void)snprintf(pad_diag + used, sizeof(pad_diag) - used, "%s=%lld\n", key,
                   static_cast<long long>(value));
    pad_diag_write();
}

static uint8_t effect_strength(uint8_t strength, uint8_t intensity)
{
    if (strength == 0 || intensity == 0)
        return 0;
    strength = strength > 8 ? 8 : strength;
    if (intensity == 2)
        return (uint8_t)((strength + 1U) / 2U);
    if (intensity == 3)
        return (uint8_t)((strength + 2U) / 3U);
    return strength;
}

/*
 * DualSense HID trigger modes (controllers.fandom.com / Sony factories):
 *   0x05 Off
 *   0x01 / 0x11 Simple|Limited Feedback
 *   0x02 / 0x12 Simple|Limited Weapon
 *   0x06 Simple Vibration
 *   0x21 Feedback (zone-packed) + MultiplePosition / Slope encodings
 *   0x22 Bow (unofficial)  0x23 Galloping  0x25 Weapon  0x26 Vibration
 *   0x27 Machine (unofficial)
 * scePad MULTIPLE_POSITION_* returns INVALID_ARG on this homebrew path, so
 * zone payloads are approximated with FEEDBACK / WEAPON / VIBRATION / SLOPE.
 */
static void bit_range(uint16_t mask, unsigned *start, unsigned *end, bool *found)
{
    *found = false;
    *start = 0;
    *end = 0;
    for (unsigned i = 0; i < 10; ++i)
    {
        if ((mask & (1U << i)) == 0)
            continue;
        if (!*found)
        {
            *start = i;
            *found = true;
        }
        *end = i;
    }
}

static void set_weapon_clamped(pad_trigger_effect_command_t *command,
                               unsigned start, unsigned end, uint8_t strength)
{
    if (strength == 0)
        return;
    if (end <= start)
        end = start + 1U;
    if (start < 2U)
        start = 2U;
    if (start > 7U)
        start = 7U;
    if (end <= start)
        end = start + 1U;
    if (end > 8U)
        end = 8U;
    command->mode = PAD_TRIGGER_EFFECT_WEAPON;
    command->data.weapon.start_position = (uint8_t)start;
    command->data.weapon.end_position = (uint8_t)end;
    command->data.weapon.strength = strength;
}

static void decode_zone_effect(pad_trigger_effect_command_t *command,
                               const uint8_t data[10], bool vibration,
                               uint8_t intensity)
{
    const uint16_t active = (uint16_t)data[0] | ((uint16_t)data[1] << 8U);
    const uint32_t packed = (uint32_t)data[2] |
        ((uint32_t)data[3] << 8U) | ((uint32_t)data[4] << 16U) |
        ((uint32_t)data[5] << 24U);
    uint8_t strengths[10] = {};
    int first = -1;
    int last = -1;
    uint8_t peak = 0;
    bool uniform = true;
    uint8_t first_strength = 0;
    for (unsigned i = 0; i < 10; ++i)
    {
        if ((active & (1U << i)) == 0)
            continue;
        strengths[i] = effect_strength(
            (uint8_t)(((packed >> (3U * i)) & 7U) + 1U), intensity);
        if (strengths[i] == 0)
            continue;
        if (first < 0)
        {
            first = (int)i;
            first_strength = strengths[i];
        }
        else if (strengths[i] != first_strength)
            uniform = false;
        last = (int)i;
        if (strengths[i] > peak)
            peak = strengths[i];
    }
    if (first < 0 || peak == 0)
        return;

    if (vibration)
    {
        const uint8_t frequency = data[8];
        if (frequency == 0)
            return;
        command->mode = PAD_TRIGGER_EFFECT_VIBRATION;
        command->data.vibration.position = (uint8_t)first;
        command->data.vibration.amplitude = peak;
        command->data.vibration.frequency = frequency;
        return;
    }

    /* Official Feedback fills position..9 at one strength — use FEEDBACK. */
    if (uniform && last == 9)
    {
        command->mode = PAD_TRIGGER_EFFECT_FEEDBACK;
        command->data.feedback.position = (uint8_t)first;
        command->data.feedback.strength = peak;
        return;
    }

    if (last > first)
    {
        uint8_t start_strength = strengths[first] ? strengths[first] : peak;
        uint8_t end_strength = strengths[last] ? strengths[last] : peak;
        if (start_strength < 1)
            start_strength = 1;
        if (end_strength < 1)
            end_strength = 1;
        /* Varying multi-zone: keep peak bite on the stronger end. */
        if (!uniform)
        {
            if (start_strength < peak && end_strength < peak)
            {
                if (strengths[first] >= strengths[last])
                    start_strength = peak;
                else
                    end_strength = peak;
            }
        }
        command->mode = PAD_TRIGGER_EFFECT_SLOPE_FEEDBACK;
        command->data.slope_feedback.start_position = (uint8_t)first;
        command->data.slope_feedback.end_position = (uint8_t)last;
        command->data.slope_feedback.start_strength = start_strength;
        command->data.slope_feedback.end_strength = end_strength;
        return;
    }

    command->mode = PAD_TRIGGER_EFFECT_FEEDBACK;
    command->data.feedback.position = (uint8_t)first;
    command->data.feedback.strength = peak;
}

static void decode_trigger_effect(pad_trigger_effect_command_t *command,
                                  uint8_t type, const uint8_t data[10],
                                  uint8_t intensity)
{
    memset(command, 0, sizeof(*command));
    if (!data || intensity == 0 || type == 0 || type == 0x05)
        return;

    switch (type)
    {
    case 0x21: /* Feedback / MultiplePositionFeedback / SlopeFeedback */
        decode_zone_effect(command, data, false, intensity);
        break;
    case 0x25: /* Weapon (start+stop bit pair) */
    {
        unsigned start = 0;
        unsigned end = 0;
        bool found = false;
        bit_range((uint16_t)data[0] | ((uint16_t)data[1] << 8U),
                  &start, &end, &found);
        if (found)
            set_weapon_clamped(command, start, end,
                               effect_strength((uint8_t)((data[2] & 7U) + 1U),
                                               intensity));
        break;
    }
    case 0x26: /* Vibration / MultiplePositionVibration */
        decode_zone_effect(command, data, true, intensity);
        break;
    case 0x22: /* Bow — snap-back weapon; approximate as Weapon */
    {
        unsigned start = 0;
        unsigned end = 0;
        bool found = false;
        bit_range((uint16_t)data[0] | ((uint16_t)data[1] << 8U),
                  &start, &end, &found);
        const uint8_t strength = effect_strength(
            (uint8_t)(((data[2] >> 0) & 7U) + 1U), intensity);
        const uint8_t snap = effect_strength(
            (uint8_t)(((data[2] >> 3) & 7U) + 1U), intensity);
        if (found)
            set_weapon_clamped(command, start, end,
                               strength > snap ? strength : snap);
        break;
    }
    case 0x23: /* Galloping — rhythmic; approximate as Vibration */
    {
        unsigned start = 0;
        unsigned end = 0;
        bool found = false;
        bit_range((uint16_t)data[0] | ((uint16_t)data[1] << 8U),
                  &start, &end, &found);
        const uint8_t frequency = data[3];
        if (found && frequency)
        {
            command->mode = PAD_TRIGGER_EFFECT_VIBRATION;
            command->data.vibration.position = (uint8_t)start;
            command->data.vibration.amplitude = effect_strength(5, intensity);
            command->data.vibration.frequency = frequency;
        }
        break;
    }
    case 0x27: /* Machine — dual-amplitude vibration */
    {
        unsigned start = 0;
        unsigned end = 0;
        bool found = false;
        bit_range((uint16_t)data[0] | ((uint16_t)data[1] << 8U),
                  &start, &end, &found);
        const uint8_t amp_a = (uint8_t)((data[2] >> 0) & 7U);
        const uint8_t amp_b = (uint8_t)((data[2] >> 3) & 7U);
        const uint8_t amplitude = effect_strength(
            (uint8_t)((amp_a > amp_b ? amp_a : amp_b) + 1U), intensity);
        const uint8_t frequency = data[3];
        if (found && amplitude && frequency)
        {
            command->mode = PAD_TRIGGER_EFFECT_VIBRATION;
            command->data.vibration.position = (uint8_t)start;
            command->data.vibration.amplitude = amplitude;
            command->data.vibration.frequency = frequency;
        }
        break;
    }
    case 0x01: /* Simple Feedback */
    case 0x11: /* Limited Feedback */
        command->mode = PAD_TRIGGER_EFFECT_FEEDBACK;
        command->data.feedback.position = data[0] > 9 ? 9 : data[0];
        command->data.feedback.strength = effect_strength(data[1], intensity);
        if (!command->data.feedback.strength)
            command->mode = PAD_TRIGGER_EFFECT_OFF;
        break;
    case 0x02: /* Simple Weapon */
        set_weapon_clamped(command, data[0], data[1],
                           effect_strength(data[2], intensity));
        break;
    case 0x12: /* Limited Weapon — coarse positions; clamp into SCE range */
    {
        unsigned start = data[0] > 16U ? (unsigned)((data[0] - 16U) / 12U) : 2U;
        unsigned end = data[1] > data[0]
            ? start + 1U + (unsigned)((data[1] - data[0]) / 25U)
            : start + 1U;
        set_weapon_clamped(command, start, end,
                           effect_strength(data[2], intensity));
        break;
    }
    case 0x06: /* Simple Vibration — freq, amplitude, position */
        command->mode = PAD_TRIGGER_EFFECT_VIBRATION;
        command->data.vibration.position = data[2] > 9 ? 9 : data[2];
        command->data.vibration.amplitude = effect_strength(data[1], intensity);
        command->data.vibration.frequency = data[0];
        if (!command->data.vibration.amplitude || !command->data.vibration.frequency)
            command->mode = PAD_TRIGGER_EFFECT_OFF;
        break;
    default:
    {
        static unsigned unknown_logs;
        if (unknown_logs < 8U)
        {
            ++unknown_logs;
            pad_diag_append("trigger_unknown_type", type);
            cloudplay::app_log("pad", "trigger_unknown_type", type);
        }
        break;
    }
    }
}

static uint64_t monotonic_milliseconds(void)
{
    return SDL_GetTicks64();
}

static int stick_direction(uint8_t x, uint8_t y)
{
    const int horizontal = (int)x - 128;
    const int vertical = (int)y - 128;
    const int horizontal_size = horizontal < 0 ? -horizontal : horizontal;
    const int vertical_size = vertical < 0 ? -vertical : vertical;
    if (horizontal_size < 64 && vertical_size < 64)
        return -1;
    if (horizontal_size > vertical_size)
        return x < STICK_LOW ? RADIO_INPUT_LEFT : x > STICK_HIGH ? RADIO_INPUT_RIGHT : -1;
    return y < STICK_LOW ? RADIO_INPUT_UP : y > STICK_HIGH ? RADIO_INPUT_DOWN : -1;
}

static void queue_push(radio_input_key_t key, bool pressed)
{
    const unsigned next = (queue_write + 1U) % INPUT_QUEUE_SIZE;
    if (next == queue_read)
    {
        /* ponytail: bounded input; discard the oldest event instead of allocating. */
        queue_read = (queue_read + 1U) % INPUT_QUEUE_SIZE;
    }
    queue[queue_write] = (radio_input_event_t){key, pressed};
    queue_write = next;
}

static void process_sample(const pad_sample_t *sample)
{
    uint32_t current = sample->buttons;
    const bool neutral = sample->connected == 0 || (current & PAD_BUTTON_INTERCEPTED) != 0;
    if (neutral)
        current = 0;
    latest_pad.buttons = current;
    latest_pad.left_x = neutral ? 128 : sample->left_x;
    latest_pad.left_y = neutral ? 128 : sample->left_y;
    latest_pad.right_x = neutral ? 128 : sample->right_x;
    latest_pad.right_y = neutral ? 128 : sample->right_y;
    latest_pad.left_trigger = neutral ? 0 : sample->left_trigger;
    latest_pad.right_trigger = neutral ? 0 : sample->right_trigger;
    latest_pad.touch_count = neutral ? 0 :
        (sample->touch.fingers < RADIO_INPUT_TOUCH_CAPACITY ?
            sample->touch.fingers : RADIO_INPUT_TOUCH_CAPACITY);
    for (uint8_t i = 0; i < RADIO_INPUT_TOUCH_CAPACITY; ++i)
    {
        latest_pad.touches[i].x = sample->touch.touches[i].x;
        latest_pad.touches[i].y = sample->touch.touches[i].y;
        latest_pad.touches[i].id = (int8_t)(sample->touch.touches[i].finger & 0x7fU);
    }
    latest_pad.orientation_x = neutral ? 0.0f : sample->orientation.x;
    latest_pad.orientation_y = neutral ? 0.0f : sample->orientation.y;
    latest_pad.orientation_z = neutral ? 0.0f : sample->orientation.z;
    latest_pad.orientation_w = neutral ? 1.0f : sample->orientation.w;
    latest_pad.acceleration_x = neutral ? 0.0f : sample->acceleration.x;
    latest_pad.acceleration_y = neutral ? 1.0f : sample->acceleration.y;
    latest_pad.acceleration_z = neutral ? 0.0f : sample->acceleration.z;
    latest_pad.angular_velocity_x = neutral ? 0.0f : sample->angular_velocity.x;
    latest_pad.angular_velocity_y = neutral ? 0.0f : sample->angular_velocity.y;
    latest_pad.angular_velocity_z = neutral ? 0.0f : sample->angular_velocity.z;
    latest_pad.timestamp = neutral ? 0 : sample->timestamp;
    latest_pad.connected = !neutral;

    const uint32_t changed = button_state ^ current;
    for (unsigned i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i)
    {
        if ((changed & buttons[i].button) != 0)
        {
            queue_push(buttons[i].key, (current & buttons[i].button) != 0);
        }
    }
    button_state = current;

    int current_analog = neutral ? -1 : stick_direction(sample->left_x, sample->left_y);
    if (current_analog != analog_key)
    {
        if (analog_key >= 0)
            queue_push((radio_input_key_t)analog_key, false);
        analog_key = current_analog;
        if (analog_key >= 0)
        {
            queue_push((radio_input_key_t)analog_key, true);
            analog_repeat_at = monotonic_milliseconds() + STICK_REPEAT_DELAY_MS;
        }
    }
}

bool radio_input_init(void)
{
    memset(&latest_pad, 0, sizeof(latest_pad));
    latest_pad.left_x = latest_pad.left_y = 128;
    latest_pad.right_x = latest_pad.right_y = 128;
    pad_diag[0] = '\0';
    pad_diag_append("pid", getpid());

    const int user_init = sceUserServiceInitialize(NULL);
    owns_user_service = user_init == 0;
    pad_diag_append("user_init", user_init);

    int32_t user_id = -1;
    const int user_get = sceUserServiceGetInitialUser(&user_id);
    pad_diag_append("user_id", user_id);
    pad_diag_append("user_get", user_get);
    const int pad_init = scePadInit();
    pad_diag_append("pad_init", pad_init);
    if (user_get < 0 || pad_init < 0)
    {
        radio_input_shutdown();
        return false;
    }
    /* Homebrew apps need elevated pad privilege for scePadSetTriggerEffect. */
    last_privilege_result = scePadSetProcessPrivilege(1);
    cloudplay::app_log("pad", "set_process_privilege", last_privilege_result);
    pad_diag_append("set_process_privilege", last_privilege_result);
    pad_handle = scePadOpen(user_id, 0, 0, NULL);
    if (pad_handle < 0)
    {
        pad_diag_append("pad_open", pad_handle);
        radio_input_shutdown();
        return false;
    }
    cloudplay::app_log("pad", "open", pad_handle);
    pad_diag_append("pad_open", pad_handle);
    /* Some titles need privilege after the handle exists. */
    last_privilege_result = scePadSetProcessPrivilege(1);
    pad_diag_append("set_process_privilege_after_open", last_privilege_result);
    (void)scePadSetMotionSensorState(pad_handle, true);
    queue_read = queue_write = 0;
    button_state = 0;
    analog_key = -1;
    analog_repeat_at = 0;
    return true;
}

bool radio_input_reset_orientation(void)
{
    return pad_handle >= 0 && scePadResetOrientation(pad_handle) >= 0;
}

static bool apply_merged_vibration(void)
{
    if (pad_handle < 0)
        return false;
    uint8_t haptic_l = last_haptic_low;
    uint8_t haptic_h = last_haptic_high;
    if (monotonic_milliseconds() > haptic_hold_until_ms)
    {
        haptic_l = 0;
        haptic_h = 0;
    }
    /* Classic rumble stays until the stream sends (0,0). */
    const pad_vibration_t vibration = {
        last_rumble_low > haptic_l ? last_rumble_low : haptic_l,
        last_rumble_high > haptic_h ? last_rumble_high : haptic_h,
    };
    return scePadSetVibration(pad_handle, &vibration) >= 0;
}

bool radio_input_set_vibration(uint8_t low_frequency, uint8_t high_frequency)
{
    if (pad_handle < 0)
        return false;
    last_rumble_low = low_frequency;
    last_rumble_high = high_frequency;
    return apply_merged_vibration();
}

bool radio_input_clear_haptic_audio(void)
{
    last_haptic_low = last_haptic_high = 0;
    haptic_hold_until_ms = 0;
    return apply_merged_vibration();
}

bool radio_input_set_haptic_audio(const int16_t *samples, size_t frame_count,
                                  uint8_t intensity)
{
    if (!samples || frame_count == 0 || pad_handle < 0)
        return false;
    if (intensity == 0)
        return radio_input_clear_haptic_audio();

    /* Peak abs (not mean) — DualSense haptic bursts are short and sparse;
     * averaging them into near-zero wiped most of the feel. */
    uint32_t left_peak = 0;
    uint32_t right_peak = 0;
    uint64_t left_sum = 0;
    uint64_t right_sum = 0;
    for (size_t i = 0; i < frame_count; ++i)
    {
        const int32_t left = samples[i * 2U];
        const int32_t right = samples[i * 2U + 1U];
        const uint32_t la = (uint32_t)(left < 0 ? -left : left);
        const uint32_t ra = (uint32_t)(right < 0 ? -right : right);
        left_sum += la;
        right_sum += ra;
        if (la > left_peak)
            left_peak = la;
        if (ra > right_peak)
            right_peak = ra;
    }
    /* Blend peak (dominant) with a boosted mean so sustained tones still land. */
    uint32_t left = (left_peak + (uint32_t)((left_sum / frame_count) * 2U)) >> 8U;
    uint32_t right = (right_peak + (uint32_t)((right_sum / frame_count) * 2U)) >> 8U;
    /* Strong (1) boosts — LRA→ERM approximation needs headroom. */
    if (intensity == 1)
    {
        left = left * 3U / 2U;
        right = right * 3U / 2U;
    }
    else if (intensity == 2)
    {
        left /= 2U;
        right /= 2U;
    }
    else if (intensity == 3)
    {
        left /= 3U;
        right /= 3U;
    }
    if (left > 255U)
        left = 255U;
    if (right > 255U)
        right = 255U;
    /* Noise floor: keep classic rumble latched; just refresh the motors. */
    constexpr uint32_t kHapticFloor = 8U;
    if (left < kHapticFloor && right < kHapticFloor)
    {
        if (monotonic_milliseconds() > haptic_hold_until_ms)
        {
            last_haptic_low = last_haptic_high = 0;
            return apply_merged_vibration();
        }
        return true;
    }
    last_haptic_low = (uint8_t)left;
    last_haptic_high = (uint8_t)right;
    haptic_hold_until_ms = monotonic_milliseconds() + 60U;
    return apply_merged_vibration();
}

bool radio_input_set_trigger_effects(uint8_t left_type, const uint8_t left[10],
                                     uint8_t right_type, const uint8_t right[10],
                                     uint8_t intensity)
{
    if (pad_handle < 0)
        return false;
    pad_trigger_effect_param_t effect{};
    effect.trigger_mask = 0x03;
    decode_trigger_effect(&effect.command[0], left_type, left, intensity);
    decode_trigger_effect(&effect.command[1], right_type, right, intensity);
    last_trigger_result = scePadSetTriggerEffect(pad_handle, &effect);
    if (last_trigger_result < 0)
    {
        /* Slope may also be rejected on some firmwares — fall back to point feedback. */
        bool converted = false;
        for (unsigned i = 0; i < 2; ++i)
        {
            if (effect.command[i].mode != PAD_TRIGGER_EFFECT_SLOPE_FEEDBACK)
                continue;
            const uint8_t start = effect.command[i].data.slope_feedback.start_position;
            uint8_t strength = effect.command[i].data.slope_feedback.start_strength;
            if (effect.command[i].data.slope_feedback.end_strength > strength)
                strength = effect.command[i].data.slope_feedback.end_strength;
            memset(&effect.command[i].data, 0, sizeof(effect.command[i].data));
            effect.command[i].mode = PAD_TRIGGER_EFFECT_FEEDBACK;
            effect.command[i].data.feedback.position = start > 9 ? 9 : start;
            effect.command[i].data.feedback.strength = strength;
            converted = true;
        }
        if (converted)
            last_trigger_result = scePadSetTriggerEffect(pad_handle, &effect);
    }
    if (last_trigger_result < 0)
    {
        static unsigned fail_logs;
        if (fail_logs < 16U)
        {
            ++fail_logs;
            cloudplay::app_log("pad", "set_trigger_effect", last_trigger_result);
            pad_diag_append("set_trigger_effect", last_trigger_result);
            pad_diag_append("trigger_left_type", left_type);
            pad_diag_append("trigger_right_type", right_type);
            pad_diag_append("trigger_left_mode", effect.command[0].mode);
            pad_diag_append("trigger_right_mode", effect.command[1].mode);
        }
        return false;
    }
    static unsigned ok_logs;
    if (ok_logs < 2U)
    {
        ++ok_logs;
        pad_diag_append("set_trigger_effect_ok", last_trigger_result);
        cloudplay::app_log("pad", "set_trigger_effect_ok", last_trigger_result);
    }
    return true;
}

bool radio_input_clear_trigger_effects(void)
{
    const uint8_t empty[10] = {};
    return radio_input_set_trigger_effects(0x05, empty, 0x05, empty, 1);
}

int radio_input_run_trigger_test(int mode)
{
    if (pad_handle < 0)
    {
        last_trigger_result = -1;
        pad_diag_append("trigger_test_no_pad", last_trigger_result);
        return last_trigger_result;
    }

    /* Retry privilege right before the effect — useful for settings tests. */
    last_privilege_result = scePadSetProcessPrivilege(1);
    pad_diag_append("trigger_test_privilege", last_privilege_result);

    uint8_t left[10] = {};
    uint8_t right[10] = {};
    uint8_t left_type = 0x05;
    uint8_t right_type = 0x05;
    switch (mode)
    {
    case 1: /* Soft resistance on both triggers */
        left_type = right_type = 0x01;
        left[0] = right[0] = 5;
        left[1] = right[1] = 8;
        break;
    case 2: /* Weapon-style hard stop */
        left_type = right_type = 0x02;
        left[0] = right[0] = 2;
        left[1] = right[1] = 7;
        left[2] = right[2] = 8;
        break;
    case 3: /* Trigger vibration — chiaki layout: freq, amp, position */
        left_type = right_type = 0x06;
        left[0] = right[0] = 8; /* frequency (0 disables the effect) */
        left[1] = right[1] = 8; /* amplitude */
        left[2] = right[2] = 5; /* position 0..9 */
        break;
    default: /* Clear / off */
        left_type = right_type = 0x05;
        mode = 0;
        break;
    }

    (void)radio_input_set_trigger_effects(left_type, left, right_type, right, 1);
    pad_diag_append("trigger_test_mode", mode);
    pad_diag_append("trigger_test_result", last_trigger_result);
    /* Also log as unsigned hex-friendly decimal for SCE error codes. */
    pad_diag_append("trigger_test_result_u",
                    static_cast<int64_t>(static_cast<uint32_t>(last_trigger_result)));
    cloudplay::app_log("pad", "trigger_test", last_trigger_result);
    return last_trigger_result;
}

int radio_input_last_privilege_result(void)
{
    return last_privilege_result;
}

int radio_input_last_trigger_result(void)
{
    return last_trigger_result;
}

bool radio_input_pad_state(radio_input_pad_state_t *state)
{
    if (!state || pad_handle < 0)
        return false;
    *state = latest_pad;
    return true;
}

void radio_input_poll(void)
{
    if (pad_handle < 0)
        return;
    const int count = scePadRead(pad_handle, samples, PAD_SAMPLE_CAPACITY);
    for (int i = 0; i < count; ++i)
		process_sample(&samples[i]);
    if (analog_key >= 0)
    {
        const uint64_t now = monotonic_milliseconds();
        if (now >= analog_repeat_at)
        {
            queue_push((radio_input_key_t)analog_key, true);
            analog_repeat_at = now + STICK_REPEAT_MS;
        }
    }
}

bool radio_input_next(radio_input_event_t *event)
{
    if (event == NULL || queue_read == queue_write)
        return false;
    *event = queue[queue_read];
    queue_read = (queue_read + 1U) % INPUT_QUEUE_SIZE;
    return true;
}

bool radio_input_pressed(radio_input_key_t key)
{
    if (key < 0 || key >= RADIO_INPUT_COUNT)
        return false;
    for (unsigned i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i)
    {
        if (buttons[i].key == key)
            return (button_state & buttons[i].button) != 0;
    }
    return false;
}

void radio_input_shutdown(void)
{
    last_rumble_low = last_rumble_high = 0;
    last_haptic_low = last_haptic_high = 0;
    haptic_hold_until_ms = 0;
    if (pad_handle >= 0)
    {
        const pad_vibration_t vibration = {0, 0};
        (void)scePadSetVibration(pad_handle, &vibration);
        (void)radio_input_clear_trigger_effects();
        (void)scePadSetMotionSensorState(pad_handle, false);
        scePadClose(pad_handle);
        pad_handle = -1;
    }
    if (owns_user_service)
    {
        sceUserServiceTerminate();
        owns_user_service = false;
    }
    queue_read = queue_write = 0;
    button_state = 0;
    analog_key = -1;
    analog_repeat_at = 0;
}
