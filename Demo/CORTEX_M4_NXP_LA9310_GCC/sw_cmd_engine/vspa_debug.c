/* SPDX-License-Identifier: BSD-3-Clause */

/*
 * Copyright 2026 Eric Mittag, Max
 *
 * Ported from LimeSDR-Micro_FW branch vspa-selfhosted-debug.
 */

#include "vspa_debug.h"
#include "io.h"

/* VSPA debug block on the M4 external PPB; see vspa_debug.h. */
#define VSPA_DBG_BASE    0xE0046000u
#define VSPA_DBG_SPAN    0x1000u

uint32_t VspaDebugCommand( uint32_t * data )
{
    const uint32_t op = data[ 0 ] & 0xFFu;
    const uint32_t fixed = data[ 0 ] & VSPA_DBG_FLAG_FIXED;
    const uint32_t count = ( data[ 0 ] >> 16 ) & 0xFFu;
    const uint32_t offset = data[ 1 ];

    if( op != VSPA_DBG_OP_READ && op != VSPA_DBG_OP_WRITE )
    {
        return VSPA_DBG_ERR_OP;
    }

    if( count == 0 || count > VSPA_DBG_MAX_WORDS )
    {
        return VSPA_DBG_ERR_COUNT;
    }

    const uint32_t span = fixed ? 4u : count * 4u;

    if( ( offset & 3u ) || ( offset + span ) > VSPA_DBG_SPAN )
    {
        return VSPA_DBG_ERR_OFFSET;
    }

    uint32_t addr = VSPA_DBG_BASE + offset;

    for( uint32_t i = 0; i < count; i++ )
    {
        if( op == VSPA_DBG_OP_READ )
        {
            data[ 2 + i ] = IN_32( ( uint32_t * )( uintptr_t ) addr );
        }
        else
        {
            OUT_32( ( uint32_t * )( uintptr_t ) addr, data[ 2 + i ] );
        }

        if( !fixed )
        {
            addr += 4u;
        }
    }

    return 0;
}
