/* SPDX-License-Identifier: BSD-3-Clause */

/*
 * Copyright 2024 NXP
 */

#ifndef __RFNM_RF_CTRL_H__
#define __RFNM_RF_CTRL_H__


#define PHYTIMER_500_US_61p44 	30720

/* RF control structure */
typedef struct {
	uint32_t target_phytimer_ts;
	uint32_t issued_phytimer_ts;
	// GPT3 is used as TTI trigger inside the i.MX. To enable it, set
	// tti_period_ts > 0. TTI will then trigger at the same time as
	// target_phytimer_ts. tti_period_ts > 0 should only be sent once,
	// unless you need to change the period, send tti_period_ts = 0
	// to disable the interrupt, set tti_period_ts = 1
	uint32_t tti_period_ts;
	uint32_t mode;
	// TDD v2 telemetry tail (host-readable at TCML 0x1C00F740 + 0x10 - exactly the
	// address the v1 comment promised; the M7 binary was built against the 4-word
	// struct and never touches this tail). tdd_late_arms == 0xFFFFFFFF marks a
	// REFUSED pattern (span floor violated at start_at).
	uint32_t tdd_cycles;		// completed periods (counted at each open edge)
	uint32_t tdd_late_arms;		// skipped edges: a rearm found its target too close
	int32_t tdd_worst_margin;	// min (target - now) right after an arm, ticks
	uint32_t tdd_last_open;		// last armed window-open tick (grid witness)
	// r10 K1 anchor-mint forensics (host: 0x1C00F760..): every rx/tx mint records the
	// anchor intent it saw and what it minted - the ground truth for phase disputes
	uint32_t mint_anchor_phase;	// intent phase as received
	uint32_t mint_anchor_granule;	// intent granule as received (0 = no anchor)
	uint32_t mint_advance;		// ticks added to the default mint for congruence
	uint32_t mint_t0;		// the minted ptmr_t0 shipped to the VSPA
	// #66 forensics (host devmem 0x1C00F770): the grid's TRUE origin vs the mint.
	// tdd_base_adv counts whole periods the start-lead loop slipped past t0 - the
	// session lottery behind the gated-stamp offset integer.
	uint32_t tdd_base;		// grid origin after the lead advance
	uint32_t tdd_base_adv;		// periods advanced from t0 to base
	uint32_t tdd_start_now;		// phytimer 'now' at rfnm_tdd_start_at entry
} rf_ctrl_s;

extern volatile rf_ctrl_s rf_ctrl;

void switch_rf(uint32_t mode);
void switch_rf_at(uint32_t mode, uint32_t target_ts);

/* TDD v2 alarm ISR (rfnm_tdd.c), vectored at IRQ_PPS_OUT in start.S: the parked PPS
 * feature's comparator 14 is repurposed as the edge-chained rearm alarm. */
void rfnm_tdd_alarm_isr(void);

/* phytimer phase 2a step 3: M4 TDD scheduler (rfnm_tdd.c) */
void rfnm_tdd_configure(uint32_t period_chunks, uint32_t duty_chunks);
void rfnm_tdd_set_txgate(uint32_t en);
uint32_t rfnm_tdd_txgate(void);
void rfnm_tdd_stop(void);
uint32_t rfnm_tdd_period_ticks(void);
uint32_t rfnm_tdd_duty_ticks(void);
uint32_t rfnm_tdd_pattern_word(void);
void rfnm_tdd_start_at(uint32_t t0, uint32_t ticks_per_chunk);
int rfnm_tdd_init(void);

uint32_t rfnm_tdd_duty_chunks(void);
uint32_t rfnm_tdd_period_chunks(void);

/* RT command ring engine (rfnm_rtc.c): kernel-fed C11 window arming, option B -
 * the M4 stays sole comparator owner. Hook is called first from the shared
 * alarm ISR; active() is the grid's defensive mode-exclusivity check. */
int rfnm_rtc_init(void);
int rfnm_rtc_alarm_hook(void);
uint32_t rfnm_rtc_active(void);
void vRfnmRtcDoorbellFromISR(void);

#endif