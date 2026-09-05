// SPDX-License-Identifier: GPL-2.0
/*
 * Elgato Game Capture HD60 S - V4L2 capture
 *
 * Copyright (c) 2026 Doug Brown <doug@schmorgal.com>
 *
 * The EP 0x83 record format is described in hd60s-parse.c; this file owns the
 * URB ring, the videobuf2 queue and the ioctl surface.
 */
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <media/v4l2-dv-timings.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-vmalloc.h>

#include "hd60s.h"

/*
 * V4L2 has no concept of USB transport, so a module parameter is the only place
 * to choose one. Isochronous unless this is set, because it reserves bandwidth
 * and is what the Windows driver picks, falling back to bulk only where no alt
 * setting can carry the mode.
 *
 * The catch is the host: some xHCI controllers cannot deliver SuperSpeed
 * isochronous with mult > 1 and complete no URBs at all, and need this.
 */
static bool hd60s_force_bulk;
module_param_named(force_bulk, hd60s_force_bulk, bool, 0644);
MODULE_PARM_DESC(force_bulk,
		 "always use bulk for frame data instead of isochronous (default: off)");

/*
 * The parser's back end, in URB-completion context. Parser state is touched
 * only from there and USB completes one endpoint's URBs in order, so it needs
 * no lock; the buffer list it draws from takes qlock.
 */

static struct hd60s_dev *parser_dev(struct hd60s_parser *p)
{
	return container_of(p, struct hd60s_dev, p);
}

void hd60s_p_get_buffer(struct hd60s_parser *p)
{
	struct hd60s_dev *d = parser_dev(p);
	unsigned long flags;

	spin_lock_irqsave(&d->qlock, flags);
	if (!list_empty(&d->bufs)) {
		p->cur = list_first_entry(&d->bufs, struct hd60s_buffer, list);
		list_del(&p->cur->list);
	}
	spin_unlock_irqrestore(&d->qlock, flags);

	if (p->cur)
		p->fb = vb2_plane_vaddr(&p->cur->vb.vb2_buf, 0);
	else
		p->dropped++;
}

void hd60s_p_finish_buffer(struct hd60s_parser *p)
{
	struct hd60s_buffer *b = p->cur;

	p->cur = NULL;
	p->fb = NULL;
	p->started = false;
	p->restart_f0 = 0;	/* a whole frame proves the fields alternate */
	if (!b)
		return;			/* complete, but nowhere to put it */
	p->frames++;

	b->vb.vb2_buf.timestamp = ktime_get_ns();
	b->vb.sequence = p->sequence++;
	b->vb.field = p->interlaced ? V4L2_FIELD_INTERLACED : V4L2_FIELD_NONE;
	vb2_set_plane_payload(&b->vb.vb2_buf, 0, p->frame_size);
	vb2_buffer_done(&b->vb.vb2_buf, VB2_BUF_STATE_DONE);
}

/*
 * A frame that ended short is not handed to userspace and not thrown away
 * either: the buffer is kept and refilled. Delivering a resync to the
 * application as a stream of ERROR buffers would be worse than a dropped
 * frame.
 */
void hd60s_p_abort_frame(struct hd60s_parser *p)
{
	p->partial++;
	p->lines = 0;
	p->started = false;
	p->needs_clear = true;		/* lines are missing; clear before reuse */

	/*
	 * Restarting only on field 0 means one field per frame: genuine
	 * 240p/288p, which the status block cannot tell from pixel-doubled
	 * 480i/576i and which a two-field weave can never complete.
	 */
	if (p->interlaced && p->field == 0 && ++p->restart_f0 >= 60 &&
	    !p->f0_warned) {
		p->f0_warned = true;
		dev_warn(&parser_dev(p)->intf->dev,
			 "every frame aborts at a field-0 restart; the source may be genuine 240p/288p, which this device reports as interlaced and cannot weave\n");
	}
}

void hd60s_p_audio(struct hd60s_parser *p, const u8 *data, u32 len)
{
	hd60s_audio_write(parser_dev(p), data, len);
}

/*
 * An URB that fails to go back on the ring is gone from it permanently, and
 * with it a sixteenth of the queue depth. A starved isochronous endpoint
 * fills the FX3's DMA buffers, which blocks the thread that also services
 * vendor requests, so the loss is counted rather than ignored.
 */
static void hd60s_resubmit(struct hd60s_dev *d, struct urb *urb)
{
	int ret, n;

	if (!READ_ONCE(d->streaming)) {
		atomic_dec(&d->inflight);
		return;
	}
	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret < 0) {
		n = atomic_dec_return(&d->inflight);
		d->resubmit_err++;
		if (d->resubmit_err <= 3)
			dev_err(&d->intf->dev,
				"URB resubmit failed: %d (%d still in flight)\n",
				ret, n);
	}
}

static void hd60s_iso_complete(struct urb *urb)
{
	struct hd60s_dev *d = urb->context;
	int i;

	switch (urb->status) {
	case 0:
	case -EXDEV:			/* partial isochronous transfer */
		break;
	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
		atomic_dec(&d->inflight);	/* canceled: off the ring */
		return;
	default:
		dev_warn_ratelimited(&d->intf->dev, "iso URB status %d\n",
				     urb->status);
		break;
	}

	/*
	 * A packet the host controller could not deliver is a hole in a byte
	 * stream whose records straddle packet boundaries, so it costs the
	 * frame it lands in. Nothing can recover it, but counting it is what
	 * separates USB-layer loss from a parser fault.
	 */
	for (i = 0; i < urb->number_of_packets; i++) {
		struct usb_iso_packet_descriptor *pk = &urb->iso_frame_desc[i];

		d->iso_total++;
		if (pk->status < 0) {
			d->iso_err++;
			dev_warn_ratelimited(&d->intf->dev,
					     "iso packet status %d\n",
					     pk->status);
			continue;
		}
		if (!pk->actual_length)
			continue;
		hd60s_parse(&d->p, urb->transfer_buffer + pk->offset,
			    pk->actual_length);
	}

	hd60s_resubmit(d, urb);
}

static void hd60s_bulk_complete(struct urb *urb)
{
	struct hd60s_dev *d = urb->context;

	d->bulk_urbs++;

	switch (urb->status) {
	case 0:
		hd60s_parse(&d->p, urb->transfer_buffer, urb->actual_length);
		break;
	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
		atomic_dec(&d->inflight);	/* canceled: off the ring */
		return;
	default:
		/*
		 * Counted, not parsed. Whether a failed bulk URB's payload is
		 * usable depends on the status, and which ones occur here has
		 * not been observed -- that is what these counters are for.
		 */
		d->bulk_err++;
		d->bulk_lost += urb->actual_length;
		d->bulk_status = urb->status;
		dev_warn_ratelimited(&d->intf->dev, "bulk URB status %d\n",
				     urb->status);
		break;
	}

	hd60s_resubmit(d, urb);
}

static void hd60s_kill_urbs(struct hd60s_dev *d)
{
	int i;

	for (i = 0; i < HD60S_NUM_URBS; i++)
		if (d->urbs[i])
			usb_kill_urb(d->urbs[i]);
}

static void hd60s_free_urbs(struct hd60s_dev *d)
{
	int i;

	for (i = 0; i < HD60S_NUM_URBS; i++) {
		struct urb *u = d->urbs[i];

		if (!u)
			continue;
		usb_free_coherent(d->udev, d->urb_size, u->transfer_buffer,
				  u->transfer_dma);
		usb_free_urb(u);
		d->urbs[i] = NULL;
	}
}

static int hd60s_alloc_urbs(struct hd60s_dev *d)
{
	unsigned int psize = d->iso_pkt_size;
	int npkts = d->iso_packets;
	int i, j;

	/*
	 * A 32-packet URB is half a megabyte of contiguous DMA. Halve the
	 * packet count rather than fail: smaller URBs still stream, they just
	 * tolerate less scheduling jitter.
	 */
	for (;;) {
		d->urb_size = d->use_bulk ? HD60S_BULK_SIZE : psize * npkts;

		for (i = 0; i < HD60S_NUM_URBS; i++) {
			struct urb *u;
			void *buf;
			dma_addr_t dma;

			u = usb_alloc_urb(d->use_bulk ? 0 : npkts, GFP_KERNEL);
			if (!u)
				goto retry;
			d->urbs[i] = u;

			buf = usb_alloc_coherent(d->udev, d->urb_size,
						 GFP_KERNEL, &dma);
			if (!buf)
				goto retry;

			u->dev = d->udev;
			u->context = d;
			u->transfer_buffer = buf;
			u->transfer_dma = dma;
			u->transfer_buffer_length = d->urb_size;
			u->transfer_flags = URB_NO_TRANSFER_DMA_MAP;

			if (d->use_bulk) {
				u->pipe = usb_rcvbulkpipe(d->udev, HD60S_EP_VIDEO);
				u->complete = hd60s_bulk_complete;
			} else {
				u->pipe = usb_rcvisocpipe(d->udev, HD60S_EP_VIDEO);
				u->complete = hd60s_iso_complete;
				u->transfer_flags |= URB_ISO_ASAP;
				u->interval = d->iso_interval ? d->iso_interval : 1;
				u->number_of_packets = npkts;
				for (j = 0; j < npkts; j++) {
					u->iso_frame_desc[j].offset = j * psize;
					u->iso_frame_desc[j].length = psize;
				}
			}
		}
		d->iso_packets = npkts;
		return 0;

retry:
		hd60s_free_urbs(d);
		if (d->use_bulk || npkts <= 2)
			return -ENOMEM;
		npkts /= 2;
		dev_info(&d->intf->dev,
			 "retrying URB allocation with %d packets\n", npkts);
	}
}

/*
 * SET_INTERFACE can land on a lower alt than the URBs were sized for, meaning
 * smaller packets. The buffers are still big enough -- the retry only walks
 * downwards -- but the packet descriptors must be re-laid or the first submit
 * is rejected for asking more per interval than the endpoint offers.
 */
static void hd60s_relayout_urbs(struct hd60s_dev *d)
{
	int i, j;

	if (d->use_bulk)
		return;
	for (i = 0; i < HD60S_NUM_URBS; i++) {
		struct urb *u = d->urbs[i];

		if (!u)
			continue;
		u->interval = d->iso_interval;
		u->number_of_packets = d->iso_packets;
		u->transfer_buffer_length = d->iso_pkt_size * d->iso_packets;
		for (j = 0; j < d->iso_packets; j++) {
			u->iso_frame_desc[j].offset = j * d->iso_pkt_size;
			u->iso_frame_desc[j].length = d->iso_pkt_size;
		}
	}
}

/* Bytes/s, or 0 if the alt carries no isochronous EP 0x83. */
static u64 hd60s_alt_capacity(struct hd60s_dev *d, int alt,
			      unsigned int *psize, unsigned int *interval)
{
	struct usb_host_interface *alts;
	struct usb_interface *intf = d->intf;
	unsigned int size, interval_us;
	int i;
	u8 bi;

	if (alt >= intf->num_altsetting)
		return 0;
	alts = &intf->altsetting[alt];

	for (i = 0; i < alts->desc.bNumEndpoints; i++) {
		struct usb_host_endpoint *ep = &alts->endpoint[i];

		if (ep->desc.bEndpointAddress != HD60S_EP_VIDEO)
			continue;
		if (!usb_endpoint_xfer_isoc(&ep->desc))
			return 0;

		size = usb_endpoint_maxp(&ep->desc);
		if (d->udev->speed >= USB_SPEED_SUPER) {
			d->alt_burst = ep->ss_ep_comp.bMaxBurst + 1;
			d->alt_mult = (ep->ss_ep_comp.bmAttributes & 3) + 1;
			size *= d->alt_burst * d->alt_mult;
		} else {
			d->alt_burst = 1;
			d->alt_mult = usb_endpoint_maxp_mult(&ep->desc);
			size *= d->alt_mult;
		}
		if (!size)
			return 0;

		/* high/super speed isochronous: 125 us << (bInterval - 1) */
		bi = clamp_t(u8, ep->desc.bInterval, 1, 16);
		interval_us = 125U << (bi - 1);
		*psize = size;
		/* urb->interval takes microframes at these speeds, not bInterval */
		if (interval)
			*interval = 1U << (bi - 1);
		return div_u64((u64)size * 1000000ULL, interval_us);
	}
	return 0;
}

/*
 * Alt selection, using the Windows driver's formula: alt 1 / alt 1 / alt 2 for
 * 480i / 720p60 / 1080p60.
 */
static int hd60s_pick_alt(struct hd60s_dev *d)
{
	bool ss = d->udev->speed >= USB_SPEED_SUPER;
	u64 bw = (u64)d->tm.hactive * d->tm.vactive * d->tm.fps;
	u64 thr = ss ? 65536000ULL : 4096000ULL;
	unsigned int psize = 0, itv = 1;
	int alt;

	bw += (bw * 0xb926fabULL) >> 32;		/* x1.0451803 */

	if (bw <= thr)
		alt = 1;
	else if (!ss || bw <= 2 * thr)
		alt = 2;
	else
		alt = 3;

	/*
	 * The formula cannot reach alt 3 for any mode this device produces, but
	 * do not select it even if a future mode would: the firmware brings
	 * that endpoint up as bulk regardless of the descriptor.
	 */
	if (alt == 3 || !hd60s_alt_capacity(d, alt, &psize, &itv) || !psize) {
		d->use_bulk = true;
		dev_info(&d->intf->dev,
			 "%llu Mpx/s: no usable isochronous alt, using bulk (alt 4)\n",
			 div_u64(bw, 1000000));
		return 4;
	}

	d->iso_pkt_size = psize;
	d->iso_interval = itv;
	d->use_bulk = false;
	dev_dbg(&d->intf->dev,
		"%llu Mpx/s -> alt %d (%u B/interval, burst %u mult %u)\n",
		div_u64(bw, 1000000), alt, psize, d->alt_burst, d->alt_mult);
	return alt;
}

/*
 * SET_INTERFACE with the Windows driver's downward retry: on failure try
 * alt-1, alt-2, and so on. The host controller can refuse a reservation
 * another device already holds, and a lower alt that merely drops frames
 * beats not streaming at all.
 */
static int hd60s_set_alt(struct hd60s_dev *d, int alt)
{
	int ret = -EINVAL;

	lockdep_assert_held(&d->ctrl_lock);
	for (; alt > 0; alt--) {
		unsigned int psize = 0, itv = 1;

		ret = usb_set_interface(d->udev, HD60S_IF_VIDEO, alt);
		if (ret == 0) {
			d->alt = alt;
			if (!d->use_bulk &&
			    hd60s_alt_capacity(d, alt, &psize, &itv) && psize) {
				d->iso_pkt_size = psize;
				d->iso_interval = itv;
			}
			return 0;
		}
		dev_warn(&d->intf->dev, "SET_INTERFACE alt %d: %d\n", alt, ret);
		if (d->use_bulk)
			break;			/* nothing below alt 4 is bulk */
	}
	return ret;
}

/*
 * SET_INTERFACE is a control transfer like any other, and it is the dangerous
 * one: the FX3's handler tears down the DMA channel and flushes EP 0x83.
 * Route it through the same wedge latch as every other transfer, so that a
 * teardown which silently spent five seconds timing out is not mistaken for
 * one that worked.
 */
static int hd60s_set_alt0(struct hd60s_dev *d)
{
	int ret = usb_set_interface(d->udev, HD60S_IF_VIDEO, 0);

	if (ret == -ETIMEDOUT && !d->wedged) {
		d->wedged = true;
		dev_err(&d->intf->dev,
			"SET_INTERFACE alt 0 timed out, EP0 is wedged\n");
	} else if (ret < 0) {
		dev_err(&d->intf->dev, "SET_INTERFACE alt 0: %d\n", ret);
	}
	return ret;
}

/*
 * Transport teardown, in the Windows driver's order: SET_INTERFACE alt 0 goes
 * out while the URBs are still live, so the FX3 drains its DMA channel into a
 * pipe the host is still servicing. Canceling first fills those buffers and
 * blocks the thread that also services vendor requests, leaving the device
 * enumerated but unresponsive until VBUS is cycled.
 *
 * ctrl_lock is held throughout: the FX3's SET_INTERFACE handler tears down the
 * DMA channel, and an I2C transaction fired into that window wedges its control
 * thread -- the status poll is exactly such a transaction.
 *
 * Safe on a partly submitted ring, so the start path uses it too.
 */
static void hd60s_stop_transport(struct hd60s_dev *d)
{
	lockdep_assert_held(&d->ctrl_lock);
	WRITE_ONCE(d->streaming, false);

	if (d->wedged)
		dev_warn(&d->intf->dev,
			 "EP0 wedged: skipping the control-plane teardown\n");
	else
		hd60s_set_alt0(d);

	hd60s_kill_urbs(d);

	if (d->wedged)
		return;
	msleep(35);
	hd60s_disarm_events_nolock(d);
	hd60s_stream_enable_nolock(d, false);
}

/*
 * Transport bring-up, the inverse of the above: stream enable, the alt setting
 * 105 ms later, then the ring. The ring must already be allocated; on failure
 * the transport is left torn down, so the caller has only to free it.
 */
static int hd60s_start_transport(struct hd60s_dev *d, int alt)
{
	int i, ret;

	mutex_lock(&d->ctrl_lock);
	hd60s_disarm_events_nolock(d);

	ret = hd60s_stream_enable_nolock(d, true);
	if (ret < 0) {
		dev_err(&d->intf->dev, "stream enable: %d\n", ret);
		goto unlock;		/* nothing started, nothing to tear down */
	}
	msleep(105);

	ret = hd60s_set_alt(d, alt);
	if (ret < 0)
		goto stop;
	hd60s_relayout_urbs(d);

	WRITE_ONCE(d->streaming, true);
	atomic_set(&d->inflight, 0);
	for (i = 0; i < HD60S_NUM_URBS; i++) {
		atomic_inc(&d->inflight);
		ret = usb_submit_urb(d->urbs[i], GFP_KERNEL);
		if (ret < 0) {
			atomic_dec(&d->inflight);
			dev_err(&d->intf->dev, "submit URB %d: %d\n", i, ret);
			goto stop;
		}
	}
	mutex_unlock(&d->ctrl_lock);
	return 0;

stop:
	hd60s_stop_transport(d);
unlock:
	mutex_unlock(&d->ctrl_lock);
	return ret;
}

/*
 * A once-per-second check that the endpoint is delivering. An isochronous
 * endpoint with mult > 1 that never delivers a packet is the signature of a
 * host controller that cannot do multi-mult SuperSpeed isochronous.
 *
 * The threshold clears a healthy slow start: the first capture after a module
 * load on rev 4 can deliver nothing for three and a half seconds and then run
 * perfectly. A host that truly cannot do this never delivers at all, so waiting
 * costs nothing. The complaint is withdrawn if data does arrive.
 */
#define HD60S_WATCHDOG_SILENT_S	8

void hd60s_watchdog(struct work_struct *work)
{
	struct hd60s_dev *d = container_of(work, struct hd60s_dev, watchdog.work);
	struct hd60s_parser *p = &d->p;

	if (!READ_ONCE(d->streaming))
		return;

	d->wd_tick++;
	if (!p->bytes && !d->use_bulk && !d->wd_warned &&
	    d->wd_tick >= HD60S_WATCHDOG_SILENT_S) {
		d->wd_warned = true;
		dev_err(&d->intf->dev,
			"no isochronous data after %u s on alt %d (burst %u mult %u)\n",
			d->wd_tick, d->alt, d->alt_burst, d->alt_mult);
		if (d->alt_mult > 1)
			dev_err(&d->intf->dev,
				"this host controller may not support mult>1 SuperSpeed isochronous; retry with force_bulk=1\n");
	} else if (d->wd_warned && p->bytes) {
		/*
		 * Safe to re-arm: p->bytes only grows until the next
		 * hd60s_parser_reset(), so the branch above cannot fire again
		 * this session.
		 */
		d->wd_warned = false;
		dev_info(&d->intf->dev,
			 "isochronous data started after %u s; the error above was premature\n",
			 d->wd_tick);
	} else if (p->bytes == d->wd_bytes && d->wd_tick > HD60S_WATCHDOG_SILENT_S) {
		dev_warn_ratelimited(&d->intf->dev,
				     "endpoint silent for a second (%u frames so far)\n",
				     p->frames);
	}

	d->wd_bytes = p->bytes;
	queue_delayed_work(d->wq, &d->watchdog, msecs_to_jiffies(1000));
}

static int hd60s_queue_setup(struct vb2_queue *q, unsigned int *nbuffers,
			     unsigned int *nplanes, unsigned int sizes[],
			     struct device *alloc_devs[])
{
	struct hd60s_dev *d = vb2_get_drv_priv(q);
	unsigned int size = d->fmt.sizeimage;

	if (*nplanes)
		return sizes[0] < size ? -EINVAL : 0;
	*nplanes = 1;
	sizes[0] = size;
	if (*nbuffers < 4)
		*nbuffers = 4;
	return 0;
}

static int hd60s_buf_prepare(struct vb2_buffer *vb)
{
	struct hd60s_dev *d = vb2_get_drv_priv(vb->vb2_queue);

	if (vb2_plane_size(vb, 0) < d->fmt.sizeimage)
		return -EINVAL;
	vb2_set_plane_payload(vb, 0, d->fmt.sizeimage);
	return 0;
}

static void hd60s_buf_queue(struct vb2_buffer *vb)
{
	struct hd60s_dev *d = vb2_get_drv_priv(vb->vb2_queue);
	struct hd60s_buffer *b =
		container_of(to_vb2_v4l2_buffer(vb), struct hd60s_buffer, vb);
	unsigned long flags;

	spin_lock_irqsave(&d->qlock, flags);
	list_add_tail(&b->list, &d->bufs);
	spin_unlock_irqrestore(&d->qlock, flags);
}

static void hd60s_return_buffers(struct hd60s_dev *d, enum vb2_buffer_state st)
{
	struct hd60s_buffer *b, *tmp;
	unsigned long flags;
	LIST_HEAD(done);

	spin_lock_irqsave(&d->qlock, flags);
	list_splice_init(&d->bufs, &done);
	if (d->p.cur) {
		list_add_tail(&d->p.cur->list, &done);
		d->p.cur = NULL;
		d->p.fb = NULL;
	}
	spin_unlock_irqrestore(&d->qlock, flags);

	list_for_each_entry_safe(b, tmp, &done, list) {
		list_del(&b->list);
		vb2_buffer_done(&b->vb.vb2_buf, st);
	}
}

/*
 * Snapshot the geometry the parser will use, all of it from the configured
 * mode, which is what the buffers were sized from. Mixing in the detected mode
 * gives a stride no frame can match once the two have diverged.
 */
static void hd60s_parser_reset(struct hd60s_dev *d)
{
	struct hd60s_parser *p = &d->p;

	memset(p, 0, sizeof(*p));
	p->interlaced = d->fmt.field == V4L2_FIELD_INTERLACED;
	p->line_bytes = d->fmt.bytesperline;
	p->stride     = p->line_bytes * (p->interlaced ? 2 : 1);
	p->frame_h    = d->fmt.height;
	p->frame_size = d->fmt.sizeimage;
	p->state      = HD60S_PS_HDR;
	p->in_blank   = true;		/* wait for a real field start */
	p->needs_clear = true;		/* first buffer holds whatever it held */

	d->iso_total = 0;
	d->iso_err = 0;
	d->bulk_urbs = 0;
	d->bulk_err = 0;
	d->bulk_lost = 0;
	d->bulk_status = 0;
	d->resubmit_err = 0;
}

static int hd60s_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct hd60s_dev *d = vb2_get_drv_priv(q);
	struct hd60s_parser *p = &d->p;
	int alt, ret;

	if (d->gone) {
		ret = -ENODEV;
		goto err;
	}
	if (!d->signal) {
		dev_dbg(&d->intf->dev, "cannot start: no signal\n");
		ret = -ENOLINK;
		goto err;
	}

	/*
	 * Geometry is snapshotted here and not revisited. A mode change
	 * mid-stream would resize the frame under buffers userspace already
	 * owns, so it is published as V4L2_EVENT_SOURCE_CHANGE and acted on by
	 * a stop/start.
	 */
	hd60s_parser_reset(d);

	/*
	 * A stream started before the detected mode was adopted can never find
	 * its geometry. The poll only notices a mismatch on a change, so catch
	 * the one that exists at start.
	 */
	{
		bool il = hd60s_interlaced(&d->tm);
		u32 dw = d->tm.hactive, dh = d->tm.vactive * (il ? 2 : 1);

		if (dw * 2 != p->line_bytes || dh != p->frame_h) {
			dev_warn(&d->intf->dev,
				 "source is %ux%u but the capture is %ux%u, frames stop until the new timings are set\n",
				 dw, dh, p->line_bytes / 2, p->frame_h);
			p->stale = true;
		}
	}

	if (hd60s_force_bulk) {
		d->use_bulk = true;
		alt = 4;
		dev_dbg(&d->intf->dev, "bulk (alt 4), forced by force_bulk\n");
	} else {
		alt = hd60s_pick_alt(d);
	}

	/*
	 * 32 packets per URB and 16 URBs is the Windows driver's geometry, and
	 * the alts that fail are exactly the ones a smaller byte budget shrinks.
	 */
	d->iso_packets = HD60S_ISO_PACKETS;

	ret = hd60s_alloc_urbs(d);
	if (ret < 0)
		goto err;

	ret = hd60s_start_transport(d, alt);
	if (ret < 0)
		goto err_urbs;

	d->wd_bytes = 0;
	d->wd_tick = 0;
	d->wd_warned = false;
	queue_delayed_work(d->wq, &d->watchdog, msecs_to_jiffies(1000));

	dev_dbg(&d->intf->dev, "streaming %ux%u%s on alt %d (%s)\n",
		d->fmt.width, d->fmt.height, p->interlaced ? "i" : "p",
		d->alt, d->use_bulk ? "bulk" : "isochronous");
	return 0;

err_urbs:
	hd60s_free_urbs(d);
err:
	hd60s_return_buffers(d, VB2_BUF_STATE_QUEUED);
	return ret;
}

static void hd60s_stop_streaming(struct vb2_queue *q)
{
	struct hd60s_dev *d = vb2_get_drv_priv(q);
	struct hd60s_parser *p = &d->p;

	cancel_delayed_work_sync(&d->watchdog);

	mutex_lock(&d->ctrl_lock);
	hd60s_stop_transport(d);
	mutex_unlock(&d->ctrl_lock);

	hd60s_free_urbs(d);
	hd60s_return_buffers(d, VB2_BUF_STATE_ERROR);

	dev_dbg(&d->intf->dev,
		"stopped: %u frames, %u partial, %u dropped, %u resyncs (%u at lock-on), %u parity errors, %llu MB, %u audio bytes, %u/%u iso packets bad\n",
		p->frames, p->partial, p->dropped, p->resyncs, p->resyncs_at_lock,
		p->parity_err, div_u64(p->bytes, 1000000), p->audio_bytes,
		d->iso_err, d->iso_total);
	if (d->use_bulk)
		dev_dbg(&d->intf->dev,
			"bulk: %u URBs, %u errors (last %d), %llu bytes discarded\n",
			d->bulk_urbs, d->bulk_err, d->bulk_status, d->bulk_lost);
	if (p->nbadhdr)
		dev_dbg(&d->intf->dev,
			"first bad headers after lock-on: %4ph %4ph %4ph %4ph\n",
			p->badhdr[0], p->badhdr[1], p->badhdr[2], p->badhdr[3]);
	/* An URB that fails to go back on the ring is gone from it for good. */
	dev_dbg(&d->intf->dev, "urbs: %u resubmit failures, %d still in flight\n",
		d->resubmit_err, atomic_read(&d->inflight));
	if (d->resubmit_err)
		dev_warn(&d->intf->dev, "%u URBs were lost from the ring\n",
			 d->resubmit_err);
}

/*
 * System-sleep PM. supports_autosuspend is not set, so these run only for
 * system sleep with userspace frozen; no ioctl can race the teardown.
 *
 * vb2 is left alone: the queue stays streaming and its buffers stay owned by
 * the driver. Only the transport goes down, and hd60s_video_resume() puts it
 * back under the same ring and alternate setting.
 *
 * Both directions change the alt setting, and usb_set_interface() recreates the
 * endpoint sysfs devices as it goes, so the PM core says
 *
 *     ep_83: PM: parent <intf> should not be sleeping
 *
 * once each way. Cosmetic, and unavoidable: alt 0 has to go out while the URBs
 * are live, which is the whole reason hd60s_stop_transport() exists.
 */
void hd60s_video_suspend(struct hd60s_dev *d)
{
	cancel_delayed_work_sync(&d->watchdog);

	mutex_lock(&d->ctrl_lock);
	d->pm_streaming = READ_ONCE(d->streaming);
	if (d->pm_streaming)
		hd60s_stop_transport(d);
	mutex_unlock(&d->ctrl_lock);
}

/*
 * Re-arm what hd60s_video_suspend() stopped. The parser is left as it is rather
 * than reset -- it resynchronizes from an arbitrary point by design, and that
 * keeps the session counters the stop path prints. Failure is handled here in
 * full; the caller has nothing to do with it.
 */
void hd60s_video_resume(struct hd60s_dev *d)
{
	int ret;

	if (!d->pm_streaming)
		return;
	d->pm_streaming = false;

	ret = hd60s_start_transport(d, d->alt);
	if (ret < 0) {
		/*
		 * Unlike a mid-stream mode change, this does not heal: the ring
		 * is down and nothing will bring it back without a restart.
		 * vb2_queue_error() is the honest answer -- DQBUF returns -EIO
		 * and the application has to stop and start the queue.
		 */
		dev_err(&d->intf->dev,
			"resume: could not restart the transport (%d)\n", ret);
		vb2_queue_error(&d->queue);
		return;
	}

	d->wd_bytes = 0;
	d->wd_tick = 0;
	d->wd_warned = false;
	queue_delayed_work(d->wq, &d->watchdog, msecs_to_jiffies(1000));
}

static const struct vb2_ops hd60s_vb2_ops = {
	.queue_setup		= hd60s_queue_setup,
	.buf_prepare		= hd60s_buf_prepare,
	.buf_queue		= hd60s_buf_queue,
	.start_streaming	= hd60s_start_streaming,
	.stop_streaming		= hd60s_stop_streaming,
};

/*
 * Audio inputs: 0 is embedded HDMI and the default, 1 the analog jack.
 *
 * The V4L2 audio-input API rather than an ALSA mixer control, because the
 * selection belongs to the capture device rather than the PCM stream and the
 * audio is muxed into the video endpoint.
 */

static const char * const hd60s_audio_names[] = {
	"HDMI",
	"Analog",
};

static int hd60s_audio_count(struct hd60s_dev *d)
{
	if (!d->ops->set_audio)
		return 0;
	return ARRAY_SIZE(hd60s_audio_names);
}

static int hd60s_querycap(struct file *file, void *priv,
			  struct v4l2_capability *cap)
{
	struct hd60s_dev *d = video_drvdata(file);

	strscpy(cap->driver, "hd60s", sizeof(cap->driver));
	strscpy(cap->card, "Elgato Game Capture HD60 S", sizeof(cap->card));
	usb_make_path(d->udev, cap->bus_info, sizeof(cap->bus_info));
	return 0;
}

static int hd60s_enum_fmt(struct file *file, void *priv,
			  struct v4l2_fmtdesc *f)
{
	struct hd60s_dev *d = video_drvdata(file);

	if (f->index)
		return -EINVAL;
	f->pixelformat = V4L2_PIX_FMT_YUYV;
	/*
	 * Only advertised where the conversion is real: the rev-4 register is a
	 * bare gain behind a luma clamp at 235.
	 */
	if (d->ops->set_color_range)
		f->flags |= V4L2_FMT_FLAG_CSC_QUANTIZATION;
	return 0;
}

static int hd60s_g_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct hd60s_dev *d = video_drvdata(file);

	f->fmt.pix = d->fmt;
	return 0;
}

/*
 * The device decides the geometry, so TRY and S both answer with what is being
 * received. The one thing userspace may choose is the quantization range, the
 * standard way: V4L2_PIX_FMT_FLAG_SET_CSC plus the quantization field, which
 * ENUM_FMT advertises with V4L2_FMT_FLAG_CSC_QUANTIZATION. Anything but
 * LIM_RANGE or FULL_RANGE means "leave it alone".
 */
static u32 hd60s_want_quantization(struct hd60s_dev *d,
				   const struct v4l2_pix_format *pix)
{
	/*
	 * Not negotiable where the front end cannot convert: it passes the
	 * input through, so the format's quantization is a statement of fact
	 * about the pixels and TRY/S must answer with what G_FMT reports.
	 * Returning the requested-output field here instead made TRY_FMT
	 * disagree with G_FMT as soon as the input was declared full range.
	 */
	if (!d->ops->set_color_range)
		return d->fmt.quantization;
	if (!(pix->flags & V4L2_PIX_FMT_FLAG_SET_CSC))
		return d->quantization;
	if (pix->quantization == V4L2_QUANTIZATION_LIM_RANGE ||
	    pix->quantization == V4L2_QUANTIZATION_FULL_RANGE)
		return pix->quantization;
	return d->quantization;
}

static int hd60s_try_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct hd60s_dev *d = video_drvdata(file);
	u32 want = hd60s_want_quantization(d, &f->fmt.pix);

	f->fmt.pix = d->fmt;
	f->fmt.pix.quantization = want;
	return 0;
}

static int hd60s_s_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct hd60s_dev *d = video_drvdata(file);
	u32 want = hd60s_want_quantization(d, &f->fmt.pix);

	if (vb2_is_busy(&d->queue))
		return -EBUSY;
	if (d->ops->set_color_range && want != d->quantization) {
		d->quantization = want;
		hd60s_apply_color_range(d);
		hd60s_update_format(d);
	}
	f->fmt.pix = d->fmt;
	return 0;
}

static int hd60s_enum_framesizes(struct file *file, void *priv,
				 struct v4l2_frmsizeenum *fsize)
{
	struct hd60s_dev *d = video_drvdata(file);

	if (fsize->index || fsize->pixel_format != V4L2_PIX_FMT_YUYV)
		return -EINVAL;
	fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fsize->discrete.width = d->fmt.width;
	fsize->discrete.height = d->fmt.height;
	return 0;
}

/*
 * The status block reports the FIELD rate, so an interlaced mode delivers half
 * as many frames as it reports. Everything here comes from the configured
 * mode, which is what the format describes.
 */
static void hd60s_frameperiod(struct hd60s_dev *d, struct v4l2_fract *tpf)
{
	bool il = hd60s_interlaced(&d->cfg);

	tpf->numerator = il ? 2 : 1;
	tpf->denominator = d->cfg.fps ? d->cfg.fps : 60;
	if (d->cfg.modeflag & HD60S_MF_FRACTIONAL) {
		tpf->numerator *= 1001;
		tpf->denominator *= 1000;
	}
}

static int hd60s_enum_frameintervals(struct file *file, void *priv,
				     struct v4l2_frmivalenum *fival)
{
	struct hd60s_dev *d = video_drvdata(file);

	if (fival->index || fival->pixel_format != V4L2_PIX_FMT_YUYV)
		return -EINVAL;
	if (fival->width != d->fmt.width || fival->height != d->fmt.height)
		return -EINVAL;
	fival->type = V4L2_FRMIVAL_TYPE_DISCRETE;
	hd60s_frameperiod(d, &fival->discrete);
	return 0;
}

/* The frame rate follows the source and cannot be set, so S_PARM answers G. */
static int hd60s_g_parm(struct file *file, void *priv,
			struct v4l2_streamparm *sp)
{
	struct hd60s_dev *d = video_drvdata(file);

	memset(&sp->parm.capture, 0, sizeof(sp->parm.capture));
	sp->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	sp->parm.capture.readbuffers = 4;
	hd60s_frameperiod(d, &sp->parm.capture.timeperframe);
	return 0;
}

static int hd60s_enum_input(struct file *file, void *priv,
			    struct v4l2_input *inp)
{
	struct hd60s_dev *d = video_drvdata(file);

	if (inp->index)
		return -EINVAL;
	strscpy(inp->name, "HDMI", sizeof(inp->name));
	inp->type = V4L2_INPUT_TYPE_CAMERA;
	inp->capabilities = V4L2_IN_CAP_DV_TIMINGS;
	inp->status = d->signal ? 0 : V4L2_IN_ST_NO_SIGNAL;
	/* every audio input can be paired with the one video input */
	inp->audioset = (1U << hd60s_audio_count(d)) - 1;
	return 0;
}

static int hd60s_g_input(struct file *file, void *priv, unsigned int *i)
{
	*i = 0;
	return 0;
}

static int hd60s_s_input(struct file *file, void *priv, unsigned int i)
{
	return i ? -EINVAL : 0;
}

/*
 * QUERY reports what is on the wire, G reports what has been set. They are
 * different questions and, after a source change userspace has not yet
 * adopted, different answers. Both derive from a single hd60s_timing so
 * neither can produce a blend of the two.
 */
static int hd60s_query_dv_timings(struct file *file, void *priv,
				  struct v4l2_dv_timings *t)
{
	struct hd60s_dev *d = video_drvdata(file);

	if (!d->signal)
		return -ENOLINK;
	hd60s_timings_from(&d->tm, t);
	return 0;
}

static int hd60s_g_dv_timings(struct file *file, void *priv,
			      struct v4l2_dv_timings *t)
{
	struct hd60s_dev *d = video_drvdata(file);

	hd60s_timings_from(&d->cfg, t);
	return 0;
}

/*
 * The receiver locks to whatever the source sends, so the only timings that
 * can be set are the ones currently detected. What S_DV_TIMINGS does that
 * nothing else may is adopt them: it is the one path allowed to move d->cfg,
 * and therefore d->fmt, to a newly detected mode.
 */
static int hd60s_s_dv_timings(struct file *file, void *priv,
			      struct v4l2_dv_timings *t)
{
	struct hd60s_dev *d = video_drvdata(file);
	struct v4l2_dv_timings cur, detected;

	/*
	 * Setting the timings already in force changes nothing, so it must
	 * succeed even with buffers allocated.
	 */
	hd60s_timings_from(&d->cfg, &cur);
	if (v4l2_match_dv_timings(t, &cur, 0, false)) {
		*t = cur;
		return 0;
	}

	/* Anything else resizes the buffers, so it cannot happen under one. */
	if (vb2_is_busy(&d->queue))
		return -EBUSY;

	if (!d->signal)
		return -ENOLINK;
	hd60s_timings_from(&d->tm, &detected);
	if (!v4l2_match_dv_timings(t, &detected, 0, false)) {
		dev_dbg(&d->intf->dev,
			"S_DV_TIMINGS rejected %ux%u%s %llu Hz; detected %ux%u%s %llu Hz\n",
			t->bt.width, t->bt.height,
			t->bt.interlaced ? "i" : "p", t->bt.pixelclock,
			detected.bt.width, detected.bt.height,
			detected.bt.interlaced ? "i" : "p",
			detected.bt.pixelclock);
		return -EINVAL;
	}

	d->cfg = d->tm;
	hd60s_update_format(d);
	*t = detected;
	return 0;
}

static const struct v4l2_dv_timings_cap hd60s_timings_cap = {
	.type = V4L2_DV_BT_656_1120,
	.bt = {
		.min_width = 320,
		.max_width = 4096,
		.min_height = 200,	/* rev4_detect()'s own floor */
		.max_height = 2160,
		.min_pixelclock = 6000000,
		.max_pixelclock = 600000000,
		.standards = V4L2_DV_BT_STD_CEA861 | V4L2_DV_BT_STD_DMT,
		.capabilities = V4L2_DV_BT_CAP_PROGRESSIVE |
				V4L2_DV_BT_CAP_INTERLACED,
	},
};

static int hd60s_dv_timings_cap(struct file *file, void *priv,
				struct v4l2_dv_timings_cap *cap)
{
	*cap = hd60s_timings_cap;
	return 0;
}

/*
 * ENUM_DV_TIMINGS enumerates what can be set, not what exists in the world.
 * This receiver locks to whatever the source sends and cannot be commanded, so
 * the only settable timing is the one currently detected, and that is the
 * whole enumeration. With no signal the configured mode is still settable,
 * so it is what gets enumerated.
 */
static int hd60s_enum_dv_timings(struct file *file, void *priv,
				 struct v4l2_enum_dv_timings *t)
{
	struct hd60s_dev *d = video_drvdata(file);

	if (t->index > 0)
		return -EINVAL;
	hd60s_timings_from(d->signal ? &d->tm : &d->cfg, &t->timings);
	return 0;
}

static int hd60s_enumaudio(struct file *file, void *priv, struct v4l2_audio *a)
{
	struct hd60s_dev *d = video_drvdata(file);
	u32 idx = a->index;

	if (idx >= (u32)hd60s_audio_count(d))
		return -EINVAL;
	memset(a, 0, sizeof(*a));
	a->index = idx;
	strscpy(a->name, hd60s_audio_names[idx], sizeof(a->name));
	return 0;
}

static int hd60s_g_audio(struct file *file, void *priv, struct v4l2_audio *a)
{
	struct hd60s_dev *d = video_drvdata(file);

	if (!hd60s_audio_count(d))
		return -EINVAL;
	memset(a, 0, sizeof(*a));
	a->index = d->audio_src;
	strscpy(a->name, hd60s_audio_names[d->audio_src], sizeof(a->name));
	return 0;
}

static int hd60s_s_audio(struct file *file, void *priv,
			 const struct v4l2_audio *a)
{
	struct hd60s_dev *d = video_drvdata(file);
	int ret;

	if (a->index >= (u32)hd60s_audio_count(d))
		return -EINVAL;
	if (a->index == d->audio_src)
		return 0;

	ret = d->ops->set_audio(d, a->index);
	if (ret < 0) {
		dev_warn(&d->intf->dev, "audio input %u: %d\n", a->index, ret);
		return ret;
	}
	d->audio_src = a->index;
	return 0;
}

static int hd60s_subscribe_event(struct v4l2_fh *fh,
				 const struct v4l2_event_subscription *sub)
{
	switch (sub->type) {
	case V4L2_EVENT_SOURCE_CHANGE:
		return v4l2_src_change_event_subscribe(fh, sub);
	case V4L2_EVENT_CTRL:
		return v4l2_ctrl_subscribe_event(fh, sub);
	default:
		return -EINVAL;
	}
}

static const struct v4l2_ioctl_ops hd60s_ioctl_ops = {
	.vidioc_querycap		= hd60s_querycap,
	.vidioc_enum_fmt_vid_cap	= hd60s_enum_fmt,
	.vidioc_g_fmt_vid_cap		= hd60s_g_fmt,
	.vidioc_s_fmt_vid_cap		= hd60s_s_fmt,
	.vidioc_try_fmt_vid_cap		= hd60s_try_fmt,
	.vidioc_enum_framesizes		= hd60s_enum_framesizes,
	.vidioc_enum_frameintervals	= hd60s_enum_frameintervals,
	.vidioc_g_parm			= hd60s_g_parm,
	.vidioc_s_parm			= hd60s_g_parm,
	.vidioc_enum_input		= hd60s_enum_input,
	.vidioc_g_input			= hd60s_g_input,
	.vidioc_s_input			= hd60s_s_input,
	.vidioc_query_dv_timings	= hd60s_query_dv_timings,
	.vidioc_g_dv_timings		= hd60s_g_dv_timings,
	.vidioc_s_dv_timings		= hd60s_s_dv_timings,
	.vidioc_dv_timings_cap		= hd60s_dv_timings_cap,
	.vidioc_enum_dv_timings		= hd60s_enum_dv_timings,
	.vidioc_enumaudio		= hd60s_enumaudio,
	.vidioc_g_audio			= hd60s_g_audio,
	.vidioc_s_audio			= hd60s_s_audio,

	.vidioc_reqbufs			= vb2_ioctl_reqbufs,
	.vidioc_create_bufs		= vb2_ioctl_create_bufs,
	.vidioc_prepare_buf		= vb2_ioctl_prepare_buf,
	.vidioc_querybuf		= vb2_ioctl_querybuf,
	.vidioc_qbuf			= vb2_ioctl_qbuf,
	.vidioc_dqbuf			= vb2_ioctl_dqbuf,
	.vidioc_expbuf			= vb2_ioctl_expbuf,
	.vidioc_streamon		= vb2_ioctl_streamon,
	.vidioc_streamoff		= vb2_ioctl_streamoff,

	.vidioc_subscribe_event		= hd60s_subscribe_event,
	.vidioc_unsubscribe_event	= v4l2_event_unsubscribe,
};

static const struct v4l2_file_operations hd60s_fops = {
	.owner		= THIS_MODULE,
	.open		= v4l2_fh_open,
	.release	= vb2_fop_release,
	.read		= vb2_fop_read,
	.poll		= vb2_fop_poll,
	.mmap		= vb2_fop_mmap,
	.unlocked_ioctl	= video_ioctl2,
};

/*
 * Controls. Rev 4 has brightness, contrast, saturation and hue in one 4-byte
 * write, 0x80 neutral; the MStar front end folds the same four into the
 * color-space matrix. ops->pic_controls says which exist.
 */

static int hd60s_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct hd60s_dev *d = container_of(ctrl->handler, struct hd60s_dev, ctrls);
	int ret;

	switch (ctrl->id) {
	case V4L2_CID_BRIGHTNESS:
		d->pic[0] = ctrl->val;
		ret = d->ops->set_picture(d);
		break;
	case V4L2_CID_CONTRAST:
		d->pic[1] = ctrl->val;
		ret = d->ops->set_picture(d);
		break;
	case V4L2_CID_SATURATION:
		d->pic[2] = ctrl->val;
		ret = d->ops->set_picture(d);
		break;
	case V4L2_CID_HUE:
		d->pic[3] = ctrl->val;
		ret = d->ops->set_picture(d);
		break;
	case V4L2_CID_DV_RX_RGB_RANGE:
		/*
		 * What the incoming range IS, not what comes out. It only
		 * changes which conversion satisfies the requested output
		 * quantization, so the format does not move and a running
		 * capture is undisturbed.
		 */
		d->rgb_range = ctrl->val;
		ret = hd60s_apply_color_range(d);
		/*
		 * On a front end that cannot convert, this is also the best
		 * available statement about what comes out. Republish the
		 * format so G_FMT's quantization follows it.
		 */
		if (!d->ops->set_color_range)
			hd60s_update_format(d);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static const struct v4l2_ctrl_ops hd60s_ctrl_ops = {
	.s_ctrl = hd60s_s_ctrl,
};

/*
 * V4L2_CID_DV_RX_POWER_PRESENT is deliberately not implemented: it reports the
 * source's +5V pin, which neither front end exposes, and d->signal is a
 * different question -- a source can be powered while sending nothing.
 * v4l2-compliance's warning is advisory.
 */
int hd60s_video_register(struct hd60s_dev *d)
{
	struct vb2_queue *q = &d->queue;
	int ret;

	/*
	 * hd60s_s_ctrl() calls ops->set_picture unconditionally, so a front
	 * end advertising pic_controls bits without the callback must not
	 * get the controls registered -- masking here is what enforces it.
	 */
	u8 pc = d->ops->set_picture ? d->ops->pic_controls : 0;

	v4l2_ctrl_handler_init(&d->ctrls, 5);
	if (pc & HD60S_PIC_BRIGHTNESS)
		v4l2_ctrl_new_std(&d->ctrls, &hd60s_ctrl_ops,
				  V4L2_CID_BRIGHTNESS, 0, 255, 1, 128);
	if (pc & HD60S_PIC_CONTRAST)
		v4l2_ctrl_new_std(&d->ctrls, &hd60s_ctrl_ops,
				  V4L2_CID_CONTRAST, 0, 255, 1, 128);
	if (pc & HD60S_PIC_SATURATION)
		v4l2_ctrl_new_std(&d->ctrls, &hd60s_ctrl_ops,
				  V4L2_CID_SATURATION, 0, 255, 1, 128);
	if (pc & HD60S_PIC_HUE)
		v4l2_ctrl_new_std(&d->ctrls, &hd60s_ctrl_ops,
				  V4L2_CID_HUE, 0, 255, 1, 128);
	/*
	 * What the incoming range is. Created on every front end, including
	 * those that cannot convert, where it is the only way userspace can say
	 * what a passed-through stream contains. The device's own Bypass /
	 * Shrink / Expand is not a control of its own: it is the resolution of
	 * this and the format's quantization.
	 */
	v4l2_ctrl_new_std_menu(&d->ctrls, &hd60s_ctrl_ops,
			       V4L2_CID_DV_RX_RGB_RANGE,
			       V4L2_DV_RGB_RANGE_FULL, 0,
			       V4L2_DV_RGB_RANGE_AUTO);
	if (d->ctrls.error) {
		ret = d->ctrls.error;
		goto err_ctrl;
	}
	d->v4l2_dev.ctrl_handler = &d->ctrls;

	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes = VB2_MMAP | VB2_USERPTR | VB2_READ;
	q->drv_priv = d;
	q->buf_struct_size = sizeof(struct hd60s_buffer);
	q->ops = &hd60s_vb2_ops;
	q->mem_ops = &vb2_vmalloc_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->min_queued_buffers = 2;
	q->lock = &d->vlock;
	ret = vb2_queue_init(q);
	if (ret < 0)
		goto err_ctrl;

	d->vdev.v4l2_dev = &d->v4l2_dev;
	d->vdev.fops = &hd60s_fops;
	d->vdev.ioctl_ops = &hd60s_ioctl_ops;
	d->vdev.release = video_device_release_empty;
	d->vdev.queue = q;
	d->vdev.lock = &d->vlock;
	d->vdev.device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING |
			      V4L2_CAP_READWRITE;
	/*
	 * V4L2_CAP_AUDIO is required whenever ENUMAUDIO returns anything. With
	 * no audio select the ioctls are disabled, so G_AUDIO answers ENOTTY.
	 */
	if (hd60s_audio_count(d)) {
		d->vdev.device_caps |= V4L2_CAP_AUDIO;
	} else {
		v4l2_disable_ioctl(&d->vdev, VIDIOC_ENUMAUDIO);
		v4l2_disable_ioctl(&d->vdev, VIDIOC_G_AUDIO);
		v4l2_disable_ioctl(&d->vdev, VIDIOC_S_AUDIO);
	}
	strscpy(d->vdev.name, "HD60 S", sizeof(d->vdev.name));
	video_set_drvdata(&d->vdev, d);

	ret = video_register_device(&d->vdev, VFL_TYPE_VIDEO, -1);
	if (ret < 0)
		goto err_ctrl;

	d->registered = true;
	return 0;

err_ctrl:
	v4l2_ctrl_handler_free(&d->ctrls);
	d->v4l2_dev.ctrl_handler = NULL;
	return ret;
}

void hd60s_video_unregister(struct hd60s_dev *d)
{
	if (!d->registered)
		return;
	d->registered = false;
	vb2_video_unregister_device(&d->vdev);
}
