/* SPDX-License-Identifier: BSD-3-Clause */

/*
 * Copyright 2017, 2021-2022 NXP
 */


#include "la9310_avi.h"
#include "la9310_avi_ds.h"
#include "la9310_error_codes.h"
#include "debug_console.h"
#include "la9310_irq.h"
#include "semphr.h"

struct avi_hndlr * pAviHndlr = NULL;
struct la9310_evt_hdlr VspaEvtHdlr;
void (* IntHndlr)( void ) = NULL;

/* Host<->M4 MBOX0 exclusion.
 *
 * IQ Player drives VSPA mailbox 0 directly from the PCIe host
 * (host-utils/vspa_mbox over /dev/mem) and the VSPA firmware replies there.
 * With the stock mask the M4's AVI ISR also watches MBOX0: it reads
 * host_in_0_msb/lsb and then clears VSPA_MBOX0_STATUS, so the reply is gone
 * before the host can read it and vspa_mbox reports
 * "MBox:0 is not responding".
 *
 * NXP hit the same wall and added a runtime mask upstream (la93xx_freertos
 * 7617c98, "Host entity needs to exclusively use a VSPA MBOX, thus BSP ...
 * should not interfere") 18 months after this tree forked. Backported here as
 * a compile-time constant, because this tree has no DFE app and therefore no
 * caller for a runtime setter.
 *
 * Undefined, this folds to VSPA_MBOX_MASK == VSPA_ENABLE_MAILBOX_IRQ == 0xF000
 * and every added term compiles away, so the stock image stays bit-identical.
 */
#ifdef LA9310_HOST_OWNS_MBOX0
    #define AVI_MBOX_MONITOR_MASK    ( CM4_MBOX1_STATUS | VSPA_MBOX1_STATUS )
#else
    #define AVI_MBOX_MONITOR_MASK    VSPA_MBOX_MASK
#endif

void vVSPAMboxInit()
{
    OUT_32( PHY_TIMER, 0x1 );
    OUT_32( TRIGGER_1, 0x84 );
    OUT_32( TRIGGER_1, 0x8 );
    OUT_32( TRIGGER_2, 0x84 );
    OUT_32( TRIGGER_2, 0x8 );
    OUT_32( TRIGGER_3, 0x84 );
    OUT_32( TRIGGER_3, 0x8 );
    OUT_32( TRIGGER_4, 0x84 );
    OUT_32( TRIGGER_4, 0x8 );
    OUT_32( TRIGGER_11, 0x8 );
    #ifdef AXIQ_LOOPBACK_ENABLE
        /* AXIQ loopback on RX1 */
        OUT_32( DBGGNCR, 0x0000005e );
    #endif
    return;
}

int iLa9310AviHostSendMboxToVspa( void * AviHndlr,
                                  uint32_t mbox_msb,
                                  uint32_t mbox_lsb,
                                  uint32_t mbox_index )
{
    struct vspa_regs * pVspaRegs = ( ( struct avi_hndlr * ) AviHndlr )->pVspaRegs;
    struct la9310_stats * pStats = pLa9310Info->stats;
    struct avi_mbox Cm4Mbox;
    int retval = 0;

    log_dbg( "%s: In\n\r", __func__ );

    /* Check if AVI init is done or not */
    if( NULL == pAviHndlr )
    {
        log_err( "ERR: AVI library INIT not done\n\r",
                 mbox_index );
        retval = -AVI_IPC_INIT_NOT_DONE;
        goto hndl_retval;
    }

    /* Invalid mbox_index*/
    if( mbox_index > 1 )
    {
        log_err( "ERR: Invalid MBOX index [%d]\n\r",
                 mbox_index );
        retval = -AVI_INVALID_MBOX_INDEX;
        goto hndl_retval;
    }

#ifdef LA9310_HOST_OWNS_MBOX0
    /* MBOX0 belongs to the PCIe host (IQ Player). This write path is NOT
     * driven by the ISR -- it stores host_out_0_msb/lsb directly -- so masking
     * the ISR alone would still let an RFIC command clobber a host round-trip
     * in flight. Refuse it.
     *
     * Log only the first refusal: callers retry in bounded loops, so an
     * unconditional log_err() here floods the target log (see the recv path
     * below, which is polled up to 2000 times per call). */
    if( 0 == mbox_index )
    {
        static uint8_t ucSendRefusedLogged = 0;

        if( !ucSendRefusedLogged )
        {
            ucSendRefusedLogged = 1;
            log_err( "MBOX0 reserved for host (IQ Player), send refused"
                     " -- further refusals are silent\n\r" );
        }

        retval = -AVI_MBOX_NOT_AVAILABLE;
        goto hndl_retval;
    }
#endif

    /* Check if MBOX MUTEX is available */
    if( 0 == mbox_index )
    {
        xSemaphoreTake( ( ( struct avi_hndlr * ) AviHndlr )->Cm4ToVspaQMutex0,
                        WAIT_TIMEOUT_FOR_MBOX );
    }
    else
    {
        xSemaphoreTake( ( ( struct avi_hndlr * ) AviHndlr )->Cm4ToVspaQMutex1,
                        WAIT_TIMEOUT_FOR_MBOX );
    }

    Cm4Mbox.msb = mbox_msb;
    Cm4Mbox.lsb = mbox_lsb;

    /* Check if VSPA read the last mailbox or not
     * if VSPA has not read the last mailbox push it into the queue
     * */
    if( IN_32( &pVspaRegs->host_mbox_status ) &
        ( 1 << mbox_index ) )
    {
        /* Host to VCPU mailbox is still pending
         * Enqueue the mailbox to the appropriate
         * Mailbox Queue */
        log_dbg( "INFO: MBOX[%d] occupied, Putting into Queue\n\r",
                 mbox_index );

        if( 0 == mbox_index )
        {
            if( pdTRUE != xQueueSendToBackFromISR( ( ( struct avi_hndlr * ) AviHndlr )->Cm4ToVspaQMbox0,
                                                   &Cm4Mbox, 0 ) )
            {
                log_err( "ERR:%s: Cm4ToVspaQMbox0 Full\n\r", __func__ );
                pStats->avi_err_queue_full++;
                retval = -1;
            }
        }
        else
        {
            if( pdTRUE != xQueueSendToBack( ( ( struct avi_hndlr * ) AviHndlr )->Cm4ToVspaQMbox1,
                                            &Cm4Mbox, 0 ) )
            {
                log_err( "ERR:%s: Cm4ToVspaQMbox1 Full\n\r", __func__ );
                pStats->avi_err_queue_full++;
                retval = -AVI_MBOX_NOT_AVAILABLE;
            }
        }
    }
    else
    {
        log_dbg( "INFO: Send MBOX[%d] to VSPA\n\r",
                 mbox_index );

        if( 0 == mbox_index )
        {
            OUT_32( &pVspaRegs->host_out_0_msb, mbox_msb );
            dmb();
            OUT_32( &pVspaRegs->host_out_0_lsb, mbox_lsb );
            pStats->avi_cm4_mbox0_tx_cnt++;
        }
        else
        {
            OUT_32( &pVspaRegs->host_out_1_msb, mbox_msb );
            dmb();
            OUT_32( &pVspaRegs->host_out_1_lsb, mbox_lsb );
            pStats->avi_cm4_mbox1_tx_cnt++;
        }
    }

hndl_retval:

    if( 0 == mbox_index )
    {
        xSemaphoreGive( ( ( struct avi_hndlr * ) AviHndlr )->Cm4ToVspaQMutex0 );
    }
    else
    {
        xSemaphoreGive( ( ( struct avi_hndlr * ) AviHndlr )->Cm4ToVspaQMutex1 );
    }

    log_dbg( "%s: Out\n\r", __func__ );
    return retval;
}

int iLa9310AviHostRecvMboxFromVspa( void * AviHndlr,
                                    struct avi_mbox * mbox,
                                    uint32_t mbox_index )
{
    int retval = AVI_MBOX_RCV_FAIL;
    struct la9310_stats * pStats = pLa9310Info->stats;

    log_dbg( "%s: In\n\r", __func__ );

    /* Check if AVI init is done or not */
    if( NULL == pAviHndlr )
    {
        log_err( "ERR: AVI library INIT not done\n\r",
                 mbox_index );
        retval = -AVI_IPC_INIT_NOT_DONE;
        goto hndl_retval;
    }

    /* Invalid mbox_index*/
    if( mbox_index > 1 )
    {
        log_err( "ERR: Invalid MBOX index [%d]\n\r",
                 mbox_index );
        retval = AVI_INVALID_MBOX_INDEX;
        goto hndl_retval;
    }

#ifdef LA9310_HOST_OWNS_MBOX0
    /* MBOX0 belongs to the PCIe host (IQ Player): draining it here would eat
     * the reply the host is waiting for.
     *
     * First refusal only. vLa9310MbxReceive() polls this up to 2000 times per
     * call, so logging every refusal fills the target log within seconds. The
     * RFIC callers are short-circuited in rfic_avi_ctrl.c / rfic_core.c so they
     * do not spin here at all; this stays as the backstop for any other caller. */
    if( 0 == mbox_index )
    {
        static uint8_t ucRecvRefusedLogged = 0;

        if( !ucRecvRefusedLogged )
        {
            ucRecvRefusedLogged = 1;
            log_err( "MBOX0 reserved for host (IQ Player), recv refused"
                     " -- further refusals are silent\n\r" );
        }

        retval = AVI_MBOX_RCV_FAIL;
        goto hndl_retval;
    }
#endif

    if( 0 == mbox_index )
    {
        log_dbg( "Read VSPA MBOX[%d]\n\r",
                 mbox_index );

        // v3 transport fix: NON-BLOCKING receive. The 10-tick block here was fatal:
        // the rfic core task's idle loop drains this queue, and while it sat inside
        // the queue wait, the host-swcmd EVENT GROUP (a different primitive) could
        // not wake it - measured 0.2-10 ms swcmd delivery scatter = the residual of
        // this timeout. Blocking ack waits now live in vLa9310MbxReceive's retry
        // loop (bounded, us-granular) instead.
        if( pdPASS == xQueueReceive( ( ( struct avi_hndlr * ) AviHndlr )->VspaToCm4QMbox0,
                                     mbox, ( TickType_t ) 0 ) )
        {
            log_dbg( "Rcvd VSPA MBOX[%d] msb_lsb %x_%x\n\r",
                     mbox_index, mbox->msb, mbox->lsb );
            pStats->avi_cm4_mbox0_rx_cnt++;
            retval = 0;
        }
    }
    else
    {
        log_info( "Read VSPA MBOX[%d]\n\r",
                  mbox_index );

        if( pdPASS == xQueueReceive( ( ( struct avi_hndlr * ) AviHndlr )->VspaToCm4QMbox1,
                                     mbox, ( TickType_t ) 0 ) )
        {
            log_dbg( "Rcvd VSPA MBOX[%d] msb_lsb %x_%x\n\r",
                     mbox_index, mbox->msb, mbox->lsb );
            pStats->avi_cm4_mbox1_rx_cnt++;
            retval = 0;
        }
    }

hndl_retval:
    log_dbg( "%s: Out\n\r", __func__ );
    return retval;
}

/* Host<->M4 mailbox ownership: while a host VSPA boot-handshake runs, the M4
 * must not consume VSPA outbox messages (its ISR read eats the host's Boot Complete
 * - reads ARE the consume, peeking is impossible). The host commands the window via
 * RF_SW_CMD_VSPA_MBOX_HANDOFF; the VSPA IRQ is masked for the duration and any
 * message latched meanwhile is consumed normally after re-enable. */
volatile int g_vspa_mbox_handoff;

void vAviVspaMboxHandoff( int on )
{
    g_vspa_mbox_handoff = on;

    if( on )
    {
        NVIC_DisableIRQ( IRQ_VSPA );
    }
    else
    {
        NVIC_EnableIRQ( IRQ_VSPA );
    }
}

void AviHndleMboxInterrupt( struct avi_hndlr * AviHndlr )
{
    if( g_vspa_mbox_handoff )
    {
        return; /* host owns the mailboxes for the handshake window */
    }

    struct vspa_regs * pVspaRegs = ( struct vspa_regs * ) AviHndlr->pVspaRegs;
    struct avi_mbox vspambox;

    log_dbg( "status %X %s: In\n\r", IN_32( &pVspaRegs->vspa_status ), __func__ );

    /* Check which MBOX is set and enqueue the message
     * to the respective Queue
     * */

    if( IN_32( &pVspaRegs->vspa_status ) & ( VSPA_MBOX0_STATUS & AVI_MBOX_MONITOR_MASK ) )
    {
        log_dbg( "%s: Rcvd mbox0 from VSPA\n\r", __func__ );
        vspambox.msb = IN_32( &pVspaRegs->host_in_0_msb );
        vspambox.lsb = IN_32( &pVspaRegs->host_in_0_lsb );

        if( pdTRUE != xQueueSendToBackFromISR( AviHndlr->VspaToCm4QMbox0,
                                               &vspambox, NULL ) )
        {
            log_dbg( "ERR:%s: VspaToCm4QMbox0 Full\n\r", __func__ );
        }

        OUT_32( &pVspaRegs->vspa_status, VSPA_MBOX0_STATUS );
    }
    else if( IN_32( &pVspaRegs->vspa_status ) & ( VSPA_MBOX1_STATUS & AVI_MBOX_MONITOR_MASK ) )
    {
        log_dbg( "%s: Rcvd mbox1 from VSPA\n\r", __func__ );
        vspambox.msb = IN_32( &pVspaRegs->host_in_1_msb );
        vspambox.lsb = IN_32( &pVspaRegs->host_in_1_lsb );

        if( pdTRUE != xQueueSendToBackFromISR( AviHndlr->VspaToCm4QMbox1,
                                               &vspambox, NULL ) )
        {
            log_dbg( "ERR:%s: VspaToCm4QMbox1 Full\n\r", __func__ );
        }

        OUT_32( &pVspaRegs->vspa_status, VSPA_MBOX1_STATUS );
    }
    else if( IN_32( &pVspaRegs->vspa_status ) & ( CM4_MBOX0_STATUS & AVI_MBOX_MONITOR_MASK ) )
    {
        log_dbg( "%s: Rcvd mbox0 ack\n\r", __func__ );

        /* Check QUEUE status and then check
         * if CM4 mailbox is queued
         * */
        if( 0 != uxQueueMessagesWaitingFromISR( AviHndlr->Cm4ToVspaQMbox0 ) )
        {
            if( pdPASS == xQueueReceiveFromISR( ( ( struct avi_hndlr * )
                                                  AviHndlr )->Cm4ToVspaQMbox0,
                                                &vspambox, NULL ) )
            {
                log_dbg( "%s:  sending msg to VSPA mbox0\n\r", __func__ );
                OUT_32( &pVspaRegs->host_out_0_msb, vspambox.msb );
                dmb();
                OUT_32( &pVspaRegs->host_out_0_lsb, vspambox.lsb );
            }
        }

        OUT_32( &pVspaRegs->vspa_status, CM4_MBOX0_STATUS );
    }
    else if( IN_32( &pVspaRegs->vspa_status ) & ( CM4_MBOX1_STATUS & AVI_MBOX_MONITOR_MASK ) )
    {
        log_dbg( "%s: Rcvd mbox1 ack\n\r", __func__ );

        if( 0 != uxQueueMessagesWaitingFromISR( AviHndlr->Cm4ToVspaQMbox1 ) )
        {
            if( pdPASS == xQueueReceiveFromISR( ( ( struct avi_hndlr * )
                                                  AviHndlr )->Cm4ToVspaQMbox1,
                                                &vspambox, NULL ) )
            {
                log_dbg( "%s:  sending msg to VSPA mbox1\n\r", __func__ );
                OUT_32( &pVspaRegs->host_out_1_msb, vspambox.msb );
                dmb();
                OUT_32( &pVspaRegs->host_out_1_lsb, vspambox.lsb );
            }
        }

        OUT_32( &pVspaRegs->vspa_status, CM4_MBOX1_STATUS );
    }
    else
    {
        log_err( "ERR: Invalid VSPA status\n" );
    }

    log_dbg( "%s: Out\n\r", __func__ );
}

void La9310VSPA_IRQDefaultHandler( void )
{
    log_isr( "ISR:%s: Handle VSPA interrupt\n\r", __func__ );
    /*iLa9310RaiseIrqEvt(pLa9310Info, IRQ_EVT_VSPA_BIT); */

    /* Clear NVIC interrupt */
    NVIC_ClearPendingIRQ( IRQ_VSPA );
}

void La9310VSPA_IRQRelayHandler( void )
{
    struct vspa_regs * pVspaRegs = ( struct vspa_regs * ) pAviHndlr->pVspaRegs;
    uint32_t status;
    struct la9310_stats * pStats = pLa9310Info->stats;

    log_isr( "ISR:%s: In\n\r", __func__ );

    /* Currently all VSPA interrupts are sent to Host
     * Raise MSI towards Host.
     * TODO: As CM4 handles Mailbox interrupts, Mailbox related
     * interrupts need not be sent to Host. Rather tasks waiting
     * on Mailbox interrupts needs to be notified
     * */
    status = IN_32( &pVspaRegs->vspa_status );
    pStats->avi_intr_raised++;

    /* Only DMA interrupts needs to be relayed to HOST */
    if( status & AVI_MBOX_MONITOR_MASK )
    {
        /* Mailbox related interrupt, got some work
         */
        pStats->avi_mbox_intr_raised++;
        log_isr( "ISR:%s: calling AviHndleMboxInterrupt\n\r", __func__ );
        AviHndleMboxInterrupt( pAviHndlr );
    }
    else
    {
        log_isr( "ISR: Raise VSPA DMA MSI to Host\n\r" );
        /*iLa9310RaiseIrqEvt(pLa9310Info, IRQ_EVT_VSPA_BIT); */
    }

    /* Clear NVIC interrupt */
    NVIC_ClearPendingIRQ( IRQ_VSPA );
    log_isr( "ISR:%s: Out\n\r", __func__ );
}

void La9310VSPA_IRQHandler( void )
{
    IntHndlr();
    #if ARM_ERRATUM_838869
        dsb();
    #endif
}


static void vLa9310AviEvtMask( struct la9310_info * pLa9310Info,
                               la9310_irq_evt_bits_t evt_bit,
                               void * cookie )
{
    /* Disable VSPA interrupt handling */
    NVIC_DisableIRQ( IRQ_VSPA );
}

static void vLa9310AviEvtUnmask( struct la9310_info * pLa9310Info,
                                 la9310_irq_evt_bits_t evt_bit,
                                 void * cookie )
{
    /* Enable VSPA interrupt handling */
    NVIC_EnableIRQ( IRQ_VSPA );
}

void iLa9310AviVspaHwVer( void )
{
    struct vspa_regs * pVspaRegs = ( struct vspa_regs * ) VSPA_BASE_ADDR;

    log_info( "INFO: VSPA Hw VER: 0x%x\n\r", pVspaRegs->hw_version );
}

void * iLa9310AviHandle()
{
    return pAviHndlr;
}

void * iLa9310AviInit( void )
{
    log_dbg( "%s: In\n\r", __func__ );

    if( NULL == pAviHndlr )
    {
        log_info( "INFO:%s: AVI Init Starting\n\r", __func__ );

        pAviHndlr = pvPortMalloc( sizeof( struct avi_hndlr ) );

        if( NULL == pAviHndlr )
        {
            log_err( "%s: Memory allocation failed for "
                     "AVI\r\n", __func__ );
            goto hndl_retval;
        }

        pAviHndlr->pVspaRegs = ( struct vspa_regs * ) VSPA_BASE_ADDR;
    }
hndl_retval:
    log_dbg( "%s: Out\n\r", __func__ );
    return pAviHndlr;
}

int iLa9310AviConfig( void )
{
    struct vspa_regs * pVspaRegs = NULL;

    log_dbg( "%s: In\n\r", __func__ );

    log_info( "INFO:%s: AVI Config Starting\n\r", __func__ );

    pAviHndlr = iLa9310AviInit();

    if( NULL == pAviHndlr )
    {
        log_err( "%s: Memory allocation failed for "
                 "AVI\r\n", __func__ );
        goto hndl_retval;
    }


    /* Register Interrupt Handler */
    IntHndlr = La9310VSPA_IRQRelayHandler;

    /* Initialize VSPA interrupt MSI handling */
    VspaEvtHdlr.type = LA9310_EVT_TYPE_VSPA;
    VspaEvtHdlr.mask = vLa9310AviEvtMask;
    VspaEvtHdlr.unmask = vLa9310AviEvtUnmask;
    VspaEvtHdlr.cookie = pLa9310Info;
    iLa9310RegisterEvt( pLa9310Info, IRQ_EVT_VSPA_BIT, &VspaEvtHdlr );

    /* Create VSPA to CM4 queues */
    pAviHndlr->VspaToCm4QMbox0 = xQueueCreate( VSPA_CM4_Q_LEN,
                                               sizeof( struct avi_mbox ) );
    pAviHndlr->VspaToCm4QMbox1 = xQueueCreate( VSPA_CM4_Q_LEN,
                                               sizeof( struct avi_mbox ) );

    /* Create CM4 to VSPA queues */
    pAviHndlr->Cm4ToVspaQMbox0 = xQueueCreate( VSPA_CM4_Q_LEN,
                                               sizeof( struct avi_mbox ) );
    pAviHndlr->Cm4ToVspaQMbox1 = xQueueCreate( VSPA_CM4_Q_LEN,
                                               sizeof( struct avi_mbox ) );

    /* Create CM4 to VSPA Q mutexes */
    pAviHndlr->Cm4ToVspaQMutex0 = xSemaphoreCreateMutex();
    pAviHndlr->Cm4ToVspaQMutex1 = xSemaphoreCreateMutex();

    /* Enable Mailbox related Interrupts */
    pVspaRegs = pAviHndlr->pVspaRegs;
    OUT_32( &pVspaRegs->vspa_irqen,
            ( IN_32( &pVspaRegs->vspa_irqen ) | AVI_MBOX_MONITOR_MASK ) );

    /* Enable VSPA interrupt handling for FreeRTOS */
    log_info( "INFO:%s: Enabling IRQ_VSPA\n\r", __func__ );
    NVIC_SetPriority( IRQ_VSPA, VSPA_IRQ_PRIORITY );
    NVIC_EnableIRQ( IRQ_VSPA );
    log_info( "INFO:%s: AVI Init Done\n\r", __func__ );

    return 0;
hndl_retval:
    log_dbg( "%s: Out\n\r", __func__ );
    return 1;
}


void iLa9310AviClose( void )
{
    struct vspa_regs * pVspaRegs = NULL;
    UBaseType_t uxNumberOfItems;

    log_dbg( "%s: Disabling IRQ line For VSPA\n", __func__ );
    NVIC_DisableIRQ( IRQ_VSPA );

    IntHndlr = La9310VSPA_IRQDefaultHandler;

    if( NULL != pAviHndlr )
    {
        pVspaRegs = ( struct vspa_regs * ) pAviHndlr->pVspaRegs;

        /* Disabling Mbox Irqs */
        OUT_32( &pVspaRegs->vspa_irqen,
                IN_32( &pVspaRegs->vspa_irqen ) &
                ( ~AVI_MBOX_MONITOR_MASK ) );
        log_dbg( "%s:MailBox Interrupts have been disabled\n",
                 __func__ );
    }
    else
    {
        log_err( "Mbox Avi Hndlr: Address not found\n" );
        goto close_err;
    }

    /* Checking for any pending mailbox */
    uxNumberOfItems = uxQueueMessagesWaiting( pAviHndlr->Cm4ToVspaQMbox0 );

    if( uxNumberOfItems )
    {
        log_info( "Queue Cm4ToVspaQMbox0: %d items will be lost\n",
                  uxNumberOfItems );
    }

    uxNumberOfItems = uxQueueMessagesWaiting( pAviHndlr->Cm4ToVspaQMbox1 );

    if( uxNumberOfItems )
    {
        log_info( "Queue Cm4ToVspaQMbox1: %d items will be lost\n",
                  uxNumberOfItems );
    }

    uxNumberOfItems = uxQueueMessagesWaiting( pAviHndlr->VspaToCm4QMbox0 );

    if( uxNumberOfItems )
    {
        log_info( "Queue VspaToCm4QMbox0: %d items will be lost\n",
                  uxNumberOfItems );
    }

    uxNumberOfItems = uxQueueMessagesWaiting( pAviHndlr->VspaToCm4QMbox1 );

    if( uxNumberOfItems )
    {
        log_info( "Queue VspaToCm4QMbox1: %d items will be lost\n",
                  uxNumberOfItems );
    }

    /* Delete Queues */
    log_dbg( "%s : Deleting Queues \n", __func__ );
    vQueueDelete( pAviHndlr->Cm4ToVspaQMbox0 );
    vQueueDelete( pAviHndlr->Cm4ToVspaQMbox1 );
    vQueueDelete( pAviHndlr->VspaToCm4QMbox0 );
    vQueueDelete( pAviHndlr->VspaToCm4QMbox1 );

    /* Delete Semaphores */
    log_dbg( "%s : Deleting Semaphores \n", __func__ );
    vQueueDelete( pAviHndlr->Cm4ToVspaQMutex0 );
    vQueueDelete( pAviHndlr->Cm4ToVspaQMutex1 );

    /*Releasing momory of struct avi_hndlr */
    log_dbg( "%s : Releasing Memory\n", __func__ );
    vPortFree( pAviHndlr );
    pAviHndlr = NULL;

    NVIC_EnableIRQ( IRQ_VSPA );
    log_dbg( "%s: Enabling IRQ with Default Handler\n", __func__ );

close_err:
    log_dbg( "Close: No AviInit Interface Identified\n" );
}

void iLa9310VspaInit( void )
{
    log_dbg( "%s: In\n\r", __func__ );
    /* Register Interrupt Handler */
    IntHndlr = La9310VSPA_IRQDefaultHandler;
    /* Enable VSPA interrupt handling for FreeRTOS */
    NVIC_EnableIRQ( IRQ_VSPA );
    log_dbg( "%s: Out\n\r", __func__ );
}
