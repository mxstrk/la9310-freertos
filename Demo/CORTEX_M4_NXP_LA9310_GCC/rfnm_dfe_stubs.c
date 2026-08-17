/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * DFE-app graft: no-op stubs for the RFNM TDD/RTC datapath symbols.
 *
 * Under LA9310_DFE_APP the RFNM radio path (rfnm_tdd.c, rfnm_rtc.c,
 * rfnm_rf_ctrl.c) is excluded (it clashes with NXP's dfe_app.c/sdr_rf_ctrl.c —
 * duplicate rf_ctrl/switch_rf, and op-code-9 vs the NXP DFE mailbox). But the
 * ARM vector table (platform/ARM_CM4/start.S), la9310_irq.c and the dormant
 * rfic_cmd/rfic_core still reference these symbols, so the image will not link
 * without definitions. They are never exercised on the DFE-app path (no RFNM RF
 * commands are issued; the DFE app drives the VSPA via NXP's own mailbox), so
 * empty no-ops are safe.
 */
#include <stdint.h>

void rfnm_tdd_alarm_isr( void ) {}
void vRfnmRtcDoorbellFromISR( void ) {}
void rfnm_tdd_configure( uint32_t period_chunks, uint32_t duty_chunks ) { (void)period_chunks; (void)duty_chunks; }
void rfnm_tdd_set_txgate( uint32_t en ) { (void)en; }
uint32_t rfnm_tdd_txgate( void ) { return 0; }
void rfnm_tdd_stop( void ) {}
uint32_t rfnm_tdd_pattern_word( void ) { return 0; }
void rfnm_tdd_start_at( uint32_t t0, uint32_t ticks_per_chunk ) { (void)t0; (void)ticks_per_chunk; }
