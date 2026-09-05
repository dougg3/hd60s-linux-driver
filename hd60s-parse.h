/* SPDX-License-Identifier: GPL-2.0 */
/*
 * EP 0x83 stream parser for the Elgato HD60 S
 *
 * Copyright (c) 2026 Doug Brown <doug@schmorgal.com>
 *
 * The frame-buffer layer supplies the four hd60s_p_* callbacks below.
 */
#ifndef _HD60S_PARSE_H_
#define _HD60S_PARSE_H_

#include <linux/types.h>
#include <linux/string.h>
#include <linux/minmax.h>

struct hd60s_buffer;

enum hd60s_pstate {
	HD60S_PS_HDR,		/* collecting a 4-byte record header        */
	HD60S_PS_LINE,		/* copying a video line payload             */
	HD60S_PS_AUDIO,		/* copying an audio payload                 */
	HD60S_PS_SKIP,		/* consuming a blanking line's payload      */
};

struct hd60s_parser {
	enum hd60s_pstate state;
	u8	hdr[4];
	u8	hdrlen;

	u32	remain;		/* payload bytes still to consume            */
	u32	line_off;	/* bytes of the current line already stored  */
	u32	dst;		/* byte offset of the current line in frame  */

	bool	in_blank;
	bool	started;	/* a frame buffer was opened at a field start */
	/* the buffer may hold lines the next frame will not overwrite */
	bool	needs_clear;
	/* source geometry changed: the stride snapshot is wrong, stop */
	bool	stale;
	u8	field;		/* field of the last active start */

	/* geometry snapshot, taken once at start_streaming */
	u32	line_bytes;	/* hactive * 2, the YUY2 payload of one line */
	u32	stride;		/* line_bytes, doubled when weaving fields   */
	u32	frame_size;
	u32	frame_h;
	bool	interlaced;

	u8	*fb;		/* vaddr of the buffer being filled, or NULL */
	struct hd60s_buffer *cur;
	u32	lines;
	u32	sequence;

	/* diagnostics */
	u64	bytes;
	u32	frames, dropped, partial, resyncs, parity_err;
	/* resyncs before the first valid header; the rest are real faults */
	u32	resyncs_at_lock;
	bool	locked;		/* a valid header has been found at least once */
	/* the first few bad header dwords after lock-on, for diagnosis */
	u8	badhdr[4][4];
	u8	nbadhdr;
	u32	audio_bytes;
	/* interlaced frames aborted by a field-0 restart; one field per frame */
	u32	restart_f0;
	bool	f0_warned;
};

/* supplied by the frame-buffer layer */
void hd60s_p_get_buffer(struct hd60s_parser *p);
void hd60s_p_finish_buffer(struct hd60s_parser *p);
void hd60s_p_abort_frame(struct hd60s_parser *p);
void hd60s_p_audio(struct hd60s_parser *p, const u8 *data, u32 len);

bool hd60s_trc_ok(u8 xy);
void hd60s_parse(struct hd60s_parser *p, const u8 *data, u32 len);

#endif /* _HD60S_PARSE_H_ */
