// SPDX-License-Identifier: GPL-2.0
/*
 * Elgato Game Capture HD60 S - USB lifecycle and control plane
 *
 * Copyright (c) 2026 Doug Brown <doug@schmorgal.com>
 *
 * Shared by every hardware revision: the FX3 firmware is byte-identical on
 * all four, so this file, hd60s-video.c, hd60s-audio.c and hd60s-parse.c are
 * revision-neutral.
 */
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <media/v4l2-event.h>

#include "hd60s.h"

static struct usb_driver hd60s_driver;

static int hd60s_ctrl_done(struct hd60s_dev *d, int ret, const char *what)
{
	if (ret == -ETIMEDOUT && !d->wedged) {
		d->wedged = true;
		dev_err(&d->intf->dev,
			"%s timed out, EP0 is stuck. Power-cycle the device\n",
			what);
	}
	return ret;
}

/*
 * _nolock: the caller holds ctrl_lock. Stream start and stop hold it across
 * their whole sequence so the status poll cannot slip an I2C transaction in.
 */
int hd60s_vout_nolock(struct hd60s_dev *d, u8 req, u16 val, u16 idx,
		      const void *buf, u16 len)
{
	int ret;

	lockdep_assert_held(&d->ctrl_lock);
	if (d->udev->state == USB_STATE_NOTATTACHED)
		return -ENODEV;
	if (d->wedged)
		return -EIO;
	ret = usb_control_msg_send(d->udev, 0, req, USB_DIR_OUT | USB_TYPE_VENDOR,
				   val, idx, buf, len, HD60S_CTRL_TIMEOUT,
				   GFP_KERNEL);
	return hd60s_ctrl_done(d, ret, "control write");
}

int hd60s_vout(struct hd60s_dev *d, u8 req, u16 val, u16 idx,
	       const void *buf, u16 len)
{
	int ret;

	mutex_lock(&d->ctrl_lock);
	ret = hd60s_vout_nolock(d, req, val, idx, buf, len);
	mutex_unlock(&d->ctrl_lock);
	return ret;
}

int hd60s_vin(struct hd60s_dev *d, u8 req, u16 val, u16 idx, void *buf, u16 len)
{
	int ret;

	mutex_lock(&d->ctrl_lock);
	if (d->udev->state == USB_STATE_NOTATTACHED) {
		mutex_unlock(&d->ctrl_lock);
		return -ENODEV;
	}
	if (d->wedged) {
		mutex_unlock(&d->ctrl_lock);
		return -EIO;
	}
	ret = usb_control_msg_recv(d->udev, 0, req, USB_DIR_IN | USB_TYPE_VENDOR,
				   val, idx, buf, len, HD60S_CTRL_TIMEOUT,
				   GFP_KERNEL);
	mutex_unlock(&d->ctrl_lock);
	return hd60s_ctrl_done(d, ret, "control read");
}

/* wValue = (level << 8) | gpio; bit 14 claims the pin, bit 15 selects mode. */
static int hd60s_gpio_write(struct hd60s_dev *d, u8 gpio, u8 level)
{
	return hd60s_vout(d, HD60S_REQ_C1, (u16)level << 8 | gpio, 0, NULL, 0);
}

static int hd60s_gpio_read(struct hd60s_dev *d, u8 gpio)
{
	u8 b;
	int ret = hd60s_vin(d, HD60S_REQ_C1, gpio, 0, &b, 1);

	return ret < 0 ? ret : b;
}

/*
 * MCU I2C bridge, addressed to the MCU itself at slave 0xAA. Wire form:
 *
 *     OUT 0xC0 wValue=0x5066   [0xAA | reading] [rxlen if reading] [tx...]
 *     IN  0xC0 wValue=0x5066   rxlen bytes            (hd60s_mcu_read)
 */
static int hd60s_mcu_write(struct hd60s_dev *d, const u8 *tx, int txlen, int rxlen)
{
	u8 buf[8];
	int n = 0;

	if (txlen + 2 > (int)sizeof(buf))
		return -EINVAL;
	buf[n++] = 0xaa | (rxlen ? 1 : 0);
	if (rxlen)
		buf[n++] = rxlen;
	memcpy(buf + n, tx, txlen);
	n += txlen;
	return hd60s_vout(d, HD60S_REQ_C0, HD60S_CH_I2C, 0, buf, n);
}

static int hd60s_mcu_read(struct hd60s_dev *d, u8 *rx, int rxlen)
{
	return hd60s_vin(d, HD60S_REQ_C0, HD60S_CH_I2C, 0, rx, rxlen);
}

/*
 * The same bridge, addressed to an arbitrary slave. Revisions 1-3 reach the
 * MST3367, the board control device and the PLL through it. Wire form:
 *
 *     OUT 0xC0 wValue=0x5066   [slave | reading] [rxlen if reading] [tx...]
 *     IN  0xC0 wValue=0x5066   rxlen bytes            (only when reading)
 *
 * These are the _nolock forms: every caller is inside a sequence that already
 * holds ctrl_lock. The Windows driver's helper caps the outgoing payload at 65
 * bytes; nothing on these paths comes close, but the cap is real.
 */
#define HD60S_I2C_MAXTX	62

/*
 * Which slaves are addressed directly rather than through the MCU. This
 * selects exactly the three chips on the MStar board's own I2C bus: the PLL
 * (0x88), the board control device (0x98) and the MST3367 (0x9C).
 */
static bool hd60s_i2c_direct(u8 slave)
{
	return ((u8)(slave + 0x78) & 0xeb) == 0 && slave != 0x8c;
}

/*
 * A write goes direct when the slave allows it, and is then performed by the
 * FX3's own I2C master with the MCU uninvolved - which is why writes are not
 * subject to the bridge staging delay. Everything else is bridged.
 */
int hd60s_i2c_write(struct hd60s_dev *d, u8 slave, const u8 *tx, int txlen)
{
	u8 buf[1 + HD60S_I2C_MAXTX];

	lockdep_assert_held(&d->ctrl_lock);
	if (txlen < 1 || txlen > HD60S_I2C_MAXTX)
		return -EINVAL;

	if (hd60s_i2c_direct(slave))
		return hd60s_vout_nolock(d, HD60S_REQ_C0,
					 0x5000 | slave, 0, tx, txlen);

	buf[0] = slave & ~1;
	memcpy(buf + 1, tx, txlen);
	return hd60s_vout_nolock(d, HD60S_REQ_C0, HD60S_CH_I2C, 0,
				 buf, txlen + 1);
}

/* A read is always bridged, whatever the slave. */
int hd60s_i2c_read(struct hd60s_dev *d, u8 slave, const u8 *tx, int txlen,
		   u8 *rx, int rxlen)
{
	u8 buf[2 + HD60S_I2C_MAXTX];
	int ret;

	lockdep_assert_held(&d->ctrl_lock);
	if (txlen < 1 || txlen > HD60S_I2C_MAXTX || rxlen < 1 || rxlen > 255)
		return -EINVAL;
	buf[0] = (slave & ~1) | 1;
	buf[1] = rxlen;
	memcpy(buf + 2, tx, txlen);
	ret = hd60s_vout_nolock(d, HD60S_REQ_C0, HD60S_CH_I2C, 0,
				buf, txlen + 2);
	if (ret < 0)
		return ret;

	if (d->udev->state == USB_STATE_NOTATTACHED)
		return -ENODEV;
	if (d->wedged)
		return -EIO;
	ret = usb_control_msg_recv(d->udev, 0, HD60S_REQ_C0,
				   USB_DIR_IN | USB_TYPE_VENDOR,
				   HD60S_CH_I2C, 0, rx, rxlen,
				   HD60S_CTRL_TIMEOUT, GFP_KERNEL);
	return hd60s_ctrl_done(d, ret, "i2c read");
}

int hd60s_stream_enable_nolock(struct hd60s_dev *d, bool on)
{
	return d->ops->stream(d, on);
}

int hd60s_stream_enable(struct hd60s_dev *d, bool on)
{
	int ret;

	mutex_lock(&d->ctrl_lock);
	ret = d->ops->stream(d, on);
	mutex_unlock(&d->ctrl_lock);
	return ret;
}

/*
 * Quieten the EP 0x81 watch list. This driver never arms it, but a device left
 * armed has the FX3 polling I2C every 50 ms from the thread that also services
 * EP0, which is how control transfers can potentially hang.
 */
int hd60s_disarm_events_nolock(struct hd60s_dev *d)
{
	return d->ops->disarm_events(d);
}

int hd60s_disarm_events(struct hd60s_dev *d)
{
	int ret;

	mutex_lock(&d->ctrl_lock);
	ret = d->ops->disarm_events(d);
	mutex_unlock(&d->ctrl_lock);
	return ret;
}

int hd60s_read_status(struct hd60s_dev *d, struct hd60s_timing *t, bool *signal)
{
	*signal = false;
	return d->ops->detect(d, t, signal);
}

/* G_DV_TIMINGS must answer even with nothing plugged in. */
static const struct hd60s_timing hd60s_default_timing = {
	.htotal = 1650, .vtotal = 750,
	.hactive = 1280, .vactive = 720,
	.fps = 60,
	.modeflag = FIELD_PREP_CONST(HD60S_MF_COLORIMETRY, 1),	/* BT.709 */
	.audio_khz = 48,
};

/*
 * Takes the timing rather than the device, so it cannot blend the detected mode
 * with the configured one. The device reports no sync/porch breakdown, so all
 * blanking is reported as back porch; totals and pixel clock are exact.
 */
void hd60s_timings_from(const struct hd60s_timing *t,
			struct v4l2_dv_timings *dv)
{
	struct v4l2_bt_timings *bt = &dv->bt;
	bool il = hd60s_interlaced(t);
	u64 clk;

	memset(dv, 0, sizeof(*dv));
	dv->type = V4L2_DV_BT_656_1120;
	bt->width = t->hactive;
	bt->height = t->vactive * (il ? 2 : 1);
	bt->interlaced = il ? V4L2_DV_INTERLACED : V4L2_DV_PROGRESSIVE;
	bt->polarities = V4L2_DV_HSYNC_POS_POL | V4L2_DV_VSYNC_POS_POL;
	bt->standards = V4L2_DV_BT_STD_CEA861;

	/*
	 * Interlaced: vtotal is the FIELD length, fps the field rate, and the
	 * frame is 2 * vtotal + 1 -- 525, 625 and 1125 lines. Field 2 carries
	 * the extra line, as in the kernel's CEA-861 presets.
	 */
	if (il)
		clk = div_u64((u64)t->htotal * (2 * t->vtotal + 1) * t->fps, 2);
	else
		clk = (u64)t->htotal * t->vtotal * t->fps;
	if (t->modeflag & HD60S_MF_FRACTIONAL)
		clk = div_u64(clk * 1000, 1001);
	bt->pixelclock = clk;

	bt->hbackporch = t->htotal - t->hactive;
	bt->vbackporch = t->vtotal - t->vactive;
	if (il) {
		bt->il_vbackporch = bt->vbackporch + 1;
		bt->flags |= V4L2_DV_FL_HALF_LINE;
	}
}

/*
 * The input range after the control's override. AUTO believes the front end;
 * sources that misdeclare their range are what V4L2_CID_DV_RX_RGB_RANGE is for.
 */
static bool hd60s_input_is_full(struct hd60s_dev *d)
{
	switch (d->rgb_range) {
	case V4L2_DV_RGB_RANGE_LIMITED:
		return false;
	case V4L2_DV_RGB_RANGE_FULL:
		return true;
	default:
		return d->input_full;
	}
}

void hd60s_update_format(struct hd60s_dev *d)
{
	struct v4l2_pix_format *f = &d->fmt;
	bool il;

	if (!d->cfg.hactive)
		return;			/* nothing configured yet */

	il = hd60s_interlaced(&d->cfg);
	f->width	= d->cfg.hactive;
	f->height	= d->cfg.vactive * (il ? 2 : 1);
	f->pixelformat	= V4L2_PIX_FMT_YUYV;
	f->field	= il ? V4L2_FIELD_INTERLACED : V4L2_FIELD_NONE;
	f->bytesperline	= f->width * 2;
	f->sizeimage	= f->bytesperline * f->height;
	f->colorspace	= FIELD_GET(HD60S_MF_COLORIMETRY, d->cfg.modeflag) == 2 ?
			  V4L2_COLORSPACE_SMPTE170M : V4L2_COLORSPACE_REC709;
	/*
	 * A front end that converts gives what was asked for; one that cannot
	 * reports the input range, because that is what it delivers.
	 */
	if (d->ops->set_color_range)
		f->quantization = d->quantization;
	else if (hd60s_input_is_full(d))
		f->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	else
		f->quantization = V4L2_QUANTIZATION_LIM_RANGE;
	f->xfer_func	= V4L2_XFER_FUNC_709;
	f->ycbcr_enc	= V4L2_YCBCR_ENC_DEFAULT;
}

/*
 * Choose the conversion from what the input is and what userspace wants out.
 *
 *   input    | wanted   | conversion
 *   ---------+----------+-----------
 *   limited  | limited  | Bypass
 *   limited  | full     | Expand
 *   full     | limited  | Shrink
 *   full     | full     | Bypass
 */
int hd60s_apply_color_range(struct hd60s_dev *d)
{
	bool in_full, out_full;
	u8 cr, prev = d->color_range;
	int ret;

	if (!d->ops->set_color_range)
		return 0;

	in_full = hd60s_input_is_full(d);
	out_full = d->quantization == V4L2_QUANTIZATION_FULL_RANGE;

	if (in_full == out_full)
		cr = HD60S_CR_BYPASS;
	else
		cr = out_full ? HD60S_CR_EXPAND : HD60S_CR_SHRINK;

	if (cr == prev)
		return 0;

	/* Set first: the MStar apply reads this rather than taking an argument. */
	d->color_range = cr;
	ret = d->ops->set_color_range(d, cr);
	if (ret < 0) {
		d->color_range = prev;
		dev_warn(&d->intf->dev, "color range %u: %d\n", cr, ret);
	}
	return ret;
}

/*
 * Signal-change detection. EP 0x81 could report changes, but arming it makes
 * the FX3 poll I2C from the thread that services EP0, so this poll is the whole
 * mechanism.
 */
#define HD60S_POLL_MS	500

/*
 * No-signal polls before a loss is believed. A source changing resolution drops
 * the link for about a second, and reporting that as a loss makes clients give
 * up; three polls absorb the relock and still report a real loss in 1.5 s.
 */
#define HD60S_NOSIG_DEBOUNCE	3

/*
 * Field by field: the struct has padding, which memcmp() would compare too.
 *
 * HDCP is excluded. It is status, not geometry, and the stream gate itself
 * clears it on revs 1-3, so comparing it would fire V4L2_EVENT_SOURCE_CHANGE at
 * the application that just started streaming.
 */
static bool hd60s_timing_eq(const struct hd60s_timing *a,
			    const struct hd60s_timing *b)
{
	return a->vtotal == b->vtotal && a->htotal == b->htotal &&
	       a->vactive == b->vactive && a->hactive == b->hactive &&
	       a->fps == b->fps &&
	       !((a->modeflag ^ b->modeflag) & (u8)~HD60S_MF_HDCP) &&
	       a->audio_khz == b->audio_khz;
}

static void hd60s_state_work(struct work_struct *work)
{
	struct hd60s_dev *d = container_of(work, struct hd60s_dev, state_work.work);
	struct hd60s_timing t = {};
	static const struct v4l2_event ev = {
		.type = V4L2_EVENT_SOURCE_CHANGE,
		.u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION,
	};
	bool signal = false, changed;
	int ret;

	if (d->gone)
		return;

	ret = hd60s_read_status(d, &t, &signal);
	if (ret < 0) {
		if (ret != -ENODEV)
			dev_warn_ratelimited(&d->intf->dev,
					     "status read failed: %d\n", ret);
		goto again;
	}

	if (!signal) {
		if (++d->nosig < HD60S_NOSIG_DEBOUNCE)
			goto again;		/* too early to call it lost */
	} else {
		d->nosig = 0;
	}

	mutex_lock(&d->vlock);
	/*
	 * Under vlock: it reads the two range controls and caches the chosen
	 * conversion, all of which S_FMT and S_CTRL also write. A lost update
	 * here leaves the cache claiming a conversion the hardware is not doing,
	 * and the early-out on an unchanged value makes that permanent.
	 */
	hd60s_apply_color_range(d);

	changed = (signal != d->signal) ||
		  (signal && !hd60s_timing_eq(&t, &d->tm));
	if (changed) {
		d->tm = t;
		d->signal = signal;
		/*
		 * d->cfg is not touched: V4L2 forbids adopting a detected mode
		 * on the driver's own initiative. The one exception is the
		 * first lock-on, which the guard makes unrepeatable.
		 */
		if (signal && !d->cfg_locked) {
			d->cfg = t;
			d->cfg_locked = true;
			hd60s_update_format(d);
		}
		if (signal) {
			bool il = hd60s_interlaced(&t);
			u32 dw = t.hactive, dh = t.vactive * (il ? 2 : 1);

			dev_info(&d->intf->dev,
				 "signal %ux%u%s @%u Hz%s%s\n",
				 dw, dh, il ? "i" : "p", t.fps,
				 (t.modeflag & HD60S_MF_FRACTIONAL) ? " /1.001" : "",
				 (t.modeflag & HD60S_MF_HDCP) ? " HDCP" : "");

			/*
			 * The stride was snapshotted at start_streaming, so a
			 * mode change cannot be absorbed. Stop parsing, and let
			 * it heal if the source comes back.
			 *
			 * Not vb2_queue_error(): that makes DQBUF return -EIO,
			 * which applications treat as unrecoverable.
			 */
			if (READ_ONCE(d->streaming)) {
				bool match = (dw * 2 == d->p.line_bytes &&
					      dh == d->p.frame_h);

				if (!match && !d->p.stale) {
					dev_warn(&d->intf->dev,
						 "source changed to %ux%u while streaming %ux%u, frames stop until the capture is restarted\n",
						 dw, dh, d->p.line_bytes / 2,
						 d->p.frame_h);
					WRITE_ONCE(d->p.stale, true);
				} else if (match && d->p.stale) {
					dev_info(&d->intf->dev,
						 "source back to %ux%u, resuming\n",
						 dw, dh);
					WRITE_ONCE(d->p.stale, false);
				}
			}
		} else {
			dev_info(&d->intf->dev, "signal lost\n");
		}
	}

	mutex_unlock(&d->vlock);

	/* On signal loss the device just stops sending; tear nothing down. */
	if (changed && d->registered)
		v4l2_event_queue(&d->vdev, &ev);

again:
	if (!d->gone)
		queue_delayed_work(d->wq, &d->state_work,
				   msecs_to_jiffies(HD60S_POLL_MS));
}

/*
 * The stream-pool and I2C block configuration.
 */
static int hd60s_config_requests(struct hd60s_dev *d)
{
	static const struct {
		u8 req;
		u16 val;
		const char *name;
		const char *caveat;
	} seq[] = {
		{ HD60S_REQ_C2, 0x0000, "0xC2 SetStreamPool",
		  "if EP 0x83 stays silent, power-cycle rather than reload" },
		{ HD60S_REQ_C7, 0x0064, "0xC7 I2C config", NULL },
	};
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(seq); i++) {
		ret = hd60s_vout(d, seq[i].req, seq[i].val, 0x0000, NULL, 0);
		if (ret == -EPIPE) {
			dev_info(&d->intf->dev,
				 "%s stalled; continuing (warm device)%s%s\n",
				 seq[i].name, seq[i].caveat ? " - " : "",
				 seq[i].caveat ? seq[i].caveat : "");
			continue;
		}
		if (ret < 0) {
			dev_err(&d->intf->dev, "%s failed: %d\n",
				seq[i].name, ret);
			return ret;
		}
	}
	return 0;
}

/*
 * Poll the MCU until it reports ready.
 *
 * The MStar MCU's I2C slave interrupt only buffers and a super-loop answers, so
 * it stages a reply only when a command arrives and every poll must re-issue
 * the write; a read-only poll would return the stale "33 44 55" forever. Rev 4
 * re-stages on its own and is written once.
 *
 * "51 10" is ready and "33 44 55" retries; anything else aborts on MStar.
 */
static int hd60s_mcu_wait_ready(struct hd60s_dev *d, bool rev4)
{
	static const u8 cmd_ready[3] = { 0x12, 0x34, 0x57 };
	u8 rx[3] = {};
	int i, ret, errs;

	for (i = 0, errs = 0; i < 200 && !d->gone; i++) {
		if (!(i && rev4)) {
			ret = hd60s_mcu_write(d, cmd_ready,
					      sizeof(cmd_ready), 3);
			if (ret < 0) {
				dev_err(&d->intf->dev,
					"MCU ready command failed: %d\n", ret);
				return ret;
			}
		}
		memset(rx, 0, sizeof(rx));
		ret = hd60s_mcu_read(d, rx, 3);
		if (ret < 0) {
			if (++errs >= 5) {
				dev_err(&d->intf->dev,
					"MCU bridge: 5 consecutive failures (%d)\n",
					ret);
				return ret;
			}
			continue;
		}
		errs = 0;
		if (rx[0] == 0x51 && rx[1] == 0x10)
			break;
		if (rx[0] == 0x33 && rx[1] == 0x44 && rx[2] == 0x55) {
			msleep(101);
			continue;
		}
		if (!rev4) {
			dev_err(&d->intf->dev,
				"MCU answered %3ph, neither 51 10 nor 33 44 55\n",
				rx);
			return -ENODEV;
		}
	}
	if (rx[0] != 0x51 || rx[1] != 0x10) {
		dev_err(&d->intf->dev, "MCU never reported ready (%3ph)\n", rx);
		return -ENODEV;
	}
	dev_dbg(&d->intf->dev, "MCU ready (%3ph) after %d polls\n", rx, i + 1);
	return 0;
}

/* Informational only on revs 1-3; rev 4 checks the date is plausible. */
static void hd60s_mcu_log_version(struct hd60s_dev *d, bool rev4)
{
	static const u8 cmd_ver[3] = { 0x12, 0x34, 0x58 };
	u8 rx[3] = {};
	int i, errs;

	for (i = 0, errs = 0; i < 10 && !d->gone; i++) {
		msleep(101);
		if (!i && hd60s_mcu_write(d, cmd_ver, sizeof(cmd_ver), 3) < 0)
			return;
		memset(rx, 0, sizeof(rx));
		if (hd60s_mcu_read(d, rx, 3) < 0) {
			if (++errs >= 5)
				return;
			continue;
		}
		errs = 0;
		if ((rx[0] >= 0x14 && rx[0] <= 0x28 &&
		     rx[1] >= 6 && rx[1] <= 12 &&
		     rx[2] >= 1 && rx[2] <= 31) || !rev4) {
			dev_info(&d->intf->dev,
				 "MCU firmware build 20%02u-%02u-%02u\n",
				 rx[0], rx[1], rx[2]);
			return;
		}
	}
}

static int hd60s_cold_start(struct hd60s_dev *d)
{
	static const u8 ec_pkt[10] = {
		0xb8, 0x22, 0x00, 0x00, 0x00, 0xc0, 0x00, 0x00, 0x50, 0xca
	};
	bool rev4 = d->ops == &hd60s_ops_rev4;
	int i, g, ret;

	/* The Windows driver issues 0xEC ~420 ms after SET_CONFIGURATION. */
	msleep(600);

	/*
	 * 0xEC is not a reset: byte 5 selects its only action, storing byte 7
	 * in a RAM flag the MStar boards use. Accepted on every replay.
	 */
	ret = hd60s_vout(d, HD60S_REQ_EC, 0x0000, 0x0000, ec_pkt, sizeof(ec_pkt));
	if (ret == -EPIPE) {
		dev_info(&d->intf->dev, "0xEC stalled; continuing\n");
	} else if (ret < 0) {
		dev_err(&d->intf->dev, "0xEC failed: %d\n", ret);
		return ret;
	}

	if (d->ops->mcu_after_config) {
		ret = hd60s_config_requests(d);
		if (ret < 0)
			goto fail;
	}

	/*
	 * Both are level/config writes rather than pulses, so re-issuing them
	 * on a live device is a no-op.
	 */
	hd60s_gpio_write(d, 0x39, 0xc0);	/* GPIO 57 override + input */
	usleep_range(2000, 3000);
	hd60s_gpio_write(d, 0x34, 0x41);	/* GPIO 52 override + high  */
	usleep_range(11000, 12000);

	/*
	 * The Windows driver retries this up to 100 times. On some units the
	 * GPIO override does not take and the read stalls, so bail out on the
	 * first failure; the MCU handshake below is the real readiness gate.
	 */
	for (i = 0, g = 0; i < 100; i++) {
		g = hd60s_gpio_read(d, 0x39);
		if (g < 0 || g == 1)
			break;
		usleep_range(10000, 11000);
	}
	dev_dbg(&d->intf->dev, "GPIO 57 (MCU ready) -> %d after %d read(s)\n",
		g, i + 1);

	ret = hd60s_mcu_wait_ready(d, rev4);
	if (ret < 0)
		return ret;
	hd60s_mcu_log_version(d, rev4);

	if (!d->ops->mcu_after_config) {
		ret = hd60s_config_requests(d);
		if (ret < 0)
			goto fail;
	}

	/* Everything above is FX3-level; the front end is not. */
	ret = d->ops->init(d);
	if (ret < 0)
		goto fail;
	return 0;

fail:
	dev_err(&d->intf->dev, "configuration sequence failed: %d\n", ret);
	return ret;
}

static void hd60s_init_work(struct work_struct *work)
{
	struct hd60s_dev *d = container_of(work, struct hd60s_dev, init_work);
	struct hd60s_timing t = {};
	bool signal = false;
	int ret;

	ret = hd60s_cold_start(d);
	if (ret < 0) {
		dev_err(&d->intf->dev, "initialization failed (%d)\n", ret);
		return;
	}

	/* Into locals: the front end's detect reads d->signal. */
	mutex_lock(&d->vlock);
	if (hd60s_read_status(d, &t, &signal) == 0 && signal) {
		d->tm = t;
		d->signal = true;
		/*
		 * Seed cfg from the wire so a client that never touches DV
		 * timings still gets a usable format. Only S_DV_TIMINGS after.
		 */
		d->cfg = d->tm;
		d->cfg_locked = true;
		hd60s_update_format(d);
	}
	mutex_unlock(&d->vlock);

	if (d->gone)
		return;

	ret = hd60s_video_register(d);
	if (ret < 0) {
		dev_err(&d->intf->dev, "video registration failed (%d)\n", ret);
		return;
	}
	hd60s_audio_register(d);

	queue_delayed_work(d->wq, &d->state_work, msecs_to_jiffies(HD60S_POLL_MS));

	dev_info(&d->intf->dev, "ready: /dev/video%d\n", d->vdev.num);
}

static void hd60s_v4l2_release(struct v4l2_device *v4l2_dev)
{
	struct hd60s_dev *d = container_of(v4l2_dev, struct hd60s_dev, v4l2_dev);

	v4l2_ctrl_handler_free(&d->ctrls);
	v4l2_device_unregister(&d->v4l2_dev);
	usb_put_dev(d->udev);
	kfree(d);
}

static int hd60s_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct hd60s_dev *d;
	int ret;

	d = kzalloc_obj(*d);
	if (!d)
		return -ENOMEM;

	d->udev = usb_get_dev(udev);
	d->intf = intf;
	d->pid = le16_to_cpu(udev->descriptor.idProduct);
	d->ops = (d->pid == HD60S_PID_REV4) ? &hd60s_ops_rev4 : &hd60s_ops_mstar;
	mutex_init(&d->vlock);
	mutex_init(&d->ctrl_lock);
	spin_lock_init(&d->qlock);
	spin_lock_init(&d->alock);
	INIT_LIST_HEAD(&d->bufs);
	d->rgb_range = V4L2_DV_RGB_RANGE_AUTO;
	d->quantization = V4L2_QUANTIZATION_LIM_RANGE;
	memset(d->pic, 0x80, sizeof(d->pic));
	memset(d->audio_vol, 0x80, sizeof(d->audio_vol));

	/* Derived, so d->fmt and d->cfg cannot drift apart. */
	d->cfg = hd60s_default_timing;
	hd60s_update_format(d);

	usb_set_intfdata(intf, d);

	d->v4l2_dev.release = hd60s_v4l2_release;
	ret = v4l2_device_register(&intf->dev, &d->v4l2_dev);
	if (ret < 0) {
		usb_put_dev(udev);
		kfree(d);
		return ret;
	}

	/*
	 * The Windows driver's SET_CONFIGURATION, which runs the FX3's
	 * application stop/start and drops every interface to alt 0, where the
	 * cold start expects to begin. Legal here: probe() owns the device lock.
	 */
	ret = usb_reset_configuration(udev);
	if (ret < 0)
		dev_warn(&intf->dev, "SET_CONFIGURATION failed: %d\n", ret);

	/* EP 0x81 is unused, but claim its interface so nothing else binds. */
	d->intf_evt = usb_ifnum_to_if(udev, HD60S_IF_EVENT);
	if (d->intf_evt) {
		ret = usb_driver_claim_interface(&hd60s_driver, d->intf_evt, d);
		if (ret < 0) {
			dev_warn(&intf->dev,
				 "cannot claim event interface: %d\n", ret);
			d->intf_evt = NULL;
		}
	}

	d->wq = alloc_ordered_workqueue("hd60s", 0);
	if (!d->wq) {
		ret = -ENOMEM;
		goto err;
	}
	INIT_WORK(&d->init_work, hd60s_init_work);
	INIT_DELAYED_WORK(&d->state_work, hd60s_state_work);
	INIT_DELAYED_WORK(&d->watchdog, hd60s_watchdog);

	/* The MCU handshake sleeps for seconds, too long for probe context. */
	dev_info(&intf->dev, "Elgato HD60 S %04x, %s, initializing\n",
		 d->pid, d->ops->name);
	queue_work(d->wq, &d->init_work);
	return 0;

err:
	if (d->intf_evt)
		usb_driver_release_interface(&hd60s_driver, d->intf_evt);
	usb_set_intfdata(intf, NULL);
	v4l2_device_put(&d->v4l2_dev);
	return ret;
}

static void hd60s_disconnect(struct usb_interface *intf)
{
	struct hd60s_dev *d = usb_get_intfdata(intf);

	if (!d)
		return;
	usb_set_intfdata(intf, NULL);

	/*
	 * Called once per claimed interface, in an order the core does not
	 * promise. Tear down on the first call and do nothing on the second.
	 */
	if (d->gone)
		return;
	d->gone = true;

	/*
	 * Both interfaces carry a pointer to this device. The teardown below
	 * frees it, so the other interface's intfdata must be cleared first or
	 * the second disconnect() dereferences freed memory.
	 */
	if (d->intf_evt && d->intf_evt != intf)
		usb_set_intfdata(d->intf_evt, NULL);
	if (d->intf && d->intf != intf)
		usb_set_intfdata(d->intf, NULL);

	/*
	 * A sysfs unbind of one interface does not release the other, which
	 * would leave it bound with no state and block a clean rebind, so drop
	 * whichever one this call is not for. On a real unplug this is a
	 * no-op: the released interface's disconnect() finds NULL intfdata
	 * and returns.
	 */
	if (d->intf_evt && d->intf_evt != intf)
		usb_driver_release_interface(&hd60s_driver, d->intf_evt);
	if (d->intf && d->intf != intf)
		usb_driver_release_interface(&hd60s_driver, d->intf);

	cancel_delayed_work_sync(&d->state_work);
	cancel_delayed_work_sync(&d->watchdog);
	cancel_work_sync(&d->init_work);

	/*
	 * Order matters: unregistering the video device runs stop_streaming,
	 * which stops the producer before the consumer. An I2C transaction
	 * issued into the middle of a live stream can hang the FX3.
	 */
	hd60s_video_unregister(d);
	hd60s_audio_unregister(d);

	destroy_workqueue(d->wq);
	d->wq = NULL;

	v4l2_device_disconnect(&d->v4l2_dev);
	v4l2_device_put(&d->v4l2_dev);
}

/*
 * System sleep. Both interfaces are bound here, so the core calls these twice;
 * all the work happens on interface 0 and the other call is a no-op.
 */
static int hd60s_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct hd60s_dev *d = usb_get_intfdata(intf);

	if (!d || intf != d->intf || d->gone)
		return 0;

	/* Interrupting a running cold start leaves the FX3 in an unknown state. */
	flush_work(&d->init_work);
	cancel_delayed_work_sync(&d->state_work);

	hd60s_audio_suspend(d);
	hd60s_video_suspend(d);

	/* Never fails: an error here aborts the whole system suspend. */
	return 0;
}

static int hd60s_resume(struct usb_interface *intf)
{
	struct hd60s_dev *d = usb_get_intfdata(intf);
	int ret;

	if (!d || intf != d->intf || d->gone)
		return 0;

	/* Never registered -- the sleep beat the first bring-up, or it failed. */
	if (!d->registered) {
		queue_work(d->wq, &d->init_work);
		return 0;
	}

	ret = hd60s_cold_start(d);
	if (ret < 0) {
		/*
		 * The poll stays down: the bring-up is not safe to retry
		 * piecemeal against an FX3 that did not answer.
		 */
		dev_err(&d->intf->dev,
			"resume: bring-up failed (%d); unplug and replug the device\n",
			ret);
		return ret;
	}

	/*
	 * The bring-up left the front end at its defaults, so everything
	 * userspace chose goes out again. Settings first, transport second: an
	 * I2C transaction issued into a live stream can hang the FX3.
	 */
	mutex_lock(&d->vlock);
	d->color_range = HD60S_CR_BYPASS;
	/* From zero: a count left over from before the sleep must not spend
	 * the debounce the re-lock needs.
	 */
	d->nosig = 0;
	v4l2_ctrl_handler_setup(&d->ctrls);
	hd60s_video_resume(d);
	mutex_unlock(&d->vlock);

	queue_delayed_work(d->wq, &d->state_work,
			   msecs_to_jiffies(HD60S_POLL_MS));
	/* The interface is back; a capture that could not restart is the
	 * application's error to see, not a second log line here.
	 */
	return 0;
}

/*
 * Match interface 0 only; interface 1 is claimed explicitly in probe so that
 * one hd60s_dev owns both.
 */
static const struct usb_device_id hd60s_id_table[] = {
	{ USB_DEVICE_INTERFACE_NUMBER(HD60S_VID, HD60S_PID_REV1, HD60S_IF_VIDEO) },
	{ USB_DEVICE_INTERFACE_NUMBER(HD60S_VID, HD60S_PID_REV2, HD60S_IF_VIDEO) },
	{ USB_DEVICE_INTERFACE_NUMBER(HD60S_VID, HD60S_PID_REV3, HD60S_IF_VIDEO) },
	{ USB_DEVICE_INTERFACE_NUMBER(HD60S_VID, HD60S_PID_REV4, HD60S_IF_VIDEO) },
	{ }
};
MODULE_DEVICE_TABLE(usb, hd60s_id_table);

static struct usb_driver hd60s_driver = {
	.name		= "hd60s",
	.probe		= hd60s_probe,
	.disconnect	= hd60s_disconnect,
	.suspend	= hd60s_suspend,
	.resume		= hd60s_resume,
	.reset_resume	= hd60s_resume,
	.id_table	= hd60s_id_table,
};

module_usb_driver(hd60s_driver);

MODULE_DESCRIPTION("Elgato Game Capture HD60 S video and audio capture");
MODULE_AUTHOR("Doug Brown <doug@schmorgal.com>");
MODULE_LICENSE("GPL");
