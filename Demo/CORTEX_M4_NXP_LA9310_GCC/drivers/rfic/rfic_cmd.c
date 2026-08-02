/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright 2021-2022 NXP
 */
#include "rfic_cmd.h"
#include "rfic_synth.h"
#include "rfic_demod.h"
#include "rfic_vga.h"
#include "rfic_sw_cmd.h"
#include "rfic_avi_ctrl.h"
#include "rfic_dac.h"
#include <la9310_irq.h>
#include "la9310_dcs.h"
#include "la9310_dcs_api.h"
#include <delay.h>
#include "phytimer.h"
#include "rfnm_rf_ctrl.h"
#include "../avi/la9310_avi_ds.h"
#include <immap.h>

int32_t iRficCtrlFemSwitch( RficDevice_t *pRficDev, rf_band_t band )
{
    uint8_t SwNum = ( band <= RF_SW_BAND_B13 ) ? RF_GPIO_RFA_PATH_SEL : RF_GPIO_RFB_PATH_SEL;
    uint8_t SwCtrlVal[4][2] = { /* Demod Sel, Switch Sel */
                               { 1, 1 },    /* N77 */
                               { 1, 0 },    /* B13 */
                               { 0, 1 },    /* B3 */
                               { 0, 0 }     /* GNSS */
                              };

    if( iGpioSetData( pRficDev->eGpio[ RF_GPIO_DEMOD_PATH_SEL ].ulPin,
                      SwCtrlVal[ band ][ 0 ] ))
    {
	log_err( "%s: GPIO-%d set failed\r\n", __func__,
	         pRficDev->eGpio[ RF_GPIO_DEMOD_PATH_SEL ].ulPin );
	return -1;
    }

    if( iGpioSetData( pRficDev->eGpio[ SwNum ].ulPin, SwCtrlVal[ band ][ 1 ] ))
    {
	log_err( "%s: GPIO-%d set failed\r\n", __func__,
	         pRficDev->eGpio[ RF_GPIO_RFA_PATH_SEL ].ulPin );
	return -1;
    }

    return 0;
}

void xRficProcessSetBand( RficDevice_t *pRficDev, rf_sw_cmd_desc_t *pSwCmdDesc )
{
    struct sw_cmddata_set_band *CmdData;
    rf_priv_mdata_t *mdata = &pRficDev->pRfHif->rf_priv_mdata;

    CmdData = ( struct sw_cmddata_set_band * )&pSwCmdDesc->data[0];
    log_dbg( "%s: Band[%d]\n\r", __func__, CmdData->band );

    /* Configure FEM */
    if( iRficCtrlFemSwitch( pRficDev, CmdData->band ))
    {
        log_err("%s: FEM Ctrl failed\r\n", __func__);
        goto err;
    }

    /* Configure Demod LO Matching */
    if( RficDemodLoMatch( pRficDev, CmdData->band, RFIC_DEMOD_N77_LO_SHIFT ))
    {
        log_err("%s: Demod LO matching failed\r\n", __func__);
        goto err;
    }

    /* Update mdata */
    mdata->band = CmdData->band;

    pSwCmdDesc->result = RF_SW_CMD_RESULT_OK;
    return;
err:
    pSwCmdDesc->result = RF_SW_CMD_RESULT_ERROR;
    return;
}

void xRficProcessAdjustPllFreq( RficDevice_t *pRficDev, rf_sw_cmd_desc_t *pSwCmdDesc )
{
    struct sw_cmddata_adjust_pll_freq *CmdData;
    rf_priv_mdata_t *mdata = &pRficDev->pRfHif->rf_priv_mdata;

    CmdData = ( struct sw_cmddata_adjust_pll_freq * )&pSwCmdDesc->data[0];
    log_dbg( "%s: Freq[%d]\n\r", __func__, CmdData->freq_khz );

    /* Select Synthesizer */
    if( iRficSelectDspiSlave( pRficDev, RF_DSPI_SLV_SYNTH ))
    {
        log_err( "%s: Select DSPI Slave failed\n\r", __func__ );
        goto err;
    }

    /* Program synthesizer for required freq */
    RficSynthAdjustPllFreq( pRficDev, CmdData->freq_khz );

    /* Configure Demod LO Matching */
    if( RF_SW_BAND_N77 == mdata->band )
    {
        if( RficDemodLoMatch( pRficDev, mdata->band, CmdData->freq_khz ))
        {
            log_err("%s: Demod LO matching failed\r\n", __func__);
            goto err;
        }
    }

    /* Update mdata */
    mdata->freq_khz = CmdData->freq_khz;

    pSwCmdDesc->result = RF_SW_CMD_RESULT_OK;
    return;
err:
    pSwCmdDesc->result = RF_SW_CMD_RESULT_ERROR;
    return;
}

void xRficProcessCtrlLna( RficDevice_t *pRficDev, rf_sw_cmd_desc_t *pSwCmdDesc )
{
    struct sw_cmddata_ctrl_lna *CmdData;
    rf_priv_mdata_t *mdata = &pRficDev->pRfHif->rf_priv_mdata;
    uint32_t uGpio;

    CmdData = ( struct sw_cmddata_ctrl_lna * )&pSwCmdDesc->data[0];
    log_dbg( "%s: State[%d]\n\r", __func__, CmdData->state );

    if( RF_SW_BAND_N77 == mdata->band )
        uGpio = RF_GPIO_BAND_N77_LNA_EN;
    else if( RF_SW_BAND_B13 == mdata->band )
	uGpio = RF_GPIO_BAND_B13_LNA_EN;
    else if( RF_SW_BAND_B3 ==  mdata->band )
	uGpio = RF_GPIO_BAND_B3_LNA_EN;
    else
	goto err;

    if( iGpioSetData( pRficDev->eGpio[ uGpio ].ulPin, CmdData->state ))
    {
        log_err( "%s: GPIO-%d set failed\r\n", __func__,
		 pRficDev->eGpio[ uGpio ].ulPin );
        goto err;
    }

    /* Update mdata */
    mdata->lna_state &= (~( 1 << mdata->band ));
    mdata->lna_state |= ( CmdData->state << mdata->band );

    pSwCmdDesc->result = RF_SW_CMD_RESULT_OK;
    return;
err:
    pSwCmdDesc->result = RF_SW_CMD_RESULT_ERROR;
    return;
}

void xRficProcessReadReg( RficDevice_t *pRficDev, rf_sw_cmd_desc_t *pSwCmdDesc )
{
    struct sw_cmddata_reg_opr *CmdData;
    int32_t iRet = 0;
    uint32_t val32;
    uint8_t val8;

    CmdData = ( struct sw_cmddata_reg_opr * )&pSwCmdDesc->data[0];
    log_dbg( "%s: DspiSlvId[%d], Addr[%x]\n\r", __func__,
	      CmdData->dspi_slv_id, CmdData->addr );

    /* Select DPSI Slave Device */
    if( iRficSelectDspiSlave( pRficDev, CmdData->dspi_slv_id ))
    {
        log_err( "%s: Select DSPI Slave failed\n\r", __func__ );
        goto err;
    }

    /* Call the respective read register */
    if( RF_DSPI_SLV_SYNTH == CmdData->dspi_slv_id )
    {
        iRet = RficSynthReadReg( pRficDev, ( uint8_t )CmdData->addr,
                                 &val32 );
        CmdData->val = val32;
    }
    else if( RF_DSPI_SLV_VGA == CmdData->dspi_slv_id )
        CmdData->val = pRficDev->xVgaRegVal;
    else if ( RF_DSPI_SLV_DEMOD == CmdData->dspi_slv_id )
    {
        iRet = RficDemodReadReg( pRficDev, ( uint8_t )CmdData->addr,
                                 &val8 );
        CmdData->val = val8;
    }
    else
	goto err;

    if( iRet )
    {
         log_err( "%s: Read register failed\n\r", __func__ );
	 goto err;
    }

    pSwCmdDesc->result = RF_SW_CMD_RESULT_OK;
    return;
err:
    pSwCmdDesc->result = RF_SW_CMD_RESULT_ERROR;
    return;
}

void xRficProcessWriteReg( RficDevice_t *pRficDev, rf_sw_cmd_desc_t *pSwCmdDesc )
{
    struct sw_cmddata_reg_opr *CmdData;
    int32_t iRet;

    CmdData = ( struct sw_cmddata_reg_opr * )&pSwCmdDesc->data[0];
    log_dbg( "%s: DspiSlvId[%d], Addr[%x], val[%x]\n\r", __func__,
              CmdData->dspi_slv_id, CmdData->addr, CmdData->val );

    /* Select DPSI Slave Device */
    if( iRficSelectDspiSlave( pRficDev, CmdData->dspi_slv_id ))
    {
        log_err( "%s: Select DSPI Slave failed\n\r", __func__ );
        goto err;
    }

    /* Call the respective write register */
    if( RF_DSPI_SLV_SYNTH == CmdData->dspi_slv_id )
        iRet = RficSynthWriteReg( pRficDev, ( uint8_t )CmdData->addr,
                                 ( uint32_t )CmdData->val );
    else if( RF_DSPI_SLV_VGA == CmdData->dspi_slv_id )
        iRet = RficVgaWriteReg( pRficDev, ( uint8_t )CmdData->val );
    else if ( RF_DSPI_SLV_DEMOD == CmdData->dspi_slv_id )
        iRet = RficDemodWriteReg( pRficDev, ( uint8_t )CmdData->addr,
                                 ( uint8_t )CmdData->val );
    else
        goto err;

    if( iRet )
    {
         log_err( "%s: Write register failed\n\r", __func__ );
         goto err;
    }

    pSwCmdDesc->result = RF_SW_CMD_RESULT_OK;
    return;
err:
    pSwCmdDesc->result = RF_SW_CMD_RESULT_ERROR;
    return;
}


#define MSI_IRQ_FLOOD_0 6
#define MSI_IRQ_FLOOD_1 7

#define RFNM_ADC_MAP_FREERTOS (int[]){0x4, 0x2, 0x3, 0x1}


// phytimer phase 2a: the M4 owns the RX allow-gate timing. adc i is pulled from AXIQ
// fifo {RX1, RX3, RX0, RX2}[i] (the VSPA's Rx_Antenna2fifo_index in dfe.c), and fifo n
// is gated by comparator n+1 - keep this table in lockstep with the VSPA.
static const uint8_t rfnm_rx_gate_comp[4] = {
        PHY_TIMER_COMP_CH2_RX_ALLOWED, PHY_TIMER_COMP_CH4_RX_ALLOWED,
        PHY_TIMER_COMP_CH1_RX_ALLOWED, PHY_TIMER_COMP_CH3_RX_ALLOWED };

// gate-open guard past 'now': covers the real stream mbox send + the VSPA's pipeline
// teardown/rebuild + the ack round trip + the arm writes below, all on-chip. Paid on
// COLD chain builds only - a TX-preserving (scoped) apply never re-arms C11 at all.
#define RFNM_PTMR_T0_GUARD ( 4 * 61440 )

// A2 scoped apply (the heal-demolition fix): latch the last
// successfully applied TX config. A later apply whose TX bits are byte-identical
// while the chain reads alive is TX-PRESERVING: the warmup ritual, the VSPA TX
// teardown/rebuild, the t0/epoch re-mint, the C11 re-arm and the parked-repair
// probe are all skipped - an RX-lane heal must not demolish healthy TX.
// TX-relevant stream word bits: 0 = tx_on, 7-8 + 13-14 = upsampling, 10 = tx_dcs.
// bit 16 = windowed (kernel addr contract): in the mask so a windowed latch can
// never serve a later free-run apply's tx_keep (and vice versa)
#define RFNM_TX_CFG_MASK ((1u << 0) | (3u << 7) | (1u << 10) | (3u << 13) | (1u << 16))
static uint32_t rfnm_tx_cfg_latch;	/* bit 31 = valid */
static uint32_t rfnm_eld_tdd_caps;	/* sticky: the loaded eld acked TDD capability */

// (The arm-repair machinery is DELETED: the strand it healed can no longer be
// minted - the eld never aborts a live ptr_rst handshake and the kernel gen-init
// guarantees a cycle per generation; measurements additionally showed the repair
// never healed the real fault face. History: git log this file.)

void vRficProcessIqDump(rf_sw_cmd_desc_t *rfic_sw_cmd)
{
        BaseType_t xRet = pdFAIL;
        struct sw_cmddata_dump_iq *cmd_data;
        struct la9310_mbox_v2h mbox_v2h = {0};
        struct la9310_mbox_h2v mbox_h2v = {0};
        uint32_t rx_mask, tx_on, ptmr_t0 = 0;
        uint32_t tx_keep = 0;
        int i;

        // RFNM_TX_GATE_EXPERIMENT: C11-gated DAC start with the warmup ritual (force
        // open -> AXIQ enable pulse -> ~1ms free-run -> force close -> arm at tx_t0).
        // The park (docs/phytimer 5m/5n) is solved by the pulse: the fifo request
        // latch dies silently when a closure catches a pended write, and an enable
        // OFF->ON pulse revives it (5q). Residual start-mode lottery (racing/parked
        // starts) is handled by the kernel's pace-check + regate self-heal; validated
        // cross-board 100/100 on-grid, sigma 0 (5t/5u).
#ifndef RFNM_TX_GATE_EXPERIMENT
#define RFNM_TX_GATE_EXPERIMENT 1
#endif
        cmd_data = ((struct sw_cmddata_dump_iq *)rfic_sw_cmd->data);

        // close every RX gate before the VSPA tears down and rebuilds its capture
        // pipeline: the first sample through a reopened gate must be the T0 sample
        rx_mask = (cmd_data->addr >> 1) & 0xF;
        tx_on = cmd_data->addr & 0x1;
        // windowed session (kernel addr bit 16, rt-cores step 2): passthrough to the
        // VSPA (ctrl bit 7 = mbox bit 23) + this apply must NOT arm C11 at T0 and
        // must NOT run the parked-repair watchdog - the rtc ring owns the gate and
        // a deliberately-parked pump is the session's design, not a defect. The
        // warmup latch mint below STAYS (enable pulse on an open gate, force-close
        // after = exactly the windowed session's initial state).
        uint32_t tx_windowed = (cmd_data->addr >> 16) & 0x1;

        // TX-preserving apply decision (A2): identical TX config + a live chain +
        // the plain free-run gate discipline. Never under a TDD-kernel eld (its
        // engagement applies rebuild the gated pump), never under the ring walker
        // (it owns C11), never TX-only (a bare re-apply keeps rebuild semantics).
        if(!tx_windowed && tx_on && rx_mask &&
           (rfnm_tx_cfg_latch & 0x80000000u) &&
           ((rfnm_tx_cfg_latch & RFNM_TX_CFG_MASK) == (cmd_data->addr & RFNM_TX_CFG_MASK)) &&
           // upsampling-0 sessions only: the VSPA's scoped parse re-writes the (identical)
           // factor but still resets the ZOH phase, which is only inert when no ZOH runs
           ((cmd_data->addr & ((3u << 7) | (3u << 13))) == 0) &&
           !rfnm_tdd_txgate() && !rfnm_eld_tdd_caps) {	/* TUP 2b: txn ring deleted */
                // aliveness proxy: SR1 bit 16 = TX0 fifo enabled, gate-qualified. The
                // free-run discipline keeps C11 open, so a healthy chain reads 1; a
                // parked/dead chain (or a mid-guard rebirth, gate still closed) reads
                // 0 and falls back to the full apply - which is also the revival path.
                struct vspa_regs *pVspaRegsProbe = (struct vspa_regs *)VSPA_BASE_ADDR;
                tx_keep = (pVspaRegsProbe->gp_in[1] >> 16) & 0x1;
        }
        if(rx_mask) {
                for(i = 0; i < 4; i++) {
                        vPhyTimerComparatorForce(rfnm_rx_gate_comp[i], ePhyTimerComparatorOut0);
                }
        }
        // phytimer phase 3: the DAC gate follows the same discipline - shut before the
        // VSPA rebuilds the TX pipeline so the first ring sample airs at exactly T0
#if RFNM_TX_GATE_EXPERIMENT
        if(tx_on && !tx_keep) {
                // Open the gate briefly BEFORE closing it: a write DMA parked behind a
                // closed gate cannot retire its abort (live signature: STAT_ABORT bit 11
                // latched, channel zombie, every later prime dead). A short open drains
                // the fifo and completes any pending write, so the VSPA's reset ritual
                // (abort/ptr-reset/clrerr) always operates on a CLEAN channel - which is
                // the only state DFE's per-window reset ever aborts.
                vPhyTimerComparatorForce(PHY_TIMER_COMP_CH5_TX_ALLOWED, ePhyTimerComparatorOut1);
                vUDelay(50);
                // Arm the AXIQ TX fifo request latch while the line is still high: the
                // latch only arms on a fifo-enable rising edge with TX_ALLOWED open, and
                // without it every write pended behind the closed gate stays pended
                // forever after the gate fires (the un-park discovery, docs 5q). AXIQ
                // CR3 is VSPA GPOUT7; bit 0 is the TX0 fifo enable. After the pulse the
                // released flow must RUN through the open gate briefly; then close
                // DIRECTLY - no fifo disable first: a disable here re-enters flush mode
                // and the pump free-runs discarding samples instead of pacing the DAC
                // (iter-2 lesson: "flowing" at 6x real-time = flush-race, not RF).
                // KNOWN RESIDUAL (docs 5r): the close can race the released flow - some
                // applies come up flush-racing. Park->fire->kick was tried and is WORSE
                // (the wedge-released pipeline underrun-storms); detect-and-reapply is
                // the interim answer. NOTE: quiescing the pump
                // here BEFORE the close (write-idle through the open gate) was tried and
                // is ALSO WORSE - the drained-empty fifo made EVERY apply lose the start
                // lottery (parked at prime or flush-racing, zero RF). The iter-3
                // mid-flow close stays; the per-WINDOW quiesce (walker path) is where
                // the write-idle discipline belongs.
                {
                        struct vspa_regs *pVspaRegs = (struct vspa_regs *)VSPA_BASE_ADDR;
                        int tries;

                        // Back-to-back CR writes can be LOST while the AXIQ's config load
                        // is still in flight (silicon-proven root cause, 2026-07-10):
                        // the enable edge then reads back fine in the CR
                        // while the fifo stays disabled - one face of the docs-5q start
                        // lottery. Space the writes and VERIFY the edge latched: SR1
                        // (gp_in[1]) bit 16 = TX0 fifo enabled, and it is gate-qualified,
                        // which is exactly valid HERE - the gate is forced open for this
                        // whole pulse window. Retry the rising edge until it takes.
                        pVspaRegs->gp_out[7] &= ~1u;
                        vUDelay(2);
                        for(tries = 0; tries < 50; tries++) {
                                pVspaRegs->gp_out[7] |= 1u;
                                vUDelay(2);
                                if(pVspaRegs->gp_in[1] & (1u << 16)) {
                                        break;
                                }
                                pVspaRegs->gp_out[7] &= ~1u;
                                vUDelay(2);
                        }
                        if(tries == 50) {
                                log_err("%s: TX0 fifo enable never latched in SR1\r\n", __func__);
                        }
                        vUDelay(1000);
                }
                // DO NOT force-close here. All evidence
                // converges on the warmup being the ritual-PRST killer, and the repair
                // path documents the handshake completing only with the allow line up.
                // The anchored C11 arm below is the closure that mints the T0 contract;
                // this early close only manufactured a gate-closed ritual window.
#ifndef RFNM_TX_GATE_HOLDOPEN_EXPERIMENT
#define RFNM_TX_GATE_HOLDOPEN_EXPERIMENT 1
#endif
#if !RFNM_TX_GATE_HOLDOPEN_EXPERIMENT
                vPhyTimerComparatorForce(PHY_TIMER_COMP_CH5_TX_ALLOWED, ePhyTimerComparatorOut0);
#endif
        }
#endif

#if 1
        mbox_h2v.ctrl.op_code = IQ_MODULATED_RX;
        mbox_h2v.ctrl.bandwidth = 0;
        mbox_h2v.ctrl.rcvr = 0;
        // the all-off pre-word under a TX-preserving apply must not park the running
        // TX chain - the flag rides both words of the exchange
        mbox_h2v.ctrl.tx_keep = tx_keep;
        mbox_h2v.ctrl.tx_windowed = tx_windowed;
        mbox_h2v.msbl16 = 0;
        //cmd_data->addr = LA9310_IQFLOOD_PHYS_ADDR;
        mbox_h2v.lsb32 = 0;
        vLa9310MbxSend(&mbox_h2v);

        vUDelay(2000);
#endif




#if 1

        //uint8_t rx_decimation = ((cmd_data->addr & (0x3 << 5)) >> 5);
        //uint8_t tx_upsampling = ((cmd_data->addr & (0x3 << 7)) >> 7);
        uint8_t rx_dcs = ((cmd_data->addr & (0x1 << 9)) >> 9);
        uint8_t tx_dcs = ((cmd_data->addr & (0x1 << 10)) >> 10);

        //log_err( "tx dcs %d rx dcs %d applying\n\r", tx_dcs, rx_dcs);


        for(int i = 0; i < 5; i++) {
			uint16_t onoff = (cmd_data->addr & (0x1 << i)) >> i;
            if(!onoff) {
                continue;
            }
            // tx_keep: identical DAC config on a running chain - reclocking the
            // converter would glitch the live stream
            if(i == 0 && tx_keep) {
                continue;
            }
            if(i == 0) {
                if(tx_dcs == 1) {
                    xLa9310ConfigAdcDacClock( XCVR_TRX_TX_DAC, Half_Freq );
                } else {
                    xLa9310ConfigAdcDacClock( XCVR_TRX_TX_DAC, Full_Freq );
                }
            } else {
                if(rx_dcs == 1) {
                    xLa9310ConfigAdcDacClock( RFNM_ADC_MAP_FREERTOS[i - 1], Half_Freq );
                } else {
                    xLa9310ConfigAdcDacClock( RFNM_ADC_MAP_FREERTOS[i - 1], Full_Freq );
                }
            }
        }
#else 
        for(int i = 0; i < 5; i++) {
			uint16_t state = (cmd_data->addr & (0x3 << (2 * i))) >> (2 * i);
            if(i == 0) {
                if(state == 2) {
                    xLa9310ConfigAdcDacClock( XCVR_TRX_TX_DAC, Half_Freq );
                } else if(state == 1) {
                    xLa9310ConfigAdcDacClock( XCVR_TRX_TX_DAC, Full_Freq );
                }
            } else {
                if(state == 2) {
                    xLa9310ConfigAdcDacClock( RFNM_ADC_MAP_FREERTOS[i - 1], Half_Freq );
                } else if(state == 1) {
                    xLa9310ConfigAdcDacClock( RFNM_ADC_MAP_FREERTOS[i - 1], Full_Freq );
                }
            }
        }
#endif

        vUDelay(2000);


        // TDD (step 3b): hand the pattern to the VSPA first, so the stream parse that
        // follows consumes it and the stamp chains step across the closed windows
        if(rx_mask) {
                struct la9310_mbox_h2v mbox_pat = {0};
                mbox_pat.ctrl.op_code = 9;	/* VSPA MBOX_OPC_TDD_PATTERN */
                mbox_pat.lsb32 = rfnm_tdd_pattern_word();
                // pairing resync BEFORE the capability-critical exchange: a stale
                // ack from any earlier timed-out verb would be read as THIS ack
                // (msb32 bit0 = 0) and the whole apply chain pairs off-by-one -
                // the TDD engagement lottery (charter 07-15)
                vLa9310MbxDrain();
                vLa9310MbxSend(&mbox_pat);
                xRet = vLa9310MbxReceive(&mbox_v2h);
                // TDD-kernel capability rides the ack's msb32 (apm-tdd.eld only).
                // OWNERSHIP, not capability, latches the gating: the TDD engine owns
                // C11 only when an actual pattern is armed AND the ring walker is not
                // live (the walker forces/configs C11 itself). A capability-only
                // latch would skip the T0 open arm below on pattern-less sessions
                // and kill plain free-run TX. Standard image acks msb32=0 -
                // everything below byte-identical to today.
                // Windowed sessions NEVER latch txgate (gNB composition, measured):
                // the rtc ring's window edges own C11 - the pattern keeps
                // the RX comparators + FE spans only. Without this the ring's arms
                // are refused while the pattern square-waves C11 over the windows
                // (~10% of aimed windows aired, released only by the pattern's own
                // per-period pulse).
                rfnm_tdd_set_txgate((pdFAIL != xRet) && (mbox_v2h.msb32 & 1u) &&
                                (mbox_pat.lsb32 != 0) && !tx_windowed);	/* txn ring deleted */
                // A2: sticky eld capability - a TDD-kernel eld must never take the
                // scoped path (its applies rebuild the gated pump machinery). Updated
                // on every acked pattern exchange, so a fastswap back to the standard
                // image re-opens scoping at its first pattern apply.
                if(pdFAIL != xRet) {
                        rfnm_eld_tdd_caps = (mbox_v2h.msb32 & 1u) ? 1 : 0;
                }
                (void)xRet;
        } else {
                // no RX side this apply: the gate latch clears, but the PATTERN only
                // dies on a genuine all-off teardown. A TX-ONLY apply must keep it -
                // tool flows run one between SET_TDD and the engagement regate, and
                // the stop was eating the pattern there: the regate's verb then echoed
                // zero and the fw side never engaged (fw batch pump free-running into
                // the M4's cycling gate = the first close kills the request latch
                // mid-data at t0+~37 ms = the ~9k-slot wedge, observed).
                if(!tx_on) {
                        rfnm_tdd_stop();
                }
                rfnm_tdd_set_txgate(0);
        }

        // mint the gate-open tick and ship the FULL 32-bit T0 with the stream word:
        // msbl16 = T0[15:0], lsb32[31:16] = T0[31:16], lsb32 bit 15 = T0 valid
        // (the stream word itself only uses lsb32 bits 0-14). The VSPA anchors its
        // computed stamp chains on this exact tick.
        if(rx_mask || tx_on) {
                uint32_t adv = 0;
                uint32_t g_used = 0;
                ptmr_t0 = ulPhyTimerCapture( PHY_TIMER_COMPARATOR_COUNT - 1 ) + RFNM_PTMR_T0_GUARD;
                if(cmd_data->anchor_granule) {
                        // v2 charter P2: the anchor is INTENT - advance the default mint
                        // to the first tick holding t0 % g == phase % g. Residue math on
                        // the u32 VALUES, never a subtract-then-mod: the difference wraps
                        // when the clock crosses 2^32 between request and mint, and
                        // 2^32 % 307200 = 4096 would silently skew the phase. The kernel
                        // owns the granule choice; a residue cannot go stale, so no
                        // accept window exists to mis-judge.
                        uint32_t g = cmd_data->anchor_granule;
                        // #51 interlock fix (b), the LATE-ARM pe lottery: a famine heal's
                        // anchor re-issue snaps its regate word while the pattern is
                        // STOPPED (tdd stop -> apply -> deferred re-arm), so the word
                        // carries the one-chunk fallback granule; that stale-snap regate
                        // fires AFTER the re-arm's good mint and re-anchors the stream at
                        // 768-tick grain = pe scatter +-100k, UL dead from the first heal
                        // (measured). The law: the
                        // granule = the TDD period WHEN COMMANDED - and this side owns
                        // the commanded pattern truth. Widen every anchored mint to the
                        // armed period so a stale word cannot shrink the congruence:
                        // late arms, heal re-arms and clobber regates all mint pe=0.
                        {
                                uint32_t pat = rfnm_tdd_pattern_word();
                                if(pat) {
                                        uint32_t gp = (pat >> 16) * (384u << rx_dcs);
                                        if(gp > g) {
                                                g = gp;
                                        }
                                }
                        }
                        g_used = g;
                        uint32_t want = cmd_data->anchor_phase % g;
                        uint32_t have = ptmr_t0 % g;
                        if(want != have) {
                                adv = (want > have) ? (want - have) : (g - (have - want));
                                ptmr_t0 += adv;
                        }
                }
                // Mint forensics: record what this mint saw and produced (host devmem
                // at 0x1C00F760 - the only ground truth for anchor-phase disputes)
                rf_ctrl.mint_anchor_phase = cmd_data->anchor_phase;
                // forensics record the granule USED (post pattern-widening), not the
                // shipped one: a late arm recording period-grain here proves the widening works
                rf_ctrl.mint_anchor_granule = g_used;
                rf_ctrl.mint_advance = adv;
                rf_ctrl.mint_t0 = ptmr_t0;
        }

        mbox_h2v.ctrl.op_code = IQ_MODULATED_RX;
        mbox_h2v.ctrl.bandwidth = 0;
        mbox_h2v.ctrl.rcvr = 0;
        mbox_h2v.ctrl.tx_keep = tx_keep;
        mbox_h2v.ctrl.tx_windowed = tx_windowed;
        mbox_h2v.msbl16 = ptmr_t0 & 0xFFFF;
        mbox_h2v.lsb32 = (cmd_data->addr & 0x7FFF) | ((rx_mask || tx_on) ? 0x8000 : 0) | (ptmr_t0 & 0xFFFF0000);
        vLa9310MbxSend(&mbox_h2v);

        //vUDelay(2000);

        log_info("LA STREAM CMD: %x\r\n", cmd_data->addr);


        xRet = vLa9310MbxReceive(&mbox_v2h);

        if ((pdFAIL == xRet) || (0 != mbox_v2h.status.err_code))
        {
                log_err("%s: iqdump failed error[%d]", __func__, mbox_v2h.status.err_code);
                // chain state now unknown - the next apply must be a full rebuild
                rfnm_tx_cfg_latch = 0;
                rfic_sw_cmd->result = RF_SW_CMD_RESULT_ERROR;
        }
        else
        {
                // the VSPA acked with its pipeline built behind the closed gates and its
                // stamp chains anchored on T0: open the enabled gates at exactly that tick
                if(rx_mask) {
                        for(i = 0; i < 4; i++) {
                                if(!(rx_mask & (1 << i))) {
                                        continue;
                                }
                                vPhyTimerComparatorConfig(rfnm_rx_gate_comp[i],
                                                PHY_TIMER_COMPARATOR_CLEAR_INT,
                                                ePhyTimerComparatorOut1, ptmr_t0);
                        }
                }
#if RFNM_TX_GATE_EXPERIMENT
                if(tx_on && !rfnm_tdd_txgate() && !tx_keep && !tx_windowed) {
                        // DAC gate opens on the same minted tick: RX and TX share one
                        // timeline. CROSS_TRIG is load-bearing (DFE study, docs 5n):
                        // the fifo-mode write DMA pends on an external trigger, and the
                        // comparator's cross-trigger pulse at compare is what releases
                        // it - without the flag the parked write never resumes.
                        // Under a TDD-engine-owned gate this arm is SKIPPED: a
                        // comparator holds ONE pending arm, so this open-at-T0 and the
                        // engine's first close-edge arm would overwrite each other (the
                        // v1 bring-up dormancy). TX must not open mid-duty anyway; the
                        // engine's first RX-close edge opens the gate and releases the
                        // pended prime span-exactly.
                        vPhyTimerComparatorConfig(PHY_TIMER_COMP_CH5_TX_ALLOWED,
                                        PHY_TIMER_COMPARATOR_CLEAR_INT | PHY_TIMER_COMPARATOR_CROSS_TRIG,
                                        ePhyTimerComparatorOut1, ptmr_t0);
                }
#endif
                if(rx_mask || tx_on) {
                        if((int32_t)(ptmr_t0 - ulPhyTimerCapture( PHY_TIMER_COMPARATOR_COUNT - 1 )) <= 0) {
                                // a past compare only fires after a ~70 s counter wrap: fail the
                                // swcmd loudly so the host resends with a fresh T0
                                log_err("%s: phytimer T0 already past\r\n", __func__);
                                rfnm_tx_cfg_latch = 0;
                                rfic_sw_cmd->result = RF_SW_CMD_RESULT_ERROR;
                                return;
                        }
                }
                if(rx_mask) {
                        // TDD engaged: the scheduler takes the grid over from this anchor
                        rfnm_tdd_start_at(ptmr_t0, 384 << rx_dcs);
                }
                // v3: in ring mode the anchor burst (window 0 = the bootstrap duty) is all
                // we need open at t0; hand the walker the close tick so it shuts the gate
                // right after and the lead to the first pushed window airs nothing.
                // A2 latch maintenance: a full TX apply latches its config; a TX-off
                // apply invalidates; a scoped apply leaves the latch standing.
                if(tx_on && !tx_keep) {
                        rfnm_tx_cfg_latch = 0x80000000u | (cmd_data->addr & RFNM_TX_CFG_MASK);
                } else if(!tx_on) {
                        rfnm_tx_cfg_latch = 0;
                }
                rfic_sw_cmd->result = RF_SW_CMD_RESULT_OK;
        }

        return;
}

// v3 phase 0: one DFE-style TX window (design review + docs 5n/5t/5u). Rebuild the
// VSPA fifo around the window payload (MBOX_OPC_TX_WINDOW ritual: quiesced pump,
// reset, prime while disabled, enable last), then own both C11 edges: open at an
// ARBITRARY tick - sample resolution, no slot alignment (the OAI amendment) - and
// close at tick + len. The handler blocks until the open edge fires so it can arm
// the close on the same comparator (debug-tool semantics; the phase-1 ring walker
// replaces this with chained arming).
void vRficProcessTxWindow(rf_sw_cmd_desc_t *rfic_sw_cmd)
{
        uint32_t tick = ((uint32_t *)rfic_sw_cmd->data)[0];
        uint32_t len_ticks = ((uint32_t *)rfic_sw_cmd->data)[1];
        uint32_t src_slot = ((uint32_t *)rfic_sw_cmd->data)[2];
        // slots to pump = the window's samples plus one residue slot, so the fifo
        // never runs dry before the close edge (which aborts the residue by design)
        uint32_t len_slots = (len_ticks + 255) / 256 + 1;
        struct la9310_mbox_h2v mbox_h2v = { 0 };
        struct la9310_mbox_v2h mbox_v2h = { 0 };
        BaseType_t xRet;

        if(len_ticks == 0 || len_slots > 0xFFFF || src_slot > 0x3FFF) {
                rfic_sw_cmd->result = RF_SW_CMD_RESULT_CMD_PARAMS_INVALID;
                return;
        }

        // close the gate before rebuilding the pipeline: a free-running stream may
        // be airing, and the window ritual's fifo disable->enable pulse revives the
        // request-latch wedge this force-close can create (docs 5q)
        vPhyTimerComparatorForce(PHY_TIMER_COMP_CH5_TX_ALLOWED, ePhyTimerComparatorOut0);

        mbox_h2v.ctrl.op_code = 0xA;	/* VSPA MBOX_OPC_TX_WINDOW */
        mbox_h2v.lsb32 = ((src_slot & 0x3FFF) << 16) | (len_slots & 0xFFFF);
        vLa9310MbxSend(&mbox_h2v);
        xRet = vLa9310MbxReceive(&mbox_v2h);
        if((pdFAIL == xRet) || (0 != mbox_v2h.status.err_code)) {
                log_err("%s: VSPA window setup failed [%d]\r\n", __func__, mbox_v2h.status.err_code);
                rfic_sw_cmd->result = RF_SW_CMD_RESULT_ERROR;
                return;
        }

        // the window must still be comfortably in the future after the setup
        if((int32_t)(tick - ulPhyTimerCapture( PHY_TIMER_COMPARATOR_COUNT - 1 )) < 6144) {	/* 100 us */
                log_err("%s: window tick already past\r\n", __func__);
                rfic_sw_cmd->result = RF_SW_CMD_RESULT_ERROR;
                return;
        }

        vPhyTimerComparatorConfig(PHY_TIMER_COMP_CH5_TX_ALLOWED,
                        PHY_TIMER_COMPARATOR_CLEAR_INT | PHY_TIMER_COMPARATOR_CROSS_TRIG,
                        ePhyTimerComparatorOut1, tick);

        // wait for the open edge (wrap-safe), then arm the close edge on the same
        // comparator. Bounded at ~500 ms of host-chosen lead.
        {
                uint32_t guard = 0;
                while((int32_t)(ulPhyTimerCapture( PHY_TIMER_COMPARATOR_COUNT - 1 ) - tick) < 0) {
                        vUDelay(10);
                        if(++guard > 50000) {
                                // disarm as well as we can: force-close and give up
                                vPhyTimerComparatorForce(PHY_TIMER_COMP_CH5_TX_ALLOWED, ePhyTimerComparatorOut0);
                                log_err("%s: open edge never came\r\n", __func__);
                                rfic_sw_cmd->result = RF_SW_CMD_RESULT_ERROR;
                                return;
                        }
                }
        }
        vPhyTimerComparatorConfig(PHY_TIMER_COMP_CH5_TX_ALLOWED,
                        PHY_TIMER_COMPARATOR_CLEAR_INT | PHY_TIMER_COMPARATOR_CROSS_TRIG,
                        ePhyTimerComparatorOut0, tick + len_ticks);

        rfic_sw_cmd->result = RF_SW_CMD_RESULT_OK;
}


void vRficGetRxDcOffset(rf_sw_cmd_desc_t *rfic_sw_cmd)
{
    BaseType_t xRet = pdFAIL;
    struct sw_cmddata_reg_opr *cmd_data;
    struct la9310_mbox_v2h mbox_v2h = {0};
    struct la9310_mbox_h2v mbox_h2v = {0};

    cmd_data = ((struct sw_cmddata_reg_opr *)rfic_sw_cmd->data);

    mbox_h2v.ctrl.op_code = DCOC_CAL;
    //mbox_h2v.ctrl.start_stop = cmd_data->start_stop;
    //mbox_h2v.ctrl.bandwidth = 0;
    mbox_h2v.ctrl.rcvr = 0;
    //mbox_h2v.msbl16 = cmd_data->size;
    //mbox_h2v.lsb32 = cmd_data->addr;

    vLa9310MbxSend(&mbox_h2v);
    xRet = vLa9310MbxReceive(&mbox_v2h);

    if ((pdFAIL == xRet) || (0 != mbox_v2h.status.err_code))
    {
        log_err("%s: dc offset failed error[%d] \r\n", __func__, mbox_v2h.status.err_code);
        rfic_sw_cmd->result = RF_SW_CMD_RESULT_ERROR;
    }
    else
    {
        rfic_sw_cmd->result = RF_SW_CMD_RESULT_OK;

        cmd_data->val = mbox_v2h.msb32;

        log_err("%s: dc offset might be %08x \r\n", __func__, mbox_v2h.msb32);
    }

    return;
}


void vRficSetDcOffset(rf_sw_cmd_desc_t *rfic_sw_cmd)
{
    BaseType_t xRet = pdFAIL;
    struct sw_cmddata_dump_iq *cmd_data;
    struct la9310_mbox_v2h mbox_v2h = {0};
    struct la9310_mbox_h2v mbox_h2v = {0};

    cmd_data = ((struct sw_cmddata_dump_iq *)rfic_sw_cmd->data);

    mbox_h2v.ctrl.op_code = TX_DC_CORRECTION;
    //mbox_h2v.ctrl.start_stop = cmd_data->start_stop;
    //mbox_h2v.ctrl.bandwidth = 0;
    mbox_h2v.ctrl.rcvr = 0;
    //mbox_h2v.msbl16 = cmd_data->size;
    mbox_h2v.lsb32 = cmd_data->addr;

    vLa9310MbxSend(&mbox_h2v);
    xRet = vLa9310MbxReceive(&mbox_v2h);

    if ((pdFAIL == xRet) || (0 != mbox_v2h.status.err_code))
    {
        log_err("%s: dc offset failed error[%d] \r\n", __func__, mbox_v2h.status.err_code);
        rfic_sw_cmd->result = RF_SW_CMD_RESULT_ERROR;
    }
    else
    {
        rfic_sw_cmd->result = RF_SW_CMD_RESULT_OK;

//        log_err("%s: dc offset set to %08x \r\n", __func__, mbox_h2v.lsb32);

    int16_t i, q;

    i = mbox_v2h.msb32 >> 16;
    q = mbox_v2h.msb32 & 0xffff;

        if(mbox_v2h.msb32 != mbox_h2v.lsb32) {
            log_err("%s: got back something else %08x vs %08x \r\n", __func__, mbox_v2h.msb32, mbox_h2v.lsb32);
        }

        log_err("%s: dc offset is %d and %d \r\n", __func__, i, q);
    }

    return;
}


void vRficSetIqImbalance(rf_sw_cmd_desc_t *rfic_sw_cmd)
{
    BaseType_t xRet = pdFAIL;
    struct sw_cmddata_dump_iq *cmd_data;
    struct la9310_mbox_v2h mbox_v2h = {0};
    struct la9310_mbox_h2v mbox_h2v = {0};

    cmd_data = ((struct sw_cmddata_dump_iq *)rfic_sw_cmd->data);

    mbox_h2v.ctrl.op_code = RFNM_IQ_IMBALANCE;
    //mbox_h2v.ctrl.start_stop = cmd_data->start_stop;
    //mbox_h2v.ctrl.bandwidth = 0;
    mbox_h2v.ctrl.rcvr = 0;
    //mbox_h2v.msbl16 = cmd_data->size;
    mbox_h2v.lsb32 = cmd_data->addr;

    vLa9310MbxSend(&mbox_h2v);
    xRet = vLa9310MbxReceive(&mbox_v2h);

    if ((pdFAIL == xRet) || (0 != mbox_v2h.status.err_code))
    {
        log_err("%s: dc imbalance failed error[%d] \r\n", __func__, mbox_v2h.status.err_code);
        rfic_sw_cmd->result = RF_SW_CMD_RESULT_ERROR;
    }
    else
    {
        rfic_sw_cmd->result = RF_SW_CMD_RESULT_OK;

//        log_err("%s: dc offset set to %08x \r\n", __func__, mbox_h2v.lsb32);

    int16_t i, q;

    i = mbox_v2h.msb32 >> 16;
    q = mbox_v2h.msb32 & 0xffff;

        if(mbox_v2h.msb32 != mbox_h2v.lsb32) {
            log_err("%s: got back something else %08x vs %08x \r\n", __func__, mbox_v2h.msb32, mbox_h2v.lsb32);
        }

        log_err("%s: dc imbalance is %d and %d \r\n", __func__, i, q);
    }

    return;
}







void vRficSetChannel(rf_sw_cmd_desc_t *rfic_sw_cmd)
{
    BaseType_t xRet = pdFAIL;
    struct sw_cmddata_dump_iq *cmd_data;
    struct la9310_mbox_v2h mbox_v2h = {0};
    struct la9310_mbox_h2v mbox_h2v = {0};

    cmd_data = ((struct sw_cmddata_dump_iq *)rfic_sw_cmd->data);

    mbox_h2v.ctrl.op_code = RFNM_SET_CHANNEL;
    //mbox_h2v.ctrl.start_stop = cmd_data->start_stop;
    //mbox_h2v.ctrl.bandwidth = 0;
    mbox_h2v.ctrl.rcvr = 0;
    //mbox_h2v.msbl16 = cmd_data->size;
    mbox_h2v.lsb32 = cmd_data->addr;

    vLa9310MbxSend(&mbox_h2v);
    xRet = vLa9310MbxReceive(&mbox_v2h);

    if ((pdFAIL == xRet) || (0 != mbox_v2h.status.err_code))
    {
        log_err("%s: failed to set channel [%d] \r\n", __func__, mbox_v2h.status.err_code);
        rfic_sw_cmd->result = RF_SW_CMD_RESULT_ERROR;
    }
    else
    {
        rfic_sw_cmd->result = RF_SW_CMD_RESULT_OK;

        log_err("%s: channel set to %08x \r\n", __func__, mbox_h2v.lsb32);
    }

    return;
}






void vRficProcessTXIqData(rf_sw_cmd_desc_t *rfic_sw_cmd)
{
    BaseType_t xRet = pdFAIL;
    struct sw_cmddata_tx_iq *cmd_data;
    struct la9310_mbox_v2h mbox_v2h = {0};
    struct la9310_mbox_h2v mbox_h2v = {0};

    cmd_data = ((struct sw_cmddata_tx_iq *)rfic_sw_cmd->data);

    mbox_h2v.ctrl.op_code = IQ_MODULATED_TX;
    mbox_h2v.ctrl.start_stop = cmd_data->start_stop;
    mbox_h2v.ctrl.bandwidth = 0;
    mbox_h2v.ctrl.rcvr = 0;
    mbox_h2v.msbl16 = cmd_data->size;
    mbox_h2v.lsb32 = cmd_data->addr;

    vLa9310MbxSend(&mbox_h2v);
    xRet = vLa9310MbxReceive(&mbox_v2h);

    if ((pdFAIL == xRet) || (0 != mbox_v2h.status.err_code))
    {
        log_err("%s: iqdump failed error[%d]", __func__, mbox_v2h.status.err_code);
        rfic_sw_cmd->result = RF_SW_CMD_RESULT_ERROR;
    }
    else
    {
        rfic_sw_cmd->result = RF_SW_CMD_RESULT_OK;
    }

    return;
}

void vRficProcessSingleToneTX( rf_sw_cmd_desc_t * rfic_sw_cmd )
{
    BaseType_t xRet = pdFAIL;
    struct sw_cmddata_single_tone_TX * cmd_data;
    struct la9310_mbox_v2h mbox_v2h = { 0 };
    struct la9310_mbox_h2v mbox_h2v = { 0 };

    cmd_data = ( ( struct sw_cmddata_single_tone_TX * ) rfic_sw_cmd->data );

    mbox_h2v.ctrl.op_code = SINGLE_TONE_TX;
    mbox_h2v.ctrl.start_stop = cmd_data->start_stop;
    mbox_h2v.msbl16 = 0;
    mbox_h2v.lsb32 = cmd_data->tone_index & 0x000000FF;


    vLa9310MbxSend( &mbox_h2v );
    xRet = vLa9310MbxReceive( &mbox_v2h );

    if( ( pdFAIL == xRet ) || ( 0 != mbox_v2h.status.err_code ) )
    {
        log_err( "%s: single tone TX failed with  error[%d]", __func__, mbox_v2h.status.err_code );
        rfic_sw_cmd->result = RF_SW_CMD_RESULT_ERROR;
        return;
    }

    rfic_sw_cmd->result = RF_SW_CMD_RESULT_OK;

    return;
}

void vRficProcessLoopback( RficDevice_t * pRficDev,
                           rf_sw_cmd_desc_t * rfic_sw_cmd )
{
    struct sw_cmddata_set_loopback * cmd_data;
    uint32_t loopback = pRficDev->xLoopback;

    cmd_data = ( ( struct sw_cmddata_set_loopback * ) rfic_sw_cmd->data );

    if( cmd_data->loopback_type == NO_LOOPBACK )      /* disable all loopback */
    {
        if( loopback == AXIQ_LOOPBACK )
        {
            OUT_32( DBGGNCR, ( REMOVE_AXIQ_LOOPBACK_MASK &
                               IN_32( DBGGNCR ) ) );
        }
        else
        {
            rfic_sw_cmd->result = RF_SW_CMD_RESULT_CMD_PARAMS_INVALID;
            log_err( "\r\n%s: No looopback set \r\n", __func__ );
        }
    }
    else
    {
        if( cmd_data->loopback_type == AXIQ_LOOPBACK )
        {
            OUT_32( DBGGNCR, ( SET_AXIQ_LOOPBACK_MASK |
                               IN_32( DBGGNCR ) ) );
        }
    }

    rfic_sw_cmd->result = RF_SW_CMD_RESULT_OK;

    return;
}

void xRficProcessCtrlDemodGain( RficDevice_t *pRficDev, rf_sw_cmd_desc_t *pSwCmdDesc )
{
    BaseType_t xRet;
    struct sw_cmddata_demod_gain *CmdData;
    rf_priv_mdata_t *mdata = &pRficDev->pRfHif->rf_priv_mdata;
    CmdData = ( struct sw_cmddata_demod_gain * )&pSwCmdDesc->data[0];
    log_dbg( "%s: Rf Attn : %d , BB Gain: %d \n\r", __func__, CmdData->rf_attn, CmdData->bb_gain );

    /* Update gain settings in demod registers */
    xRet = RficDemodGainCtrl(pRficDev, CmdData->rf_attn, CmdData->bb_gain);
    if( xRet < 0 )
    {
        log_err( "%s: Ctrl Demod Gain Failed, error[%d]\n\r", __func__, xRet );
        goto err;
    }

    /* Update mdata*/
    mdata->demod_rf_attn = CmdData->rf_attn;
    mdata->demod_bb_gain = CmdData->bb_gain;

    pSwCmdDesc->result = RF_SW_CMD_RESULT_OK;
    return;
err:
    pSwCmdDesc->result = RF_SW_CMD_RESULT_ERROR;
    return;
}

void xRficProcessCtrlVgaGain( RficDevice_t *pRficDev, rf_sw_cmd_desc_t *pSwCmdDesc )
{
    struct sw_cmddata_vga_gain *CmdData;
    rf_priv_mdata_t *mdata = &pRficDev->pRfHif->rf_priv_mdata;
    CmdData = ( struct sw_cmddata_vga_gain * )&pSwCmdDesc->data[0];

    /* Writing val for DAC1*/
    if( mdata->vga_dac1_val != CmdData->dac1_val )
    {
        prvDacWriteUpdate(CmdData->dac1_val, DAC1_I2C_ADDR);

	/* Update mdata*/
	mdata->vga_dac1_val = CmdData->dac1_val;
    }

    /* Writing val for DAC2*/
    if( mdata->vga_dac2_val != CmdData->dac2_val )
    {
        prvDacWriteUpdate(CmdData->dac2_val, DAC2_I2C_ADDR);

	/* Update mdata*/
	mdata->vga_dac2_val = CmdData->dac2_val;
    }

    /*TODO : Error handling part is missing as I2C API doesn't return any pass
     * or fail val*/

    pSwCmdDesc->result = RF_SW_CMD_RESULT_OK;
    return;
}

void xRficProcessFastCalib( RficDevice_t *pRficDev )
{
    RficSynthAdjustPllFastCal( pRficDev );
}

