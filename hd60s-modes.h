/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Mode decision for the MStar front end (revisions 1-3)
 *
 * Copyright (c) 2026 Doug Brown <doug@schmorgal.com>
 *
 * hd60s-mstar.c does the I2C; this does the arithmetic.
 *
 * The detect registers give htotal, vtotal, hactive and the interlace bit, but
 * no vactive, which is why a 376-row table is here at all. The chip will
 * measure its own active window on request, but that costs a frame, disagrees
 * with itself when the receiver is mis-locked, and yields no CEA VIC, so the
 * Windows driver keeps it for timings the table misses and so does this
 * (mstar_measure_mode(), HD60S_MT_MEASURED).
 *
 * The kernel's own matchers cannot stand in: v4l2_find_dv_timings_cap()
 * matches on height -- the answer being looked for -- and on porches and sync
 * widths this front end does not measure, while v4l2_detect_cvt() and _gtf()
 * need the vertical sync width and polarities, which the chip cannot report.
 */
#ifndef _HD60S_MODES_H_
#define _HD60S_MODES_H_

#include <linux/types.h>

#define HD60S_MT_EIA	0	/* CEA-861 timings   */
#define HD60S_MT_VESA	1	/* VESA/DMT timings  */
#define HD60S_MT_MEASURED 2	/* not a table row: the chip's own window */

struct hd60s_mode {
	u16	hactive, vactive;	/* vactive is the FIELD height when il */
	u16	htotal, vtotal;
	u32	pixclk;			/* kHz                                */
	u8	vfreq;			/* whole Hz, as the table stores it   */
	u8	interlaced;
	/* CEA-861 code for the AVI InfoFrame, 0 = none */
	u8	vic;
	u8	src;			/* HD60S_MT_*                         */
};

/* The registers the chip's mode detect reads, in the order it reads them. */
struct hd60s_mstar_regs {
	u8	b0_55;		/* sync status                        */
	u8	b0_6a, b0_6b;	/* htotal, in TMDS clocks             */
	u8	b0_57, b0_58;	/* horizontal period counter          */
	u8	b0_59, b0_5a;	/* vertical period counter            */
	u8	b0_5b, b0_5c;	/* vtotal                             */
	u8	b0_5f;		/* bit 3 = interlaced                 */
	u8	b2_29, b2_28;	/* hactive                            */
	u8	b1_01;		/* bit 2 = deep color valid           */
	u8	b2_47;		/* color depth, low nibble            */
	u8	b0_5c_again;	/* re-read of 0x5C, for the stability test */
};

enum hd60s_detect_result {
	HD60S_DET_OK,
	HD60S_DET_NO_SYNC,	/* no source; blank the output       */
	HD60S_DET_UNSTABLE,	/* measurement did not settle        */
	HD60S_DET_NO_MODE,	/* stable, but no table entry matched */
	HD60S_DET_DEEP_MISLOCK,	/* 36-bit link decoded as 24-bit     */
};

struct hd60s_detect {
	u16	htotal, vtotal, hactive;
	u16	hfreq;		/* units of 100 Hz  */
	u16	fv;		/* units of 0.1 Hz  */
	u16	calc_fv;	/* hfreq * 1000 / vtotal, for the log */
	u16	vper;		/* the raw counter fv is derived from */
	bool	interlaced;
	const struct hd60s_mode *mode;	/* set only on HD60S_DET_OK */
	/*
	 * DEEP_MISLOCK only: the row the reading becomes once the 36-bit
	 * scaling is applied. Evidence for the diagnosis, not a mode to
	 * program -- the chip's pixel data is noise.
	 */
	const struct hd60s_mode *deep_row;
};

enum hd60s_detect_result hd60s_mstar_decode(const struct hd60s_mstar_regs *r,
					    struct hd60s_detect *out);

const struct hd60s_mode *hd60s_find_mode(u16 hactive, u16 htotal, u16 vtotal,
					 u16 vfreq_dhz, bool interlaced);

bool hd60s_vper_fractional(u16 vper, u16 fv);

#define HD60S_CSC_VALUES	12

/* Color-range conversion; rev 4 writes the same values to a register. */
#define HD60S_CR_BYPASS		0	/* no conversion            */
#define HD60S_CR_SHRINK		1	/* 0-255  -> 16-235         */
#define HD60S_CR_EXPAND		2	/* 16-235 -> 0-255          */

/*
 * Twelve Q12 values for bank 0 registers 0x93-0xAA: the Y, Cr and Cb rows,
 * each as (G, R, B), then the three output offsets.
 *
 * pic[] is d->pic[], all 128 for neutral: brightness is the Y output offset,
 * contrast a gain on the luma row, saturation a gain on the chroma rows and
 * hue a rotation of the chroma rows into each other. color_range is
 * HD60S_CR_*, folded into the Q12 scale (hd60s_csc_num[] in hd60s-modes.c).
 *
 * passthrough says the source already sends YCbCr, so the matrix becomes the
 * identity with the picture controls still applied; converting anyway converts
 * twice and looks like it. It does not affect the output offsets -- whether
 * the incoming chroma is centered on 128 or 0 is bank 0 0x92, see
 * mstar_reg92().
 */
void hd60s_csc_matrix(bool bt709, bool passthrough, u8 color_range,
		      const u8 pic[4], s16 out[HD60S_CSC_VALUES]);

#endif /* _HD60S_MODES_H_ */
