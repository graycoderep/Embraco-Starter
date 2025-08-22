#pragma once
#include <furi_hal_gpio.h>
#include <stddef.h>

typedef struct { const char* name; const GpioPin* pin; } PinDef;

// Returns a table of *allowed* pins for the UI to offer.
// EDIT pins_table.c to include the actual extern GpioPin symbols from your SDK.
const PinDef* pins_get_table(size_t* count);
const char*  pins_name_from_ptr(const GpioPin* p);
