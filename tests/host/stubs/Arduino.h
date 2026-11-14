// Minimal Arduino shim for compiling pure terminal logic on the host.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
