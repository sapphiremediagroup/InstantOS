#pragma once

#include <stddef.h>
#include <stdint.h>

extern "C" {

void* memset(void* dest, int val, size_t count);
void* memset32(void* dest, uint32_t val, size_t count);
void* memcpy(void* dest, const void* src, size_t count);
void* memmove(void* dest, const void* src, size_t count);
int memcmp(const void* b1, const void* b2, size_t count);
void* memchr(const void* b1, int val, size_t count);
void* memrchr(const void* b1, int val, size_t count);

size_t strlen(const char* str);
char* strcpy(char* dest, const char* src);
char* strncpy(char* dest, const char* src, size_t n);
int strcmp(const char* s1, const char* s2);
int strncmp(const char* s1, const char* s2, size_t n);

}