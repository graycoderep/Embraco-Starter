#pragma once
#include "profile.h"
#include <stdbool.h>

bool cfg_io_load_profile(const char* prof_name, GpioProfile* out);
bool cfg_io_save_profile(const char* prof_name, const GpioProfile* in);
