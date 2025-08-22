#pragma once
#include "profile.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    // live states
    bool out_state;
    bool in_state;
    uint32_t in_last_change_ms;
    // frequency mode
    uint32_t next_toggle_ms;
    bool phase_on;
    uint16_t current_freq_hz;
    // general
    bool armed; // outputs enabled only when safe
} EngineState;

void engine_init(EngineState* st, const GpioProfile* p);
void engine_tick(EngineState* st, const GpioProfile* p); // call every ~10-20 ms
void engine_apply_profile(EngineState* st, const GpioProfile* p);

// Serial helpers (600 baud framing for CF10B); transport send is left to user
// Checksum: 0x100 - ((ID + CMD + LSB + MSB) & 0xFF)
uint8_t engine_cf10b_checksum(uint8_t id, uint8_t cmd, uint8_t lsb, uint8_t msb);
// Build "Set Speed" packet for given RPM (clamped)
// Returns length in out[0..4] (always 5)
uint8_t engine_cf10b_build_set_speed(uint16_t rpm, uint8_t* out5);
// Transport TX stub — replace with your UART TX function (600 baud) or bitbang
void engine_serial_send_bytes(const uint8_t* data, uint8_t len);
