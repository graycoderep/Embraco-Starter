#pragma once
#include <furi_hal_gpio.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    MODEL_FREQUENCY = 0,
    MODEL_SERIAL = 1,
    MODEL_DROPIN = 2,
    MODEL_COUNT
} ModelKind;

typedef struct {
    const char* name;             // "FREQUENCY", "SERIAL", "DROPIN"
    ModelKind kind;
    const GpioPin* out0;          // selectable via UI
    const GpioPin* in0;           // selectable via UI (if needed)
    bool out_active_high;         // UI toggle
    uint16_t debounce_ms;         // UI editable
    uint16_t pwm_freq_hz;         // used as square-wave freq for FREQUENCY model (Hz)
    uint8_t  pwm_duty_pc;         // duty (0..100); FREQUENCY uses 50% regardless; left for future
    struct {                      // safety rules
        bool boot_all_off;
        bool interlock_in0_blocks_out0;
    } safety;
    uint16_t version;
} GpioProfile;

typedef struct {
    GpioProfile* active;          // points into profiles[]
    uint8_t active_index;         // 0..N-1
    bool locked;                  // prevent changes in field
} ProfileRuntime;

extern ProfileRuntime g_rt;

void profile_init(void);                 // loads built-ins and SD overrides
uint8_t profile_count(void);
GpioProfile* profile_at(uint8_t idx);
void profile_set_active(uint8_t idx);
void profile_lock(bool on);

bool profile_load_override(GpioProfile* p); // from SD; returns true if applied
bool profile_save_override(const GpioProfile* p);
