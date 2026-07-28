// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef _BASOUND_HDSP_H_
#define _BASOUND_HDSP_H_

#include <sys/param.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/conf.h>
#include <linux/workqueue.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pci.h>
#include <sound/control.h>
#include "../audio_stream.h"

/* Register Offsets */
#define HDSP_resetPointer               0
#define HDSP_freqReg                    0
#define HDSP_outputBufferAddress        32
#define HDSP_inputBufferAddress         36
#define HDSP_controlRegister            64
#define HDSP_interruptConfirmation      96
#define HDSP_outputEnable               128
#define HDSP_control2Reg                256
#define HDSP_midiDataOut0               352
#define HDSP_midiDataOut1               356
#define HDSP_fifoData                   368
#define HDSP_inputEnable                384

#define HDSP_statusRegister             0
#define HDSP_timecode                   128
#define HDSP_status2Register            192
#define HDSP_midiDataIn0                360
#define HDSP_midiDataIn1                364

#define HDSP_midiControl                368
#define HDSP_midiStatus                 368

/* MIDI status bits */
#define HDSP_midiStatusInputAvailable  (1<<0)
#define HDSP_midiStatusOutputReady     (1<<1)
#define HDSP_midiControlOutputEnable   (1<<0)
#define HDSP_midiControlInputEnable    (1<<1)

#define HDSP_fifoStatus                 400

/* Peak / RMS level meter registers (direct BAR reads, no FIFO) */
#define HDSP_playbackPeakLevel  4096    /* 26 × uint32 */
#define HDSP_inputPeakLevel     4224    /* 26 × uint32 */
#define HDSP_outputPeakLevel    4352    /* 28 × uint32 */
#define HDSP_playbackRmsLevel   4612    /* 26 × uint64 (lo/hi split) */
#define HDSP_inputRmsLevel      4868    /* 26 × uint64 (lo/hi split) */

#define HDSP_MATRIX_MIXER_SIZE          2048

#define UNITY_GAIN              32768   /* 0x8000 = 0 dB, maximum HDSP gain */
#define MINUS_INFINITY_GAIN     0       /* 0x0000 = silence */

enum HDSP_IO_Type {
	Digiface,
	Multiface,
	H9652,
	H9632,
	RPM,
	Unknown
};

struct hdsp_midi {
	struct hdsp *hdsp;
	int id;
	/* struct snd_rawmidi_substream *input; */
	/* struct snd_rawmidi_substream *output; */
	char pending;
	struct mtx lock;
	/* struct timer_list timer; */
	int istimer;
};

struct hdsp {
	struct mtx            lock;
	struct snd_pcm_substream *capture_substream;
	struct snd_pcm_substream *playback_substream;
	
	/* MIDI */
	struct hdsp_midi      midi[2];
	struct work_struct    midi_work;
	
	uint32_t              control_register;      /* cached value */
	uint32_t              control2_register;     /* cached value */
	
	char                 *card_name;
	enum HDSP_IO_Type     io_type;
	unsigned short        firmware_rev;
	unsigned short        state;
	
	size_t                period_bytes;	/* kept for compat; prefer runtime->period_bytes */
	unsigned char         max_channels;
	unsigned char         ss_in_channels;
	unsigned char         ss_out_channels;
	
	struct snd_dma_buffer capture_dma_buf;
	struct snd_dma_buffer playback_dma_buf;
	unsigned char         *capture_buffer;
	unsigned char         *playback_buffer;
	
	int                   running;
	int                   system_sample_rate;
	uint32_t              dds_value;
	
	int                   irq;
	void                 *iobase;
	
	struct snd_card      *card;
	struct snd_pcm       *pcm;
	struct pci_dev       *pci;
	struct cdev          *cdev;          /* /dev/hdspN mixer interface */
	
	unsigned short        mixer_matrix[HDSP_MATRIX_MIXER_SIZE];
	
	/* Audio streaming framework */
	struct audio_stream   capture_stream;
	struct audio_stream   playback_stream;
};

#define HDSP_audioIRQPending    (1<<0)
#define HDSP_midi0IRQPending    (1<<1)
#define HDSP_midi1IRQPending    (1<<2)

#define HDSP_BufferPositionMask 0x000FFC0 /* Bit 6..15 : h/w buffer pointer */
#define HDSP_BufferID           (1<<26)

#define HDSP_InitializationComplete (1<<0)
#define HDSP_Start              (1<<0)  /* start DMA engine (control reg bit 0) */
#define HDSP_Latency0           (1<<1)  /* interrupt period = 2^(latency+7) samples */
#define HDSP_Latency1           (1<<2)
#define HDSP_Latency2           (1<<3)
#define HDSP_LatencyMask        (HDSP_Latency0|HDSP_Latency1|HDSP_Latency2)
#define HDSP_ClockModeMaster    (1<<4)
#define HDSP_AudioInterruptEnable (1<<5)
#define HDSP_SPDIFInputSelect0  (1<<14)
#define HDSP_SPDIFInputCoaxial  (HDSP_SPDIFInputSelect0)
#define HDSP_LineOut            (1<<24)

/* Encode/decode latency field (bits 1-3 of control register).
 * period_bytes = 1 << (decode_latency + 8).  Max latency=7 → 32768 bytes. */
#define hdsp_encode_latency(x)  (((x)<<1) & HDSP_LatencyMask)
#define hdsp_decode_latency(x)  (((x) & HDSP_LatencyMask) >> 1)

/*
 * Per-channel planar DMA buffer size.
 * Supports double-buffering at the maximum hardware period (latency=7 → 8192 frames):
 *   8192 frames × 4 bytes/sample × 2 periods = 65536 bytes per channel.
 */
#define HDSP_CHANNEL_BUFFER_BYTES   (8192 * 4 * 2)

/*
 * Total DMA area that must be allocated for the hardware to access.
 * The HDSP always DMA's 26 channels' worth of data regardless of the
 * I/O box type (Multiface, Digiface, etc.).  We allocate for 27 channels
 * (26 + 1 extra) to match Linux, which adds a guard channel because the
 * hardware writes one byte beyond the last page.
 */
#define HDSP_DMA_AREA_BYTES	(27 * HDSP_CHANNEL_BUFFER_BYTES)

#define HDSP_DllError (1<<21)
#define HDSP_PROGRAM	        0x020
#define HDSP_CONFIG_MODE_0	0x040
#define HDSP_CONFIG_MODE_1	0x080
#define HDSP_VERSION_BIT	(0x100 | HDSP_S_LOAD)
#define HDSP_S200		0x800
#define HDSP_S300		(0x100 | HDSP_S200)
#define HDSP_CYCLIC_MODE	0x1000
#define HDSP_S_PROGRAM		(HDSP_CYCLIC_MODE|HDSP_PROGRAM|HDSP_CONFIG_MODE_0)
#define HDSP_S_LOAD		(HDSP_CYCLIC_MODE|HDSP_PROGRAM|HDSP_CONFIG_MODE_1)

/* status2Register version bits (read after firmware load) */
#define HDSP_version1		(1<<1)	/* set for Multiface */
#define HDSP_version2		(1<<2)	/* set for RPM */

#define DDS_NUMERATOR 110000000000ULL

/* HDSP_FIRMWARE_SIZE is the firmware size in bytes (24413 uint32_t words) */
#define HDSP_FIRMWARE_SIZE (24413 * 4)
/* Iteration counts for hdsp_fifo_wait() at 100µs per iteration */
#define HDSP_SHORT_WAIT  30	/*   3 ms — for JTAG probe */
#define HDSP_LONG_WAIT  5000	/* 500 ms — for firmware upload */

/* PCI revision IDs used to select firmware variant */
#define HDSP_PCI_REVISION_DSP		0x37	/* original Digiface/Multiface */
#define HDSP_PCI_REVISION_DSP_11	0x11	/* rev11 Digiface/Multiface */

/* Internal helper */
static inline void *snd_kcontrol_chip(struct snd_kcontrol *kcontrol)
{
	return kcontrol->private_data;
}

/*
 * Matrix mixer address encoding — matches Linux hdsp.c.
 * firmware_rev == 0xa is a very old card; all modern cards use the else branch.
 *
 * Playback channel `in' routed to output `out':
 *   addr = (52 * out) + (26 + in)   [firmware_rev != 0xa]
 *
 * Capture (input) channel `in' monitored on output `out':
 *   addr = (52 * out) + in           [firmware_rev != 0xa]
 */
static inline int hdsp_playback_to_output_key(struct hdsp *hdsp, int in, int out)
{
	if (hdsp->firmware_rev == 0xa)
		return (64 * out) + (32 + in);
	else
		return (52 * out) + (26 + in);
}

static inline int hdsp_input_to_output_key(struct hdsp *hdsp, int in, int out)
{
	if (hdsp->firmware_rev == 0xa)
		return (64 * out) + in;
	else
		return (52 * out) + in;
}

/* Planar DMA buffer management — called from hdsp_bsd.c */
int  hdsp_alloc_dma_buffers(struct hdsp *hdsp);
void hdsp_free_dma_buffers(struct hdsp *hdsp);

/* Internal functions to be ported */
int snd_hdsp_create(struct snd_card *card, struct hdsp *hdsp);
int snd_hdsp_enable_io(struct hdsp *hdsp);
void snd_hdsp_interrupt(void *arg);
int snd_hdsp_upload_firmware(struct hdsp *hdsp);
int hdsp_set_rate(struct hdsp *hdsp, int rate, int called_internally);
int hdsp_read_gain(struct hdsp *hdsp, int addr);
int hdsp_write_gain(struct hdsp *hdsp, unsigned int addr, unsigned short data);
void snd_hdsp_create_mixer(struct snd_card *card, struct hdsp *hdsp);
void snd_hdsp_midi_work(struct work_struct *work);

/* Character device for the native FreeBSD mixer tool */
int  hdsp_cdev_create(struct hdsp *hdsp, int unit);
void hdsp_cdev_destroy(struct hdsp *hdsp);

/* PCM operations table — registered via snd_pcm_set_ops() in snd_hdsp_create() */
extern const struct snd_pcm_ops hdsp_pcm_ops;

/* PCM operations */
int snd_hdsp_hw_params(struct snd_pcm_substream *substream, void *hw_params);
int snd_hdsp_prepare(struct snd_pcm_substream *substream);
int snd_hdsp_trigger(struct snd_pcm_substream *substream, int cmd);
unsigned long snd_hdsp_pointer(struct snd_pcm_substream *substream);

/* Register access helpers */
static inline uint32_t hdsp_read(struct hdsp *hdsp, int reg)
{
	return bus_space_read_4(rman_get_bustag(hdsp->pci->res[0]), 
				rman_get_bushandle(hdsp->pci->res[0]), reg);
}

static inline void hdsp_write(struct hdsp *hdsp, int reg, uint32_t val)
{
	bus_space_write_4(rman_get_bustag(hdsp->pci->res[0]), 
			  rman_get_bushandle(hdsp->pci->res[0]), reg, val);
}

#endif /* _BASOUND_HDSP_H_ */
