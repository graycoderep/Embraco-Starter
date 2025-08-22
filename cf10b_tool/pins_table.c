#include "pins_table.h"
#include <string.h>

// ===== IMPORTANT =====
// Replace these extern declarations with the real exported pin symbols
// that exist in your Flipper SDK version.
// Common examples (may vary): gpio_ext_pc0, gpio_ext_pc1, gpio_ext_pb2, etc.
extern const GpioPin gpio_ext_pc0;
extern const GpioPin gpio_ext_pc1;
extern const GpioPin gpio_ext_pb2;

static const PinDef kPins[] = {
    { "PC0", &gpio_ext_pc0 },
    { "PC1", &gpio_ext_pc1 },
    { "PB2", &gpio_ext_pb2 },
};

const PinDef* pins_get_table(size_t* count) {
    if(count) *count = sizeof(kPins)/sizeof(kPins[0]);
    return kPins;
}

const char* pins_name_from_ptr(const GpioPin* p){
    size_t n; const PinDef* T = pins_get_table(&n);
    for(size_t i=0;i<n;i++) if(T[i].pin==p) return T[i].name;
    return "NA";
}
