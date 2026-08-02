/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The RT command ring: the kernel's one command plane into the real-time cores
 * (ruling: the host never arms comparators directly - it only posts
 * absolute-tick verbs). Producer = host
 * kernel (posted PCIe writes into LA9310 TCM + the MSG1 doorbell), consumer =
 * the M4 rtc engine (rfnm_rtc.c), which stays the sole owner of every phytimer
 * comparator arm.
 *
 * Verbs are typed ring entries stamped with the ABSOLUTE phytimer tick at which
 * they execute. ARM_TX_WINDOW is live; FE_STATE / RX_WINDOW / SPI_WRITE are
 * chartered and reserved here so the contract never re-numbers.
 *
 * Discipline (inherited from the retired txn ring):
 *  - entries monotonic by tick; a late entry is SKIPPED and counted, never
 *    executed late
 *  - repetition is REFILL (no loop mode; the host unrolls patterns)
 *  - gen bump = drop everything queued, chain killed, C11 held CLOSED (the
 *    session continues windowed); magic clear = disarm (C11 returns to the
 *    legacy OPEN state, ownership back to the free-run/apply path)
 *  - SPSC: the kernel writes ONLY magic/gen/prod + entries; the M4 owns cons +
 *    every telemetry field. Arming: producer writes prod=0, pre-loads any
 *    entries, sets magic LAST, rings the doorbell; the consumer treats the
 *    magic edge as start-from-entry-0 and zeroes its own counters there.
 *
 * Home: M4-local 0x1f818000 (host TCML 0x1c000000 + 0x18000), the unused TCM
 * gap between m_data end (0x1f817740) and the HIF (0x1f81c000). 16 KB max.
 * The retired txn ring lived at this address with a DIFFERENT magic, so a
 * stale producer on either side can never arm the wrong consumer.
 */

#ifndef RFNM_RTC_RING_H_
#define RFNM_RTC_RING_H_

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

#ifndef RFNM_PACKED_STRUCT
#define RFNM_PACKED_STRUCT( __Declaration__ ) __Declaration__ __attribute__((__packed__))
#endif

#define RFNM_RTC_RING_M4_ADDR    ( 0x1f818000 )
#define RFNM_RTC_RING_HOST_OFF   ( 0x18000 )	/* from TCML_START (0x1c000000) */

/* 16-byte entries; 512 entries = 8 KB + a 64 B header, half the gap */
#define RFNM_RTC_RING_ENTRIES    ( 512 )

/* verbs */
#define RFNM_RTC_EMPTY           ( 0 )	/* not yet written / consumed slot */
#define RFNM_RTC_ARM_TX_WINDOW   ( 1 )	/* C11 open at tick, close at tick + len */
#define RFNM_RTC_FE_STATE        ( 2 )	/* reserved: FE flip via the M7 at tick */
#define RFNM_RTC_RX_WINDOW       ( 3 )	/* reserved: RX allow-gates at tick */
#define RFNM_RTC_SPI_WRITE       ( 4 )	/* reserved: phase-2 changeover SPI */

RFNM_PACKED_STRUCT(
struct rfnm_rtc_cmd {
	uint32_t tick;	/* absolute phytimer tick - the entry's identity */
	uint8_t  kind;
	uint8_t  flags;
	uint16_t type;	/* verb-private (unused by ARM_TX_WINDOW) */
	uint32_t len;	/* ARM_TX_WINDOW: window length in TICKS (close = tick + len) */
	uint32_t bind;	/* verb-private (unused by ARM_TX_WINDOW) */
}
);

RFNM_PACKED_STRUCT(
struct rfnm_rtc_ring {
	uint32_t magic;		/* RFNM_RTC_RING_MAGIC: consumer ignores the ring until set */
	uint32_t gen;		/* producer bumps to flush; consumer drops everything queued */
	uint32_t prod;		/* producer index, free-running (kernel writes) */
	uint32_t cons;		/* consumer index, free-running (M4 writes) */
	/* consumer telemetry, host-readable (honesty: nothing is silently late) */
	uint32_t executed;	/* windows fully run (counted at the close edge) */
	uint32_t missed;	/* skipped: below the arm floor at arm time */
	uint32_t rejected;	/* malformed: bad kind/len/non-monotonic tick */
	uint32_t refused;	/* arm attempts while the TDD grid owned C11 */
	uint32_t gen_seen;	/* last gen the consumer acknowledged */
	uint32_t disarms;	/* magic-clear teardowns observed */
	int32_t  worst_margin;	/* min (target - now) across all comparator arms, ticks */
	uint32_t last_open;	/* last armed open tick */
	uint32_t last_close;	/* last armed close tick */
	uint32_t reserved[3];
	struct rfnm_rtc_cmd e[RFNM_RTC_RING_ENTRIES];
}
);

#define RFNM_RTC_RING_MAGIC      ( 0x52544350 )	/* 'RTCP' */

/* host->M4 doorbell: MSG unit 1, the bit the txn ring used (dispatch restored
 * in la9310_irq.c). Raised after every push / gen bump / magic change. */
#define RFNM_RTC_DOORBELL_MSG_UNIT_BIT   ( 2 )

#endif /* RFNM_RTC_RING_H_ */
