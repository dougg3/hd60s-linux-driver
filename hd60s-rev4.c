// SPDX-License-Identifier: GPL-2.0
/*
 * Elgato HD60 S revision 4 (PID 0x0076) - the ITE front end
 *
 * Copyright (c) 2026 Doug Brown <doug@schmorgal.com>
 *
 * The rev-4 MCU owns the IT6802E receiver and the IT66121 transmitter; the
 * host never programs either. All the driver does is read a 32-byte status
 * block and write a handful of registers, all on channel 0x64.
 */
#include <linux/delay.h>

#include "hd60s.h"

/*
 * Status block. The control endpoint has been seen returning stale bytes when
 * the device is unhappy, so an implausible block is -EPROTO rather than a bogus
 * format. The characteristic failure is horizontal lock without vertical.
 */
static int rev4_detect(struct hd60s_dev *d, struct hd60s_timing *t, bool *signal)
{
	u8 s[HD60S_STATUS_LEN];
	bool h_ok;
	int ret;

	ret = hd60s_vin(d, HD60S_REQ_C0, HD60S_CH64, 0x0000, s, sizeof(s));
	if (ret < 0)
		return ret;

	t->vtotal    = get_unaligned_le16(s + 4);
	t->htotal    = get_unaligned_le16(s + 6);
	t->vactive   = get_unaligned_le16(s + 8);
	t->hactive   = get_unaligned_le16(s + 10);
	t->fps       = s[12];
	t->modeflag  = s[13];
	t->audio_khz = s[14];

	if (!t->hactive && !t->vactive) {	/* no signal: a valid answer */
		*signal = false;
		return 0;
	}

	if (t->hactive >= 320 && t->hactive <= 4096 &&
	    t->vactive >= 200 && t->vactive <= 2160 &&
	    t->htotal >= t->hactive && t->vtotal >= t->vactive &&
	    t->fps >= 20 && t->fps <= 130 && t->audio_khz <= 192) {
		d->implausible_run = 0;
		*signal = true;
		return 0;
	}

	h_ok = t->hactive >= 320 && t->hactive <= 4096 && t->htotal >= t->hactive;
	if (d->implausible_run++ < 3)
		dev_warn(&d->intf->dev,
			 "implausible status block: %ux%u@%u total %ux%u aud %u\n",
			 t->hactive, t->vactive, t->fps,
			 t->htotal, t->vtotal, t->audio_khz);
	else if (d->implausible_run == 4)
		dev_warn(&d->intf->dev,
			 "status block still implausible after 4 reads%s; power-cycle the capture device, a module reload will not clear this\n",
			 h_ok ? " (horizontal timing is locked, vertical is not)" : "");
	return -EPROTO;
}

/*
 * Audio input select, as the Windows driver does it: the source index, then the
 * gain register that belongs to it -- 0x3B for embedded HDMI, 0x3C for the
 * jack. ctrl_lock is held throughout so the status poll cannot read in between.
 */
static int rev4_set_audio_seq(struct hd60s_dev *d, u8 src)
{
	int ret;

	ret = hd60s_vout_nolock(d, HD60S_REQ_C0, HD60S_CH64,
				HD60S_REG_AUDIO_SRC, &src, 1);
	if (ret < 0)
		return ret;
	usleep_range(5000, 6000);

	if (src == 0)
		return hd60s_vout_nolock(d, HD60S_REQ_C0, HD60S_CH64,
					 HD60S_REG_AUDIO_VOL2,
					 &d->audio_vol[1], 1);
	return hd60s_vout_nolock(d, HD60S_REQ_C0, HD60S_CH64,
				 HD60S_REG_AUDIO_VOL, &d->audio_vol[0], 1);
}

static int rev4_set_audio(struct hd60s_dev *d, u8 src)
{
	int ret;

	if (src > 1)
		return -EINVAL;

	mutex_lock(&d->ctrl_lock);
	ret = rev4_set_audio_seq(d, src);
	mutex_unlock(&d->ctrl_lock);
	return ret;
}

/*
 * Color range: one byte, 0 Bypass, 1 Shrink, 2 Expand.
 *
 * Deliberately not offered as ops->set_color_range: the receiver clamps luma at
 * 235 unconditionally, so Expand cannot reach 0..255 and advertising
 * V4L2_FMT_FLAG_CSC_QUANTIZATION would promise a range it does not produce.
 * Written only by rev4_init(), to pin it at Bypass.
 *
 * The firmware also converts a declared-full source to limited on its own with
 * no status bit for it, so G_FMT can report full while the device sends
 * limited.
 */
static int rev4_set_color_range(struct hd60s_dev *d, u8 cr)
{
	u8 v = cr & 3;

	if (cr > HD60S_CR_EXPAND)
		return -EINVAL;
	return hd60s_vout(d, HD60S_REQ_C0, HD60S_CH64,
			  HD60S_REG_COLOR_RANGE, &v, 1);
}

static int rev4_init(struct hd60s_dev *d)
{
	static const u8 neutral[4] = { 0x80, 0x80, 0x80, 0x80 };
	int ret;

	ret = hd60s_disarm_events(d);		/* the Windows driver disarms twice */
	if (ret < 0)
		return ret;
	ret = hd60s_disarm_events(d);
	if (ret < 0)
		return ret;
	ret = hd60s_vout(d, HD60S_REQ_C0, HD60S_CH64, HD60S_REG_PICTURE,
			 neutral, sizeof(neutral));
	if (ret < 0)
		return ret;
	ret = rev4_set_audio(d, d->audio_src);
	if (ret < 0)
		return ret;

	/*
	 * A deliberate divergence: the Windows driver writes this only from its
	 * property page. Without it the register keeps whatever another OS left
	 * behind, and the pixels carry a gain the driver knows nothing about.
	 */
	return rev4_set_color_range(d, HD60S_CR_BYPASS);
}

static int rev4_stream(struct hd60s_dev *d, bool on)
{
	u8 v = on ? 1 : 0;

	return hd60s_vout_nolock(d, HD60S_REQ_C0, HD60S_CH64,
				 HD60S_REG_STREAM_EN, &v, 1);
}

/* wIndex 0x0100, the enable bit low. */
static int rev4_disarm_events(struct hd60s_dev *d)
{
	return hd60s_vout_nolock(d, HD60S_REQ_C6, 0x0000, 0x0100, NULL, 0);
}

static int rev4_set_picture(struct hd60s_dev *d)
{
	return hd60s_vout(d, HD60S_REQ_C0, HD60S_CH64, HD60S_REG_PICTURE,
			  d->pic, sizeof(d->pic));
}

const struct hd60s_ops hd60s_ops_rev4 = {
	.name		= "rev 4 (ITE IT6802E)",
	.init		= rev4_init,
	.detect		= rev4_detect,
	.stream		= rev4_stream,
	.disarm_events	= rev4_disarm_events,
	.pic_controls	= HD60S_PIC_ALL,
	.set_picture	= rev4_set_picture,
	.set_audio	= rev4_set_audio,
};
