# Digi 002/003 Driver Debugging Memo — 2026-08-02

## RESOLVED — 2026-08-05: Playback confirmed working on Digi 002 Rack
## (tag: digi002rack-playback-working, commit 84706e5)
##
## Root cause: dg00x_begin_session() writes the ISOC_CHANNELS register as
## (tx_ch<<16)|rx_ch, copying Linux's formula literally. Linux's
## tx_resources/rx_resources are DEVICE-centric (tx = channel the device
## transmits on = our capture; rx = channel device receives on = our
## playback). Ours are HOST-centric (tx = we transmit = playback; rx = we
## receive = capture) — the opposite convention. The pcm_trigger call site
## passed them in our own (host-centric) order, silently swapping the two
## channel numbers in the register: the device listened for playback on
## the channel we actually used for capture, and vice versa. The TX DMA
## was transmitting perfectly valid, correctly DOT-encoded, non-silent
## audio the whole time (confirmed with a debug test-tone injector added
## for digi00x, hw.basound.debug.test_tone) with zero FireWire/ISO errors
## — the device just never received it on the channel it was listening
## to. Fixed by swapping the two channel arguments at the single call
## site in digi00x-pcm.c (pcm_trigger). See commit 84706e5 for details.
##
## The debugging below (2026-08-02) remains for historical reference; the
## "PCM buffer is all zeros" observation in that session was a *separate*
## test-methodology artifact (test.tone's `dd if=/dev/zero` legitimately
## writes silence) rather than a driver bug — the dma_area sync fix in
## commit 49c46fe was still a real and correct fix, just not the cause of
## the reported silence.

## Current Status (historical, 2026-08-02): FireWire DMA working, PCM buffer empty

The driver initializes, starts a bidirectional isochronous session, transmits
correctly-formatted CIP packets at 8000/sec, but the PCM DMA buffer contains
only zeros — so the device receives DOT-encoded silence and produces no audio.

## What WORKS

1. **Device discovery & session setup** — Digi 002/003 Rack (model=0x000002)
   detected, registers registers, begin_session returns OK

2. **Isochronous DMA** — stfree_before=8, refilled=8 every callout (~1ms),
   meaning 8000 ISO packets/sec flow on the wire. hwptr advances and wraps
   correctly. The OHCI TX context is running and completing.

3. **CIP header** — q0=0x41130000 q1=0xa001ffff matches Linux exactly.
   SPH=1, SID=1, DBS=19 (18 channels + MIDI), FMT=0x10 (AMDTP), FDF=0x00,
   SYT=0xffff.

4. **CIP TAG and channel** — xferq flag = 0x00016342. Bits 0-5 = 2 (channel),
   bits 6-7 = 1 (TAG=CIP). fwohci reads chtag from flag & 0xff, so both
   channel and tag are set correctly on the wire.

5. **Payload layout** — SPH[2]=0x00000000, MIDI[3]=be32toh(0x80000000)=0x80
   (first byte = 0x80 on wire, the "no MIDI data" marker).

6. **Session start flow** — begin_session with tx=2, rx=3, then start_tx
   fills 32 chunks, single itx_enable returns 0, start_rx starts capture.
   No crashes, no timeouts.

## What's BROKEN

**PCM buffer is all zeros.** dma_area[0]=0 throughout the session.
tx_peaks shows 18 on channel 0 (quantization noise, not real audio).
The first TX packet shows PCM[4]=0x40000000 etc. — DOT-encoded silence.

The encode loop reads from:
    ps->substream->runtime->dma_area + (ps->hwptr / 4)
and the samples at that address are zero.

## Hypothesis

The OSS->ALSA glue layer (bsm_digi00x -> snd_pcm -> basound) may be writing
audio data to a different buffer than ps->substream->runtime->dma_area.
The ALSA/FreeBSD buffer mapping uses sndbuf_setmap / sndbuf_resize, and
the pointer the driver uses might not match where the app actually writes.

## Files Modified

sys/dev/basound/digi00x/:
  - digi00x-pcm.c      — PCM trigger logic, callout, stream start/stop
  - digi00x-stream.c   — Session begin/finish, register read/write helpers
  - digi00x-streaming.c — ISO DMA open/close, CIP header, TX fill, refill
  - digi00x.h          — DG00X_ISO_TAG_CIP, struct dg00x_pcm_stream

## Active Debug Prints

1. start_tx: CIP header dump + first 6 payload quadlets
2. refill: stfree_before, refilled count, hwptr, dma_area[0] (every 100th)
3. fill_chunk: raw samples at hwptr position (first 5 calls only) -- NEW,
   NOT YET TESTED

## Recent Fixes

- Removed destructive writes to register 0x0120 (was being zeroed, likely
  disabling output routing -- value was 0xc0)
- Added SPH timestamp quadlet after CIP header (required for non-blocking
  AMDTP mode)
- CIP header bit positions match Linux exactly
- Bidirectional DMA: RX context started alongside TX (Digi 002/003 requires
  bidirectional session)
- Channel numbers changed from 0/1 to 2/3 to avoid bus conflicts

## TODO Next Session

1. **Test fill_chunk debug** — kldunload/kldload, play audio, check dmesg
   for "fill_chunk hwptr=... raw samples[0..3]=..." output

2. **If samples are zero at fill time**: The PCM buffer mapping is wrong.
   Investigate:
   - How bsm_digi00x writes to the buffer
   - Whether sndbuf_setmap/sndbuf_resize remaps after stream start
   - Whether dma_area pointer matches where OSS writes

3. **If samples are non-zero**: The DOT encoding or payload layout is wrong.
   Verify by comparing wire capture with Linux output.

4. **Consider wire capture**: If available, capture FireWire bus traffic
   with a separate machine running Linux to compare packet contents.

5. **Register dump**: The 0x0100-0x0140 register space has gaps we haven't
   fully explored. The 0x0120 (value 0xc0) and 0x0124 registers may control
   output routing -- investigate the Linux driver for clues about what these
   registers mean.
