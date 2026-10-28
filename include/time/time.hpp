#pragma once

#include <stdint.h>

extern uint64_t boot_unix_time;

bool time_init();
uint64_t time_get_unix();
uint64_t time_get_unix_ns();
// Sets the wall clock (settimeofday, sntp). False if it would lie before boot.
bool time_set_unix_ns(uint64_t unix_ns);
uint64_t time_get_boot_unix();
