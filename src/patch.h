// KSNetFix shared helpers (defined in ksnetfix.cpp).
#pragma once
#include <stddef.h>
#include <stdint.h>

void Log(const char* fmt, ...);

// Code patching; every address is verified against expected bytes before use.
bool Match(uint32_t va, const uint8_t* bytes, size_t n);
bool WriteCode(uint32_t va, const void* bytes, size_t n);
bool WriteRel32(uint32_t site, uint8_t opcode, const void* target, size_t pad = 0);
bool MatchCall(uint32_t site, uint32_t target);

template <class T> static inline T& G(uint32_t va) { return *reinterpret_cast<T*>(static_cast<uintptr_t>(va)); }
