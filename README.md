# enc28j60.device for TF534

This is a combo change, rework has been done to the SPI interface in order 
to point a the SPI on the **TF534**, this port is unused but it's still
functional, so one day, I just got bored and decided to do something with
it, like connect a **ENC28J60** SPI Ethernet module. 

This is a fork of [Mathesar/enc28j60.device][upstream], he is the real VIP.

I also added a **cross-compiles from Linux** using a Docker'd vbcc, removing the 
requirement to build on a real Amiga.

## Status

Working. Validated on real hardware with live ARP capture. TCP/IP up
and stable under both Roadshow and MiamiDx with DHCP.

Measured throughput: ~100 KB/s sustained TCP RX over a local LAN. See
*Known limitations* for the architectural ceiling and *Future work* for
the path to higher numbers.

## Hardware

- **Amiga 500** (or any 68k Amiga that accepts the TF534)
- **TF534** accelerator: 68030, configurable up to 50 MHz
- **AmigaOS 3.2 / 3.2.x** (Kickstart 47.111+)
- **ENC28J60** SPI Ethernet module, wired to the TF534 SPIPORT header:

  | SPIPORT pin | ENC28J60 pin | Notes                          |
  | ----------- | ------------ | ------------------------------ |
  | 1 (3V3)     | VCC          |                                |
  | 2 (GND)     | GND          |                                |
  | 3 (MOSI)    | SI           |                                |
  | 4 (MISO)    | SO           |                                |
  | 5 (SCK)     | SCK          | via 74HC14N inverter (1 gate)  |
  | 6 (CS1)     | CS           |                                |

The 74HC14N inverter is required because the TF534 SPI port operates in
**SPI Mode 2** (CPOL=1, CPHA=0) while the ENC28J60 needs **Mode 0**.
Inverting just the clock line converts between the two without any
software changes.

If you want to go ahead and invert this in the CPLD source go ahead, 
I wanted people to be able to use this without faffing around with CPLD
compilers and JTAG programmers.

## What changed vs. upstream

### SPI transport (`sd-plus/spi.c`, `sd-plus/spi.h`)

Replaced. The public API is preserved byte-for-byte so the rest of the
driver compiles unchanged.

#### 1. Register layout

| Concept       | Upstream (a500-sd-plus) | This fork (TF534)  |
| ------------- | ----------------------- | ------------------ |
| Control reg   | gayle / IDE-style       | `$E90000`          |
| Data reg      | gayle / IDE-style       | `$E90004`          |
| CS for ENC    | dedicated GPIO          | CTRL bit 1 (CS1)   |
| Clock divider | software                | hardwired CLKCPU/4 |

#### 2. Read-side-effect workaround (the only non-trivial change)

The TF534 SPI port has a hardware quirk: reading the data register at
`$E90004` returns the byte from the previous transfer **and**
immediately starts a new 8-bit transfer with `$FF` on MOSI. Naive
back-to-back reads while CS is asserted would clock spurious `$FF`
bytes into the slave - and for the ENC28J60, `$FF` is the **System
Reset Command**.

The fix is built around a new `pending_target` field on the `spi_t`
handle:

- `spi_read` reads all but the last byte normally, then leaves the final
  byte parked in the shift register and stores its destination address
  in `pending_target`.
- The next `spi_read` consumes that pending byte as its own first byte;
  the triggered `$FF` clocks the next slave byte in - which is exactly
  what a chained read wants.
- `spi_deselect` raises CS first, then drains the pending byte. The
  triggered `$FF` is now invisible to the slave because CS is high.

This produces the same on-the-wire transactions as the validated
`enc_rbm()` / `enc_rcr()` paths in the standalone `encping.c` test
program in the upstream TF534 reference materials.

#### 3. CRC stubs

The SimpleSPI core has a CRC7/CRC16 accelerator for SD framing. The
TF534 SPI port has none. The ENC28J60 computes Ethernet CRC32 in
silicon, so this is a no-op for the network path. `spi_crc_reset()`
and `spi_crc_read()` are stubbed out and
`SPI_CRC_ACCELERATION_SUPPORTED` is left undefined so any SD-only code
paths compile out cleanly.

#### 4. CS line selection

`spi_select()` ignores the `channel` argument and unconditionally
drives CS1. This is safe because only the ENC28J60 channel is functional in this build.

#### 5. Speed control

`spi_set_speed()` is a stub. The TF534 SPI clock divider is fixed in
the CPLD at CLKCPU/4 and has no software-visible control.

### Build system (`build_net`)

New. Cross-compiles from Linux x86 using
[walkero/docker4amigavbcc][docker]. The upstream project required
building on a real Amiga with vbcc installed natively. This fork drops
that requirement: hack on a fast machine, deploy the device file to the
CF card via `affs`, run on the Amiga.

[docker]: https://github.com/walkero-gr/docker4amigavbcc

## Building

```sh
sudo docker run --rm \
    -v ${PWD}:/opt/code \
    -v ${PWD}/build:/amiga \
    -w /opt/code \
    walkero/docker4amigavbcc:latest-m68k \
    bash -lc './build_net'
```

Outputs (in `build/`):

- `enc28j60.device`  (~6.6 KB) - the SANA-II driver
- `nic_test`         (~12 KB) - standalone hardware test, captures
  raw frames from the ENC and dumps them. Useful for validating the
  SPI layer without involving a TCP/IP stack.

`build_net` invokes `vc -c` per object then `vlink` directly,
sidestepping a vbcc bug that mishandles `+aos68k` when object files
appear on the same command line.

## Installing

You can follow Mathesar's instructions, from the amiga point of view
nothing changed.

## Known limitations

- **Expunge crash.** Mathesar's upstream warns that the driver Gurus on
  expunge (e.g. on `NetShutdown` or AmiTCP `avail flush`). This carries
  over unchanged. Workaround: reboot. Outside the scope of the SPI
  transport change.
- **10 Mbit half-duplex.** Inherent to the ENC28J60 silicon.
- **No hardware interrupt.** The TF534 CPLD has the EXTINT path
  declared but unrouted by default; the SPI_INT pad is not wired
  through. RX is driven by VBI polling (50 Hz on PAL). The architectural
  RX throughput ceiling sits around 150 KB/s as a result.


## License

GPL v3, inheriting from [Mathesar/enc28j60.device][upstream].

## Acknowledgments

- Mathesar - the original `enc28j60.device` and the SPI abstraction
  layer this fork plugs into.
- Olaf Barthel - Roadshow, `smbfs`, and decades of Amiga networking
  work.
- The TF534 community - reverse-engineering the ZXMMC IP block in the
  CPLD and documenting the SPI port behaviour.
- walkero - the `docker4amigavbcc` image that makes cross-compiling
  this driver painless.
- Stephen Leary for his dedication to creating faster and faster 
accelerators and inspiring me. 