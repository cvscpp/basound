# Alesis IO26 Playback Issue - Root Cause Analysis (REVISED)

## Symptom
- Driver loads and detects Alesis IO26 device
- Playback stream starts (FreeBSD reports playback running)
- No audio is heard from the device

## Debug Output Evidence
```
basound_dice0: dice: ensure_tx dmach=0... itx_enable ret=0 run=1
basound_dice0: dice: ensure_rx dmach=0... irx_enable ret=0 run=1
basound_dice0: dice: playback start rate=44100 pcm_ch=2 dev_ch=8 dbs=9 fdf=0x01
fwohci0: Isochronous receive err 845d(ack data_err)
```

The device does start transmitting back (the host receives packets on the
armed channel), but the received packets fail the data-CRC check
(ack_data_error, fwohcicode[0x1d]).

## What the error means (direction matters)
`Isochronous receive ... data_err` is the OHCI **receive** context: the
device's capture transmitter is running but producing bad packets.  A
device whose capture transmitter is healthy is a device that has
partially locked to the host's playback stream — so the host's TX is
*being received*, yet the stream is wrong enough that the device never
outputs audio and its own transmitter stays broken.

## Earlier conclusion (CMP) was WRONG
The previous version of this file blamed missing CMP (iPCR/oPCR plug
management).  That is not the cause:

- The upstream ALSA dice driver (linux/sound/firewire/dice/dice-stream.c)
  performs **no PCR plug management at all**.  It programs the DICE
  registers directly: TX_ISOCHRONOUS / RX_ISOCHRONOUS / TX_SPEED, then
  GLOBAL_ENABLE=1.  The FreeBSD port already does exactly this
  (dice_program_device()).
- `cmp.c` in sys/dev/basound/dice/ is the unmodified Linux file and is
  not compiled; adding it would not fix anything.

## Actual root cause
Upstream amdtp-stream.c is the only firewire audio driver that uses
**CIP_BLOCKING without CIP_UNAWARE_SYT** — i.e. DICE is both blocking-mode
and SYT-aware:

1. **Blocking mode**: each cycle transmits a packet with either
   `syt_interval` data blocks (8 at 32/44.1/48 kHz, 16 at 88.2/96 kHz,
   32 at 176.4/192 kHz) and a *valid SYT presentation time*, or an empty
   NODATA packet (FDF=0xff, SYT=0xffff, zero data blocks).  The port
   previously sent the non-blocking pattern (~5.5 variable blocks every
   cycle) that the DICE firmware does not schedule against.

2. **SYT presentation timestamps**: the port always sent SYT=0xffff
   (NO_INFO).  DICE devices recover their media clock from the SYT
   sequence of the host stream; with no timestamps the device never
   locks its DAC clock, so no audio (and its own transmitter stays
   broken, matching the data_err above).

3. The port also never tracked the isochronous cycle, so it could not
   generate the timestamps even if it wanted to.  fwohci starts the IT
   context at `fwohci_next_cycle()` (cycle+8 rounded up to 16); one
   packet goes out per 125 us cycle.

## Fix (implemented in dice_streaming.c)
- Ported the upstream SYT machinery 1:1:
  - `dice_calculate_syt_offset()`  = amdtp-stream.c calculate_syt_offset()
  - `dice_compute_syt()`           = amdtp-stream.c compute_syt()
  - `dice_blocking_framing()`      = pool_ideal_syt_offsets() +
                                     pool_blocking_data_blocks()
  - `dice_blocking_init()`         = initial_state table + transfer_delay
- TX fill now emits blocking-mode packets with computed SYT; empty cycles
  carry the CIP header with FDF=0xff/SYT=0xffff (NODATA).
- `dice_first_tx_cycle()` mirrors fwohci_next_cycle(); the stream tracks
  the transmission cycle (one packet per cycle) and re-syncs on every
  context (re)start (ensure_host_tx count 0->1, TX stall restart).
- RX handler now derives the data-block count from the received CIP
  header (FDF=0xff => empty packet), matching the device's blocking-mode
  capture stream instead of assuming the old non-blocking pattern.

## Test Case
```bash
# After fix, this should produce audio:
tools/dice_play_test.sh /dev/dsp1 6
# Verify the first TX packet carries a real SYT:
dmesg | grep "dice: itx_enable"
#   q1 should now be 0x1xfxxx (FMT=0x10, FDF=sfc, SYT != 0xffff)
#   on 44.1 kHz: q1 = 0x10 01 ssss, i.e. be32(q1) = 0x1001xxxx
# Isochronous data_err on receive should disappear once the device locks:
dmesg | grep "data_err"   # should be empty or only the first ~100ms
```
