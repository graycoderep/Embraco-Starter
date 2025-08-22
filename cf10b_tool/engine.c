#include "engine.h"
#include <furi.h>
#include <furi_hal.h>

static inline uint32_t now_ms(void){ return furi_get_tick(); }

void engine_init(EngineState* st, const GpioProfile* p){
    st->out_state = false;
    st->in_state = false;
    st->in_last_change_ms = now_ms();
    st->next_toggle_ms = now_ms();
    st->phase_on = false;
    st->armed = false;
    st->current_freq_hz = 0;

    // GPIO init
    if(p->out0){
        furi_hal_gpio_init(p->out0, GpioModeOutputPushPull, GpioPullNo, GpioSpeedVeryHigh);
        // ensure OFF
        furi_hal_gpio_write(p->out0, p->out_active_high ? false : true);
    }
    if(p->in0){
        furi_hal_gpio_init(p->in0, GpioModeInput, GpioPullUp, GpioSpeedLow);
    }
}

void engine_apply_profile(EngineState* st, const GpioProfile* p){
    // re-init pins if needed
    if(p->out0){
        furi_hal_gpio_init(p->out0, GpioModeOutputPushPull, GpioPullNo, GpioSpeedVeryHigh);
        furi_hal_gpio_write(p->out0, p->out_active_high ? false : true);
    }
    if(p->in0){
        furi_hal_gpio_init(p->in0, GpioModeInput, GpioPullUp, GpioSpeedLow);
    }
    st->armed = false; // require re-arm on profile change
}

static bool read_debounced(EngineState* st, const GpioProfile* p){
    if(!p->in0) return false;
    const bool raw = furi_hal_gpio_read(p->in0);
    const uint32_t t = now_ms();
    static bool last_raw = false;
    static bool debounced = false;
    if(raw != last_raw){ st->in_last_change_ms = t; last_raw = raw; }
    if((t - st->in_last_change_ms) >= p->debounce_ms) debounced = raw;
    return debounced;
}

static void write_out(const GpioProfile* p, bool on){
    if(!p->out0) return;
    bool level = p->out_active_high ? on : !on;
    furi_hal_gpio_write(p->out0, level);
}

// FREQUENCY model: square wave generation (50% duty)
static void tick_frequency(EngineState* st, const GpioProfile* p){
    // Clamp to CF10B limits: Hz 66..150 => RPM 1980..4500
    uint16_t hz = p->pwm_freq_hz;
    if(hz < 66) hz = 66;
    if(hz > 150) hz = 150;
    st->current_freq_hz = hz;

    const uint32_t t = now_ms();
    // Period in ms
    uint32_t period_ms = 1000 / (hz ? hz : 1);
    if(period_ms == 0) period_ms = 1;
    uint32_t half = (period_ms / 2);
    if(half == 0) half = 1;

    if(t >= st->next_toggle_ms){
        st->phase_on = !st->phase_on;
        write_out(p, st->armed && st->phase_on);
        st->next_toggle_ms = t + half;
    }
}

void engine_tick(EngineState* st, const GpioProfile* p){
    // Read input (debounced)
    st->in_state = read_debounced(st, p);

    // Safety interlock
    if(p->safety.interlock_in0_blocks_out0 && st->in_state){
        st->armed = false; // block
    }

    switch(p->kind){
        case MODEL_FREQUENCY:
            tick_frequency(st, p);
            break;
        case MODEL_SERIAL:
            // Nothing periodic by default. You can add polling or TX here.
            break;
        case MODEL_DROPIN:
            // Read-only indicators handled in UI.
            break;
        default: break;
    }
}

// ===== CF10B serial helpers =====
uint8_t engine_cf10b_checksum(uint8_t id, uint8_t cmd, uint8_t lsb, uint8_t msb){
    uint8_t sum = (uint8_t)(id + cmd + lsb + msb);
    uint8_t ck = (uint8_t)(0x100 - (sum & 0xFF));
    return ck;
}

uint8_t engine_cf10b_build_set_speed(uint16_t rpm, uint8_t* out5){
    if(rpm > 4500) rpm = 4500;
    // rpm -> Hz = rpm/30 ; CF10B expects RPM split LSB/MSB as integer
    // Manual frames are (ID=0xA5, CMD=0xC3 for overwrite set speed), LSB, MSB, CK
    uint8_t id = 0xA5;
    uint8_t cmd = 0xC3;
    uint8_t lsb = (uint8_t)(rpm & 0xFF);
    uint8_t msb = (uint8_t)((rpm >> 8) & 0xFF);
    uint8_t ck  = engine_cf10b_checksum(id, cmd, lsb, msb);
    out5[0]=id; out5[1]=cmd; out5[2]=lsb; out5[3]=msb; out5[4]=ck;
    return 5;
}

__attribute__((weak))
void engine_serial_send_bytes(const uint8_t* data, uint8_t len){
    // WEAK stub — replace this with your UART TX.
    // Example if your SDK exposes furi_hal_uart_tx():
    // furi_hal_uart_tx(FuriHalUartIdUSART1, data, len);
    (void)data; (void)len;
}
