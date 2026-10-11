#pragma once

#include <stdint.h>
#include <stddef.h>
#include "irlmp.h"

// Tiny Transport Protocol (TinyTP) Definitions
#define IRTTP_DEFAULT_CREDITS           4  // Granted to the Palm in the connect confirm
#define IRTTP_MAX_CREDITS               8  // Most frames the Palm may have outstanding
#define IRTTP_LOW_CREDITS               2  // Send a credit-only frame at or below this
#define IRTTP_FLAG_MORE_BIT             0x80 // Parameter / More bit
#define IRTTP_FLAG_DELTA_CREDITS_MASK   0x7F

#pragma pack(push, 1)

typedef struct {
    IrLMP_Head irlmp;
    uint8_t credit_header;
} IrTTP_Head;

#pragma pack(pop)

