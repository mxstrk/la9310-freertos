/* SPDX-License-Identifier: BSD-3-Clause */

/*
 * Copyright 2026 RFNM
 *
 * phytimer phase 2a step 3, v2: the M4 TDD scheduler. One schedule drives BOTH the RX
 * allow-gate comparators and the frontend flips (switch_rf_at -> RFCTL_5 edge -> M7
 * latch trigger) at the same phytimer ticks, so converter gating and FE switching are
 * coherent by construction. The grid is monotonic (base += period from the stream
 * anchor's t0), so scheduling jitter never accumulates - every edge fires on an exact
 * comparator tick.
 *
 * v2 (2026-07-14, the "1 ms bug"): v1 re-armed from a FreeRTOS task (1 ms tick), which
 * forced duty and period-duty >= ~2 ms and scattered the band-39 4ms/1ms shape (bench-
 * measured, port-tdd doc §4a). Now every edge re-arms the NEXT edge from the alarm ISR
 * (comparator 14 = the parked PPS-OUT feature's channel, IRQ_PPS_OUT, vectored in
 * start.S; its pin toggles at edge rate - cosmetic until PPS ships, then move channels).
 * The only floor left is the M7's 500 us preload + ISR margin:
 *   duty >= 600 us AND period - duty >= 600 us   (RFNM_TDD_MIN_SPAN_TICKS)
 * A pattern below the floor is REFUSED at start (tdd_late_arms = 0xFFFFFFFF marker).
 * Telemetry lives in the rf_ctrl tail (host: TCML 0x1C00F740 + 0x10): cycles,
 * late_arms (skipped edges, self-healing grid jump), worst arm margin, last open tick.
 *
 * Known v2 limits:
 *  - one rf_ctrl mailbox = one FE flip armed at a time (also true in v1: its initial
 *    RX flip was silently overwritten by the first close arm - dropped here)
 *  - RX stamps: the VSPA chain still shows (loud, counted) breaks at window seams
 */

#include "FreeRTOS.h"
#include "task.h"
#include "common.h"
#include "phytimer.h"
#include "rfnm_rf_ctrl.h"

// adc i -> AXIQ fifo {RX1, RX3, RX0, RX2}[i] -> comparator fifo+1; lockstep with the
// VSPA's Rx_Antenna2fifo_index and the rfic_cmd.c stream-start table
const uint8_t rfnm_tdd_gate_comp[4] = {
	PHY_TIMER_COMP_CH2_RX_ALLOWED, PHY_TIMER_COMP_CH4_RX_ALLOWED,
	PHY_TIMER_COMP_CH1_RX_ALLOWED, PHY_TIMER_COMP_CH3_RX_ALLOWED };

#define RFNM_TDD_ALARM_COMP		PHY_TIMER_COMP_PPS_OUT
#define RFNM_TDD_TX_COMP		PHY_TIMER_COMP_CH5_TX_ALLOWED	/* the C11 gate */
#define RFNM_TDD_MIN_ARM_TICKS		32768	/* M7 preload 30720 + ISR margin */
#define RFNM_TDD_MIN_SPAN_TICKS		36864	/* 600 us per span: the v2 floor */

// TDD-kernel capability (charter 2026-07-15): latched from the VSPA's pattern-verb
// ack (msb32 bit0 - only apm-tdd.eld sets it). When set, the edge program below
// ALSO drives the TX comparator with the INVERSE polarity, so the DAC drains only
// during TX-open spans (the gated counted pump owns write-idle at every close).
// When clear (the standard image), the TX comparator is never touched here and
// mode-1b free-run behavior is byte-identical to today.
static uint8_t rfnm_tdd_txgate_en;
void rfnm_tdd_set_txgate(uint32_t en) {
	rfnm_tdd_txgate_en = en ? 1 : 0;
}
uint32_t rfnm_tdd_txgate(void) {
	return rfnm_tdd_txgate_en;
}

static struct {
	uint32_t period_chunks;	// pattern in CHUNKS (768 ADC samples) - window alignment is structural
	uint32_t duty_chunks;
	uint32_t period_ticks;
	uint32_t duty_ticks;
	uint32_t base;		// cycle base on the anchor grid: close = base + duty,
				// open = base + period; advanced ONLY in the ISR
	volatile uint32_t run;
	volatile uint32_t pending_open;	// 0: close edge armed next; 1: open edge armed next
} rfnm_tdd;

static void rfnm_tdd_arm_gates(enum ePhyTimerComparatorTrigger eVal, uint32_t ts) {
	int i;
	for(i = 0; i < 4; i++) {
		vPhyTimerComparatorConfig(rfnm_tdd_gate_comp[i],
				PHY_TIMER_COMPARATOR_CLEAR_INT, eVal, ts);
	}
}

static inline uint32_t rfnm_tdd_now(void) {
	return ulPhyTimerCapture(PHY_TIMER_COMPARATOR_COUNT - 1);
}

static inline void rfnm_tdd_alarm(uint32_t ts) {
	vPhyTimerComparatorConfig(RFNM_TDD_ALARM_COMP,
			PHY_TIMER_COMPARATOR_CLEAR_INT | PHY_TIMER_COMPARATOR_CROSS_TRIG,
			ePhyTimerComparatorOutToggle, ts);
}

// arm one edge (FE flip + gates + the chained alarm), with the late self-heal: if the
// target is inside the arm floor, jump WHOLE periods (grid preserved, loudly counted)
static void rfnm_tdd_arm_edge(uint32_t now) {
	uint32_t target;

	if(!rfnm_tdd.pending_open) {
		target = rfnm_tdd.base + rfnm_tdd.duty_ticks;
		while((int32_t)(target - now) < RFNM_TDD_MIN_ARM_TICKS) {
			rfnm_tdd.base += rfnm_tdd.period_ticks;
			target += rfnm_tdd.period_ticks;
			rf_ctrl.tdd_late_arms++;
		}
		switch_rf_at(0xAAAAAAAA, target);
		rfnm_tdd_arm_gates(ePhyTimerComparatorOut0, target);
		if(rfnm_tdd_txgate_en) {
			// RX-close edge = TX-open (complementary spans). CROSS_TRIG +
			// CLEAR_INT|Out1 = the windowed path's proven release arm: the
			// pulse releases the span-head prime pended on the fresh fifo.
			// (A/B tested: the pulses are NOT the RX park cause, and
			// without the fifo-side zombie fix the prime never released with
			// or without them.)
			vPhyTimerComparatorConfig(RFNM_TDD_TX_COMP,
					PHY_TIMER_COMPARATOR_CLEAR_INT | PHY_TIMER_COMPARATOR_CROSS_TRIG,
					ePhyTimerComparatorOut1, target);
		}
	} else {
		target = rfnm_tdd.base + rfnm_tdd.period_ticks;
		while((int32_t)(target - now) < RFNM_TDD_MIN_ARM_TICKS) {
			rfnm_tdd.base += rfnm_tdd.period_ticks;
			target += rfnm_tdd.period_ticks;
			rf_ctrl.tdd_late_arms++;
		}
		switch_rf_at(0xBBBBBBBB, target);
		rfnm_tdd_arm_gates(ePhyTimerComparatorOut1, target);
		if(rfnm_tdd_txgate_en) {
			// RX-open edge (period start) = TX-close; the gated pump completed
			// its zero pad 64 samples earlier (windowed-template close: fifo
			// holds zeros, no active write - docs-5q safe)
			vPhyTimerComparatorConfig(RFNM_TDD_TX_COMP,
					PHY_TIMER_COMPARATOR_CLEAR_INT | PHY_TIMER_COMPARATOR_CROSS_TRIG,
					ePhyTimerComparatorOut0, target);
		}
	}
	{
		int32_t margin = (int32_t)(target - rfnm_tdd_now());
		if(margin < rf_ctrl.tdd_worst_margin) {
			rf_ctrl.tdd_worst_margin = margin;
		}
	}
	rfnm_tdd_alarm(target);
}

// the v2 core: fires AT each edge, immediately arms the next one. Pure register/TCM
// writes (no RTOS calls) at NVIC prio 1; worst-case work is 6 comparator configs.
void rfnm_tdd_alarm_isr(void) {
	NVIC_ClearPendingIRQ(IRQ_PPS_OUT);
	// the RT command ring engine shares this alarm (mode-exclusive with the
	// grid); a live chain consumes its edge here and the grid body never runs
	if(rfnm_rtc_alarm_hook()) {
		return;
	}
	if(!rfnm_tdd.run) {
		vPhyTimerComparatorDisable(RFNM_TDD_ALARM_COMP);
		return;
	}
	if(!rfnm_tdd.pending_open) {
		// a close edge just fired: the open of the SAME cycle is next
		rfnm_tdd.pending_open = 1;
	} else {
		// an open edge just fired: cycle done, next cycle's close is next
		rf_ctrl.tdd_cycles++;
		rf_ctrl.tdd_last_open = rfnm_tdd.base + rfnm_tdd.period_ticks;
		rfnm_tdd.base += rfnm_tdd.period_ticks;
		rfnm_tdd.pending_open = 0;
	}
	rfnm_tdd_arm_edge(rfnm_tdd_now());
}

// full stop: engine off AND pattern forgotten. Called from the teardown-shaped apply
// (rx_mask==0): the M4's pattern state must NOT survive the session - the apply echoes
// rfnm_tdd_pattern_word() back to the VSPA, so a stale pattern re-arms itself on the
// next stream (and on every ksw regate re-apply), which seeded the v1 bring-up's
// session-2+ regate storms. The pending alarm arm is disarmed here too: the ISR only
// self-disarms when it FIRES with run==0.
void rfnm_tdd_stop(void) {
	rfnm_tdd.run = 0;
	rfnm_tdd.period_chunks = 0;
	rfnm_tdd.duty_chunks = 0;
	vPhyTimerComparatorDisable(RFNM_TDD_ALARM_COMP);
}

// swcmd entry (rfic task context): pattern in chunks; 0 = stop. The loop only (re)starts
// at the next stream anchor (rfnm_tdd_start_at from vRficProcessIqDump), which also hands
// the same pattern to the VSPA so the stamp chains step across the closed windows.
void rfnm_tdd_configure(uint32_t period_chunks, uint32_t duty_chunks) {
	if(period_chunks == 0 || duty_chunks == 0 || duty_chunks >= period_chunks ||
			period_chunks > 0xFFFF) {
		rfnm_tdd.run = 0;
		rfnm_tdd.period_chunks = 0;
		return;
	}
	rfnm_tdd.run = 0;
	rfnm_tdd.period_chunks = period_chunks;
	rfnm_tdd.duty_chunks = duty_chunks;
}

// (period_chunks << 16) | duty_chunks, the VSPA MBOX_OPC_TDD_PATTERN payload; 0 = off
uint32_t rfnm_tdd_pattern_word(void) {
	return rfnm_tdd.period_chunks ? ((rfnm_tdd.period_chunks << 16) | rfnm_tdd.duty_chunks) : 0;
}

void rfnm_tdd_start_at(uint32_t t0, uint32_t ticks_per_chunk) {
	uint32_t now;

	if(!rfnm_tdd.period_chunks) {
		return;
	}
	// mode exclusivity, grid side: only a TXGATE-latched pattern fights the ring
	// over C11. A windowed session's pattern (txgate unlatched - gNB composition)
	// runs RX gates + FE and coexists with an armed ring by design.
	if(rfnm_rtc_active() && rfnm_tdd_txgate_en) {
		log_err("tdd: start refused - rtc ring owns C11\r\n");
		return;
	}
	rfnm_tdd.run = 0;
	rfnm_tdd.period_ticks = rfnm_tdd.period_chunks * ticks_per_chunk;
	rfnm_tdd.duty_ticks = rfnm_tdd.duty_chunks * ticks_per_chunk;

	// the v2 span floor: below it the ISR chain cannot beat the M7 preload. REFUSE
	// loudly-in-telemetry instead of scattering edges (the v1 failure shape).
	if(rfnm_tdd.duty_ticks < RFNM_TDD_MIN_SPAN_TICKS ||
			rfnm_tdd.period_ticks - rfnm_tdd.duty_ticks < RFNM_TDD_MIN_SPAN_TICKS) {
		rf_ctrl.tdd_late_arms = 0xFFFFFFFF;
		return;
	}

	rf_ctrl.tdd_cycles = 0;
	rf_ctrl.tdd_late_arms = 0;
	rf_ctrl.tdd_worst_margin = 0x7FFFFFFF;
	rf_ctrl.tdd_last_open = 0;

	NVIC_SetPriority(IRQ_PPS_OUT, 1);
	NVIC_EnableIRQ(IRQ_PPS_OUT);

	// enter the chain AT THE OPEN EDGE (#66 K=-1 fix). The old close-first entry
	// armed the gates Out0@T0+duty from here, which DESTROYED the apply's pending
	// Out1@T0 arm - a comparator holds ONE pending arm and Config disables first -
	// so the gates never opened at T0: first real open = T0+period, window 0 dark,
	// every stamp label one period early forever (the realign chain is
	// self-referential and can never recover an anchor error). With base =
	// T0-period and pending_open=1 the first armed edge IS the open at exactly T0
	// (re-arming the value the apply armed); the first close chains from the T0
	// ISR with a full duty span of runway. The lead floor guards the first TARGET
	// (base+period); congruence mod period is preserved by whole-period stepping.
	rfnm_tdd.base = t0 - rfnm_tdd.period_ticks;
	now = rfnm_tdd_now();
	rf_ctrl.tdd_start_now = now;
	rf_ctrl.tdd_base_adv = 0;
	while((int32_t)((rfnm_tdd.base + rfnm_tdd.period_ticks) - now) <
			(int32_t)(2 * PHYTIMER_500_US_61p44)) {
		rfnm_tdd.base += rfnm_tdd.period_ticks;
		rf_ctrl.tdd_base_adv++;
	}
	// reporting semantics unchanged: tdd_base = the first gate-open tick (== mint_t0
	// whenever no lead advance fired)
	rf_ctrl.tdd_base = rfnm_tdd.base + rfnm_tdd.period_ticks;
	rfnm_tdd.pending_open = 1;
	rfnm_tdd.run = 1;
	if(rfnm_tdd_txgate_en) {
		// explicit initial gate state (one pending arm per comparator): TX stays
		// CLOSED from here - the stream apply skips its C11-open-at-T0 arm when
		// this engine owns the gate, so the span-head prime pends until the first
		// RX-close edge below releases it (Out1 + CROSS_TRIG). The force makes
		// the state independent of whatever the warmup pulse or a previous
		// session left behind.
		vPhyTimerComparatorForce(RFNM_TDD_TX_COMP, ePhyTimerComparatorOut0);
		log_err("tdd: txgate armed p=%u d=%u base=%u\r\n",
				(unsigned)rfnm_tdd.period_ticks, (unsigned)rfnm_tdd.duty_ticks,
				(unsigned)rfnm_tdd.base);
	}
	rfnm_tdd_arm_edge(now);
}

int rfnm_tdd_init(void) {
	// v2: no task - the alarm ISR owns the loop; init kept for main.c compatibility
	return 0;
}

// 0 when no pattern is configured: the anchor congruence step falls back to one chunk
uint32_t rfnm_tdd_duty_chunks(void) {
	return rfnm_tdd.duty_chunks;
}

// tick-domain pattern accessors (valid after rfnm_tdd_start_at computed them; 0 when
// the engine is not engaged) - the TDD-aware parked-at-start watchdog sizes its
// GO-silence window off these
uint32_t rfnm_tdd_period_ticks(void) {
	return rfnm_tdd.run ? rfnm_tdd.period_ticks : 0;
}

uint32_t rfnm_tdd_duty_ticks(void) {
	return rfnm_tdd.run ? rfnm_tdd.duty_ticks : 0;
}

uint32_t rfnm_tdd_period_chunks(void) {
	return rfnm_tdd.period_chunks;
}