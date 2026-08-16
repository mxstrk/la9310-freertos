/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright 2021 NXP
 */
#include <rfic_avi_ctrl.h>
#include "delay.h"	/* vUDelay */

void vLa9310MbxSend( struct la9310_mbox_h2v *mbox_h2v )
{
    struct avi_hndlr *avihndl = NULL;

    avihndl = iLa9310AviHandle();
    //log_info("\r\n ****H2V: MSB_LSB(Hex):%x::%x\r\n",
    //	(*(uint16_t *)(&mbox_h2v->ctrl)) << 16 |
    //	mbox_h2v->msbl16, mbox_h2v->lsb32);
    if( NULL != avihndl )
    {
        if( 0 != iLa9310AviHostSendMboxToVspa( avihndl, (*(uint16_t *)(&mbox_h2v->ctrl)) << 16 | mbox_h2v->msbl16,
                                mbox_h2v->lsb32, 0 ))
        {
            log_err( "\r\n***ERR: Send Host MBOX 0 Fail\n\r" );
        }
    }
    else
    {
        log_err( "MBox AVIhandler NULL\n\r" );
    }
    return;
}

// Pairing hygiene (the TDD engagement lottery, charter 07-15): a timed-out
// exchange leaves its LATE ack in the VSPA outbox, and every later
// Send/Receive pair then reads its PREDECESSOR's ack - off-by-one that
// persists across sessions (the capability latch read a stale msb32=0 and
// txgate engaged bimodally). Drain stale messages before a pairing-critical
// exchange; returns how many were discarded (nonzero = a desync existed).
uint32_t vLa9310MbxDrain( void )
{
    struct avi_hndlr *avihndl = iLa9310AviHandle();
    struct avi_mbox vspa_mbox;
    uint32_t n = 0;

#ifdef LA9310_HOST_OWNS_MBOX0
    /* MBOX0 is the host's: there is nothing here for us to drain, and every
     * attempt would be refused. Nothing stale can exist from our side either,
     * since we never post to it. */
    return 0;
#endif

    if( NULL != avihndl )
    {
        while( n < 4 && 0 == iLa9310AviHostRecvMboxFromVspa( avihndl, &vspa_mbox, 0 ) )
        {
            n++;
        }
        if( n )
        {
            log_err( "mbox drain: %u stale ack(s) discarded (pairing resync)\r\n", ( unsigned ) n );
        }
    }
    return n;
}

BaseType_t vLa9310MbxReceive(struct la9310_mbox_v2h *mbox_v2h)
{
    struct avi_hndlr *avihndl = NULL;
    struct avi_mbox vspa_mbox;
    uint32_t retries = 0;

#ifdef LA9310_HOST_OWNS_MBOX0
    /* MBOX0 is the host's, so this can never succeed. Fail immediately instead
     * of spending the full retry budget on it: the loop below polls 2000 times
     * with a 10 us yield, i.e. ~20 ms of M4 time per call, and every poll would
     * be refused. */
    return pdFAIL;
#endif

    avihndl = iLa9310AviHandle();
    if( NULL != avihndl )
    {
        while (retries < MAILBOX_VALID_STATUS_RETRIES * 10)
        {
            /* Read VSPA inbox 0. The AVI receive is non-blocking now (v3 transport
             * fix - its old 10-tick xQueueReceive block starved the host-swcmd
             * event wake), so this loop provides the bounded wait: 10 us yields
             * give ~10 us ack wake latency with a ~20 ms total budget. */
            if ( 0 == iLa9310AviHostRecvMboxFromVspa(avihndl, &vspa_mbox, 0 ))
            {
                mbox_v2h->msb32 = vspa_mbox.msb;
                *((uint32_t *)(&mbox_v2h->status))  = vspa_mbox.lsb;
        //        log_info("\r\n **V2H: MSB_LSB(Hex):%x::%x retries %d\r\n", mbox_v2h->msb32, *((uint32_t *)(&mbox_v2h->status)), retries);
                return pdPASS;
            }
            vUDelay( 10 );
            retries++;
        }
        if (retries == MAILBOX_VALID_STATUS_RETRIES)
            log_err("VSPA timeout\r\n");
    }
    else
    {
        log_err("MBox AVIhandler NULL\n\r");
    }
    return pdFAIL;
}
