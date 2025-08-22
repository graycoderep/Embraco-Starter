#include "profile.h"
#include "pins_table.h"
#include "cfg_io.h"
#include <furi.h>
#include <string.h>

ProfileRuntime g_rt;

static GpioProfile profiles[] = {
    { "FREQUENCY", MODEL_FREQUENCY, NULL, NULL, true, 20, 150, 50, {true, true}, 1 },
    { "SERIAL",    MODEL_SERIAL,    NULL, NULL, true, 20, 0,   0,  {true, false},1 },
    { "DROPIN",    MODEL_DROPIN,    NULL, NULL, true, 20, 0,   0,  {true, false},1 },
};

static void apply_safe_defaults(GpioProfile* p) {
    size_t n; const PinDef* T = pins_get_table(&n);
    if(n >= 2) { p->out0 = T[0].pin; p->in0 = T[1].pin; }
}

void profile_init(void) {
    for(size_t i=0;i<sizeof(profiles)/sizeof(profiles[0]);++i) {
        apply_safe_defaults(&profiles[i]);
        (void)profile_load_override(&profiles[i]); // try SD override
    }
    g_rt.active_index = 0;
    g_rt.active = &profiles[0];
    g_rt.locked = false;
}

uint8_t profile_count(void){ return (uint8_t)(sizeof(profiles)/sizeof(profiles[0])); }
GpioProfile* profile_at(uint8_t idx){ return (idx<profile_count()) ? &profiles[idx] : NULL; }

void profile_set_active(uint8_t idx){
    if(idx >= profile_count()) return;
    g_rt.active_index = idx;
    g_rt.active = &profiles[idx];
}

void profile_lock(bool on){ g_rt.locked = on; }

bool profile_load_override(GpioProfile* p){ return cfg_io_load_profile(p->name, p); }
bool profile_save_override(const GpioProfile* p){ return cfg_io_save_profile(p->name, p); }
