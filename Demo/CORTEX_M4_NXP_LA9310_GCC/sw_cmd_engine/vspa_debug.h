/* SPDX-License-Identifier: BSD-3-Clause */

/*
 * Copyright 2026 Eric Mittag, Max
 *
 * Ported from LimeSDR-Micro_FW branch vspa-selfhosted-debug
 * (LIME_M4_VSPA_DBG); bench-verified on LA9310 silicon against the
 * CodeWarrior TAP path (28/28 vspa-lib kernel tests, identical cycle
 * counts).
 */

#ifndef _VSPA_DEBUG_H_
#define _VSPA_DEBUG_H_

#include <stdint.h>

/* Host-driven proxy for the VSPA debug register block (VSPA_DBG).
 *
 * The block lives at 0xE0046000 on the Cortex-M4 external PPB (DCSR root
 * 0xE0000000 + 0x46000) and is not reachable over any PCIe BAR, so the host
 * asks the M4 to access it through the LA9310_SW_CMD_VSPA_DBG sw command.
 *
 * Payload layout in la9310_sw_cmd_desc.data[32]:
 *   in : data[0] = op word: bits[7:0] op (1=read, 2=write),
 *                  bit[8] fixed-address flag (burst on one register,
 *                  e.g. the auto-incrementing RAVID portal data register),
 *                  bits[23:16] word count (1..28)
 *        data[1] = byte offset inside the 4 KB block (word aligned)
 *        data[2..2+count-1] = values to write (write op only)
 *   out: data[0] = 0 on success, VSPA_DBG_ERR_* otherwise
 *        data[2..2+count-1] = values read (read op only)
 */

#define VSPA_DBG_OP_READ      1u
#define VSPA_DBG_OP_WRITE     2u
#define VSPA_DBG_FLAG_FIXED   ( 1u << 8 )
#define VSPA_DBG_MAX_WORDS    28u

#define VSPA_DBG_ERR_OP       1u
#define VSPA_DBG_ERR_OFFSET   2u
#define VSPA_DBG_ERR_COUNT    3u

uint32_t VspaDebugCommand( uint32_t * data );

#endif /* _VSPA_DEBUG_H_ */
