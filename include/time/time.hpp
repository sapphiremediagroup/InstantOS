#pragma once

#include <stdint.h>

extern uint64_t boot_unix_time;

bool time_init();
uint64_t time_get_unix();
uint64_t time_get_boot_unix();
