// SPDX-License-Identifier: GPL-2.0
/*
 * Elgato Game Capture HD60 S - ALSA capture
 *
 * Copyright (c) 2026 Doug Brown <doug@schmorgal.com>
 *
 * Audio is not a separate endpoint. It is muxed into the EP 0x83 video stream
 * as `FF 00 FF nn` escapes carrying nn DWORDs of s16 stereo LE.
 *
 * The PCM rate is pinned at 48 kHz. If a different sample rate is supplied,
 * the hardware resamples.
 *
 * Samples only arrive while the video stream is running.
 */
#include <linux/module.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>

#include "hd60s.h"

#define HD60S_RATE	48000
#define HD60S_CHANNELS	2

static const struct snd_pcm_hardware hd60s_pcm_hw = {
	.info			= SNDRV_PCM_INFO_MMAP |
				  SNDRV_PCM_INFO_MMAP_VALID |
				  SNDRV_PCM_INFO_INTERLEAVED |
				  SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats		= SNDRV_PCM_FMTBIT_S16_LE,
	.rates			= SNDRV_PCM_RATE_48000,
	.rate_min		= HD60S_RATE,
	.rate_max		= HD60S_RATE,
	.channels_min		= HD60S_CHANNELS,
	.channels_max		= HD60S_CHANNELS,
	.buffer_bytes_max	= 256 * 1024,
	.period_bytes_min	= 1024,
	.period_bytes_max	= 64 * 1024,
	.periods_min		= 2,
	.periods_max		= 64,
};

static int hd60s_pcm_open(struct snd_pcm_substream *ss)
{
	struct hd60s_dev *d = snd_pcm_substream_chip(ss);
	unsigned long flags;

	ss->runtime->hw = hd60s_pcm_hw;

	spin_lock_irqsave(&d->alock, flags);
	d->substream = ss;
	spin_unlock_irqrestore(&d->alock, flags);
	return 0;
}

static int hd60s_pcm_close(struct snd_pcm_substream *ss)
{
	struct hd60s_dev *d = snd_pcm_substream_chip(ss);
	unsigned long flags;

	spin_lock_irqsave(&d->alock, flags);
	d->substream = NULL;
	d->arunning = false;
	spin_unlock_irqrestore(&d->alock, flags);
	return 0;
}

static int hd60s_pcm_prepare(struct snd_pcm_substream *ss)
{
	struct hd60s_dev *d = snd_pcm_substream_chip(ss);
	unsigned long flags;

	spin_lock_irqsave(&d->alock, flags);
	d->apos = 0;
	d->aperiod = 0;
	spin_unlock_irqrestore(&d->alock, flags);
	return 0;
}

static int hd60s_pcm_trigger(struct snd_pcm_substream *ss, int cmd)
{
	struct hd60s_dev *d = snd_pcm_substream_chip(ss);
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&d->alock, flags);
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		d->arunning = true;
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		d->arunning = false;
		break;
	default:
		ret = -EINVAL;
	}
	spin_unlock_irqrestore(&d->alock, flags);
	return ret;
}

static snd_pcm_uframes_t hd60s_pcm_pointer(struct snd_pcm_substream *ss)
{
	struct hd60s_dev *d = snd_pcm_substream_chip(ss);

	return bytes_to_frames(ss->runtime, READ_ONCE(d->apos));
}

static const struct snd_pcm_ops hd60s_pcm_ops = {
	.open		= hd60s_pcm_open,
	.close		= hd60s_pcm_close,
	.prepare	= hd60s_pcm_prepare,
	.trigger	= hd60s_pcm_trigger,
	.pointer	= hd60s_pcm_pointer,
};

/* Called from URB-completion context, once per audio escape payload chunk. */
void hd60s_audio_write(struct hd60s_dev *d, const u8 *data, u32 len)
{
	struct snd_pcm_substream *ss;
	unsigned int bufsize, period, n;
	unsigned long flags;
	bool elapsed = false;

	spin_lock_irqsave(&d->alock, flags);
	ss = d->substream;
	if (!ss || !d->arunning || !ss->runtime || !ss->runtime->dma_area) {
		spin_unlock_irqrestore(&d->alock, flags);
		return;
	}

	bufsize = snd_pcm_lib_buffer_bytes(ss);
	period = snd_pcm_lib_period_bytes(ss);
	if (!bufsize || !period) {
		spin_unlock_irqrestore(&d->alock, flags);
		return;
	}

	/*
	 * hw_params can only run between a stop and a prepare, and prepare
	 * zeroes the offset, so a buffer that shrank under a live offset is
	 * unreachable. Stated here rather than left to that argument: the
	 * subtraction below is unsigned.
	 */
	if (d->apos >= bufsize)
		d->apos = 0;

	while (len) {
		n = min(len, bufsize - d->apos);
		memcpy(ss->runtime->dma_area + d->apos, data, n);
		data += n;
		len -= n;
		d->aperiod += n;
		d->apos += n;
		if (d->apos >= bufsize)
			d->apos = 0;
	}
	if (d->aperiod >= period) {
		d->aperiod %= period;
		elapsed = true;
	}
	spin_unlock_irqrestore(&d->alock, flags);

	if (elapsed)
		snd_pcm_period_elapsed(ss);
}

int hd60s_audio_register(struct hd60s_dev *d)
{
	struct snd_card *card;
	struct snd_pcm *pcm;
	int ret;

	ret = snd_card_new(&d->intf->dev, -1, "HD60S", THIS_MODULE, 0, &card);
	if (ret < 0) {
		dev_warn(&d->intf->dev, "ALSA card: %d (audio disabled)\n", ret);
		return ret;
	}
	card->private_data = d;
	strscpy(card->driver, "hd60s", sizeof(card->driver));
	strscpy(card->shortname, "HD60 S", sizeof(card->shortname));
	snprintf(card->longname, sizeof(card->longname),
		 "Elgato Game Capture HD60 S at %s", dev_name(&d->udev->dev));

	ret = snd_pcm_new(card, "HD60 S", 0, 0, 1, &pcm);
	if (ret < 0)
		goto err;
	pcm->private_data = d;
	strscpy(pcm->name, "HD60 S HDMI", sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &hd60s_pcm_ops);
	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL,
				       64 * 1024, 256 * 1024);

	ret = snd_card_register(card);
	if (ret < 0)
		goto err;

	d->card = card;
	d->pcm = pcm;
	dev_info(&d->intf->dev, "ALSA capture registered (48 kHz s16 stereo)\n");
	return 0;

err:
	snd_card_free(card);
	dev_warn(&d->intf->dev, "ALSA registration failed: %d\n", ret);
	return ret;
}

/*
 * Put a running capture into SNDRV_PCM_STATE_SUSPENDED so that the gap is
 * reported as a suspend rather than turning up as an unexplained xrun.
 * SNDRV_PCM_INFO_RESUME is deliberately not advertised: samples arrive only
 * while the video stream runs, and the ring cannot be picked up mid-period
 * across a full front-end bring-up, so the application restarts the stream.
 */
void hd60s_audio_suspend(struct hd60s_dev *d)
{
	if (d->pcm)
		snd_pcm_suspend_all(d->pcm);
}

void hd60s_audio_unregister(struct hd60s_dev *d)
{
	unsigned long flags;

	if (!d->card)
		return;

	spin_lock_irqsave(&d->alock, flags);
	d->arunning = false;
	d->substream = NULL;
	spin_unlock_irqrestore(&d->alock, flags);

	snd_card_disconnect(d->card);
	snd_card_free_when_closed(d->card);
	d->card = NULL;
	d->pcm = NULL;
}
