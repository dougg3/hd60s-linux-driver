/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Elgato Game Capture HD60 S - V4L2 + ALSA driver
 *
 * Copyright (c) 2026 Doug Brown <doug@schmorgal.com>
 *
 * Two hardware generations share one driver:
 *
 *   rev 4 (PID 0x0076)    ITE IT6802E receiver owned by the on-board MCU.
 *                         The host reads a status block and writes one
 *                         enable byte.
 *   revs 1-3 (0x004F,     MStar MST3367 driven register-by-register from
 *   0x005E, 0x0074)       the host over the MCU's I2C bridge.
 *
 * The Cypress FX3 firmware is byte-identical across all four, so the USB
 * layer, the EP 0x83 stream format, the alt-setting formula and the cold
 * start are shared. Only the video front end differs, and it is reached
 * through struct hd60s_ops.
 */
#ifndef _HD60S_H_
#define _HD60S_H_

#include "hd60s-compat.h"

#include <linux/atomic.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/usb.h>
#include <linux/workqueue.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-dev.h>
#include <media/videobuf2-v4l2.h>

#include "hd60s-modes.h"	/* HD60S_CR_*, shared with the MStar matrix */
#include "hd60s-parse.h"

#define HD60S_VID		0x0fd9
#define HD60S_PID_REV1		0x004f
#define HD60S_PID_REV2		0x005e
#define HD60S_PID_REV3		0x0074
#define HD60S_PID_REV4		0x0076

#define HD60S_IF_VIDEO		0	/* EP 0x83, alt 0-4                  */
#define HD60S_IF_EVENT		1	/* EP 0x81; claimed, never opened    */
#define HD60S_EP_VIDEO		0x83

/*
 * Vendor requests. Both generations use the same six; revs 1-3 route 0xC0
 * through the MCU's I2C bridge rather than addressing chips directly.
 */
#define HD60S_REQ_C0		0xC0	/* channel read/write (MCU + I2C bridge) */
#define HD60S_REQ_C1		0xC1	/* GPIO                                  */
#define HD60S_REQ_C2		0xC2	/* FX3 stream pool byte                  */
#define HD60S_REQ_C6		0xC6	/* EP 0x81 watch list disarm             */
#define HD60S_REQ_C7		0xC7	/* I2C bitrate                           */
#define HD60S_REQ_EC		0xEC	/* cold start                            */

#define HD60S_CH64		0x0064	/* rev-4 register file                   */
#define HD60S_CH_I2C		0x5066	/* MCU I2C bridge, mode 1                */

/* I2C slaves behind the bridge on revs 1-3 */
/* 0x94 is the audio codec and 0xa2 the EDID EEPROM; neither is touched here. */
#define HD60S_I2C_PLL		0x88	/* clock/PLL block                       */
#define HD60S_I2C_CTL		0x98	/* board control device                  */
#define HD60S_I2C_TX		0x9a	/* HDMI loop-through transmitter         */
#define HD60S_I2C_MSTAR		0x9c	/* MST3367; reg 0x00 is the bank latch   */
#define HD60S_I2C_MCU		0xaa	/* the Nuvoton                           */
/* a 208-register device that is not on the capture path */
#define HD60S_I2C_EXT		0xd4

/* Channel 0x64 register file; the status block is its first 32 bytes. */
#define HD60S_REG_STREAM_EN	0x10
#define HD60S_REG_COLOR_RANGE	0x12
#define HD60S_REG_PICTURE	0x13	/* brightness/contrast/saturation/hue    */
#define HD60S_REG_AUDIO_SRC	0x3a
#define HD60S_REG_AUDIO_VOL2	0x3b	/* "volume extra" gain                   */
#define HD60S_REG_AUDIO_VOL	0x3c	/* analog volume                         */
#define HD60S_STATUS_LEN	32

/*
 * Control transfers are issued once, never retried and never canceled. The FX3
 * is very picky.
 */
#define HD60S_CTRL_TIMEOUT	5000

#define HD60S_NUM_URBS		16
#define HD60S_ISO_PACKETS	32	/* per URB, as Windows; halved on ENOMEM */
/*
 * One bulk URB. An xHCI TRB buffer pointer cannot cross a 64 KB boundary and a
 * multi-TRB transfer loses sync measurably, so 32 KB aligned to its own size is
 * one TRB whatever the allocator returns. Also the Windows driver's geometry.
 */
#define HD60S_BULK_SIZE		(32 * 1024)

/*
 * Input color space, as bank 2 register 0x48 bits 6:5 report it on the MStar
 * front end. The values are the AVI InfoFrame's Y1Y0 field.
 */
#define HD60S_CS_RGB		0
#define HD60S_CS_YCBCR_422	1
#define HD60S_CS_YCBCR_444	2

/* indices into d->pic[], and the ops->pic_controls bits that gate them */
#define HD60S_PIC_BRIGHTNESS	BIT(0)
#define HD60S_PIC_CONTRAST	BIT(1)
#define HD60S_PIC_SATURATION	BIT(2)
#define HD60S_PIC_HUE		BIT(3)
#define HD60S_PIC_ALL		(HD60S_PIC_BRIGHTNESS | HD60S_PIC_CONTRAST | \
				 HD60S_PIC_SATURATION | HD60S_PIC_HUE)

/* Timing as reported by the 32-byte status block. */
struct hd60s_timing {
	u16	vtotal, htotal;
	u16	vactive;		/* FIELD height when interlaced        */
	u16	hactive;
	u8	fps;			/* field rate when interlaced          */
	u8	modeflag;
	u8	audio_khz;
};

/* modeflag bits */
#define HD60S_MF_INTERLACED	BIT(0)
#define HD60S_MF_FRACTIONAL	BIT(1)	/* 1000/1001 rate                      */
#define HD60S_MF_HDCP		BIT(3)
#define HD60S_MF_COLORIMETRY	GENMASK(5, 4)	/* 1 = BT.709, 2 = BT.601      */

/*
 * vactive is the FIELD height for interlaced input. The three half-heights of
 * 480i, 576i and 1080i are treated as interlaced regardless of what the scan
 * bit says.
 */
static inline bool hd60s_interlaced(const struct hd60s_timing *t)
{
	return (t->modeflag & HD60S_MF_INTERLACED) ||
	       t->vactive == 240 || t->vactive == 288 || t->vactive == 540;
}

struct hd60s_buffer {
	struct vb2_v4l2_buffer	vb;
	struct list_head	list;
};

struct hd60s_dev;

/*
 * Per-generation video front end. Everything outside these calls is shared.
 *
 * stream() and disarm_events() are called with ctrl_lock held: the start and
 * stop sequences must run without the status poll interleaving.
 */
struct hd60s_ops {
	const char *name;
	/* revs 1-3 run the MCU handshake after 0xC2/0xC7, rev 4 before */
	bool mcu_after_config;
	/* front-end bring-up, after the shared cold start */
	int (*init)(struct hd60s_dev *d);
	/* detected geometry; *signal false is a valid, non-error answer */
	int (*detect)(struct hd60s_dev *d, struct hd60s_timing *t, bool *signal);
	/* the front end's own stream gate; ctrl_lock held */
	int (*stream)(struct hd60s_dev *d, bool on);
	/* quieten the EP 0x81 watch list; ctrl_lock held */
	int (*disarm_events)(struct hd60s_dev *d);
	u8 pic_controls;	/* HD60S_PIC_* this front end implements */
	/* apply d->pic[]; NULL if the front end has none */
	int (*set_picture)(struct hd60s_dev *d);
	/* audio input select; NULL if not selectable */
	int (*set_audio)(struct hd60s_dev *d, u8 src);
	/* color-range conversion; NULL if not exposed */
	int (*set_color_range)(struct hd60s_dev *d, u8 cr);
};

extern const struct hd60s_ops hd60s_ops_rev4;
extern const struct hd60s_ops hd60s_ops_mstar;

/* State the MStar front end carries between calls. */
struct hd60s_mstar {
	u8	blk[5];		/* read once at init, restored on stop         */
	bool	blk_valid;
	u32	audio_hz;
	bool	warned_nomode;	/* only complain about an unmatched mode once  */
	bool	tx_probed;	/* its ID has been read                        */
	bool	tx_present;	/* the loop-through transmitter answered       */
	u32	tx_audio_hz;	/* the rate its audio path is programmed for   */
	bool	tx_audio_dirty;	/* bank 2 was touched; reprogram it            */
	bool	tx_up;		/* the takeover ran and the canary still holds */
	/* from the sink's EDID; an unidentified sink is driven as DVI */
	bool	tx_sink_hdmi;
	bool	tx_sink_audio;
	bool	tx_edid_valid;	/* current for this hot-plug session           */
	u8	tx_edid_tries;
	/* the received AVI InfoFrame's DB4 and DB2 bits 5:4; 0 = none */
	u8	src_vic;
	u8	src_aspect;
	/* what the loop-through AVI InfoFrame last emitted (DB2, DB4) */
	u8	tx_avi_db2;
	u8	tx_avi_vic;
	bool	hdcp;		/* bank 1 0x01 bit 0 | 0x34 bit 7 */
	/* the chip's measured window as a mode row; applied may point at it */
	struct hd60s_mode measured;
	bool	applied_frac;	/* the applied mode is a 1000/1001 rate */
	/* deep-color mis-lock recovery, bounded per signal session */
	u8	deep_tries;
	bool	deep_zeroed;	/* the gate block is being held at zero        */
	bool	warned_deep;
	/* an unmatched timing, seen once and awaiting a second poll */
	bool	nomode_pending;
	bool	nomode_interlaced;
	u8	nomode_tries;
	u16	nomode_hactive, nomode_vtotal, nomode_fv;
	/* what is programmed, and the detection that chose it */
	const struct hd60s_mode *applied;
	u16	applied_vtotal, applied_fv, applied_hactive;
	bool	applied_interlaced;
	bool	bt709;		/* colorimetry of the detected mode */
	/* declared by the received AVI InfoFrame; 0xff = nothing yet */
	u8	input_cs;	/* HD60S_CS_*                                 */
	u8	input_q;
	/* and what was last programmed, so a re-apply is level-triggered */
	u8	applied_cs, applied_q;
	bool	applied_bt709;
};

struct hd60s_dev {
	struct usb_device	*udev;
	struct usb_interface	*intf;		/* interface 0 */
	struct usb_interface	*intf_evt;	/* interface 1, may be NULL */

	u16			pid;
	const struct hd60s_ops	*ops;
	struct hd60s_mstar	mst;

	struct v4l2_device	v4l2_dev;
	struct video_device	vdev;
	struct vb2_queue	queue;
	struct v4l2_ctrl_handler ctrls;

	struct mutex		vlock;		/* v4l2 ioctl + vb2 queue lock */
	struct mutex		ctrl_lock;	/* serializes vendor requests  */

	spinlock_t		qlock;		/* buffer list + parser handoff */
	struct list_head	bufs;

	/* detected signal; guarded by vlock outside URB context */
	struct hd60s_timing	tm;
	bool			signal;
	/*
	 * What S_DV_TIMINGS last accepted, and all d->fmt and G_DV_TIMINGS
	 * derive from. The driver may not switch timings on its own: it posts
	 * V4L2_EVENT_SOURCE_CHANGE and userspace adopts the new mode. cfg
	 * starts at a placeholder so G_DV_TIMINGS can answer immediately;
	 * cfg_locked says it has since been replaced by something real.
	 */
	struct hd60s_timing	cfg;
	bool			cfg_locked;
	struct v4l2_pix_format	fmt;

	/* streaming */
	struct urb		*urbs[HD60S_NUM_URBS];
	unsigned int		urb_size;
	int			iso_packets;
	unsigned int		iso_pkt_size;
	unsigned int		iso_interval;	/* urb->interval, microframes */
	u8			alt_burst, alt_mult;
	int			alt;
	bool			use_bulk;
	bool			streaming;
	/* the transport was live when the system suspended; resume re-arms it */
	bool			pm_streaming;
	/* URB completion only, where the HCD serializes; not atomic */
	u32			iso_total, iso_err;
	/* bulk has no per-packet status, so transport errors need counting */
	u32			bulk_urbs, bulk_err;
	u64			bulk_lost;	/* bytes in URBs that errored */
	int			bulk_status;	/* the last error seen        */
	u32			resubmit_err;	/* URBs lost from the ring     */
	atomic_t		inflight;	/* URBs the HCD currently owns */
	struct hd60s_parser	p;

	struct delayed_work	state_work;
	struct delayed_work	watchdog;
	u64			wd_bytes;
	unsigned int		wd_tick;
	bool			wd_warned;
	u32			implausible_run;
	unsigned int		nosig;		/* consecutive no-signal polls */

	struct workqueue_struct	*wq;
	struct work_struct	init_work;
	bool			registered;	/* video device live */
	bool			gone;		/* disconnect seen   */
	bool			wedged;		/* EP0 stopped answering */

	u8			pic[4];		/* bright, contrast, sat, hue */
	u8			audio_src;	/* 0 = embedded HDMI          */
	/* the input range and the wanted output; hd60s_apply_color_range() */
	u8			rgb_range;	/* V4L2_DV_RGB_RANGE_*        */
	u32			quantization;	/* V4L2_QUANTIZATION_* wanted */
	bool			input_full;	/* detected input is full     */
	u8			color_range;	/* HD60S_CR_*, the result     */
	u8			audio_vol[2];	/* {0x3C volume, 0x3B extra}  */

	/* ALSA */
	struct snd_card		*card;
	struct snd_pcm		*pcm;
	struct snd_pcm_substream *substream;
	spinlock_t		alock;		/* PCM ring + substream state */
	unsigned int		apos;		/* write offset in dma_area   */
	unsigned int		aperiod;	/* bytes since last elapsed   */
	bool			arunning;
};

/* core */
int hd60s_vout(struct hd60s_dev *d, u8 req, u16 val, u16 idx,
	       const void *buf, u16 len);
int hd60s_vin(struct hd60s_dev *d, u8 req, u16 val, u16 idx, void *buf, u16 len);
int hd60s_read_status(struct hd60s_dev *d, struct hd60s_timing *t, bool *signal);
int hd60s_stream_enable(struct hd60s_dev *d, bool on);
/* the I2C pair, like the _nolock calls below, needs ctrl_lock held */
int hd60s_i2c_write(struct hd60s_dev *d, u8 slave, const u8 *tx, int txlen);
int hd60s_i2c_read(struct hd60s_dev *d, u8 slave, const u8 *tx, int txlen,
		   u8 *rx, int rxlen);
int hd60s_disarm_events(struct hd60s_dev *d);
/* _nolock: the caller must hold ctrl_lock for the whole sequence */
int hd60s_vout_nolock(struct hd60s_dev *d, u8 req, u16 val, u16 idx,
		      const void *buf, u16 len);
int hd60s_stream_enable_nolock(struct hd60s_dev *d, bool on);
int hd60s_disarm_events_nolock(struct hd60s_dev *d);
void hd60s_update_format(struct hd60s_dev *d);
int hd60s_apply_color_range(struct hd60s_dev *d);
void hd60s_timings_from(const struct hd60s_timing *t,
			struct v4l2_dv_timings *dv);

/* video */
int hd60s_video_register(struct hd60s_dev *d);
void hd60s_video_unregister(struct hd60s_dev *d);
void hd60s_video_suspend(struct hd60s_dev *d);
void hd60s_video_resume(struct hd60s_dev *d);
void hd60s_watchdog(struct work_struct *work);

/* audio */
int hd60s_audio_register(struct hd60s_dev *d);
void hd60s_audio_unregister(struct hd60s_dev *d);
void hd60s_audio_suspend(struct hd60s_dev *d);
void hd60s_audio_write(struct hd60s_dev *d, const u8 *data, u32 len);

#endif /* _HD60S_H_ */
