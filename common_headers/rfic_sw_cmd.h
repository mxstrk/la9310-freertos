/* SPDX-License-Identifier: (BSD-3-Clause OR GPL-2.0)
 * Copyright 2021-2022 NXP
 */
#ifndef __RFIC_SW_CMD_H
#define __RFIC_SW_CMD_H

#pragma pack(push)
#define RF_SWCMD_DATA_SIZE    12

/* Supported RF Band */
typedef enum rf_band {
    RF_SW_BAND_N77,
    RF_SW_BAND_B13,
    RF_SW_BAND_B3,
    RF_SW_BAND_GNSS,
    RF_SW_BAND_MAX
}rf_band_t;

/* Supported Bandwidth */
typedef enum rf_bw {
    RF_BW_36MHZ,
    RF_BW_72MHZ,
    RF_BW_144MHZ,
    RF_BW_288MHZ,
    RF_BW_432MHZ,
    RF_BW_576MHZ,
    RF_BW_720MHZ,
    RF_BW_BYPASS
}rf_bw_t;

/* RFIC Software commands */
typedef enum RfSwCmdId {
    RF_SW_CMD_SET_BAND,
    RF_SW_CMD_ADJUST_PLL_FREQ,
    RF_SW_CMD_CTRL_GAIN,
    RF_SW_CMD_GET_ABS_GAIN,
    RF_SW_CMD_REG_READ,
    RF_SW_CMD_REG_WRITE,
    RF_SW_CMD_CTRL_VGA_GAIN,
    RF_SW_CMD_CTRL_DEMOD_GAIN,
    RF_SW_CMD_CTRL_LNA,
    RF_SW_CMD_DUMP_IQ_DATA,
    RF_SW_CMD_SINGLE_TONE_TX,
    RF_SW_CMD_SET_LOOPBACK,
    RF_SW_CMD_FAST_CALIB,
    RF_SW_CMD_TX_IQ_DATA,

    RF_SW_GET_RX_DC_OFFSET,

    RF_SW_SET_RX_DC_OFFSET,

    RF_SW_SET_CHANNEL,

    RF_SW_SET_IQ_IMBALANCE,

    /* phytimer phase 2a: host-triggered timed FE switch - runs switch_rf(mode) on the M4
     * (RFCTL_5 edge + rf_ctrl handoff to the i.MX8MP M7). data[0] = u32 mode
     * (0xAAAAAAAA = fe_tdd[TX] profile, 0xBBBBBBBB = fe_tdd[RX] profile). */
    RF_SW_CMD_SWITCH_RF,

    /* phytimer phase 2a step 3: M4 TDD scheduler. data[0] = period ticks (0 = stop),
     * data[1] = RX-window (duty) ticks. Gates + FE flips ride one tick grid. */
    RF_SW_CMD_TDD,

    /* v3 phase 0: one DFE-style TX window. data[0] = absolute open tick (SAMPLE
     * resolution - no slot alignment), data[1] = window length in ticks, data[2] =
     * source DAC-ring slot. The M4 rebuilds the VSPA fifo (MBOX_OPC_TX_WINDOW),
     * arms C11 open at the tick and close at tick+len; the handler blocks until
     * the open edge fires (debug-tool semantics; the phase-1 ring walker replaces it). */
    RF_SW_CMD_TX_WINDOW,

    /* Host<->M4 mailbox ownership: host boot-handshakes with the VSPA
     * (registry kernel swaps) take exclusive mailbox ownership - data[0]=1 begin
     * (M4 masks its VSPA IRQ and stops consuming), 0 end. Old M4 fw ignores the
     * unknown cmd; the host keeps its data-register fallback as belt. */
    RF_SW_CMD_VSPA_MBOX_HANDOFF,

    RF_SW_CMD_END
} RfSwCmdId_t;

typedef enum rf_sw_cmd_status {
    RF_SW_CMD_STATUS_FREE,
    RF_SW_CMD_STATUS_POSTED,
    RF_SW_CMD_STATUS_PENDING,
    RF_SW_CMD_STATUS_IN_PROGRESS,
    RF_SW_CMD_STATUS_TIMEOUT,
    RF_SW_CMD_STATUS_DONE
} rf_sw_cmd_status_t;

typedef enum rf_sw_cmd_result {
    RF_SW_CMD_RESULT_OK = 0,
    RF_SW_CMD_RESULT_CMD_INVALID,
    RF_SW_CMD_RESULT_CMD_PARAMS_INVALID,
    RF_SW_CMD_RESULT_ERROR,
    RF_SW_CMD_RESULT_NOT_IMPLEMENTED
} rf_sw_cmd_result_t;

typedef enum rf_sw_cmd_type {
    RF_SW_CMD_LOCAL = 1,
    RF_SW_CMD_REMOTE
} rf_sw_cmd_type_t;

/* RFIC Loopback types */
typedef enum loopback_type{
    NO_LOOPBACK = 0,
    AXIQ_LOOPBACK,
    LOOPBACK_END
} rf_loopback_type_t;

/* CMD: RF_SWCMD_SET_BAND */
struct sw_cmddata_set_band {
    uint32_t band;
};

/* CMD: RF_SWCMD_SET_FREQ */
struct sw_cmddata_adjust_pll_freq {
    uint32_t freq_khz;
};

/* CMD: RF_SWCMD_CTRL_GAIN */
/* CMD: RF_SWCMD_GET-ABS_GAIN */
struct sw_cmddata_gain {
    uint32_t gain_in_db;
};

/* CMD: RF_SWCMD_REG_READ */
/* CMD: RF_SWCMD_REG_WRITE */
struct sw_cmddata_reg_opr {
    uint32_t dspi_slv_id;
    uint32_t addr;
    uint32_t val;
};

/* CMD: RF_SWCMD_CTRL_VGA_GAIN */
struct sw_cmddata_vga_gain {
    uint32_t dac1_val;
    uint32_t dac2_val;
};

/* CMD: RF_SWCMD_CTRL_DEMOD_GAIN */
struct sw_cmddata_demod_gain {
    uint32_t rf_attn;
    uint32_t bb_gain;
};

/* CMD: RF_SWCMD_CTRL_LNA */
struct sw_cmddata_ctrl_lna {
    uint32_t state;
};

/* CMD: RF_SWCMD_DUMP_IQ_DATA (the stream word). The anchor is INTENT, not a tick
 * (v2 charter P2): the mint must satisfy t0 == anchor_phase (mod anchor_granule) at
 * the first opportunity at or after the default mint. granule == 0 = no request.
 * A phase cannot go stale, so the pre-P2 staleness heuristics (kernel send-time
 * projection, M4 10 s accept window) are gone WITH the absolute-tick field. */
struct sw_cmddata_dump_iq {
    uint32_t addr;
    uint32_t anchor_phase;
    uint32_t anchor_granule;
};

/* CMD: RF_SW_CMD_TX_IQ_DATA (legacy NXP TX IQ test path - kept on its original layout) */
struct sw_cmddata_tx_iq {
    uint32_t addr;
    uint32_t size;
    uint32_t start_stop;
};

/* CMD: RF_SW_CMD_SINGLE_TONE_TX */
struct sw_cmddata_single_tone_TX {
    uint32_t tone_index;
    uint32_t start_stop;
};

/* CMD: RF_SW_CMD_SET_LOOPBACK */
struct sw_cmddata_set_loopback {
    uint32_t loopback_type;
};


/* Software commands parameters */
typedef struct rf_sw_cmd_desc {
    uint32_t cmd;        /* command id */
    uint32_t flags;      /* remote or local */
    uint32_t timeout;    /* command specific timeout in us */
    uint32_t core_id;    /* Descriptor owner */
    uint32_t ipi_event_id;
    uint32_t status;     /* desc status */
    uint32_t result;
    uint8_t  data[RF_SWCMD_DATA_SIZE]; /* command specific data */
} rf_sw_cmd_desc_t;

#pragma pack(pop)
#endif //__RFIC_SW_CMD_H

