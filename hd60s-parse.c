// SPDX-License-Identifier: GPL-2.0
/*
 * Elgato Game Capture HD60 S - EP 0x83 stream parser
 *
 * Copyright (c) 2026 Doug Brown <doug@schmorgal.com>
 *
 * The stream is a sequence of 32-bit-aligned records:
 *
 *   FF 00 00 XY   video line;  XY is a BT.656 timing reference code and
 *                 exactly `hactive*2` bytes of YUY2 follow - for blanking
 *                 lines too, where the payload is consumed and discarded
 *                 rather than skipped.
 *   FF 00 FF nn   audio escape; nn DWORDs of s16 stereo PCM follow.
 *
 * In XY: bit7 = 1, bit6 = F (field), bit5 = V (blanking), bit4 = H (0 = SAV).
 * Only SAV is emitted, so the four values seen on the wire are 0x80/0xAB for
 * field 1 and 0xC7/0xEC for field 2. The low nibble is BT.656 parity over
 * F/V/H, which makes every marker self-checking.
 *
 * Records straddle USB packet boundaries freely, so this is a resumable state
 * machine rather than a scan over a reassembled buffer: nothing is copied
 * twice, and a 40 KB isochronous packet costs one memcpy per line it touches.
 */
#include "hd60s-parse.h"

/* BT.656 status word: P3 = V^H, P2 = F^H, P1 = F^V, P0 = F^V^H. */
bool hd60s_trc_ok(u8 xy)
{
	u8 F, V, H;

	if (!(xy & 0x80))
		return false;
	F = (xy >> 6) & 1;
	V = (xy >> 5) & 1;
	H = (xy >> 4) & 1;
	return ((xy >> 3) & 1) == (V ^ H) && ((xy >> 2) & 1) == (F ^ H) &&
	       ((xy >> 1) & 1) == (F ^ V) && (xy & 1) == (F ^ V ^ H);
}

/* YUY2 black is 0x10 0x80 0x10 0x80, not zero -- zero is bright green. */
static void hd60s_fill_black(u8 *fb, u32 size)
{
	memset32((u32 *)fb, 0x80108010, size / 4);
}

static void hd60s_marker(struct hd60s_parser *p, u8 xy)
{
	bool blank = xy & 0x20;			/* V */
	u8 F = (xy >> 6) & 1;

	if (!hd60s_trc_ok(xy))
		p->parity_err++;

	if (!blank && p->in_blank) {		/* entering the active region */
		p->in_blank = false;
		p->field = F;

		if (!p->interlaced || F == 0) {
			if (p->started)
				hd60s_p_abort_frame(p);
			if (!p->cur)
				hd60s_p_get_buffer(p);
			/*
			 * A short field would leave the other field's stale
			 * lines interleaved. Only after an incomplete frame:
			 * a full one overwrote every line, and a 1.4 MB memset
			 * per frame is 41 MB/s inside a completion handler.
			 */
			if (p->fb && p->interlaced && p->needs_clear) {
				hd60s_fill_black(p->fb, p->frame_size);
				p->needs_clear = false;
			}
			p->lines = 0;
			p->started = true;
		}
		/* F=0 fills the even lines from 0, F=1 the odd lines from 1 */
		p->dst = (p->interlaced && F) ? p->line_bytes : 0;
	} else if (blank && !p->in_blank) {	/* leaving the active region */
		p->in_blank = true;
		if (p->started && p->lines == p->frame_h)
			hd60s_p_finish_buffer(p);
		else if (p->started && (!p->interlaced || F == 1))
			hd60s_p_abort_frame(p);
	}
}

void hd60s_parse(struct hd60s_parser *p, const u8 *data, u32 len)
{
	u32 n;

	p->bytes += len;

	/*
	 * The source changed resolution, so the stride snapshotted at
	 * start_streaming is wrong: no frame boundaries would be found and the
	 * audio would be worse than none. Stop until the queue is restarted.
	 */
	if (p->stale)
		return;

	while (len) {
		switch (p->state) {
		case HD60S_PS_HDR:
			n = min_t(u32, 4 - p->hdrlen, len);
			memcpy(p->hdr + p->hdrlen, data, n);
			p->hdrlen += n;
			data += n;
			len -= n;
			if (p->hdrlen < 4)
				return;
			p->hdrlen = 0;

			if (p->hdr[0] != 0xff || p->hdr[1] != 0x00) {
				/*
				 * Records are a whole number of dwords, so
				 * resync by discarding these four bytes, never
				 * by scanning byte-wise.
				 */
				if (p->locked && p->nbadhdr < 4)
					memcpy(p->badhdr[p->nbadhdr++],
					       p->hdr, 4);
				p->resyncs++;
				break;
			}
			if (p->hdr[2] == 0xff) {		/* audio escape */
				if (!p->locked) {
					p->resyncs_at_lock = p->resyncs;
					p->locked = true;
				}
				p->remain = (u32)p->hdr[3] * 4;
				p->state = HD60S_PS_AUDIO;
			} else if (p->hdr[2] == 0x00) {		/* line marker */
				if (!p->locked) {
					p->resyncs_at_lock = p->resyncs;
					p->locked = true;
				}
				hd60s_marker(p, p->hdr[3]);
				p->remain = p->line_bytes;
				p->line_off = 0;
				p->state = (p->hdr[3] & 0x20) ?
					   HD60S_PS_SKIP : HD60S_PS_LINE;
			} else {
				p->resyncs++;
				break;
			}
			if (!p->remain)
				p->state = HD60S_PS_HDR;
			break;

		case HD60S_PS_LINE:
			n = min(p->remain, len);
			if (p->fb && p->dst + p->line_off + n <= p->frame_size)
				memcpy(p->fb + p->dst + p->line_off, data, n);
			p->line_off += n;
			p->remain -= n;
			data += n;
			len -= n;
			if (!p->remain) {
				p->dst += p->stride;
				p->lines++;
				p->state = HD60S_PS_HDR;
			}
			break;

		case HD60S_PS_AUDIO:
			n = min(p->remain, len);
			hd60s_p_audio(p, data, n);
			p->audio_bytes += n;
			p->remain -= n;
			data += n;
			len -= n;
			if (!p->remain)
				p->state = HD60S_PS_HDR;
			break;

		case HD60S_PS_SKIP:
			n = min(p->remain, len);
			p->remain -= n;
			data += n;
			len -= n;
			if (!p->remain)
				p->state = HD60S_PS_HDR;
			break;
		}
	}
}
