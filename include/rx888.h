#ifndef RX888_H
#define RX888_H

/*
 * RX888 / RX888 MkII protocol constants + streamer API.
 *
 * The FX3 command IDs, SETARG IDs, and GPIO bit assignments below track
 * ringof/rx888-firmware (SDDC_FX3/protocol.h) — the firmware this toolset
 * loads, and therefore the authority for the wire protocol. The names follow
 * KA9Q-radio where the two agree; where they differ (notably the LED GPIO
 * bits), the firmware wins. If you started from other RX888 streamer
 * codebases you may see different names/values; prefer these.
 */

#include <stdint.h>
#include <libusb-1.0/libusb.h>

/* ------------------------------ USB identity ----------------------------- */
/* Cypress FX3 vendor id; product id differs between the ROM bootloader and
 * the loaded application firmware. */
#define RX888_VID      0x04b4u
#define RX888_PID_BOOT 0x00f3u   /* FX3 bootloader (firmware not yet loaded) */
#define RX888_PID_APP  0x00f1u   /* SDDC_FX3 application firmware running */

/* ------------------------------ FX3 commands ----------------------------- */
/* (KA9Q-radio constants) */
enum FX3Command {
  // Start GPII engine and stream the data from ADC
  // WRITE: UINT32
  STARTFX3 = 0xAA,

  // Stop GPII engine
  // WRITE: UINT32
  STOPFX3 = 0xAB,

  // Get the information of device
  // including model, version
  // READ: UINT32
  TESTFX3 = 0xAC,

  // Control GPIOs
  // WRITE: UINT32
  GPIOFX3 = 0xAD,

  // Write data to I2c bus
  // WRITE: DATA
  // INDEX: reg
  // VALUE: i2c_addr
  I2CWFX3 = 0xAE,

  // Read data from I2c bus
  // READ: DATA
  // INDEX: reg
  // VALUE: i2c_addr
  I2CRFX3 = 0xAF,

  // Reset USB chip and get back to bootloader mode
  // WRITE: NONE
  RESETFX3 = 0xB1,

  // Set Argument, packet Index/Vaule contains the data
  // WRITE: (Additional Data)
  // INDEX: Argument_index
  // VALUE: arguement value
  SETARGFX3 = 0xB6,

  // Start ADC with the specific frequency
  // Optional, if ADC is running with crystal, this is not needed.
  // WRITE: UINT32 -> adc frequency
  STARTADC = 0xB2,

  // Read firmware diagnostic counters (DMA/GPIF/PIB/I2C/Si5351/GPIO state).
  // See struct fx3_stats in src/fx3_cmd/fx3_stats.h for the payload layout.
  // READ: DATA
  GETSTATS = 0xB3,

  // R82XX family Tuner functions
  // Initialize R82XX tuner
  // WRITE: NONE
  TUNERINIT = 0xB4,

  // Tune to a sepcific frequency
  // WRITE: UINT64
  TUNERTUNE = 0xB5,

  // Stop Tuner
  // WRITE: NONE
  TUNERSTDBY = 0xB8,

  // Read Debug string if any
  // READ:
  READINFODEBUG = 0xBA,
};

/* ------------------------------ SETARG IDs ------------------------------- */
/* (KA9Q-radio constants) */
enum ArgumentList {
    // Set R8xx lna/mixer gain
    // value: 0-29
    R82XX_ATTENUATOR = 1,

    // Set R8xx vga gain
    // value: 0-15
    R82XX_VGA = 2,

    // Set R8xx sideband
    // value: 0/1
    R82XX_SIDEBAND = 3,

    // Set R8xx harmonic
    // value: 0/1
    R82XX_HARMONIC = 4,

    // Set DAT-31 Att
    // Value: 0-63
    DAT31_ATT = 10,

    // Set AD8370 VGA gain
    // Value: 0-255
    AD8370_VGA = 11,

    // Preselector
    // Value: 0-2
    PRESELECTOR = 12,

    // VHFATT
    // Value: 0-15
    VHF_ATTENUATOR = 13,

    // Watchdog max recovery count before escalating to reset (0 = unlimited)
    WDG_MAX_RECOV = 14,

    // Enable(!=0)/disable(0) cold-start reset escalation
    WDG_RESET_ESCALATE = 15,
};

#define OUTXIO0 (1U << 0) 	// ATT_LE
#define OUTXIO1 (1U << 1) 	// ATT_CLK
#define OUTXIO2 (1U << 2) 	// ATT_DATA
#define OUTXIO3 (1U << 3)  	// SEL0
#define OUTXIO4 (1U << 4) 	// SEL1
#define OUTXIO5 (1U << 5)  	// SHDWN
#define OUTXIO6 (1U << 6)  	// DITH
#define OUTXIO7 (1U << 7)  	// RAND

#define OUTXIO8 (1U << 8) 	// 256
#define OUTXIO9 (1U << 9) 	// 512
#define OUTXI10 (1U << 10) 	// 1024
#define OUTXI11 (1U << 11)  	// 2048
#define OUTXI12 (1U << 12) 	// 4096
#define OUTXI13 (1U << 13)  	// 8192
#define OUTXI14 (1U << 14)  	// 16384
#define OUTXI15 (1U << 15)  	// 32768
#define OUTXI16 (1U << 16)

/* ------------------------------- GPIO bits ------------------------------- */
/* Matches enum GPIOPin in ringof/rx888-firmware SDDC_FX3/protocol.h — the
 * firmware this toolset loads, and the authority for the GPIO control word
 * (GPIOFX3) and the GETSTATS gpio_state readback.  Note this differs from the
 * KA9Q-radio map, which assigns LED_YELLOW/LED_RED/LED_BLUE to bits 10/11/12;
 * the firmware exposes only LED_BLUE, at bit 11. */
enum GPIOPin {
    SHDWN    = OUTXIO5,
    DITH     = OUTXIO6,
    RANDO    = OUTXIO7,
    BIAS_HF  = OUTXIO8,
    BIAS_VHF = OUTXIO9,
    LED_BLUE = OUTXI11,
    ATT_SEL0 = OUTXI13,
    ATT_SEL1 = OUTXI14,
    VHF_EN   = OUTXI15,
    PGA_EN   = OUTXI16,
};

/* ----------------------------- Si5351 constants -------------------------- */
/*
 * This project only intends to support basic, fixed SI5351 setup (if/when
 * used). No arbitrary-rate synthesis helpers are exposed here.
 */
static const uint8_t SI5351_ADDR = (uint8_t)(0x60u << 1);

enum SI5351Registers {
  SI5351_REGISTER_PLL_SOURCE   = 15,
  SI5351_REGISTER_CLK_BASE     = 16,
  SI5351_REGISTER_MSNA_BASE    = 26,
  SI5351_REGISTER_MSNB_BASE    = 34,
  SI5351_REGISTER_MS0_BASE     = 42,
  SI5351_REGISTER_MS1_BASE     = 50,
  SI5351_REGISTER_PLL_RESET    = 177,
  SI5351_REGISTER_CRYSTAL_LOAD = 183
};

enum SI5351CrystalLoadValues {
  SI5351_VALUE_CLK_PDN          = 0x80,
  SI5351_VALUE_CRYSTAL_LOAD_6PF = 0x01 << 6 | 0x12,
  SI5351_VALUE_PLLA_RESET       = 0x20,
  SI5351_VALUE_PLLB_RESET       = 0x80,
  SI5351_VALUE_MS_INT           = 0x40,
  SI5351_VALUE_CLK_SRC_MS       = 0x0c,
  SI5351_VALUE_CLK_DRV_8MA      = 0x03,
  SI5351_VALUE_MS_SRC_PLLA      = 0x00,
  SI5351_VALUE_MS_SRC_PLLB      = 0x20
};

/* The streaming engine that used to live here is now in librx888.
 * See include/librx888.h for the public API.
 */

#endif
