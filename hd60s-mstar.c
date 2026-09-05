// SPDX-License-Identifier: GPL-2.0
/*
 * Elgato HD60 S revisions 1-3 (PIDs 0x004F, 0x005E, 0x0074) - the MStar
 * MST3367 front end
 *
 * Copyright (c) 2026 Doug Brown <doug@schmorgal.com>
 *
 * The FX3 firmware is byte-identical on all four revisions, so the USB layer,
 * the stream format, the alt formula and the cold start are shared with rev 4.
 * Only the front end differs, and that is this file.
 */
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/math64.h>
#include <linux/string.h>

#include "hd60s.h"
#include "hd60s-modes.h"
#include "hd60s-txinit.h"

/*
 * Every helper here needs ctrl_lock: a detect is thirty-odd transfers and none
 * may interleave with a stream start or stop.
 */

static int chip_write(struct hd60s_dev *d, u8 slave, u8 reg, u8 val)
{
	u8 tx[2] = { reg, val };

	lockdep_assert_held(&d->ctrl_lock);
	return hd60s_i2c_write(d, slave, tx, sizeof(tx));
}

static int chip_read(struct hd60s_dev *d, u8 slave, u8 reg)
{
	u8 rx = 0;
	int ret;

	lockdep_assert_held(&d->ctrl_lock);
	ret = hd60s_i2c_read(d, slave, &reg, 1, &rx, 1);
	return ret < 0 ? ret : rx;
}

/*
 * Bank select is slave 0x9C register 0x00. Re-selected on every access and
 * never cached, as the Windows driver does: a stale bank reads the wrong
 * register silently.
 */
static int mst_write(struct hd60s_dev *d, u8 bank, u8 reg, u8 val)
{
	int ret;

	ret = chip_write(d, HD60S_I2C_MSTAR, 0x00, bank);
	if (ret < 0)
		return ret;
	return chip_write(d, HD60S_I2C_MSTAR, reg, val);
}

static int mst_read(struct hd60s_dev *d, u8 bank, u8 reg)
{
	int ret = chip_write(d, HD60S_I2C_MSTAR, 0x00, bank);

	if (ret < 0)
		return ret;
	return chip_read(d, HD60S_I2C_MSTAR, reg);
}

static int mst_rmw(struct hd60s_dev *d, u8 bank, u8 reg, u8 and_mask, u8 or_mask)
{
	int v = mst_read(d, bank, reg);

	if (v < 0)
		return v;
	return mst_write(d, bank, reg, (u8)((v & and_mask) | or_mask));
}

/*
 * The 0xD4 device: 208 registers written once at bring-up, with a leading page
 * byte. What the chip is has not been established.
 */
static const u8 mstar_d4_init[0xD0] = {
	0x01, 0x00, 0x08, 0x08, 0x80, 0x04, 0x00, 0x01, 0x78, 0x00, 0x11, 0x11,
	0x11, 0x91, 0x11, 0x91, 0x02, 0x15, 0x94, 0xfd, 0x99, 0xe3, 0x00, 0x00,
	0x18, 0xa0, 0x20, 0x80, 0x00, 0x00, 0x00, 0x00, 0x01, 0x04, 0x10, 0x10,
	0x00, 0x00, 0x10, 0x11, 0x00, 0x00, 0x37, 0x2d, 0x2d, 0x5b, 0x40, 0x80,
	0x00, 0x00, 0x00, 0xb6, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x91, 0x91,
	0x93, 0x93, 0x91, 0x93, 0x03, 0x07, 0x00, 0x07, 0x00, 0x00, 0x5b, 0x5b,
	0x30, 0xc8, 0x5b, 0xc8, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
	0x03, 0x03, 0x00, 0x00, 0x0c, 0x0c, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x03, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x04, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f,
	0x7f, 0x7f, 0x7f, 0x7f, 0x86, 0x85, 0xab, 0x97, 0x9e, 0x9e, 0x7f, 0x7f,
	0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0xff, 0xff, 0xff, 0xff, 0x7f, 0x7f,
	0x7f, 0x7f, 0x7f, 0x7f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0xe8, 0xe8,
	0xe9, 0xe9, 0xe8, 0xe8, 0x46, 0x46, 0x46, 0x46, 0x46, 0x46, 0x74, 0x74,
	0x6c, 0x6c, 0x74, 0x74,
};

static int mstar_ext_init(struct hd60s_dev *d)
{
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(mstar_d4_init); i++) {
		u8 tx[3] = { 0x00, (u8)i, mstar_d4_init[i] };
		u8 rd[2] = { 0x00, (u8)i };
		u8 back;

		ret = hd60s_i2c_write(d, HD60S_I2C_EXT, tx, sizeof(tx));
		if (ret < 0)
			return ret;
		/* Read back and discarded, as the Windows driver does; the read
		 * doubles as the inter-write delay.
		 */
		hd60s_i2c_read(d, HD60S_I2C_EXT, rd, sizeof(rd), &back, 1);
	}
	return 0;
}

/*
 * The HDMI loop-through transmitter, an ITE IT66121 at 0x9A. None of it is
 * needed to capture, and on rev 4 the MCU owns it.
 *
 * Downstream HDCP is deliberately not implemented. The Windows driver does it,
 * authenticating the display and muting until the handshake succeeds while the
 * input is encrypted and idle. Capture never needs it -- mstar_gate_block()
 * zeroes the key block while streaming, so the source sends clear video -- and
 * for the idle case an untested mute-until-authenticated sequence could turn a
 * working picture into a black screen.
 */

#define TX_SW_RST		0x04
#define  TX_RST_AUD		0x04	/* the audio block                     */
#define  TX_RST_VID		0x08	/* the video path, released to output  */
#define TX_INT_CTRL		0x05
#define TX_INT_MASK1		0x09
#define TX_SYS_STATUS		0x0e
#define  TX_STATUS_HPD		0x40	/* a sink is connected                 */
#define  TX_STATUS_RXSEN	0x20	/* and its receiver is powered         */
#define TX_BANK			0x0f
#define TX_AVI_DB1		0x58	/* DB1..DB5 at 0x58..0x5C, bank 1 */
#define TX_AVI_SUM		0x5d
#define TX_AVI_DB6		0x5e	/* DB6..DB13 at 0x5E..0x65        */
#define TX_AFE_DRV		0x61	/* bank 0; DB9 shares the number   */
#define TX_CSC_CTRL		0x72
#define TX_AVIINFO_CTRL		0xcd
#define TX_AUDINFO_CTRL		0xce
#define TX_PKT_ENABLE		0x03	/* enable + repeat                 */

static int tx_write(struct hd60s_dev *d, u8 reg, u8 val)
{
	u8 tx[2] = { reg, val };

	return hd60s_i2c_write(d, HD60S_I2C_TX, tx, sizeof(tx));
}

static int tx_read(struct hd60s_dev *d, u8 reg)
{
	u8 rx = 0;
	int ret = hd60s_i2c_read(d, HD60S_I2C_TX, &reg, 1, &rx, 1);

	return ret < 0 ? ret : rx;
}

static int tx_write_block(struct hd60s_dev *d, u8 reg, const u8 *val, int n)
{
	u8 tx[1 + 18];

	if (n > (int)sizeof(tx) - 1)
		return -EINVAL;
	tx[0] = reg;
	memcpy(tx + 1, val, n);
	return hd60s_i2c_write(d, HD60S_I2C_TX, tx, n + 1);
}

/*
 * The sink's EDID, over the DDC engine, to answer one question: HDMI or DVI.
 *
 * A 32-byte FIFO, one chunk per command. 0x10 selects the host as DDC master,
 * 0x15 is the command (0x09 FIFO clear, 0x03 read, 0x0F abort), 0x11-0x14 are
 * address/offset/count/segment, 0x16 the status (bit 7 done, bits 5:3 error)
 * and 0x17 the FIFO window. Commands are bracketed by 0x65 = 0x02 / 0x00.
 */

/* As the Windows driver does, before and after. */
static void tx_ddc_abort(struct hd60s_dev *d)
{
	int r04 = tx_read(d, TX_SW_RST);
	int r20 = tx_read(d, 0x20);

	tx_write(d, 0x65, 0x02);
	if (r20 >= 0)
		tx_write(d, 0x20, r20 & ~1);	/* HDCP desire off */
	if (r04 >= 0)
		tx_write(d, TX_SW_RST, r04 | 1);
	tx_write(d, 0x10, 0x01);
	tx_write(d, 0x15, 0x0f);		/* abort, twice */
	tx_write(d, 0x15, 0x0f);
	tx_write(d, 0x65, 0x00);
}

static int tx_ddc_wait(struct hd60s_dev *d)
{
	int i, v;

	for (i = 0; i < 200; i++) {
		v = tx_read(d, 0x16);
		if (v < 0)
			return v;
		if (v & 0x80)
			return (v & 0x38) ? -EIO : 0;
		if (v & 0x38)
			return -EIO;
		usleep_range(2000, 3000);
	}
	return -ETIMEDOUT;
}

static int tx_ddc_read_block(struct hd60s_dev *d, u8 block, u8 *buf)
{
	u8 seg = block >> 1;
	u8 off = (block & 1) << 7;
	int left = 128, n, i, v, ret;

	v = tx_read(d, 0x06);
	if (v < 0)
		return v;
	if (v & 0x04)			/* a DDC fault is latched */
		tx_ddc_abort(d);

	tx_write(d, 0x10, 0x01);
	tx_write(d, 0x15, 0x09);	/* FIFO clear */
	tx_write(d, TX_BANK, 0x00);

	while (left > 0) {
		n = min(left, 32);
		tx_write(d, 0x10, 0x01);
		tx_write(d, 0x15, 0x09);
		tx_write(d, 0x65, 0x02);
		tx_write(d, 0x10, 0x01);
		tx_write(d, 0x11, 0xa0);
		tx_write(d, 0x12, off);
		tx_write(d, 0x13, n);
		tx_write(d, 0x14, seg);
		tx_write(d, 0x15, 0x03);	/* EDID read */
		ret = tx_ddc_wait(d);
		if (ret < 0) {
			tx_write(d, 0x65, 0x00);
			return ret;
		}
		for (i = 0; i < n; i++) {
			v = tx_read(d, 0x17);
			if (v < 0) {
				tx_write(d, 0x65, 0x00);
				return v;
			}
			*buf++ = v;
		}
		off += n;
		left -= n;
	}
	tx_write(d, 0x65, 0x00);
	return 0;
}

/*
 * Only a parsed CEA extension carrying the HDMI vendor block sets the sink
 * flags, so an unreadable sink, a malformed block and a non-CEA extension are
 * all DVI -- the answer that cannot cost a picture, DVI being displayable on an
 * HDMI sink where the reverse is not. A read failure leaves tx_edid_valid clear
 * so the next takeover retries; a malformed EDID is an answer and does not.
 */
static void mstar_tx_edid(struct hd60s_dev *d)
{
	static const u8 header[8] = {
		0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00
	};
	u8 blk[128];
	u8 sum = 0;
	unsigned int i, end;
	bool hdmi = false, audio = false;

	d->mst.tx_sink_hdmi = false;
	d->mst.tx_sink_audio = false;
	d->mst.tx_edid_valid = true;

	if (tx_ddc_read_block(d, 0, blk) < 0)
		goto retry;
	for (i = 0; i < sizeof(blk); i++)
		sum += blk[i];
	if (sum != 0 || memcmp(blk, header, sizeof(header))) {
		dev_info(&d->intf->dev,
			 "sink EDID is malformed: DVI, RGB output\n");
		return;
	}

	if (blk[126] == 0) {			/* no CEA extension: DVI */
		dev_info(&d->intf->dev,
			 "sink EDID has no CEA extension: DVI, RGB output\n");
		return;
	}

	if (tx_ddc_read_block(d, 1, blk) < 0)
		goto retry;
	if (blk[0] != 0x02) {
		/* HDMI requires a CEA extension, so this is a DVI sink. */
		dev_info(&d->intf->dev,
			 "sink EDID extension is not CEA: DVI, RGB output\n");
		return;
	}
	audio = blk[3] & 0x40;			/* basic audio */
	end = min_t(unsigned int, blk[2], sizeof(blk));
	/* walk the data blocks for the HDMI vendor block, OUI 00 0C 03 */
	for (i = 4; i + 1 < end;) {
		u8 tag = blk[i] >> 5, len = blk[i] & 0x1f;

		if (i + 1 + len > end)
			break;
		if (tag == 3 && len >= 3 &&
		    blk[i + 1] == 0x03 && blk[i + 2] == 0x0c && blk[i + 3] == 0x00)
			hdmi = true;
		i += 1 + len;
	}
	d->mst.tx_sink_hdmi = hdmi;
	d->mst.tx_sink_audio = hdmi && audio;
	dev_info(&d->intf->dev, "sink EDID: %s%s\n",
		 hdmi ? "HDMI" : "DVI, RGB output",
		 hdmi && !audio ? ", no audio" : "");
	return;

retry:
	/*
	 * Bounded: the poll re-runs the takeover, and each run is a full DDC
	 * attempt. After three the DVI answer stands for this hot-plug session.
	 */
	if (++d->mst.tx_edid_tries < 3) {
		d->mst.tx_edid_valid = false;
	} else {
		dev_warn(&d->intf->dev,
			 "sink EDID unreadable after %u attempts; DVI, RGB output\n",
			 d->mst.tx_edid_tries);
	}
}

/*
 * The transmitter's YCbCr-to-RGB conversion, for a DVI sink, which can take
 * neither YCbCr nor an InfoFrame. Offsets at 0x73, Q11 coefficients at 0x76,
 * selected by the same BT.601/BT.709 decision that drives the MStar matrix so
 * the pair cannot disagree.
 */
static const u8 tx_csc_offset[3] = { 0x00, 0x80, 0x00 };
static const u8 tx_csc_rgb_601[18] = {
	0x00, 0x08, 0x6a, 0x3a, 0x4f, 0x3d, 0x00, 0x08, 0xf7,
	0x0a, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0xdb, 0x0d,
};

static const u8 tx_csc_rgb_709[18] = {
	0x00, 0x08, 0x53, 0x3c, 0x89, 0x3e, 0x00, 0x08, 0x51,
	0x0c, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x87, 0x0e,
};

/*
 * The transmitter's audio path: channel status, clock regeneration, InfoFrame.
 *
 * The MCU sets this up once and never re-syncs, so touching the MStar's audio
 * path leaves the loop-through silent until a power cycle. Reprogramming it is
 * the Windows driver's answer. Always stereo 24-bit; only the rate varies.
 */
static u8 tx_fs_code(u32 hz)
{
	switch (hz) {
	case 44100:	return 0x0;
	case 48000:	return 0x2;
	case 32000:	return 0x3;
	case 88200:	return 0x8;
	case 96000:	return 0xa;
	case 176400:	return 0xc;
	case 192000:	return 0xe;
	default:	return 0x1;	/* the Windows driver's "not indicated" */
	}
}

/*
 * Has the MCU finished with the transmitter? It takes a varying time, and
 * writing into a half-configured chip leaves the passthrough black until the
 * next replug, so ask rather than wait: a sink connected and detected with the
 * video path out of reset means it has finished. Registers 0x04 and 0x0E are
 * below the banked range, so this needs no bank select.
 */
static bool mstar_tx_ready(struct hd60s_dev *d)
{
	int st, rst;

	st = tx_read(d, TX_SYS_STATUS);
	if (st < 0)
		return false;
	if (!(st & TX_STATUS_HPD) || !(st & TX_STATUS_RXSEN))
		return false;

	rst = tx_read(d, TX_SW_RST);
	if (rst < 0)
		return false;
	return !(rst & TX_RST_VID);
}

/*
 * Lazily rather than at cold start, where the MCU is still bringing the chip
 * up. Once only; < 0 when there is nothing to talk to.
 *
 * Two IDs accepted, as the Windows driver does: 0xCA/0x11/0x16 and
 * 0x49/0x12/0x16, 0x49 being the top byte of ITE's vendor ID 0x4954.
 */
static int mstar_tx_probe(struct hd60s_dev *d)
{
	static const u8 reg[4] = { 0x01, 0x02, 0x03, 0x05 };
	int id[4];
	unsigned int i;

	if (d->mst.tx_probed)
		return d->mst.tx_present ? 0 : -ENODEV;

	/*
	 * Latched only once all four reads have answered. A failed read is a
	 * transport problem, not an absent chip -- a missing slave still
	 * answers the bridge, with garbage the ID test rejects -- and latching
	 * on it would silently disable the loop-through for the whole session
	 * over one bad transfer. Unlatched, the next poll retries.
	 */
	for (i = 0; i < ARRAY_SIZE(reg); i++) {
		id[i] = tx_read(d, reg[i]);
		if (id[i] < 0)
			return id[i];
	}
	d->mst.tx_probed = true;
	d->mst.tx_present =
		(id[0] == 0xca && id[1] == 0x11 && id[2] == 0x16 &&
		 (id[3] & 0x20)) ||
		(id[0] == 0x49 && id[1] == 0x12 && id[2] == 0x16);
	dev_info(&d->intf->dev,
		 d->mst.tx_present ?
		 "HDMI loop-through transmitter: ITE device %03x rev %u\n" :
		 "no HDMI transmitter at 0x9A (%03x %u), loop-through not programmed\n",
		 ((id[2] & 0x0f) << 8) | id[1], (u8)id[2] >> 4);
	return d->mst.tx_present ? 0 : -ENODEV;
}

/*
 * The AVI InfoFrame's DB2 aspect field and DB5 pixel-repetition field, from
 * the mode. Of the CEA codes the tables carry, only VIC 1 is 4:3, and only
 * the pixel-doubled SD codes 7 and 22 are sent twice per pixel; a VESA mode
 * has no VIC and says nothing about its aspect.
 */
static u8 mstar_avi_aspect(const struct hd60s_mode *m)
{
	if (!m->vic)
		return 0x00;			/* no data */
	return m->vic == 1 ? 0x10 : 0x20;	/* 4:3 : 16:9 */
}

static u8 mstar_avi_repeat(const struct hd60s_mode *m)
{
	return (m->vic == 7 || m->vic == 22) ? 1 : 0;
}

/*
 * DB2 and DB4 for the mode being sent. A VIC and aspect from the source's own
 * InfoFrame are re-emitted verbatim -- the source knows its 4:3-against-16:9 SD
 * variants where the table holds one; otherwise both come from the table, where
 * 0 is the legal "refer to the timing" value.
 */
static void mstar_avi_db(struct hd60s_dev *d, const struct hd60s_mode *m,
			 u8 *db2, u8 *db4)
{
	*db2 = 0x08 | (d->mst.bt709 ? 0x80 : 0x40);
	if (d->mst.src_vic) {
		*db2 |= (u8)(d->mst.src_aspect << 4);
		*db4 = d->mst.src_vic;
	} else {
		*db2 |= mstar_avi_aspect(m);
		*db4 = m->vic;
	}
}

/*
 * Write and latch the AVI InfoFrame. All thirteen bytes, checksum last -- 0x5D
 * is what latches the packet -- so the frame on the wire is exactly db[]
 * whatever the replay table left behind. DB2/DB4 are cached so the keeper can
 * re-emit when the source's own InfoFrame arrives after a mode change.
 */
static void mstar_tx_avi(struct hd60s_dev *d, const struct hd60s_mode *m)
{
	u8 db[13] = { 0 };	/* DB1..DB13 */
	unsigned int i;
	u8 sum;

	db[0] = 0x30;				/* YCbCr 4:2:2, active format */
	mstar_avi_db(d, m, &db[1], &db[3]);
	db[4] = mstar_avi_repeat(m);
	sum = 0x82 + 0x02 + 0x0d;
	for (i = 0; i < ARRAY_SIZE(db); i++)
		sum += db[i];

	tx_write(d, TX_BANK, 0x01);
	for (i = 0; i < 5; i++)
		tx_write(d, TX_AVI_DB1 + i, db[i]);
	for (; i < ARRAY_SIZE(db); i++)
		tx_write(d, TX_AVI_DB6 + (i - 5), db[i]);
	tx_write(d, TX_AVI_SUM, (u8)(0x100 - sum));	/* latches it */
	tx_write(d, TX_BANK, 0x00);
	tx_write(d, TX_AVIINFO_CTRL, TX_PKT_ENABLE);

	d->mst.tx_avi_db2 = db[1];
	d->mst.tx_avi_vic = db[3];
}

/*
 * Take the transmitter over completely, as the Windows driver does.
 *
 * Reprogramming the MStar disturbs what the transmitter is fed and the MCU only
 * sometimes re-syncs; an intermittently black passthrough after a driver load
 * is exactly that. The answer is to re-establish it rather than tiptoe.
 *
 * hd60s_tx_takeover[] is verbatim from a capture and should stay that way: on
 * this board the sync generator is not driven from a per-VIC table at all,
 * 0x90..0xA3 being 0xFF, and the input mode is 0x40, separate syncs. The
 * session was 1080p60, so its AFE values are the >80 MHz set.
 */
/*
 * The TMDS analog front end switches class at an 80 MHz pixel clock, and the
 * replay carries the >80 MHz values from its 1080p60 session. Substituted
 * during the replay rather than after it: entry 53 releases the video path from
 * reset, so a later override would land on a live, un-muted output.
 *
 * Bank 0 only -- bank 1 has its own 0x62, 0x64 and 0x68.
 */
static u8 tx_afe_low_clock(u8 reg, u8 val)
{
	switch (reg) {
	case 0x62:	return 0x18;
	case 0x64:	return 0x1d;
	case 0x68:	return 0x10;
	default:	return val;
	}
}

static int mstar_tx_takeover(struct hd60s_dev *d, const struct hd60s_mode *m)
{
	static const struct { u8 reg, val; } bringup[] = {
		{ TX_INT_CTRL,	0x00 },
		{ TX_SW_RST,	0x3d },
		{ TX_SW_RST,	0x1d },
		{ TX_AFE_DRV,	0x30 },
		{ TX_INT_MASK1,	0xb2 },
		{ 0x0a,		0xf8 },
		{ 0x0b,		0x37 },
		{ 0xc9, 0x00 }, { 0xca, 0x00 }, { 0xcb, 0x00 }, { 0xcc, 0x00 },
		{ 0xcd, 0x00 }, { 0xce, 0x00 }, { 0xcf, 0x00 }, { 0xd0, 0x00 },
	};
	unsigned int i;
	bool sink;
	int bank = -1, ret, st;

	if (mstar_tx_probe(d) < 0)
		return 0;

	/*
	 * Not up until the whole sequence lands: the canary is rewritten near
	 * the top of the replay, so a partial takeover would look healthy.
	 */
	d->mst.tx_up = false;

	/*
	 * Is a sink attached? With one present its EDID decides DVI against
	 * HDMI; without one there is nothing to ask, so the sink defaults
	 * hold and tx_up stays false -- the detect poll re-runs this when a
	 * display appears. A failed read is a transport problem, not an
	 * absent sink: abort and let the poll retry, rather than tearing a
	 * correct DVI determination down to the defaults.
	 */
	st = tx_read(d, TX_SYS_STATUS);
	if (st < 0)
		return st;
	sink = (st & TX_STATUS_HPD) && (st & TX_STATUS_RXSEN);
	if (sink) {
		if (!d->mst.tx_edid_valid)
			mstar_tx_edid(d);
	} else {
		d->mst.tx_sink_hdmi = true;
		d->mst.tx_sink_audio = true;
		d->mst.tx_edid_valid = false;
		d->mst.tx_edid_tries = 0;
	}

	ret = tx_write(d, TX_BANK, 0x00);
	if (ret < 0)
		return ret;
	for (i = 0; i < ARRAY_SIZE(bringup); i++) {
		ret = tx_write(d, bringup[i].reg, bringup[i].val);
		if (ret < 0)
			return ret;
	}

	for (i = 0; i < ARRAY_SIZE(hd60s_tx_takeover); i++) {
		const struct hd60s_tx_op *o = &hd60s_tx_takeover[i];
		u8 val = o->val;

		if (o->bank != bank) {
			ret = tx_write(d, TX_BANK, o->bank);
			if (ret < 0)
				return ret;
			bank = o->bank;
		}
		if (o->bank == 0 && m->pixclk <= 80000)
			val = tx_afe_low_clock(o->reg, val);
		ret = tx_write(d, o->reg, val);
		if (ret < 0)
			return ret;
	}
	if (!d->mst.tx_sink_hdmi) {
		/*
		 * A DVI sink: no YCbCr, no packets. The replay already set
		 * 0x72 = 0x03, so the conversion is kept and given coefficients
		 * and everything HDMI is switched off -- 0xC0 selects DVI on
		 * the wire and both InfoFrames stay unsent.
		 *
		 * UNTESTED: no DVI monitor has been attached. A sink whose EDID
		 * cannot be read takes this path too.
		 */
		tx_write(d, TX_BANK, 0x00);
		tx_write_block(d, 0x73, tx_csc_offset, sizeof(tx_csc_offset));
		tx_write_block(d, 0x76,
			       d->mst.bt709 ? tx_csc_rgb_709 : tx_csc_rgb_601,
			       18);
		tx_write(d, TX_CSC_CTRL, 0x03);	/* YCbCr 4:2:2 -> RGB */
		tx_write(d, 0xc0, 0x00);	/* DVI */
		tx_write(d, TX_AVIINFO_CTRL, 0x00);
		tx_write(d, TX_AUDINFO_CTRL, 0x00);

		d->mst.tx_audio_dirty = false;
		goto done;
	}

	/*
	 * An HDMI sink (or none to ask): bypass the conversion. The captured
	 * session's coefficients assume the MStar setup the Windows driver
	 * produced, which this driver programs itself, so replaying them gives
	 * wrong colors. Passing 4:2:2 through is lossless and is also the
	 * transmitter's own pre-EDID default.
	 */
	tx_write(d, TX_BANK, 0x00);
	tx_write(d, TX_CSC_CTRL, 0x00);		/* no conversion */

	/* Computed for this mode, not replayed from the captured 1080p60. */
	mstar_tx_avi(d, m);

	/*
	 * The replay's audio constants are the captured 48 kHz, so that is what
	 * the cache records -- the measured rate would never trip the level
	 * trigger in mstar_detect() and the channel status would stay at 48.
	 */
	d->mst.tx_audio_hz = 48000;
	d->mst.tx_audio_dirty = d->mst.tx_sink_audio;
done:
	/*
	 * Configured for this sink. Without a sink the flag stays false so
	 * that the poll runs the takeover again -- EDID and all -- the moment
	 * a display is attached.
	 */
	d->mst.tx_up = sink;
	dev_dbg(&d->intf->dev,
		"loop-through transmitter programmed (%s sink%s)\n",
		sink ? (d->mst.tx_sink_hdmi ? "HDMI" : "DVI") : "no",
		m->pixclk <= 80000 ? ", low-clock AFE" : "");
	return 0;
}

/*
 * The Audio Clock Regeneration N value, per HDMI's recommended table as the
 * Windows driver computes it: the base N for the rate family, the
 * fractional-TMDS alternates when the mode is a 1000/1001 rate (with a second
 * alternate above a 145 MHz pixel clock), doubled and quadrupled up the x2/x4
 * rates. The fallback for a rate outside the families is the spec's default
 * 128*fs/1000.
 */
static u32 tx_acr_n(u32 hz, bool frac, u32 pixclk_khz)
{
	u32 n;

	switch (hz) {
	case 32000:
		return frac ? 11648 : 4096;
	case 44100:
	case 88200:
	case 176400:
		n = frac ? (pixclk_khz > 145000 ? 8918 : 17836) : 6272;
		if (hz == 88200)
			n *= 2;
		else if (hz == 176400)
			n *= 4;
		return n;
	case 48000:
	case 96000:
	case 192000:
		n = frac ? (pixclk_khz > 145000 ? 5824 : 11648) : 6144;
		if (hz == 96000)
			n *= 2;
		else if (hz == 192000)
			n *= 4;
		return n;
	default:
		return hz * 128 / 1000;
	}
}

static int mstar_tx_audio(struct hd60s_dev *d)
{
	/*
	 * DB1 is the channel count less one; DB2 and DB3 are left zero, which
	 * tells the sink to take the sample size and rate from the channel
	 * status rather than the InfoFrame. They still count towards the
	 * checksum.
	 */
	static const u8 db[5] = { 0x01, 0x00, 0x00, 0x00, 0x00 };
	u32 hz = d->mst.audio_hz ? d->mst.audio_hz : 48000;
	u32 n = tx_acr_n(hz, d->mst.applied_frac,
			 d->mst.applied ? d->mst.applied->pixclk : 0);
	u8 fs = tx_fs_code(hz);
	u8 sum = 0x71;			/* 0x100 - (0x84 + 0x01 + 0x0A) */
	unsigned int i;
	int v;

	if (mstar_tx_probe(d) < 0)
		return 0;

	/*
	 * bank 0: hold the audio block in reset while it is reprogrammed.
	 *
	 * Absolute values, not a read-modify-write. The Windows driver reads
	 * first, but of a transmitter it configured itself; ours is one the MCU
	 * owns, over a bridge whose reads are staged, and a stale answer read
	 * back into this register puts the video path into reset. The values
	 * below are what its arithmetic produces against a running chip.
	 */
	v = tx_write(d, TX_BANK, 0x00);
	if (v < 0)
		return v;
	tx_write(d, TX_SW_RST, TX_RST_AUD);

	tx_write(d, 0xe2, 0xe4);
	tx_write(d, 0xe3, 0x00);
	tx_write(d, 0xe4, 0x08);	/* two channels          */
	tx_write(d, 0xe0, 0xc1);	/* 24-bit sample, enable */
	tx_write(d, 0xe1, 0x01);
	/*
	 * CTS is regenerated by the chip (0xC5 = 0, 0x58 bit 2); 0x15 is the
	 * captured value of the Windows driver's clear-bit-3-set-bit-2 on 0x58.
	 */
	tx_write(d, 0xc5, 0x00);
	tx_write(d, 0x58, 0x15);

	/* bank 1: the clock-regeneration N, then the channel status */
	v = tx_write(d, TX_BANK, 0x01);
	if (v < 0)
		return v;
	tx_write(d, 0x33, (u8)n);
	tx_write(d, 0x34, (u8)(n >> 8));
	tx_write(d, 0x35, (u8)((n >> 16) & 0x0f));
	tx_write(d, 0x91, 0x00);	/* not single-channel     */
	tx_write(d, 0x92, 0x00);	/* category code          */
	tx_write(d, 0x93, 0x01);	/* two channels           */
	tx_write(d, 0x94, 0x21);	/* the channel map, 1..8  */
	tx_write(d, 0x95, 0x43);
	tx_write(d, 0x96, 0x65);
	tx_write(d, 0x97, 0x87);
	tx_write(d, 0x98, fs);
	/*
	 * The original-rate field is the sampling frequency inverted, over a
	 * word length of 0x0B -- 24 bits. At 48 kHz this is the 0xDB the
	 * Windows driver's trace carries.
	 */
	tx_write(d, 0x99, (u8)((u8)~fs << 4 | 0x0b));

	/*
	 * Drop every reset bit but the audio one, which is held across the
	 * InfoFrame -- the Windows driver's channel-status routine ends here and its
	 * InfoFrame routine begins after it. 0x04 is below the banked range,
	 * so this needs no return to bank 0 and the InfoFrame below continues
	 * in bank 1. Two bank switches for the whole sequence rather than
	 * four: every one of them is a window for the MCU.
	 */
	tx_write(d, TX_SW_RST, TX_RST_AUD);

	/* still bank 1: the audio InfoFrame */
	tx_write(d, 0x68, db[0] & 0x07);
	tx_write(d, 0x6b, db[3]);
	tx_write(d, 0x6c, db[4]);
	for (i = 0; i < ARRAY_SIZE(db); i++)
		sum -= db[i];
	tx_write(d, 0x6d, sum);

	v = tx_write(d, TX_BANK, 0x00);
	if (v < 0)
		return v;
	tx_write(d, TX_AUDINFO_CTRL, TX_PKT_ENABLE);

	d->mst.tx_audio_hz = hz;
	d->mst.tx_audio_dirty = false;
	dev_dbg(&d->intf->dev,
		"loop-through audio programmed, %u Hz (fs code %u, N %u)\n",
		hz, fs, n);
	/*
	 * The audio block comes out of reset only now: releasing it before the
	 * packet is enabled leaves the output silent.
	 */
	return tx_write(d, TX_SW_RST, 0x00);
}

/*
 * AVMUTE, as the Windows driver sets it around every transition. Clearing tx_up
 * schedules the re-establishment: the next detect poll runs the takeover, whose
 * final write is the un-mute.
 *
 * Never on the way out -- disconnect() sets d->gone first and the poll is
 * already canceled, so nothing would clear the mute and an unloaded driver
 * would leave the loop-through latched black.
 */
static void mstar_tx_mute(struct hd60s_dev *d)
{
	if (d->gone || !d->mst.tx_present || !d->mst.tx_up)
		return;
	tx_write(d, TX_BANK, 0x00);
	tx_write(d, 0xc1, 0x01);
	tx_write(d, 0xc6, 0x03);
	d->mst.tx_up = false;
}

/*
 * The transmitter's keeper, one call per detect poll while a mode is applied.
 *
 * The health test is a canary, as in the Windows driver: register 0x09 must
 * still hold the 0xB2 the bring-up wrote. Anything else means the chip lost its
 * configuration and the bring-up runs again. The same poll notices a display
 * unplugged (the AFE is powered down) or plugged in (the takeover runs).
 *
 * UNTESTED: the re-takeover the canary triggers -- it has not had to fire --
 * and the HPD unplug/replug service. Neither can break capture, and the canary
 * heals whatever misfires below it.
 */
static void mstar_tx_poll(struct hd60s_dev *d)
{
	int st, canary;

	if (!d->mst.applied || mstar_tx_probe(d) < 0)
		return;

	st = tx_read(d, TX_SYS_STATUS);
	if (st < 0)
		return;
	if (!(st & TX_STATUS_HPD) || !(st & TX_STATUS_RXSEN)) {
		if (d->mst.tx_up) {
			dev_dbg(&d->intf->dev, "sink lost, AFE down\n");
			tx_write(d, TX_SW_RST, 0x1d);
			tx_write(d, 0x61, 0x30);	/* AFE power-down */
			d->mst.tx_up = false;
		}
		d->mst.tx_edid_valid = false;	/* the next sink is re-read */
		d->mst.tx_edid_tries = 0;
		return;
	}

	canary = tx_read(d, 0x09);
	if (canary < 0)
		return;
	if (!d->mst.tx_up || canary != 0xb2 || !d->mst.tx_edid_valid) {
		if (canary != 0xb2)
			dev_dbg(&d->intf->dev,
				"transmitter canary 0x09 = %02x, re-establishing\n",
				canary);
		mstar_tx_takeover(d, d->mst.applied);
	}

	/*
	 * The source's InfoFrame can arrive a poll or two after a mode change,
	 * once the takeover has emitted the table's VIC. Re-emit just the AVI
	 * packet -- fifteen writes, no mute, no takeover.
	 */
	if (d->mst.tx_up && d->mst.tx_sink_hdmi) {
		u8 db2, db4;

		mstar_avi_db(d, d->mst.applied, &db2, &db4);
		if (db2 != d->mst.tx_avi_db2 || db4 != d->mst.tx_avi_vic)
			mstar_tx_avi(d, d->mst.applied);
	}

	if ((d->mst.audio_hz ?: 48000) != d->mst.tx_audio_hz)
		d->mst.tx_audio_dirty = true;
	if (d->mst.tx_audio_dirty && d->mst.tx_sink_audio && mstar_tx_ready(d))
		mstar_tx_audio(d);
}

/* Bank 0 register 0xB7 bit 1. */
static int mstar_b7(struct hd60s_dev *d, bool set)
{
	return mst_rmw(d, 0, 0xb7, set ? 0xff : (u8)~0x02, set ? 0x02 : 0x00);
}

/*
 * Bank 1 0x24-0x27 are an indirect port: 0x25/0x26 a 16-bit address, 0x27 an
 * auto-incrementing data window, 0x24 bit 0 the write enable. Five bytes at
 * address 0 are read once per bring-up, zeroed by stream start and restored by
 * stream stop.
 *
 * Five bytes at address 0 of an HDMI receiver's bank-1 indirect port is very
 * probably the BKSV, zeroed to tell a source the sink has no HDCP. Whatever
 * it is, it is reproduced verbatim in both directions.
 */
static int mstar_save_block(struct hd60s_dev *d)
{
	int i, v;

	if (d->mst.blk_valid)
		return 0;

	v = mst_write(d, 1, 0x25, 0x00);
	if (v < 0)
		return v;
	v = mst_write(d, 1, 0x26, 0x00);
	if (v < 0)
		return v;
	for (i = 0; i < 5; i++) {
		msleep(20);			/* the driver's own delay */
		v = mst_read(d, 1, 0x27);
		if (v < 0)
			return v;
		d->mst.blk[i] = v;
	}
	d->mst.blk_valid = true;
	dev_dbg(&d->intf->dev, "saved bank-1 block %5ph\n", d->mst.blk);
	return 0;
}

/*
 * The tail of the Windows driver's hardware init. Its VID/PID gate is a SKIP
 * list: on all four HD60 S PIDs it jumps past the GPIO 45 reset pulse, MStar
 * bank 0 register 0x51 and the two PLL writes at 0x88, which belong to other
 * boards. Doing them here would hardware-reset the MST3367 on every probe.
 */
static int mstar_hw_init(struct hd60s_dev *d)
{
	int ret, v;

	v = chip_read(d, HD60S_I2C_CTL, 0x3b);
	if (v < 0)
		return v;
	if (v != 0) {
		/* input <= 1 is the HDMI input on this board */
		ret = chip_write(d, HD60S_I2C_CTL, 0x3b, 0x02);
		if (ret < 0)
			return ret;
	}
	ret = chip_write(d, HD60S_I2C_CTL, 0x20, 0x05);
	if (ret < 0)
		return ret;
	ret = chip_write(d, HD60S_I2C_CTL, 0x00, 0x01);
	if (ret < 0)
		return ret;
	/* the Windows driver's per-board switch rejects all four HD60 S PIDs here */
	return chip_write(d, HD60S_I2C_CTL, 0x10, 0xfe);
}

struct mstar_regval {
	u8 bank, reg, val;
};

static int mstar_write_seq(struct hd60s_dev *d,
			   const struct mstar_regval *seq, unsigned int n)
{
	unsigned int i;
	int ret;

	for (i = 0; i < n; i++) {
		ret = mst_write(d, seq[i].bank, seq[i].reg, seq[i].val);
		if (ret < 0)
			return ret;
	}
	return 0;
}

/*
 * Every branch in the Windows driver resolved for this board, with the registry
 * properties at their defaults. The EDID programming it does first is
 * deliberately not reproduced.
 */
static int mstar_front_end_init(struct hd60s_dev *d)
{
	static const struct mstar_regval seq2[] = {
		{ 0, 0x41, 0x6f }, { 0, 0xb8, 0x00 },
		{ 1, 0x0f, 0x02 }, { 1, 0x16, 0x30 },
		{ 1, 0x17, 0x00 }, { 1, 0x18, 0x00 }, { 1, 0x19, 0x00 },
		{ 1, 0x1a, 0x50 },
	};
	static const struct mstar_regval seq3[] = {
		{ 1, 0x30, 0x80 }, { 1, 0x31, 0x00 }, { 1, 0x32, 0x00 },
		{ 0, 0xb0, 0x14 },
	};
	static const struct mstar_regval seq4[] = {
		{ 0, 0xad, 0x05 }, { 0, 0xb1, 0xc0 }, { 0, 0xb2, 0x00 },
		{ 0, 0xb3, 0x00 }, { 0, 0xb4, 0x55 },
	};
	static const struct mstar_regval seq5[] = {
		{ 2, 0x01, 0x61 }, { 2, 0x02, 0xf5 },
	};
	static const struct mstar_regval seq6[] = {
		{ 2, 0x04, 0x01 }, { 2, 0x05, 0x00 }, { 2, 0x06, 0x08 },
		{ 2, 0x1c, 0x1a }, { 2, 0x1d, 0x00 },
		{ 2, 0x1e, 0x00 }, { 2, 0x1f, 0x00 },
	};
	static const struct mstar_regval seq7[] = {
		{ 2, 0x17, 0xc0 }, { 2, 0x19, 0xff }, { 2, 0x1a, 0xff },
		{ 2, 0x1b, 0xfc }, { 2, 0x20, 0x00 },
	};
	/* the indirect port, in its not-streaming state */
	static const struct mstar_regval port_idle[] = {
		{ 1, 0x25, 0x00 }, { 1, 0x26, 0x00 },
		{ 1, 0x27, 0x00 }, { 1, 0x27, 0x00 }, { 1, 0x27, 0x00 },
		{ 1, 0x27, 0x00 }, { 1, 0x27, 0x00 },
	};
	/* the HDMI tail; only reached when the input index is 0 or 1 */
	static const struct mstar_regval hdmi_tail[] = {
		{ 0, 0xb8, 0x10 }, { 0, 0xb8, 0x00 },	/* a pulse */
		{ 2, 0x07, 0xf4 }, { 2, 0x07, 0x04 },	/* a pulse */
		{ 0, 0x51, 0x89 },
	};
	int ret, v;

	ret = mst_write(d, 0, 0x13, 0x08);
	if (ret < 0)
		return ret;
	ret = mstar_b7(d, true);
	if (ret < 0)
		return ret;
	ret = mstar_write_seq(d, seq2, ARRAY_SIZE(seq2));
	if (ret < 0)
		return ret;
	ret = mst_rmw(d, 1, 0x2a, 0xff, 0x07);
	if (ret < 0)
		return ret;
	ret = mst_write(d, 2, 0x08, 0x03);
	if (ret < 0)
		return ret;

	ret = mstar_save_block(d);		/* once per bring-up */
	if (ret < 0)
		return ret;

	ret = mst_write(d, 1, 0x24, 0x40);
	if (ret < 0)
		return ret;
	v = mst_read(d, 1, 0x24);
	if (v < 0)
		return v;
	if (v & 1) {
		ret = mstar_write_seq(d, port_idle, ARRAY_SIZE(port_idle));
		if (ret < 0)
			return ret;
	}

	ret = mstar_write_seq(d, seq3, ARRAY_SIZE(seq3));
	if (ret < 0)
		return ret;
	ret = mst_rmw(d, 0, 0xae, 0xff, 0x04);
	if (ret < 0)
		return ret;
	ret = mstar_write_seq(d, seq4, ARRAY_SIZE(seq4));
	if (ret < 0)
		return ret;
	ret = mst_rmw(d, 0, 0xb4, 0xfc, 0x00);
	if (ret < 0)
		return ret;
	ret = mstar_write_seq(d, seq5, ARRAY_SIZE(seq5));
	if (ret < 0)
		return ret;
	ret = mst_rmw(d, 2, 0x03, 0xff, 0x02);
	if (ret < 0)
		return ret;
	ret = mstar_write_seq(d, seq6, ARRAY_SIZE(seq6));
	if (ret < 0)
		return ret;
	ret = mst_rmw(d, 2, 0x25, 0xff, 0xa2);
	if (ret < 0)
		return ret;
	ret = mst_rmw(d, 2, 0x02, 0xff, 0x80);
	if (ret < 0)
		return ret;
	ret = mst_rmw(d, 2, 0x07, 0xff, 0x04);
	if (ret < 0)
		return ret;
	ret = mstar_write_seq(d, seq7, ARRAY_SIZE(seq7));
	if (ret < 0)
		return ret;
	ret = mst_rmw(d, 2, 0x21, 0xfc, 0x00);
	if (ret < 0)
		return ret;
	ret = mst_write(d, 2, 0x22, 0x26);
	if (ret < 0)
		return ret;
	ret = mst_write(d, 2, 0x27, 0x00);
	if (ret < 0)
		return ret;
	ret = mst_rmw(d, 2, 0x2e, 0xff, 0xa1);
	if (ret < 0)
		return ret;
	/*
	 * The output clamp. The Windows driver's other branch -- (v & 0xAA) |
	 * 0x2A and v | 0xBF -- only clamps chroma to 240 rather than letting it
	 * saturate at 254; luma is 16..235 either way. It does NOT decide
	 * whether chroma arrives centered on 128 or 0; that is mstar_reg92().
	 */
	ret = mst_rmw(d, 0, 0xab, 0x95, 0x15);
	if (ret < 0)
		return ret;
	ret = mst_rmw(d, 0, 0xac, 0xd5, 0x15);
	if (ret < 0)
		return ret;

	ret = mstar_b7(d, true);
	if (ret < 0)
		return ret;
	ret = mstar_write_seq(d, hdmi_tail, ARRAY_SIZE(hdmi_tail));
	if (ret < 0)
		return ret;
	ret = mstar_b7(d, false);
	if (ret < 0)
		return ret;

	/* board control device, register 0x20 bit 4 -- the audio mux */
	v = chip_read(d, HD60S_I2C_CTL, 0x20);
	if (v < 0)
		return v;
	ret = chip_write(d, HD60S_I2C_CTL, 0x20, (u8)(v | 0x10));
	if (ret < 0)
		return ret;

	ret = mst_write(d, 0, 0xb7, 0x00);
	if (ret < 0)
		return ret;

	ret = mst_rmw(d, 2, 0x01, 0x0f, 0x60);
	if (ret < 0)
		return ret;
	ret = mst_rmw(d, 2, 0x04, 0xff, 0x01);
	if (ret < 0)
		return ret;
	ret = mst_write(d, 2, 0x06, 0x08);
	if (ret < 0)
		return ret;
	ret = mst_rmw(d, 2, 0x09, 0xff, 0x20);
	if (ret < 0)
		return ret;
	ret = mst_rmw(d, 0, 0x54, 0xef, 0x00);
	if (ret < 0)
		return ret;

	msleep(300);				/* the Windows driver's own settle */
	return 0;
}

/*
 * The MCU handshake runs before this, in the shared cold start: every register
 * access below travels over the bridge, which does not answer until the
 * handshake completes. The steps reordered around it touch no chip.
 */
static int mstar_init(struct hd60s_dev *d)
{
	int ret;

	/*
	 * Every field describes a chip that is about to be re-initialized, so
	 * none of it survives. A no-op at probe; on resume it is the difference
	 * between working and silent, because a stale applied tells the detect
	 * poll the mode is still programmed and the endpoint stays quiet behind
	 * a healthy-looking control plane. The saved bank-1 block goes too --
	 * the stream gate restored it on the way down and it is re-read below.
	 */
	memset(&d->mst, 0, sizeof(d->mst));

	/*
	 * 0 is a real answer for both -- RGB, and a source stating no range --
	 * so without a sentinel the commonest case looks like "no change".
	 */
	d->mst.input_cs = 0xff;
	d->mst.input_q = 0xff;

	mutex_lock(&d->ctrl_lock);

	/*
	 * The MStar tail of the MCU presence check: write bank 0 register 7 and
	 * read it back. This is the first chip access after the MCU answers.
	 */
	ret = mst_write(d, 0, 0x07, 0xa5);
	if (ret < 0)
		goto out;
	ret = mst_read(d, 0, 0x07);
	if (ret < 0)
		goto out;
	if (ret != 0xa5)
		dev_warn(&d->intf->dev,
			 "MStar readback 0:07 = %02x, expected a5\n", ret);

	ret = mstar_ext_init(d);
	if (ret < 0)
		goto out;

	ret = mstar_hw_init(d);
	if (ret < 0)
		goto out;
	ret = mstar_front_end_init(d);
out:
	mutex_unlock(&d->ctrl_lock);
	return ret;
}

/*
 * The three registers in the apply block that depend on the mode, computed the
 * way the Windows driver computes them rather than replayed. The Windows
 * driver's signal monitor turns (hactive, vactive, fps) into a format code and
 * feeds it to two routines, which write bank 0 registers 0x03, 0x12 and 0x13.
 */
static u8 mstar_format_code(u16 hactive, u16 vactive, u16 fps)
{
	static const struct {
		u16 hactive, vactive, fps;
		u8 code;
	} tbl[] = {
		{  640,  480, 60, 0x12 }, {  640,  480, 72, 0x19 },
		{  640,  480, 75, 0x1a }, {  640,  480, 85, 0x1b },
		{  720,  240, 60, 0x00 }, {  720,  288, 50, 0x01 },
		{  720,  400, 85, 0x1c },
		{  720,  480, 60, 0x10 }, {  720,  576, 50, 0x11 },
		{  800,  600, 56, 0x1d }, {  800,  600, 60, 0x13 },
		{  800,  600, 72, 0x1e }, {  800,  600, 75, 0x1f },
		{  800,  600, 85, 0x20 },
		{  832,  624, 75, 0x42 },
		{ 1024,  600, 60, 0x21 },
		{ 1024,  768, 50, 0x22 }, { 1024,  768, 60, 0x14 },
		{ 1024,  768, 70, 0x23 }, { 1024,  768, 75, 0x25 },
		{ 1024,  768, 85, 0x26 },
		{ 1152,  864, 75, 0x27 },
		{ 1280,  720, 59, 0x02 }, { 1280,  720, 60, 0x02 },
		{ 1280,  720, 50, 0x03 },
		{ 1280,  768, 60, 0x28 }, { 1280,  800, 60, 0x35 },
		{ 1280,  960, 60, 0x2a },
		{ 1280, 1024, 59, 0x15 }, { 1280, 1024, 60, 0x15 },
		{ 1360,  768, 60, 0x2c }, { 1360,  768, 61, 0x2c },
		{ 1400, 1050, 60, 0x2e },
		{ 1440,  540, 50, 0x3a }, { 1440,  900, 60, 0x2d },
		{ 1920,  540, 60, 0x07 }, { 1920,  540, 50, 0x08 },
		{ 1920, 1080, 24, 0x0b }, { 1920, 1080, 25, 0x0a },
		{ 1920, 1080, 30, 0x09 }, { 1920, 1080, 50, 0x0f },
		{ 1920, 1080, 60, 0x0e },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(tbl); i++)
		if (tbl[i].hactive == hactive && tbl[i].vactive == vactive &&
		    tbl[i].fps == fps)
			return tbl[i].code;

	/*
	 * The fallback for anything the chain does not match, including the
	 * pixel-repeated 480i modes. (The Windows driver's own default is 0x42;
	 * both land in the same default rows of the two register writers
	 * below, so the difference is unobservable.)
	 */
	return 0x44;
}

/*
 * Bank 0 register 0x03. One 40-byte region in the Windows driver holds three
 * overlapping tables; it indexes the region three different ways, and the only
 * call site on this path fixes the second selector at zero.
 */
static int mstar_write_reg03(struct hd60s_dev *d, u8 fmt)
{
	static const u8 tbl[40] = {
		/* +0x00 */ 0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38,
			    0x40, 0x48, 0x50, 0x58, 0x60, 0x68, 0x70, 0x78,
		/* +0x10 */ 0x40, 0x48, 0x50, 0x58, 0x60, 0x68, 0x70, 0x78,
			    0xc0, 0xc8, 0xd0, 0xd8, 0xe0, 0xe8, 0xf0, 0xf8,
		/* +0x20 */ 0xc0, 0xc8, 0xd0, 0xd8, 0xe0, 0xe8, 0xf0, 0xf8,
	};
	u8 v;

	if (fmt == 0x45 || fmt == 0x46)
		return 0;			/* the Windows driver writes nothing */

	switch (fmt) {
	case 0x00:
	case 0x01:
	case 0x3c:
	case 0x3e:
		v = tbl[0x00 + 1];	/* 0x08 */
		break;
	case 0x02:
	case 0x03:
		v = tbl[0x10 + 0];	/* 0x40 */
		break;
	case 0x0e:
	case 0x0f:
		v = tbl[0x20 + 1];	/* 0xC8 */
		break;
	case 0x10:
	case 0x11:
		v = tbl[0x00 + 9];	/* 0x48 */
		break;
	default:
		v = tbl[0x10 + 8];	/* 0xC0 */
		break;
	}
	return mst_write(d, 0, 0x03, v);
}

/*
 * Bank 0 registers 0x12 and 0x13. The three-entry script that precedes them
 * in the Windows driver is mode-independent and stays in the replay table;
 * only these two writes depend on the format code.
 */
static int mstar_write_reg12_13(struct hd60s_dev *d, u8 fmt)
{
	int ret;

	if (fmt > 0x11)			/* 0x12 keeps the script's 0x04 */
		return mst_write(d, 0, 0x13, 0x04);

	ret = mst_write(d, 0, 0x12,
			(fmt >= 0x10 || (fmt & 0xee) == 0) ? 0x04 : 0x00);
	if (ret < 0)
		return ret;
	return mst_write(d, 0, 0x13, 0x14);
}

enum mstar_opcode {
	OP_MST,		/* MST3367 bank/register write   */
	OP_DIR,		/* direct chip write             */
	OP_I2C,		/* bridged two-byte write        */
	OP_EXT,		/* the 0xD4 device, paged        */
	OP_SUBFN,	/* MCU GPIO write                */
	OP_FMT03,	/* computed bank 0 register 0x03 */
	OP_FMT1213,	/* computed bank 0 regs 0x12/13  */
	OP_AUDIO,	/* the audio block for d->audio_src */
	OP_CSC,		/* the computed color-space matrix    */
	OP_R92,		/* computed bank 0 register 0x92      */
};

struct mstar_op {
	u8 op, a, b, c;
};

/*
 * The two audio-source configurations.
 *
 * Selecting an input is this whole block, not one mux bit: the codec's serial
 * interface, its power control, two MCU GPIO pins and the 0xD4 device all move
 * together, and writing the mux bit alone leaves the codec configured for the
 * other source.
 *
 * The line-in block is what the Windows driver does.
 *
 * UNTESTED: nothing has been connected to that jack.
 */
static const struct mstar_op mstar_audio_embedded[] = {
	{ OP_I2C, 0x94, 0x09, 0x46 },
	{ OP_I2C, 0x94, 0x18, 0x0f },
	{ OP_MST, 0x02, 0x27, 0x00 },
	{ OP_I2C, 0x94, 0x03, 0xa0 },	/* MIC_PWR_CTL   */
	{ OP_SUBFN, 0x05, 0x00, 0x00 },	/* MCU GPIO 0.5  */
	{ OP_SUBFN, 0x03, 0x00, 0x00 },	/* MCU GPIO 0.3  */
	{ OP_EXT, 0x00, 0x04, 0x03 },
	{ OP_I2C, 0x94, 0x0e, 0x80 },	/* ADCA_MIX_VOL  */
	{ OP_I2C, 0x94, 0x0f, 0x80 },	/* ADCB_MIX_VOL  */
	{ OP_I2C, 0x94, 0x10, 0x00 },
	{ OP_I2C, 0x94, 0x11, 0x00 },
	{ OP_I2C, 0x94, 0x04, 0x0e },	/* IFACE_CTL     */
	{ OP_I2C, 0x94, 0x07, 0x00 },	/* ADC_IN_SEL    */
	{ OP_I2C, 0x94, 0x10, 0x05 },
	{ OP_I2C, 0x94, 0x11, 0x05 },
	{ OP_I2C, 0x94, 0x0a, 0x00 },	/* ALC_PGAA      */
	{ OP_I2C, 0x94, 0x0b, 0x00 },	/* ALC_PGAB      */
	{ OP_I2C, 0x94, 0x0c, 0x00 },	/* ADCA_ATT      */
	{ OP_I2C, 0x94, 0x0d, 0x00 },	/* ADCB_ATT      */
};

static const struct mstar_op mstar_audio_linein[] = {
	{ OP_I2C, 0x94, 0x09, 0x46 },
	{ OP_I2C, 0x94, 0x18, 0x0f },
	{ OP_MST, 0x02, 0x27, 0xff },
	{ OP_I2C, 0x94, 0x03, 0xa1 },
	{ OP_SUBFN, 0x05, 0x01, 0x00 },
	{ OP_SUBFN, 0x03, 0x01, 0x00 },
	{ OP_EXT, 0x00, 0x04, 0x80 },
	{ OP_I2C, 0x94, 0x0e, 0x80 },
	{ OP_I2C, 0x94, 0x0f, 0x80 },
	{ OP_I2C, 0x94, 0x10, 0x00 },
	{ OP_I2C, 0x94, 0x11, 0x00 },
	{ OP_I2C, 0x94, 0x04, 0x4c },
	{ OP_I2C, 0x94, 0x07, 0x00 },
};

/* The apply-mode sequence, generated from a capture of the Windows driver. */
static const struct mstar_op mstar_apply_seq[] = {
	{ OP_MST, 0x00, 0xac, 0x95 },
	{ OP_MST, 0x00, 0xce, 0x80 },
	{ OP_MST, 0x00, 0xcf, 0x02 },
	{ OP_MST, 0x80, 0xd0, 0x00 },
	{ OP_MST, 0x80, 0xcf, 0x00 },
	{ OP_I2C, 0x9a, 0x0f, 0x00 },
	{ OP_I2C, 0x9a, 0xc1, 0x01 },
	{ OP_I2C, 0x9a, 0xc6, 0x03 },
	{ OP_MST, 0x00, 0xab, 0x15 },
	{ OP_MST, 0x00, 0xac, 0x95 },
	{ OP_MST, 0x00, 0xad, 0x05 },
	{ OP_MST, 0x00, 0x1e, 0x11 },
	{ OP_MST, 0x00, 0x1f, 0x01 },
	{ OP_CSC, 0x00, 0x00, 0x00 },
	{ OP_AUDIO, 0x00, 0x00, 0x00 },
	{ OP_DIR, 0x98, 0x20, 0x05 },
	{ OP_MST, 0x00, 0xe2, 0x00 },
	{ OP_MST, 0x02, 0x07, 0x14 },
	{ OP_MST, 0x02, 0x07, 0x04 },
	{ OP_MST, 0x00, 0xab, 0x95 },
	{ OP_MST, 0x00, 0x90, 0x15 },
	{ OP_MST, 0x00, 0x91, 0x15 },
	{ OP_R92, 0x00, 0x00, 0x00 },
	{ OP_MST, 0x00, 0xab, 0x95 },
	{ OP_MST, 0x00, 0xac, 0x95 },
	{ OP_MST, 0x00, 0xad, 0x05 },
	{ OP_MST, 0x00, 0x1e, 0x11 },
	{ OP_MST, 0x00, 0x1f, 0x01 },
	{ OP_CSC, 0x00, 0x00, 0x00 },
	{ OP_MST, 0x00, 0xb0, 0x21 },
	{ OP_MST, 0x00, 0xab, 0x15 },
	{ OP_MST, 0x00, 0xab, 0x15 },
	{ OP_MST, 0x00, 0xac, 0x95 },
	{ OP_MST, 0x00, 0xad, 0x05 },
	{ OP_MST, 0x00, 0x1e, 0x11 },
	{ OP_MST, 0x00, 0x1f, 0x01 },
	{ OP_CSC, 0x00, 0x00, 0x00 },
	{ OP_MST, 0x00, 0xb3, 0x00 },
	{ OP_MST, 0x02, 0x27, 0x00 },
	{ OP_MST, 0x02, 0x20, 0x00 },
	{ OP_I2C, 0x9a, 0x0f, 0x00 },
	{ OP_I2C, 0x9a, 0xc1, 0x01 },
	{ OP_I2C, 0x9a, 0xc6, 0x03 },
	{ OP_I2C, 0x9a, 0x0f, 0x00 },
	{ OP_I2C, 0x9a, 0xc1, 0x01 },
	{ OP_I2C, 0x9a, 0xc6, 0x03 },
	{ OP_MST, 0x00, 0xb2, 0x00 },
	{ OP_MST, 0x00, 0xb5, 0x00 },
	{ OP_FMT03, 0x00, 0x00, 0x00 },
	{ OP_MST, 0x00, 0x0f, 0x28 },
	{ OP_MST, 0x00, 0x12, 0x04 },
	{ OP_MST, 0x00, 0x17, 0x02 },
	{ OP_MST, 0x00, 0x12, 0x00 },
	{ OP_FMT1213, 0x00, 0x00, 0x00 },
	{ OP_I2C, 0x9a, 0x0f, 0x01 },
	{ OP_I2C, 0x9a, 0x58, 0x30 },
	{ OP_I2C, 0x9a, 0x59, 0x98 },
	{ OP_I2C, 0x9a, 0x5a, 0x00 },
	{ OP_I2C, 0x9a, 0x5b, 0x00 },
	{ OP_I2C, 0x9a, 0x5c, 0x00 },
	{ OP_I2C, 0x9a, 0x5e, 0x00 },
	{ OP_I2C, 0x9a, 0x5f, 0x00 },
	{ OP_I2C, 0x9a, 0x60, 0x00 },
	{ OP_I2C, 0x9a, 0x61, 0x00 },
	{ OP_I2C, 0x9a, 0x62, 0x00 },
	{ OP_I2C, 0x9a, 0x63, 0x00 },
	{ OP_I2C, 0x9a, 0x64, 0x00 },
	{ OP_I2C, 0x9a, 0x65, 0x00 },
	{ OP_I2C, 0x9a, 0x5d, 0xa7 },
	{ OP_I2C, 0x9a, 0x0f, 0x00 },
	{ OP_I2C, 0x9a, 0xcd, 0x03 },
	{ OP_MST, 0x00, 0x60, 0x10 },
};

/*
 * One executor for both the mode table and the audio blocks.  fmt is consulted
 * only by OP_FMT03/OP_FMT1213 and is ignored by the audio blocks, which
 * contain neither.
 */
static int mstar_run_ops(struct hd60s_dev *d, const struct mstar_op *seq,
			 unsigned int n, u8 fmt);
static int mstar_write_csc(struct hd60s_dev *d);
static u8 mstar_reg92(struct hd60s_dev *d);

/*
 * Program the audio path for a source. Caller holds ctrl_lock. The blocks
 * contain no OP_AUDIO, so the one level of recursion through mstar_run_ops()
 * terminates.
 */
static int mstar_write_audio_block(struct hd60s_dev *d, u8 src)
{
	const struct mstar_op *seq = src ? mstar_audio_linein
					 : mstar_audio_embedded;
	unsigned int n = src ? ARRAY_SIZE(mstar_audio_linein)
			     : ARRAY_SIZE(mstar_audio_embedded);

	return mstar_run_ops(d, seq, n, 0);
}

static int mstar_run_ops(struct hd60s_dev *d, const struct mstar_op *seq,
			 unsigned int n, u8 fmt)
{
	unsigned int i;
	int ret;

	for (i = 0; i < n; i++) {
		u8 op = seq[i].op, a = seq[i].a, b = seq[i].b, c = seq[i].c;
		u8 tx[5];

		switch (op) {
		case OP_MST:
			ret = mst_write(d, a, b, c);
			break;
		case OP_DIR:
			ret = chip_write(d, a, b, c);
			break;
		case OP_I2C:
			/*
			 * The replay carries the Windows driver's transmitter writes.
			 * Dropped: mstar_tx_takeover() reprograms the whole
			 * transmitter right after this sequence runs.
			 */
			if (a == HD60S_I2C_TX) {
				ret = 0;
				break;
			}
			tx[0] = b; tx[1] = c;
			ret = hd60s_i2c_write(d, a, tx, 2);
			break;
		case OP_EXT:
			tx[0] = a; tx[1] = b; tx[2] = c;
			ret = hd60s_i2c_write(d, HD60S_I2C_EXT, tx, 3);
			break;
		case OP_SUBFN:
			/* "12 34 90 <pin> <level>" is an MCU GPIO write */
			tx[0] = 0x12; tx[1] = 0x34; tx[2] = 0x90;
			tx[3] = a; tx[4] = b;
			ret = hd60s_i2c_write(d, HD60S_I2C_MCU, tx, 5);
			break;
		case OP_FMT03:
			ret = mstar_write_reg03(d, fmt);
			break;
		case OP_FMT1213:
			ret = mstar_write_reg12_13(d, fmt);
			break;
		case OP_AUDIO:
			ret = mstar_write_audio_block(d, d->audio_src);
			break;
		case OP_CSC:
			ret = mstar_write_csc(d);
			break;
		case OP_R92:
			ret = mst_write(d, 0, 0x92, mstar_reg92(d));
			break;
		default:
			ret = -EINVAL;
			break;
		}
		if (ret < 0) {
			dev_err(&d->intf->dev,
				"step %u (op %u) failed: %d\n", i, op, ret);
			return ret;
		}
	}
	return 0;
}

/*
 * Bank 0 registers 0x93-0xAA: nine coefficients then three offsets, as
 * big-endian 16-bit values written low byte first, negatives in 15-bit two's
 * complement.
 */
static int mstar_write_csc(struct hd60s_dev *d)
{
	static const u8 reg[HD60S_CSC_VALUES] = {
		0x9b, 0x95, 0xa1,	/* Y  from G, R, B */
		0x99, 0x93, 0x9f,	/* Cr from G, R, B */
		0x9d, 0x97, 0xa3,	/* Cb from G, R, B */
		0xa5, 0xa7, 0xa9,	/* offsets Cr, Y, Cb */
	};
	s16 m[HD60S_CSC_VALUES];
	unsigned int i;
	int ret;

	hd60s_csc_matrix(d->mst.bt709, d->mst.input_cs != HD60S_CS_RGB,
			 d->color_range, d->pic, m);
	/*
	 * The Y output pedestal, by the Windows driver's rule: it selects -0x1C00
	 * instead of -0x2000 exactly when the source declares limited range,
	 * which is +16 at this scale. That pairs with 0x92, which tells the
	 * chip to strip the input pedestal in the same case -- normalize the
	 * input, then put the pedestal back on the way out, so Bypass is the
	 * identity either way. It is not a term of the color-range
	 * conversion and must not be keyed to one.
	 */
	if (d->mst.input_q == 1)
		m[10] += 16 * 64;

	for (i = 0; i < HD60S_CSC_VALUES; i++) {
		u16 raw = (u16)m[i] & 0x7fff;

		ret = mst_write(d, 0, reg[i] + 1, raw & 0xff);
		if (ret < 0)
			return ret;
		ret = mst_write(d, 0, reg[i], raw >> 8);
		if (ret < 0)
			return ret;
	}
	return 0;
}

static int mstar_apply_mode(struct hd60s_dev *d, const struct hd60s_mode *m)
{
	u8 fmt = mstar_format_code(m->hactive, m->vactive, m->vfreq);
	int ret;

	dev_dbg(&d->intf->dev, "format code 0x%02x for %ux%u@%u, audio input %u\n",
		fmt, m->hactive, m->vactive, m->vfreq, d->audio_src);

	ret = mstar_run_ops(d, mstar_apply_seq, ARRAY_SIZE(mstar_apply_seq), fmt);
	if (ret < 0)
		return ret;
	/*
	 * A measured mode needs the free-measurement window left enabled: the
	 * Windows driver leaves 0xE2 at 0x80 on that path where a table mode writes it
	 * back to 0, and the replay carries the table mode's 0. Re-asserting
	 * it at the end rather than editing the replay reaches the same final
	 * state, since the Windows driver never interleaves the two.
	 */
	if (m->src == HD60S_MT_MEASURED) {
		ret = mst_write(d, 0, 0xe2, 0x80);
		if (ret < 0)
			return ret;
		mst_write(d, 0, 0xe3, 0x08);
		mst_write(d, 0, 0xe4, 0x00);
	}
	mstar_tx_takeover(d, m);
	return 0;
}

/*
 * Bank 2 registers 0x11/0x12 are a period counter against a fixed reference;
 * ref[i] * rate[i] is 2,147,520 across the whole table.
 */
static u32 mstar_audio_rate(struct hd60s_dev *d)
{
	static const u16 ref[7]  = { 6711, 4870, 4474, 2435, 2237, 1217, 1118 };
	static const u16 rate[7] = {  320,  441,  480,  882,  960, 1764, 1920 };
	int lo = mst_read(d, 2, 0x11);
	int hi = mst_read(d, 2, 0x12);
	unsigned int i;
	u32 v;

	if (lo < 0 || hi < 0)
		return 0;
	/*
	 * Bit 7 of 0x12 is not part of the count. Without the mask a set bit
	 * puts v above every reference and the rate silently reads 48 kHz.
	 */
	v = ((u32)(hi & 0x7f) << 8) | lo;
	for (i = 0; i < ARRAY_SIZE(ref); i++)
		if (v >= ref[i] - 30u && v <= ref[i] + 30u)
			return rate[i] * 100u;
	return 0;
}

/* Blank the output, as the Windows driver does on loss of sync. */
static void mstar_blank(struct hd60s_dev *d)
{
	mst_write(d, 0, 0xb3, 0xff);
	mst_write(d, 2, 0x27, 0xff);
	/* the sink sees a mute rather than the blanked chip's noise */
	mstar_tx_mute(d);

	/*
	 * The InfoFrame the receiver has latched is not cleared, because only
	 * re-running the front-end init clears it and a source that sends none
	 * cannot displace it; the Windows driver behaves the same way.
	 *
	 * The driver's own cache is dropped whole -- the programmed mode and
	 * the source's declared color space, range and VIC. Across a signal
	 * loss they describe the departed source, and a replacement that
	 * declares nothing would be converted by its predecessor's
	 * declaration.
	 */
	d->mst.applied = NULL;
	d->mst.applied_frac = false;
	d->mst.applied_cs = 0xff;	/* nothing programmed; force a re-apply */
	d->mst.applied_q = 0xff;
	d->mst.input_cs = 0xff;
	d->mst.input_q = 0xff;
	d->mst.src_vic = 0;
	d->mst.src_aspect = 0;
	d->mst.nomode_pending = false;
	d->mst.nomode_tries = 0;
	d->mst.deep_tries = 0;
	d->mst.warned_deep = false;
	/*
	 * Deliberately not reset: audio_hz (outlives a sync loss and re-seeds
	 * the transmitter), deep_zeroed (it describes the chip, not the
	 * departed signal), the tx_* sink state (mstar_tx_poll() owns it, keyed
	 * to HPD). Everything above describes the departed signal; anything
	 * added to that state belongs above too.
	 */
}

/*
 * Read the fifteen registers the chip's mode detect reads, in the order it
 * reads them. The arithmetic that turns them into a mode lives in
 * hd60s-modes.c; this half is the I2C.
 */
static int mstar_read_regs(struct hd60s_dev *d, struct hd60s_mstar_regs *r)
{
	static const struct { u8 bank, reg, off; } list[] = {
		{ 0, 0x55, offsetof(struct hd60s_mstar_regs, b0_55) },
		{ 0, 0x6a, offsetof(struct hd60s_mstar_regs, b0_6a) },
		{ 0, 0x6b, offsetof(struct hd60s_mstar_regs, b0_6b) },
		{ 0, 0x57, offsetof(struct hd60s_mstar_regs, b0_57) },
		{ 0, 0x58, offsetof(struct hd60s_mstar_regs, b0_58) },
		{ 0, 0x59, offsetof(struct hd60s_mstar_regs, b0_59) },
		{ 0, 0x5a, offsetof(struct hd60s_mstar_regs, b0_5a) },
		{ 0, 0x5b, offsetof(struct hd60s_mstar_regs, b0_5b) },
		{ 0, 0x5c, offsetof(struct hd60s_mstar_regs, b0_5c) },
		{ 0, 0x5f, offsetof(struct hd60s_mstar_regs, b0_5f) },
		{ 2, 0x29, offsetof(struct hd60s_mstar_regs, b2_29) },
		{ 2, 0x28, offsetof(struct hd60s_mstar_regs, b2_28) },
		{ 1, 0x01, offsetof(struct hd60s_mstar_regs, b1_01) },
		{ 2, 0x47, offsetof(struct hd60s_mstar_regs, b2_47) },
		{ 0, 0x5c, offsetof(struct hd60s_mstar_regs, b0_5c_again) },
	};
	unsigned int i;
	int v;

	memset(r, 0, sizeof(*r));
	for (i = 0; i < ARRAY_SIZE(list); i++) {
		/*
		 * 0x55 says whether there is a source at all. The Windows driver reads
		 * it first and returns immediately when the sync bits are
		 * clear, so the other fourteen reads never happen on an idle
		 * input, which is most of the time. Do the same.
		 */
		v = mst_read(d, list[i].bank, list[i].reg);
		if (v < 0)
			return v;
		*((u8 *)r + list[i].off) = v;
		if (i == 0 && (v & 0x3c) != 0x3c)
			return 0;
	}
	return 0;
}

/*
 * The received AVI InfoFrame, at its CEA-861 bit positions:
 *
 *   0x48 bits 6:5  Y1Y0  color space   0 RGB, 1 YCbCr 4:2:2, 2 YCbCr 4:4:4
 *   0x49 bits 7:6  C1C0  colorimetry   1 BT.601, 2 BT.709
 *   0x4A bits 3:2  Q1Q0  quantization  0 default, 1 limited, 2 full
 *
 * No YQ: the block stops being a raw dump past 0x4B, so a YCbCr source's range
 * is the CEA-861 default of limited. A zero in 0x48 is ambiguous -- RGB, or no
 * InfoFrame -- so the mode is re-applied on disagreement, not on a change.
 */

/*
 * RxHdmiPacketStatus, bank 2 registers 0x0B, 0x0C and 0x0E. All three are read
 * for the side effect as much as the value: without the reads the latched
 * InfoFrame goes stale and 0x48 reports RGB for ever after a YCbCr source is
 * plugged in. Do not optimize them away.
 *
 * Bit 3 of 0x0B says whether a frame has arrived.
 */
static bool mstar_avi_ready(struct hd60s_dev *d)
{
	int b = mst_read(d, 2, 0x0b);

	mst_read(d, 2, 0x0c);
	mst_read(d, 2, 0x0e);
	return b >= 0 && (b & 0x08);
}

/*
 * Y1Y0 as the receiver latched it, decoded the Windows driver's way: mask 0x60,
 * 0x20 to 1 and 0x40 to 2, anything else 0.
 *
 * It matters because the matrix is an RGB-to-YCbCr conversion: a YCbCr source
 * has nothing to convert and doing it anyway converts twice.
 */
static int mstar_input_cs(struct hd60s_dev *d)
{
	int v = mst_read(d, 2, 0x48);

	if (v < 0)
		return v;
	switch (v & 0x60) {
	case 0x20:
		return HD60S_CS_YCBCR_422;
	case 0x40:
		return HD60S_CS_YCBCR_444;
	default:
		return HD60S_CS_RGB;
	}
}

/*
 * Quantization range, from bank 2 register 0x4A bits 3:2 -- DB3's Q1Q0 in the
 * same received AVI InfoFrame that 0x48 carries DB1 of. The Windows driver
 * reads it in its signal monitor two instructions before the color space and
 * caches both.
 */
static int mstar_input_q(struct hd60s_dev *d)
{
	int v = mst_read(d, 2, 0x4a);

	return v < 0 ? v : (v >> 2) & 3;
}

/*
 * C1C0: 1 is BT.601, 2 BT.709. 0 is nothing said, 3 is "extended" and lives in
 * DB3's EC field, which is not decoded here.
 *
 * The Windows driver calls everything but 1 BT.709. Not copied: 0 also means no
 * InfoFrame at all, and calling an SD source BT.709 on that is worse than the
 * height convention. So the register decides when it says anything, and the
 * convention -- on the FRAME height, where m->vactive is a field -- covers the
 * two values that carry no colorimetry.
 */
static bool mstar_bt709(struct hd60s_dev *d, const struct hd60s_mode *m)
{
	int v = mst_read(d, 2, 0x49);

	if (v >= 0) {
		switch ((v >> 6) & 3) {
		case 1:
			return false;
		case 2:
			return true;
		}
	}
	return m->vactive * (m->interlaced ? 2 : 1) > 576;
}

/*
 * Bank 0 register 0x92, computed rather than replayed -- a capture shows only
 * one of the four values the Windows driver picks from the range and the input
 * color space.
 *
 * It tells the chip whether incoming chroma is already centered on 128, so it
 * and the matrix's chroma offsets are one decision: either half alone clips
 * both chroma channels.
 */
static u8 mstar_reg92(struct hd60s_dev *d)
{
	bool ycbcr = d->mst.input_cs != HD60S_CS_RGB;

	if (d->mst.input_q == 1)		/* limited */
		return ycbcr ? 0x66 : 0x55;
	return ycbcr ? 0x62 : 0x40;
}

/*
 * The fallback for a stable timing the tables do not hold: ask the chip. A free
 * measurement -- bank 0 0xE2 = 0x80, 0xE3/0xE4 = 0x08/0x00 -- after which
 * 0xF3..0xFA read back one frame later as the active region, supplying the
 * vactive the tables exist for.
 *
 * The Windows driver's checks are kept: the measured width aligned down to 8
 * must equal the hactive the normal detect measured, two independent counters
 * agreeing being what makes the window believable, and the area is bounded.
 *
 * The row lives in d->mst.measured so applied can point at it, and is rewritten
 * only for a changed signal -- the field-to-field vtotal alternation would
 * otherwise rewrite it every other poll.
 */
static const struct hd60s_mode *mstar_measure_mode(struct hd60s_dev *d,
						   const struct hd60s_detect *det)
{
	struct hd60s_mode *m = &d->mst.measured;
	int lo, hi;
	u32 hs, he, vs, ve, w, h;
	unsigned int ms;

	/* the same tolerance the keep-mode logic uses: same signal, keep */
	if (d->mst.applied == m && d->signal &&
	    det->hactive == d->mst.applied_hactive &&
	    det->interlaced == d->mst.applied_interlaced &&
	    abs((int)det->vtotal - (int)d->mst.applied_vtotal) <= 1 &&
	    abs((int)det->fv - (int)d->mst.applied_fv) <= 3)
		return m;

	/*
	 * Two consecutive NO_MODE polls with the same measurement before the
	 * chip is put into free-measurement mode: one corrupted read under a
	 * healthy stream must not reach the hardware.
	 */
	if (det->hactive != d->mst.nomode_hactive ||
	    det->interlaced != d->mst.nomode_interlaced ||
	    abs((int)det->vtotal - (int)d->mst.nomode_vtotal) > 1 ||
	    abs((int)det->fv - (int)d->mst.nomode_fv) > 3) {
		/* a different timing, and it gets its own attempts */
		d->mst.nomode_pending	 = true;
		d->mst.nomode_tries	 = 0;
		d->mst.nomode_hactive	 = det->hactive;
		d->mst.nomode_vtotal	 = det->vtotal;
		d->mst.nomode_fv	 = det->fv;
		d->mst.nomode_interlaced = det->interlaced;
		return NULL;
	}
	if (!d->mst.nomode_pending) {
		d->mst.nomode_pending = true;
		return NULL;
	}
	d->mst.nomode_pending = false;
	/*
	 * Bounded: each failure disarms the debounce, so the timing would
	 * otherwise be re-measured every second while the source stays put.
	 */
	if (d->mst.nomode_tries >= 3)
		return NULL;
	d->mst.nomode_tries++;

	if (mst_write(d, 0, 0xe2, 0x80) < 0 ||
	    mst_write(d, 0, 0xe3, 0x08) < 0 ||
	    mst_write(d, 0, 0xe4, 0x00) < 0)
		goto fail;
	/* the Windows driver reads these before waiting; kept as part of the recipe */
	mst_read(d, 0, 0x6c);
	mst_read(d, 0, 0x6d);
	/* one frame, so the window registers describe a whole frame */
	ms = clamp(10000u / det->fv + 1, 5u, 100u);
	msleep(ms);

	hi = mst_read(d, 0, 0xf4);
	lo = mst_read(d, 0, 0xf3);
	if (hi < 0 || lo < 0)
		goto fail;
	hs = ((hi & 0xf) << 8) | lo;
	hi = mst_read(d, 0, 0xf6);
	lo = mst_read(d, 0, 0xf5);
	if (hi < 0 || lo < 0)
		goto fail;
	he = ((hi & 0xf) << 8) | lo;
	hi = mst_read(d, 0, 0xf8);
	lo = mst_read(d, 0, 0xf7);
	if (hi < 0 || lo < 0)
		goto fail;
	vs = ((hi & 0xf) << 8) | lo;
	hi = mst_read(d, 0, 0xfa);
	lo = mst_read(d, 0, 0xf9);
	if (hi < 0 || lo < 0)
		goto fail;
	ve = ((hi & 0xf) << 8) | lo;

	if (he <= hs || ve <= vs)
		goto fail;
	w = (he - hs + 1) & ~7u;
	h = ve - vs + 1;
	if (w != (det->hactive & ~7u))		/* the two counters disagree */
		goto fail;
	if (h < 100 || h > det->vtotal || (u64)w * h > 2764800)
		goto fail;

	m->hactive	= det->hactive;
	m->vactive	= h;			/* FIELD height when interlaced */
	m->htotal	= det->htotal;
	m->vtotal	= det->vtotal;
	m->pixclk	= (u32)div_u64((u64)det->htotal * det->vtotal * det->fv,
				       10000);
	m->vfreq	= (det->fv + 5) / 10;
	m->interlaced	= det->interlaced;
	m->vic		= 0;
	m->src		= HD60S_MT_MEASURED;
	/*
	 * A fresh measurement rewrote the row d->mst.applied may point at, so
	 * the pointer no longer proves the chip is programmed for what the
	 * row now says. Dropping the cache is what makes the apply run: the
	 * apply decision is pointer identity, which one shared row defeats.
	 */
	d->mst.applied = NULL;
	dev_info(&d->intf->dev,
		 "no table entry for hactive %u htotal %u vtotal %u fv %u.%u; using the chip's measured window %ux%u\n",
		 det->hactive, det->htotal, det->vtotal,
		 det->fv / 10, det->fv % 10, m->hactive, m->vactive);
	return m;

fail:
	/* The window must not stay enabled behind the next matched mode. */
	mst_write(d, 0, 0xe2, 0x00);
	return NULL;
}

static const u8 mstar_gate_zeros[5] = { 0, 0, 0, 0, 0 };

/*
 * Write the five-byte block behind the bank-1 indirect port at address 0.
 *
 * Bank 1 0x25/0x26 are the 16-bit address, 0x27 the auto-incrementing data
 * window and 0x24 bit 0 the write enable. The block is read once at init and
 * is five bytes at address 0 of an HDMI receiver's indirect port, which is
 * almost certainly the BKSV; the Windows driver zeroes it while streaming, so
 * that a source sees a sink with no HDCP capability and sends unencrypted
 * video, and restores it on stop.
 *
 * The output is held and blanked across the write and released afterwards,
 * which is the Windows driver's bracketing and not optional -- the block is
 * live state and the chip must not act on it half-written.
 */
static int mstar_gate_block(struct hd60s_dev *d, const u8 *blk, bool wren)
{
	u8 saved_ab, saved_227;
	int i, ret, v;

	v = mst_read(d, 0, 0xab);
	if (v < 0)
		return v;
	saved_ab = v & 0x7f;
	v = mst_read(d, 2, 0x27);
	if (v < 0)
		return v;
	saved_227 = (u8)v;

	ret = mst_write(d, 0, 0xab, saved_ab | 0x80);		/* hold  */
	if (ret < 0)
		return ret;
	ret = mst_write(d, 2, 0x27, 0xff);			/* blank */
	if (ret < 0)
		return ret;
	ret = mst_write(d, 0, 0xb3, 0xff);
	if (ret < 0)
		return ret;
	ret = mst_write(d, 1, 0x24, wren ? 0x41 : 0x40);
	if (ret < 0)
		return ret;
	ret = mst_write(d, 1, 0x25, 0x00);
	if (ret < 0)
		return ret;
	ret = mst_write(d, 1, 0x26, 0x00);
	if (ret < 0)
		return ret;
	for (i = 0; i < 5; i++) {
		ret = mst_write(d, 1, 0x27, blk[i]);
		if (ret < 0)
			return ret;
	}
	ret = mst_write(d, 0, 0xab, saved_ab);			/* release */
	if (ret < 0)
		return ret;
	ret = mst_write(d, 2, 0x27, saved_227);
	if (ret < 0)
		return ret;
	return mst_write(d, 0, 0xb3, 0x00);
}

/*
 * Recover a receiver that is sampling a 36-bit link as 24-bit.
 *
 * Nothing in the poll clears it: the reading is self-consistent, so the chip
 * does not know it is wrong and stays wrong until the source retrains. Minutes
 * and several signal-lost transitions do not shift it.
 *
 * The stream gate does clear it, and its two directions differ only in the
 * five-byte block and the bank 2 0x0E loops, so the block is what repairs the
 * lock. This writes the zeros half and nothing else.
 *
 * The zeros are then left in place: restoring them is what the stop gate does,
 * and the mis-lock comes back across exactly that restore. While the
 * block is zero the sink presents no HDCP capability, so a protected source may
 * refuse the loop-through output -- already true whenever the device streams,
 * and now true of an idle device that has mis-locked, until the next stop.
 *
 * Bounded per signal session; the caller reports no signal either way.
 */
static void mstar_deep_recover(struct hd60s_dev *d)
{
	/*
	 * Already zero and still mis-locked: the block is not the lever here,
	 * and writing it again will not help.
	 * Cleared when the stop gate restores the block, so a later mis-lock
	 * gets a fresh attempt.
	 */
	if (d->mst.deep_zeroed || d->mst.deep_tries >= 3)
		return;
	d->mst.deep_tries++;

	if (mstar_gate_block(d, mstar_gate_zeros, true) < 0)
		return;
	d->mst.deep_zeroed = true;
	dev_info(&d->intf->dev,
		 "deep-color mis-lock: zeroed the gate block to re-lock the receiver (attempt %u); the loop-through output presents no HDCP capability until the next stream stop\n",
		 d->mst.deep_tries);
}

/*
 * Fills the same struct the rev-4 status block does, so everything downstream
 * -- format derivation, the interlace weave, the source-change contract -- is
 * shared code.
 */
static int mstar_detect(struct hd60s_dev *d, struct hd60s_timing *t, bool *signal)
{
	struct hd60s_mstar_regs regs;
	struct hd60s_detect det;
	const struct hd60s_mode *m;
	bool kept = false, avi_fresh = false, frac;
	u8 q;
	u32 audio;
	int ret, v;

	mutex_lock(&d->ctrl_lock);

	ret = mstar_read_regs(d, &regs);
	if (ret < 0)
		goto out;

	switch (hd60s_mstar_decode(&regs, &det)) {
	case HD60S_DET_NO_SYNC:
		mstar_blank(d);
		goto out;
	case HD60S_DET_UNSTABLE:
		dev_dbg(&d->intf->dev,
			"unstable: htotal %u hactive %u vtotal %u hfreq %u fv %u calc %u r55 %02x\n",
			det.htotal, det.hactive, det.vtotal, det.hfreq,
			det.fv, det.calc_fv, regs.b0_55);
		d->mst.nomode_pending = false;	/* the run has to be consecutive */
		goto out;
	case HD60S_DET_DEEP_MISLOCK:
		/*
		 * The chip is sampling a 36-bit stream as 24-bit, so its pixel
		 * data is noise and its geometry is 1.5x too wide. Report no
		 * signal: there is no mode here to program and nothing worth
		 * showing. Deliberately NOT handed to mstar_measure_mode() --
		 * the free measurement runs in the same wrong clock domain, so
		 * it corroborates the bad reading rather than checking it.
		 */
		if (!d->mst.warned_deep) {
			d->mst.warned_deep = true;
			dev_warn(&d->intf->dev,
				 "the receiver is decoding a 36-bit link as 24-bit: hactive %u htotal %u vtotal %u is 3/2 of %ux%u (b1_01 %02x b2_47 %02x)\n",
				 det.hactive, det.htotal, det.vtotal,
				 det.deep_row->hactive, det.deep_row->vactive,
				 regs.b1_01, regs.b2_47);
		}
		mstar_deep_recover(d);
		d->mst.nomode_pending = false;
		goto out;
	case HD60S_DET_NO_MODE:
		/*
		 * The chip reports no vactive of its own, so with no table
		 * row the only remaining source is the chip's free-measured
		 * window. When even that fails -- the counters disagree, or
		 * the window is implausible -- report no signal and name the
		 * measurement once.
		 */
		det.mode = mstar_measure_mode(d, &det);
		if (!det.mode) {
			/* an armed first sighting is not yet a failure */
			if (!d->mst.nomode_pending && !d->mst.warned_nomode) {
				dev_warn(&d->intf->dev,
					 "no mode-table entry for hactive %u htotal %u vtotal %u fv %u.%u %s, and the measured window failed its checks; please report this line\n",
					 det.hactive, det.htotal, det.vtotal,
					 det.fv / 10, det.fv % 10,
					 det.interlaced ? "interlaced" : "progressive");
				d->mst.warned_nomode = true;
			}
			goto out;
		}
		break;
	case HD60S_DET_OK:
		break;
	}
	d->mst.warned_nomode = false;
	d->mst.nomode_pending = false;
	d->mst.nomode_tries = 0;
	/*
	 * A decode this good means the last recovery worked, so the bound is
	 * three CONSECUTIVE failures rather than three per signal session.
	 * It exists to stop writing to a chip that will not come back, not to
	 * cap how often one that does may be brought back -- and a source
	 * driven through repeated capture cycles re-breaks on every stop gate,
	 * which would otherwise exhaust the bound while it was still working.
	 */
	d->mst.deep_tries = 0;
	d->mst.warned_deep = false;
	m = det.mode;

	/*
	 * Keep the programmed mode when this detection is the same signal. An
	 * interlaced source alternates vtotal between the two field lengths --
	 * 262 and 263 at 480i -- and the tables hold a row for each, so a fresh
	 * lookup would flip-flop and re-apply the block over a live stream.
	 *
	 * A lookup landing on the programmed row refreshes what the tolerance
	 * is taken against, which it must: the tables match hactive within 2
	 * and the rate within 1.5 Hz, both wider than the tolerance below.
	 *
	 * That tolerance is one line and 0.3 Hz -- inside the gap between any
	 * two real modes, and wide enough for the field-to-field variation.
	 */
	if (d->mst.applied && d->signal) {
		if (m == d->mst.applied) {
			d->mst.applied_hactive    = det.hactive;
			d->mst.applied_vtotal     = det.vtotal;
			d->mst.applied_fv         = det.fv;
			d->mst.applied_interlaced = det.interlaced;
			kept = true;
		} else if (det.hactive == d->mst.applied_hactive &&
			   det.interlaced == d->mst.applied_interlaced &&
			   abs((int)det.vtotal - (int)d->mst.applied_vtotal) <= 1 &&
			   abs((int)det.fv - (int)d->mst.applied_fv) <= 3) {
			m = d->mst.applied;
			kept = true;
		}
	}

	t->hactive  = m->hactive;
	t->vactive  = m->vactive;		/* FIELD height when interlaced */
	/*
	 * The pixel-repeated SD rows carry htotal in 13.5 MHz units -- 858
	 * against hactive 1440 -- so report it in the same units as hactive.
	 */
	t->htotal   = m->htotal < m->hactive ? m->htotal * 2 : m->htotal;
	t->vtotal   = m->vtotal;
	t->fps      = m->vfreq;

	t->modeflag = 0;
	/*
	 * From the measured scan bit, not the table's flag: the tables mark the
	 * pixel-doubled SD rows progressive and the chip disagrees. The chip
	 * is right.
	 */
	if (det.interlaced)
		t->modeflag |= HD60S_MF_INTERLACED;
	/*
	 * 1000/1001, from the RAW period counter rather than the truncated
	 * fv, which cannot separate 59.94 from 60.00 -- the counter itself
	 * can, and hd60s_vper_fractional() carries the Windows driver's thresholds.
	 *
	 * Classified once per mode change, as the Windows driver does: while the
	 * keep-mode logic holds the signal is the same signal, and
	 * re-classifying it from a counter that wobbles by a count near the
	 * threshold would flap the reported rate poll to poll.
	 */
	frac = kept ? d->mst.applied_frac
		    : hd60s_vper_fractional(det.vper, det.fv);
	if (frac)
		t->modeflag |= HD60S_MF_FRACTIONAL;
	/*
	 * Both the matrix this programs and the colorspace reported to V4L2
	 * come from one answer, so the two cannot disagree.
	 */
	d->mst.bt709 = mstar_bt709(d, m);
	t->modeflag |= FIELD_PREP(HD60S_MF_COLORIMETRY, d->mst.bt709 ? 1 : 2);
	/*
	 * HDCP: bank 1 register 0x01 bit 0 or register 0x34 bit 7, the two the
	 * Windows driver ORs into its own flag. Reported only -- nothing on the
	 * capture path acts on it, and the Windows driver's one consumer is its
	 * transmitter's downstream authentication.
	 */
	v = mst_read(d, 1, 0x34);
	d->mst.hdcp = (regs.b1_01 & 0x01) || (v >= 0 && (v & 0x80));
	if (d->mst.hdcp)
		t->modeflag |= HD60S_MF_HDCP;
	/*
	 * Polled rather than read at bring-up: bank 2 0x48 is unpopulated for
	 * about the first detect, and 0 there reads as a real RGB answer. The
	 * Windows driver polls it the same way.
	 */
	/*
	 * Read the packet status first: it re-arms the capture, and its bit 3
	 * says whether 0x48 and 0x4A hold a frame that actually arrived. If not,
	 * keep what was last believed -- 0 there is an ordinary answer (RGB, no
	 * range stated) and so indistinguishable from no answer at all.
	 */
	if (!mstar_avi_ready(d)) {
		/*
		 * Nothing has arrived yet. On the very first detect fall back
		 * to what CEA-861 says to assume without an InfoFrame -- RGB,
		 * with no range stated -- rather than leaving the sentinel in
		 * place, where it would read as "not RGB" and take the
		 * pass-through path.
		 */
		if (d->mst.input_cs == 0xff) {
			d->mst.input_cs = HD60S_CS_RGB;
			d->mst.input_q = 0;
			dev_info(&d->intf->dev,
				 "source: no InfoFrame, assuming RGB\n");
		}
		goto avi_done;
	}

	ret = mstar_input_q(d);
	if (ret < 0)
		goto out;
	q = ret;

	ret = mstar_input_cs(d);
	if (ret < 0)
		goto out;
	if ((u8)ret != d->mst.input_cs || q != d->mst.input_q) {
		static const char * const cs_name[] = {
			"RGB", "YCbCr 4:2:2", "YCbCr 4:4:4"
		};
		static const char * const q_name[] = {
			"unstated", "limited", "full", "reserved"
		};

		/*
		 * Both come from the received AVI InfoFrame and both change
		 * what the color-space matrix has to do, so report them
		 * together.  "unstated" is the common case -- plenty of
		 * sources send no InfoFrame at all, and then a full-range one
		 * is indistinguishable from a limited one until userspace says
		 * which it is with V4L2_CID_DV_RX_RGB_RANGE.
		 */
		dev_info(&d->intf->dev, "source: %s, %s range\n",
			 cs_name[ret], q_name[q]);
		d->mst.input_cs = ret;
	}
	/*
	 * Q1Q0 describes an RGB source and the block carries no YQ, so a YCbCr
	 * source is taken as limited. V4L2_CID_DV_RX_RGB_RANGE overrides that
	 * for a source which is not, the case the control exists for.
	 */
	d->mst.input_q = q;
	/*
	 * The frame's own VIC (DB4, bank 2 0x4B) and picture aspect (DB2 bits
	 * 5:4, bank 2 0x49), cached for re-emission on the loop-through
	 * output: the source knows its 4:3-against-16:9 SD variants where the
	 * mode table stores only one code.
	 */
	v = mst_read(d, 2, 0x4b);
	if (v >= 0)
		d->mst.src_vic = v & 0x7f;
	v = mst_read(d, 2, 0x49);
	if (v >= 0)
		d->mst.src_aspect = (v >> 4) & 3;
	avi_fresh = true;
avi_done:
	d->input_full = d->mst.input_cs == HD60S_CS_RGB && d->mst.input_q == 2;
	ret = 0;

	audio = mstar_audio_rate(d);
	if (audio) {
		if (audio != d->mst.audio_hz)
			dev_info(&d->intf->dev, "source audio %u Hz%s\n",
				 audio, audio == 48000 ?
				 "" : " (delivered resampled to 48 kHz)");
		d->mst.audio_hz = audio;
	}
	t->audio_khz = d->mst.audio_hz ? d->mst.audio_hz / 1000 : 48;

	/*
	 * Only touch the chip when the mode actually changed. This compares
	 * the chosen table row against the one that is programmed, not the row
	 * against the raw timing: the raw vtotal alternates by a line between
	 * fields and would never settle.
	 */
	/*
	 * Level-triggered on the color space, deliberately unlike the Windows driver,
	 * which compares against its cache and acts on the edge. It can do
	 * that because it caches and acts in one path; here the cache is
	 * updated above and the apply can still fail below, which would spend
	 * the edge and leave the matrix programmed for the wrong color space
	 * for as long as the source stays put. Comparing against what was
	 * actually programmed cannot desynchronize that way.
	 */
	if (m != d->mst.applied || !d->signal ||
	    d->mst.input_cs != d->mst.applied_cs ||
	    d->mst.input_q != d->mst.applied_q ||
	    d->mst.bt709 != d->mst.applied_bt709) {
		/*
		 * The mode changed but this poll carried no fresh InfoFrame:
		 * whatever VIC is cached describes the mode that just left,
		 * and re-emitting it would tell the sink the wrong raster for
		 * the whole session. The table's VIC serves until the new
		 * source's frame arrives.
		 */
		if (m != d->mst.applied && !avi_fresh) {
			d->mst.src_vic = 0;
			d->mst.src_aspect = 0;
		}
		ret = mstar_apply_mode(d, m);
		if (ret < 0)
			goto out;
		d->mst.applied           = m;
		d->mst.applied_frac      = frac;
		d->mst.applied_cs        = d->mst.input_cs;
		d->mst.applied_q         = d->mst.input_q;
		d->mst.applied_bt709     = d->mst.bt709;
		d->mst.applied_hactive   = det.hactive;
		d->mst.applied_vtotal    = det.vtotal;
		d->mst.applied_fv        = det.fv;
		d->mst.applied_interlaced = det.interlaced;
		dev_dbg(&d->intf->dev, "applied %ux%u (%s, %u kHz)\n",
			m->hactive, m->vactive,
			m->src == HD60S_MT_EIA ? "EIA table" :
			m->src == HD60S_MT_VESA ? "VESA table" : "measured",
			m->pixclk);
	}

	/*
	 * The transmitter's keeper: canary, hot-plug, and the audio path --
	 * the audio level-triggered on a flag every path that disturbs bank 2
	 * sets, so anything that gets it out of step heals on the next poll.
	 * Nothing touches the chip until the MCU has finished bringing it up;
	 * mstar_tx_ready() asks it rather than waiting a fixed time.
	 */
	mstar_tx_poll(d);

	*signal = true;
out:
	mutex_unlock(&d->ctrl_lock);
	return ret;
}

/*
 * Stream gate. Start and stop are the same sequence bar two things: bank 1
 * register 0x24 gets 0x41 (write enable) rather than 0x40, and the five bytes
 * pushed through the indirect port are zeros rather than the saved originals.
 *
 * Only start runs the poll loops: [A] waits up to 50 ms for the low three bits
 * of bank 2 register 0x0E to come up, then [B] writes that value back --
 * write-1-to-clear -- and waits up to 2.5 s for them to clear.
 */

static int mstar_stream(struct hd60s_dev *d, bool on)
{
	const u8 *blk = on ? mstar_gate_zeros : d->mst.blk;
	int i, ret, v;

	/*
	 * The gate disturbs what the transmitter is fed, so the sink is muted
	 * across it -- the Windows driver does the same around both directions -- and
	 * the detect poll re-establishes the transmitter afterwards.
	 */
	mstar_tx_mute(d);

	ret = mstar_gate_block(d, blk, on);
	if (ret < 0)
		return ret;

	if (!on) {
		d->mst.deep_zeroed = false;	/* the block is the saved one again */
		d->mst.tx_audio_dirty = true;
		return 0;
	}

	for (i = 0; i < 10; i++) {		/* loop [A] */
		v = mst_read(d, 2, 0x0e);
		if (v < 0)
			return v;
		if (v & 7)
			break;
		usleep_range(5000, 6000);
	}
	dev_dbg(&d->intf->dev, "stream start: loop A took %d iterations\n", i);

	for (i = 0; i < 500; i++) {		/* loop [B] */
		v = mst_read(d, 2, 0x0e);
		if (v < 0)
			return v;
		ret = mst_write(d, 2, 0x0e, (u8)v);	/* write-1-to-clear */
		if (ret < 0)
			return ret;
		if (!(v & 7))
			break;
		usleep_range(5000, 6000);
	}
	dev_dbg(&d->intf->dev, "stream start: loop B took %d iterations\n", i);

	/*
	 * The gate works in bank 2, where the audio the transmitter is fed comes
	 * from, so its channel status has to be rewritten. Marked rather than
	 * done here: the MStar's audio takes a moment to resume, and programming
	 * the transmitter first describes a stream that is not flowing. The
	 * detect poll picks it up.
	 */
	d->mst.tx_audio_dirty = true;
	return 0;
}

/* wIndex 0x0100, the enable bit low. */
static int mstar_disarm_events(struct hd60s_dev *d)
{
	return hd60s_vout_nolock(d, HD60S_REQ_C6, 0x0000, 0x0100, NULL, 0);
}

/*
 * Audio input select. Source 0 is embedded HDMI, source 1 the analog jack. The
 * last step is the board control device's register 0x20 bit 4, cleared for 0
 * and set for 1, but the codec, its power control and two MCU GPIOs all move
 * with it and the bit alone leaves the codec set up for the other source.
 */
static int mstar_set_audio(struct hd60s_dev *d, u8 src)
{
	int ret, v;

	if (src > 1)
		return -EINVAL;

	mutex_lock(&d->ctrl_lock);
	ret = mstar_write_audio_block(d, src);
	if (ret < 0)
		goto out;

	v = chip_read(d, HD60S_I2C_CTL, 0x20);
	if (v < 0) {
		ret = v;
		goto out;
	}
	dev_dbg(&d->intf->dev, "audio input %u: 0x98:0x20 %02x -> %02x\n",
		src, v, src ? (v | 0x10) : (v & ~0x10));
	ret = chip_write(d, HD60S_I2C_CTL, 0x20,
			 (u8)(src ? (v | 0x10) : (v & ~0x10)));
	if (ret < 0)
		goto out;
	d->mst.tx_audio_dirty = true;
out:
	mutex_unlock(&d->ctrl_lock);
	return ret;
}

/*
 * Picture controls are the color-space matrix: the Windows driver multiplies
 * the colorimetry rows by them before converting to Q12, so contrast is a gain
 * on the luma row, saturation a gain on the two chroma rows, brightness the Y
 * output offset and hue a rotation of the chroma rows into each other.
 */
static int mstar_set_picture(struct hd60s_dev *d)
{
	int ret;

	mutex_lock(&d->ctrl_lock);
	ret = mstar_write_csc(d);
	mutex_unlock(&d->ctrl_lock);
	return ret;
}

/*
 * Color range. The same Bypass / Shrink / Expand rev 4 exposes, but with no
 * register of its own: the conversion is folded into the Q12 scale the matrix
 * is converted with, selecting 256, 220 or 297 from a code that lines up with
 * HD60S_CR_*. Applying it is just rewriting the matrix.
 */
static int mstar_set_color_range(struct hd60s_dev *d, u8 cr)
{
	int ret;

	if (cr > HD60S_CR_EXPAND)
		return -EINVAL;

	mutex_lock(&d->ctrl_lock);
	ret = mstar_write_csc(d);
	mutex_unlock(&d->ctrl_lock);
	return ret;
}

const struct hd60s_ops hd60s_ops_mstar = {
	.name		= "revs 1-3 (MStar MST3367)",
	.mcu_after_config = true,
	.init		= mstar_init,
	.detect		= mstar_detect,
	.stream		= mstar_stream,
	.disarm_events	= mstar_disarm_events,
	.pic_controls	= HD60S_PIC_ALL,
	.set_picture	= mstar_set_picture,
	.set_audio	= mstar_set_audio,
	.set_color_range = mstar_set_color_range,
};
