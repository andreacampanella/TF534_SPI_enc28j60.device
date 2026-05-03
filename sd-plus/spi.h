/*  spi.h - SPI transport for ENC28J60 on TF534 SPI port
 *
 *  Drop-in replacement for the a500-sd-plus "Simple SPI" header used by
 *  Mathesar's enc28j60.device (https://github.com/Mathesar/enc28j60.device).
 *  API surface preserved verbatim; only the underlying register layout
 *  and transfer mechanics differ.
 *
 *  Hardware target
 *  ---------------
 *    TF534 accelerator card with the TF534 SPI port in the CPLD:
 *      $E90000 - control / status register
 *      $E90004 - data register
 *    SPIPORT header pin 7 (CS1) drives the ENC28J60 module's CS line.
 *    SPIPORT header pin 6 (CS0) is shared with U1 flash and probed by
 *    AmigaOS at boot, so it is never used here.
 *    The TF534 SPI port operates in Mode 2 (CPOL=1, CPHA=0); a 74HC14N inverter
 *    on the CLK line converts to Mode 0 for the ENC28J60.
 *
 *  Original SPI library (c) Mathesar - GPL v3.
 *  TFSPI transport modifications (c) 2026 - GPL v3.
 */

#ifndef SPI_H_INCLUDED
#define SPI_H_INCLUDED

#include <exec/exec.h>
#include <stdint.h>

/* The a500-sd-plus / SimpleSPI cores include a CRC7/CRC16 accelerator for
 * SD card framing. The TF534 SPI port has none, and the ENC28J60 computes its own
 * CRCs in silicon, so this is a no-op for the network driver. The define
 * is left OUT so any SD-only code paths in the upper layers compile out
 * cleanly. */
/* #define SPI_CRC_ACCELERATION_SUPPORTED */

/* TF534 SPI port register addresses */
#define TFSPI_CTRL_ADDR     0xE90000UL
#define TFSPI_DATA_ADDR     0xE90004UL

/* Control register bit values (WCS=1 throughout - controller stays
 * powered; CS bits are active-low) */
#define TFSPI_CTRL_IDLE     0x0B    /* CS1=1, CS0=1 - all deasserted */
#define TFSPI_CTRL_CS1_LOW  0x09    /* CS1=0, CS0=1 - ENC selected */
#define TFSPI_CTRL_CS0_LOW  0x0A    /* CS1=1, CS0=0 - U1 flash (avoid) */
#define TFSPI_STATUS_DONE   0x04    /* status bit 2: shift complete */

/* Speed selection - the TF534 SPI clock is hardwired in the CPLD to CLKCPU/4,
 * so these are decorative no-ops kept for header API compatibility. */
#define SPI_SPEED_SLOW          0x00
#define SPI_SPEED_MEDIUM        0x01
#define SPI_SPEED_FAST          0x02

/* Logical channel IDs - kept identical to upstream so any SD-aware code
 * still compiles. Only ETHERNET is functional in this build. */
#define SPI_CHANNEL_SD          0
#define SPI_CHANNEL_ETHERNET    2
#define SPI_N_SD_CHANNELS       2
#define SPI_N_TOTAL_CHANNELS    3

/* CRC mode constants - unused, present for header parity */
#define SPI_CRC_MODE_WRITE      0x00
#define SPI_CRC_MODE_READ       0x01

/* exec.library Resource name - kept verbatim so any caller that looks
 * up the bus singleton by name still works. */
#define SPI_RESOURCE_NAME       "sd-plus"

/* Bus singleton, registered with AddResource() on first init. */
struct spi_resource_t
{
    struct Node                 node;
    UBYTE                       pad1;
    UBYTE                       pad2;
    UWORD                       pad3;
    UWORD                       pad4;
    UWORD                       Version;
    UWORD                       Revision;
    struct SignalSemaphore      semaphore;
    char                        name[sizeof(SPI_RESOURCE_NAME)];
};

/* Per-channel handle.
 *
 * The only field added vs. upstream is pending_target. It tracks the
 * byte left in the TF534 shift register at the end of an spi_read, so
 * that subsequent operations (the next spi_read, or spi_deselect) can
 * drain it in a way that does not generate a spurious $FF on MOSI while
 * CS is still asserted. See spi.c for the full rationale. */
typedef struct
{
    struct spi_resource_t       *resource;
    struct ExecBase             *SysBase;
    UBYTE                       speed;
    UBYTE                       bus_taken;
    UBYTE                       channel;
    UBYTE                       pad;
    UBYTE                       *pending_target;
} spi_t;

/* Public API - signatures match Mathesar's upstream byte for byte. */
void     spi_obtain(spi_t *spi);
void     spi_release(spi_t *spi);
void     spi_select(spi_t *spi);
void     spi_deselect(void);
void     spi_set_speed(spi_t *spi, UBYTE speed);
void     spi_read(spi_t *spi, UBYTE *buf, UWORD size);
void     spi_write(spi_t *spi, const UBYTE *buf, UWORD size);
void     spi_crc_reset(uint8_t mode);
uint16_t spi_crc_read(void);
int      spi_initialize(spi_t *spi, unsigned char channel, struct ExecBase *SysBase);
void     spi_shutdown(spi_t *spi);

#endif
