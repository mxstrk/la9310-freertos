/* SPDX-License-Identifier: BSD-3-Clause */

/*
 * Copyright 2026 RFNM
 *
 * The RT command ring consumer. Design ruling: this engine is the sole owner
 * of every phytimer comparator arm - the kernel never touches comparators, it
 * only queues absolute-tick verbs (common_headers/rfnm_rtc_ring.h) which this
 * engine executes on the comparators. ARM_TX_WINDOW drives BOTH C11 edges
 * for the windowed TX substrate: open at tick (Out1), close at tick + len
 * (Out0), each edge armed from the alarm ISR exactly like the TDD grid - one
 * comparator holds one pending arm, so chaining is the only correct shape.
 * The retired txn walker solved this with task-context busy-waits (a whole
 * window per pass); this engine keeps the task OUT of the timing path: the
 * doorbell-notified task only validates, takes/returns C11 ownership and kicks
 * an idle chain - every steady-state arm is ISR work (pure register/TCM writes
 * at NVIC prio 1, same discipline as rfnm_tdd_alarm_isr).
 *
 * No M7 preload rides this path (FE flips are the FE_STATE verb, chartered
 * separately), so the floors here are ISR-latency class, not the grid's 600 us:
 * task-kick lead 50 us, ISR chain lead 4 us, minimum window 8 us. The kernel's
 * published client contract stays 267 us (owner ruling 07-18) - these are
 * internal floors, bench-measured via the worst_margin telemetry.
 *
 * Ownership: ring armed -> this engine owns C11, held CLOSED between windows
 * (windowed silence is hardware). Disarm (magic clear) -> C11 forced OPEN,
 * ownership back to the legacy apply/free-run path. The TDD grid and this
 * engine are mode-exclusive (kernel session latch); both sides also refuse
 * defensively and loudly.
 */

#include "FreeRTOS.h"
#include "task.h"
#include "common.h"
#include "phytimer.h"
#include "rfnm_rf_ctrl.h"
#include "rfnm_rtc_ring.h"

#define RFNM_RTC_TX_COMP	PHY_TIMER_COMP_CH5_TX_ALLOWED	/* the C11 gate */
#define RFNM_RTC_ALARM_COMP	PHY_TIMER_COMP_PPS_OUT		/* shared with the grid, mode-exclusive */

#define RFNM_RTC_MIN_LEAD_TASK	( 3072 )	/* 50 us: notify wake + validate + arm writes */
#define RFNM_RTC_MIN_LEAD_ISR	( 256 )		/* 4.2 us: arming within ~us of the previous edge */
#define RFNM_RTC_MIN_LEN_TICKS	( 512 )		/* 8.3 us: the close arm needs ISR-latency room */
#define RFNM_RTC_MAX_LEN_TICKS	( 0x40000000 )

#define RFNM_RTC_RING_P ((volatile struct rfnm_rtc_ring *)RFNM_RTC_RING_M4_ADDR)

/* chain phases. IDLE with owned=1 is the legitimate between-schedules state:
 * gate closed, alarm disabled, next doorbell kicks the chain again. */
#define RFNM_RTC_IDLE		( 0 )
#define RFNM_RTC_WAIT_OPEN	( 1 )
#define RFNM_RTC_WAIT_CLOSE	( 2 )

static TaskHandle_t rfnm_rtc_task_handle;
static volatile uint32_t rfnm_rtc_phase;
static volatile uint32_t rfnm_rtc_close_tick;	/* close of the in-flight window */
static volatile uint32_t rfnm_rtc_owned;	/* this engine holds C11 */

static inline uint32_t rfnm_rtc_now(void) {
	return ulPhyTimerCapture(PHY_TIMER_COMPARATOR_COUNT - 1);
}

static inline void rfnm_rtc_alarm(uint32_t ts) {
	vPhyTimerComparatorConfig(RFNM_RTC_ALARM_COMP,
			PHY_TIMER_COMPARATOR_CLEAR_INT | PHY_TIMER_COMPARATOR_CROSS_TRIG,
			ePhyTimerComparatorOutToggle, ts);
}

static inline void rfnm_rtc_margin(uint32_t target) {
	int32_t margin = (int32_t)(target - rfnm_rtc_now());
	if(margin < RFNM_RTC_RING_P->worst_margin) {
		RFNM_RTC_RING_P->worst_margin = margin;
	}
}

uint32_t rfnm_rtc_active(void) {
	return rfnm_rtc_owned;
}

// pop entries until one is armable, then arm its open edge + the chained alarm.
// Runs with the alarm IRQ quiet (either inside the ISR or with it NVIC-masked
// from the task): phase/cons are single-writer by construction here.
static void rfnm_rtc_advance(uint32_t min_lead) {
	volatile struct rfnm_rtc_ring *r = RFNM_RTC_RING_P;

	while(r->cons != r->prod) {
		volatile struct rfnm_rtc_cmd *e = &r->e[r->cons % RFNM_RTC_RING_ENTRIES];

		if(e->kind != RFNM_RTC_ARM_TX_WINDOW ||
				e->len < RFNM_RTC_MIN_LEN_TICKS || e->len > RFNM_RTC_MAX_LEN_TICKS) {
			// FE_STATE/RX_WINDOW/SPI land in later steps: until then they
			// reject loudly rather than silently vanish
			r->rejected++;
			r->cons++;
			continue;
		}
		if((int32_t)(e->tick - rfnm_rtc_now()) < (int32_t)min_lead) {
			// late: skipped and counted, never executed late
			r->missed++;
			r->cons++;
			continue;
		}
		rfnm_rtc_close_tick = e->tick + e->len;
		vPhyTimerComparatorConfig(RFNM_RTC_TX_COMP,
				PHY_TIMER_COMPARATOR_CLEAR_INT | PHY_TIMER_COMPARATOR_CROSS_TRIG,
				ePhyTimerComparatorOut1, e->tick);
		rfnm_rtc_alarm(e->tick);
		rfnm_rtc_margin(e->tick);
		r->last_open = e->tick;
		rfnm_rtc_phase = RFNM_RTC_WAIT_OPEN;
		return;
	}
	rfnm_rtc_phase = RFNM_RTC_IDLE;
}

// alarm ISR hook, called from rfnm_tdd_alarm_isr (prio 1, no RTOS calls).
// Returns 1 when the edge belonged to this engine.
int rfnm_rtc_alarm_hook(void) {
	volatile struct rfnm_rtc_ring *r = RFNM_RTC_RING_P;

	if(rfnm_rtc_phase == RFNM_RTC_WAIT_OPEN) {
		// the open edge fired: arm the close on the same comparator
		vPhyTimerComparatorConfig(RFNM_RTC_TX_COMP,
				PHY_TIMER_COMPARATOR_CLEAR_INT | PHY_TIMER_COMPARATOR_CROSS_TRIG,
				ePhyTimerComparatorOut0, rfnm_rtc_close_tick);
		rfnm_rtc_margin(rfnm_rtc_close_tick);
		r->last_close = rfnm_rtc_close_tick;
		if((int32_t)(rfnm_rtc_close_tick - rfnm_rtc_now()) <= 0) {
			// a delayed ISR armed past the edge: the compare (and its alarm)
			// would only fire after a ~70 s wrap. Force the close (idempotent
			// if the compare did land) and chain on immediately - the window
			// ran long, never wedged.
			vPhyTimerComparatorForce(RFNM_RTC_TX_COMP, ePhyTimerComparatorOut0);
			r->executed++;
			r->cons++;
			rfnm_rtc_advance(RFNM_RTC_MIN_LEAD_ISR);
			if(rfnm_rtc_phase == RFNM_RTC_IDLE) {
				vPhyTimerComparatorDisable(RFNM_RTC_ALARM_COMP);
			}
			return 1;
		}
		rfnm_rtc_alarm(rfnm_rtc_close_tick);
		rfnm_rtc_phase = RFNM_RTC_WAIT_CLOSE;
		return 1;
	}
	if(rfnm_rtc_phase == RFNM_RTC_WAIT_CLOSE) {
		// the close edge fired: window done, chain the next one
		r->executed++;
		r->cons++;
		rfnm_rtc_advance(RFNM_RTC_MIN_LEAD_ISR);
		if(rfnm_rtc_phase == RFNM_RTC_IDLE) {
			vPhyTimerComparatorDisable(RFNM_RTC_ALARM_COMP);
		}
		return 1;
	}
	return 0;	/* IDLE: not ours - the grid's edge */
}

// host doorbell (MSG1 bit RFNM_RTC_DOORBELL_MSG_UNIT_BIT): a push/flush/disarm
// just landed - cut whatever wait the task is in short now
void vRfnmRtcDoorbellFromISR(void) {
	BaseType_t xHigherPriorityTaskWoken = pdFALSE;

	if(rfnm_rtc_task_handle) {
		vTaskNotifyGiveFromISR(rfnm_rtc_task_handle, &xHigherPriorityTaskWoken);
		portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
	}
}

// kill a live chain and drop everything queued. Alarm IRQ must be NVIC-masked
// by the caller. C11 is left to the caller (flush holds it closed, disarm opens).
static void rfnm_rtc_chain_kill(volatile struct rfnm_rtc_ring *r) {
	vPhyTimerComparatorDisable(RFNM_RTC_ALARM_COMP);
	rfnm_rtc_phase = RFNM_RTC_IDLE;
	r->cons = r->prod;
}

static void rfnm_rtc_task(void *arg) {
	volatile struct rfnm_rtc_ring *r = RFNM_RTC_RING_P;

	for( ;; ) {
		if(r->magic != RFNM_RTC_RING_MAGIC) {
			if(rfnm_rtc_owned) {
				// disarm: ownership back to the legacy path, gate OPEN
				NVIC_DisableIRQ(IRQ_PPS_OUT);
				rfnm_rtc_chain_kill(r);
				vPhyTimerComparatorForce(RFNM_RTC_TX_COMP, ePhyTimerComparatorOut1);
				rfnm_rtc_owned = 0;
				r->disarms++;
				NVIC_EnableIRQ(IRQ_PPS_OUT);
				log_err("rtc: disarmed (exec %u miss %u rej %u)\r\n",
						(unsigned)r->executed, (unsigned)r->missed, (unsigned)r->rejected);
			}
			ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
			continue;
		}

		if(!rfnm_rtc_owned) {
			// arm attempt: refuse while the grid owns C11. Ownership = the txgate
			// latch alone: a pattern in a WINDOWED session runs RX gates + FE with
			// txgate unlatched, and the ring must arm freely alongside it (gNB
			// composition; the old period-nonzero refusal starved every window's
			// open-edge release - GC6: 21/200 aimed windows aired).
			if(rfnm_tdd_txgate()) {
				r->refused++;
				ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
				continue;
			}
			// fresh arm: consumer-owned state starts clean (producer rewrote
			// the header before setting magic; cons is ours alone)
			r->cons = 0;
			r->gen_seen = r->gen;
			r->executed = 0;
			r->missed = 0;
			r->rejected = 0;
			r->disarms = 0;
			r->worst_margin = 0x7FFFFFFF;
			rfnm_rtc_phase = RFNM_RTC_IDLE;
			// windowed silence is hardware: gate CLOSED between windows. On a
			// never-enabled comparator (no apply ran yet) the Force is a no-op
			// by the phytimer caution - the first armed open edge enables and
			// owns the line from then on.
			vPhyTimerComparatorForce(RFNM_RTC_TX_COMP, ePhyTimerComparatorOut0);
			rfnm_rtc_owned = 1;
			NVIC_SetPriority(IRQ_PPS_OUT, 1);
			NVIC_EnableIRQ(IRQ_PPS_OUT);
			log_err("rtc: armed (gen %u prod %u)\r\n", (unsigned)r->gen, (unsigned)r->prod);
		}

		if(r->gen != r->gen_seen) {
			// producer flush: drop queued, kill the chain, stay armed + closed
			NVIC_DisableIRQ(IRQ_PPS_OUT);
			rfnm_rtc_chain_kill(r);
			vPhyTimerComparatorForce(RFNM_RTC_TX_COMP, ePhyTimerComparatorOut0);
			r->gen_seen = r->gen;
			NVIC_EnableIRQ(IRQ_PPS_OUT);
		}

		// kick an idle chain (fresh pushes); serialize vs the alarm ISR
		NVIC_DisableIRQ(IRQ_PPS_OUT);
		if(rfnm_rtc_phase == RFNM_RTC_IDLE && r->cons != r->prod) {
			rfnm_rtc_advance(RFNM_RTC_MIN_LEAD_TASK);
		}
		NVIC_EnableIRQ(IRQ_PPS_OUT);

		// the ISR owns the steady state; the doorbell re-wakes us for pushes,
		// flushes and disarms. 100 ms is the no-doorbell safety net.
		ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
	}
}

int rfnm_rtc_init(void) {
	// make the ring header sane before the host finds it (magic stays 0 until
	// the PRODUCER writes it - the consumer never trusts an unset ring)
	volatile struct rfnm_rtc_ring *r = RFNM_RTC_RING_P;
	r->magic = 0;
	r->gen = 0;
	r->prod = 0;
	r->cons = 0;
	r->executed = 0;
	r->missed = 0;
	r->rejected = 0;
	r->refused = 0;
	r->gen_seen = 0;
	r->disarms = 0;
	r->worst_margin = 0x7FFFFFFF;
	r->last_open = 0;
	r->last_close = 0;
	return xTaskCreate(rfnm_rtc_task, "rfnm rtc", configMINIMAL_STACK_SIZE * 2,
			NULL, tskIDLE_PRIORITY + 2, &rfnm_rtc_task_handle) == pdPASS ? 0 : -1;
}
