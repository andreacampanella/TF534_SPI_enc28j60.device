/*  spi.c - SPI transport for ENC28J60 on TF534 SPI port
 *
 *  Drop-in replacement for Mathesar's a500-sd-plus/spi.c. Implements the
 *  same public API (spi_obtain / spi_release / spi_select / spi_deselect /
 *  spi_read / spi_write / ...) on completely different hardware.
 *
 *  Targets:
 *    $E90000 - control / status register
 *    $E90004 - data register: reading this returns the byte that
 *              was clocked in by the previous transfer AND immediately
 *              starts a new 8-bit transfer with $FF on MOSI. This is the
 *              central hardware quirk worked around throughout this file.
 *    Bit 1 of CTRL is CS1, the chip-select for the ENC28J60 on TF534
 *    SPIPORT pin 7. CS0 (pin 6) is shared with U1 flash and probed by
 *    AmigaOS at boot, so it is never asserted here.
 *
 *  The TF534 SPI port uses SPI mode 2 (CPOL=1, CPHA=0). A 74HC14N inverter on the CLK
 *  line converts to Mode 0 as required by the ENC28J60.
 *
 *  Read-side-effect workaround
 *  ---------------------------
 *  Because reading the data register both consumes the current shift-
 *  register byte AND triggers another 8-bit transfer with $FF, naive
 *  back-to-back reads while CS is asserted would clock spurious $FF
 *  bytes into the slave. For the ENC28J60, $FF is the System Reset
 *  Command (SRC), which would be catastrophic.
 *
 *  Strategy: every spi_read leaves its LAST byte un-read, recording
 *  its destination in spi->pending_target. The pending byte is drained
 *  in one of two places:
 *    1. The next spi_read consumes it as its own first byte; the
 *       triggered $FF clocks the next slave byte into the register,
 *       which is exactly what a chained read wants.
 *    2. spi_deselect raises CS first, THEN reads the data register.
 *       The triggered $FF is now invisible to the slave because CS
 *       is high.
 *
 *  This produces the same on-the-wire transactions as encping.c's
 *  validated enc_rbm() (Read Buffer Memory) and enc_rcr() (Read
 *  Control Register).
 *
 *  Original (c) Mathesar - GPL v3.
 *  TFSPI transport (c) 2026 - GPL v3.
 */

#include "spi.h"
#include <proto/exec.h>
#include <string.h>

#define CTRL    (*(volatile UBYTE *)TFSPI_CTRL_ADDR)
#define DATA    (*(volatile UBYTE *)TFSPI_DATA_ADDR)

/* Bounded busy-wait for the TF534 SPI port's "transfer complete" status bit.
 * The CPLD asserts STATUS_DONE within one CLKCPU/4 byte time
 * (~1us at 33MHz). 10000 iterations is comfortably above worst-case
 * and gives a clean exit if the controller ever gets stuck. */
static void wait_done(void)
{
    int i;
    for (i = 0; i < 10000; i++) {
        if (CTRL & TFSPI_STATUS_DONE) return;
    }
}

/* ---------------------------------------------------------------------
 * Bus arbitration
 * ------------------------------------------------------------------- */

void spi_obtain(spi_t *spi)
{
    struct ExecBase *SysBase = spi->SysBase;
    if (!spi->bus_taken) {
        ObtainSemaphore(&spi->resource->semaphore);
        spi->bus_taken = 1;
    }
}

void spi_release(spi_t *spi)
{
    struct ExecBase *SysBase = spi->SysBase;
    if (spi->bus_taken) {
        ReleaseSemaphore(&spi->resource->semaphore);
        spi->bus_taken = 0;
    }
}

/* ---------------------------------------------------------------------
 * Chip-select control
 *
 * The bus is semaphore-protected, so at most one channel is active at a
 * time. We track the active spi_t in a static so spi_deselect (which
 * has a void signature inherited from upstream) can reach back into the
 * handle to drain pending_target.
 * ------------------------------------------------------------------- */

static spi_t *g_active_spi = NULL;

void spi_select(spi_t *spi)
{
    /* TF534: only CS1 is exposed on the SPI header and untouched by
     * AmigaOS, so we always use it regardless of the logical channel
     * passed at init. */
    g_active_spi = spi;
    CTRL = TFSPI_CTRL_CS1_LOW;
}

void spi_deselect(void)
{
    spi_t *spi = g_active_spi;
    volatile int j;

    /* Raise CS first; any subsequent triggered $FF transfer will be
     * invisible to the slave. */
    CTRL = TFSPI_CTRL_IDLE;

    /* CS hold delay - the ENC28J60 wants tCSH >= 50ns. The volatile
     * counter prevents the compiler from optimising the loop away. */
    for (j = 0; j < 20; j++) { }

    /* Drain any byte deferred by the last spi_read. The data-register
     * read both retrieves the byte and kicks off a harmless $FF
     * transfer (CS is high). */
    if (spi && spi->pending_target) {
        Disable();
        *spi->pending_target = DATA;
        wait_done();
        Enable();
        spi->pending_target = NULL;
    }
}

void spi_set_speed(spi_t *spi, UBYTE speed)
{
    /* TF534 SPI clock is fixed at CLKCPU/4 in the CPLD; nothing to set.
     * Stored only so spi_get_speed-style callers see a consistent value. */
    spi->speed = speed;
}

/* ---------------------------------------------------------------------
 * Data transfer
 * ------------------------------------------------------------------- */

void spi_write(spi_t *spi, const UBYTE *buf, UWORD size)
{
    UWORD i;

    /* If a previous spi_read parked a byte in pending_target and the
     * caller now writes without first reading or deselecting, the
     * parked byte's destination buffer is silently lost. The ENC28J60
     * driver never does read-then-write within a single CS-asserted
     * transaction (RCR/RBM are followed by deselect; WCR/WBM/BFC/BFS
     * are pure writes), so this branch is unreachable in practice. */
    spi->pending_target = NULL;

    Disable();
    for (i = 0; i < size; i++) {
        wait_done();
        DATA = buf[i];
        wait_done();
        /* The slave's response byte now sits in the shift register.
         * We deliberately leave it there - reading would trigger an
         * unwanted $FF transfer. The next operation (another write,
         * a read, or a deselect) handles it correctly. */
    }
    Enable();
}

void spi_read(spi_t *spi, UBYTE *buf, UWORD size)
{
    UWORD i;

    if (size == 0) return;

    Disable();

    if (spi->pending_target) {
        /* Resolve the previous spi_read's deferred byte. The data-reg
         * read returns that byte AND triggers a new $FF transfer that
         * clocks the next slave byte into the shift register - exactly
         * what we want for a chained read. */
        *spi->pending_target = DATA;
        wait_done();
        spi->pending_target = NULL;
    } else {
        /* No pending byte: prime the pipeline by writing $FF. The
         * resulting transfer clocks one slave byte into the shift
         * register, ready to be consumed below. */
        wait_done();
        DATA = 0xFF;
        wait_done();
    }

    /* The shift register now holds the byte destined for buf[0].
     * Read all but the last via the consume-and-trigger pattern: each
     * DATA read returns the current byte and starts the next transfer. */
    for (i = 0; i < (UWORD)(size - 1); i++) {
        buf[i] = DATA;
        wait_done();
    }

    /* Defer the final byte. spi_deselect or the next spi_read will
     * fetch it; the shift register currently holds it. */
    spi->pending_target = &buf[size - 1];

    Enable();
}

/* ---------------------------------------------------------------------
 * CRC accelerator stubs
 *
 * The TF534 SPI port has no CRC unit. The ENC28J60 computes Ethernet CRC32 in
 * silicon, and SD framing is not used. These exist only because
 * Mathesar's upstream calls them through the same header. They are
 * never invoked from the network driver path.
 * ------------------------------------------------------------------- */

void spi_crc_reset(uint8_t mode)
{
    (void)mode;
}

uint16_t spi_crc_read(void)
{
    return 0;
}

/* ---------------------------------------------------------------------
 * Init / shutdown
 * ------------------------------------------------------------------- */

int spi_initialize(spi_t *spi, unsigned char channel, struct ExecBase *SysBase)
{
    struct spi_resource_t *spi_resource;

    if (channel >= SPI_N_TOTAL_CHANNELS) return -1;

    /* Find or create the shared bus singleton. Multiple drivers (e.g.
     * an SD driver in the future) can share the bus this way. */
    spi_resource = (struct spi_resource_t *)OpenResource(SPI_RESOURCE_NAME);
    if (spi_resource == NULL) {
        spi_resource = AllocMem(sizeof(struct spi_resource_t),
                                MEMF_PUBLIC | MEMF_CLEAR);
        if (spi_resource == NULL) return -1;

        memcpy(spi_resource->name, SPI_RESOURCE_NAME, sizeof(SPI_RESOURCE_NAME));
        InitSemaphore(&spi_resource->semaphore);
        spi_resource->node.ln_Type = NT_RESOURCE;
        spi_resource->node.ln_Pri  = 0;
        spi_resource->node.ln_Name = spi_resource->name;
        spi_resource->Version  = 2;
        spi_resource->Revision = 0;

        AddResource(spi_resource);
    }

    spi->resource         = spi_resource;
    spi->SysBase          = SysBase;
    spi->channel          = channel;
    spi->bus_taken        = 0;
    spi->speed            = SPI_SPEED_SLOW;
    spi->pending_target   = NULL;

    /* Idle the controller: CS deasserted, no transfer in flight. */
    CTRL = TFSPI_CTRL_IDLE;

    return 1;
}

void spi_shutdown(spi_t *spi)
{
    spi_deselect();   /* drains any pending byte, raises CS */
    spi_release(spi);
}
