// SPDX-License-Identifier: GPL-2.0-only
/*
 * Amazon Radar-Puffin FPGA SPI audio capture driver
 *
 * Copyright (c) 2016 Amazon.com, Inc. or its affiliates.
 */

#include <linux/build_bug.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "amzn-mt-spi-pcm.h"
#include "amzn-fpga-oracle-tail.h"

/* Diagnostic-only register snapshot of the SPI controller and the pin-53-56
 * MODE registers, to compare against the known-good Linux 3.18 oracle values
 * (CFG0=0x03030101 CFG1=0x03ff0503 CMD=0x00033300 PAD_SEL=0x00000000,
 *  MODE 0x6a0=0x00001200 MODE 0x6b0=0x00000249). Read-only; no writes. */
static void amzn_spi_dump_regs(struct device *dev)
{
	void __iomem *spi_base;
	void __iomem *pctl_base;

	spi_base = ioremap(0x1100a000, 0x40);
	if (!spi_base) {
		dev_info(dev, "FPGA regdiag: ioremap SPI failed\n");
		return;
	}
	dev_info(dev,
		 "FPGA regdiag SPI CFG0=0x%08x CFG1=0x%08x CMD=0x%08x PAD_SEL=0x%08x\n",
		 readl_relaxed(spi_base + 0x00), readl_relaxed(spi_base + 0x04),
		 readl_relaxed(spi_base + 0x18), readl_relaxed(spi_base + 0x24));
	iounmap(spi_base);

	pctl_base = ioremap(0x10005000, 0x1000);
	if (!pctl_base) {
		dev_info(dev, "FPGA regdiag: ioremap pinctrl failed\n");
		return;
	}
	dev_info(dev,
		 "FPGA regdiag PIN MODE 0x6a0=0x%08x 0x6b0=0x%08x DIN(0x530)=0x%08x DOUT(0x430)=0x%08x\n",
		 readl_relaxed(pctl_base + 0x6a0), readl_relaxed(pctl_base + 0x6b0),
		 readl_relaxed(pctl_base + 0x530), readl_relaxed(pctl_base + 0x430));
	dev_info(dev,
		 "FPGA regdiag I2S MODE 0x6e0(p70-74)=0x%08x DIN(0x540 p64-79)=0x%08x\n",
		 readl_relaxed(pctl_base + 0x6e0), readl_relaxed(pctl_base + 0x540));
	iounmap(pctl_base);
}

static_assert(sizeof(struct dough_status_frame) == 27);
static_assert(offsetof(struct dough_status_frame, fpga_rev) == 26);
static_assert(sizeof(struct dough_frame) == 6912);
static_assert(sizeof(amzn_fpga_oracle_tail) == 1804);

struct amzn_spi_priv {
	struct spi_device *spi;
	struct gpio_desc *reset_gpio;
	struct pinctrl *pinctrl;
	struct pinctrl_state *i2s_mode0;
	struct pinctrl_state *i2s_mode1;
	struct pinctrl_state *mclk;
	struct workqueue_struct *spi_wq;
	struct work_struct spi_work;
	struct snd_pcm_substream *substream;
	spinlock_t state_lock; /* protects running */
	spinlock_t write_lock; /* protects cur_write_offset */
	size_t cur_write_offset;
	size_t elapsed;
	size_t kernel_overruns;
	size_t fpga_overruns;
	bool running;
};

static const struct snd_pcm_hardware amzn_mt_spi_pcm_hardware = {
	.info = SNDRV_PCM_INFO_INTERLEAVED,
	.formats = SNDRV_PCM_FMTBIT_S24_3LE,
	.rates = SNDRV_PCM_RATE_16000,
	.rate_min = SAMPLING_RATE,
	.rate_max = SAMPLING_RATE,
	.channels_min = SPI_N_CHANNELS,
	.channels_max = SPI_N_CHANNELS,
	.buffer_bytes_max = SPI_BUFFER_BYTES_MAX,
	.period_bytes_max = SPI_PERIOD_BYTES_MAX,
	.period_bytes_min = SPI_PERIOD_BYTES_MIN,
	.periods_min = SPI_N_PERIODS_MIN,
	.periods_max = SPI_N_PERIODS_MAX,
};

static const char * const spi_timestamp_texts[] = { "Off", "On" };
static SOC_ENUM_SINGLE_EXT_DECL(spi_timestamp_enum, spi_timestamp_texts);
static bool transfer_timestamps;

static int transfer_timestamps_get(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.enumerated.item[0] = transfer_timestamps;
	return 0;
}

static int transfer_timestamps_set(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	unsigned int value = ucontrol->value.enumerated.item[0];

	if (value >= ARRAY_SIZE(spi_timestamp_texts))
		return -EINVAL;

	if (transfer_timestamps == value)
		return 0;

	transfer_timestamps = value;
	return 1;
}

static const struct snd_kcontrol_new amzn_mt_spi_controls[] = {
	SOC_ENUM_EXT("SpiTimeStamps", spi_timestamp_enum,
		     transfer_timestamps_get, transfer_timestamps_set),
};

static int amzn_spi_txrx(struct amzn_spi_priv *priv, const void *tx,
			 void *rx, size_t len)
{
	struct spi_transfer xfer = {
		.tx_buf = tx,
		.rx_buf = rx,
		.len = len,
		.bits_per_word = 8,
		.speed_hz = SPI_SPEED_HZ,
	};
	struct spi_message msg;

	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);

	return spi_sync(priv->spi, &msg);
}

static int amzn_spi_probe_txrx(struct amzn_spi_priv *priv, const void *tx,
			       void *rx, size_t len, const char *stage)
{
	struct spi_transfer xfer = {
		.tx_buf = tx,
		.rx_buf = rx,
		.len = len,
		.bits_per_word = 8,
		.speed_hz = SPI_SPEED_HZ,
	};
	struct spi_message msg;
	ktime_t started;
	int ret;

	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);
	started = ktime_get();
	ret = spi_sync(priv->spi, &msg);
	dev_info(&priv->spi->dev,
		 "FPGA probe diag stage=%s tx=%u rx=%u len=%zu actual=%u ret=%d status=%d elapsed_us=%lld\n",
		 stage, !!tx, !!rx, len, msg.actual_length, ret, msg.status,
		 (long long)ktime_us_delta(ktime_get(), started));

	return ret;
}

static void amzn_spi_log_fpga_status(struct amzn_spi_priv *priv,
				     const struct dough_frame *rx_frame)
{
	const u8 *bytes = (const u8 *)rx_frame;
	size_t dump_len = min_t(size_t, sizeof(*rx_frame), 64);
	size_t nonzero = 0;
	size_t rev34_count = 0;
	int first_nonzero = -1;
	int first_rev34 = -1;
	size_t i;

	for (i = 0; i < sizeof(*rx_frame); i++) {
		if (bytes[i]) {
			nonzero++;
			if (first_nonzero < 0)
				first_nonzero = i;
		}
		if (bytes[i] == 34) {
			rev34_count++;
			if (first_rev34 < 0)
				first_rev34 = i;
		}
	}

	dev_info(&priv->spi->dev,
		 "FPGA status diag len=%zu nonzero=%zu first_nonzero=%d rev34_count=%zu first_rev34=%d fpga_rev=%u\n",
		 sizeof(*rx_frame), nonzero, first_nonzero, rev34_count,
		 first_rev34, rx_frame->dsf.fpga_rev);
	print_hex_dump(KERN_INFO, "amzn-fpga-status: ", DUMP_PREFIX_OFFSET,
		       16, 1, bytes, dump_len, false);
}

static bool amzn_spi_valid_fpga_rev(struct device *dev, u8 revision)
{
	if (revision < DOUGH_FPGA_REV_MIN ||
	    revision > DOUGH_FPGA_REV_MAX) {
		dev_err(dev, "unrecognized FPGA revision %u\n", revision);
		return false;
	}

	return true;
}

static void amzn_spi_set_running(struct amzn_spi_priv *priv, bool running)
{
	unsigned long flags;

	spin_lock_irqsave(&priv->state_lock, flags);
	priv->running = running;
	spin_unlock_irqrestore(&priv->state_lock, flags);
}

static bool amzn_spi_is_running(struct amzn_spi_priv *priv)
{
	unsigned long flags;
	bool running;

	spin_lock_irqsave(&priv->state_lock, flags);
	running = priv->running;
	spin_unlock_irqrestore(&priv->state_lock, flags);

	return running;
}

static unsigned long amzn_spi_ktime_diff_us(ktime_t lhs, ktime_t rhs)
{
	if (ktime_before(lhs, rhs))
		return SPI_READ_WAIT_MIN_US;

	return ktime_to_us(ktime_sub(lhs, rhs));
}

static void amzn_spi_data_read(struct work_struct *work)
{
	struct amzn_spi_priv *priv =
		container_of(work, struct amzn_spi_priv, spi_work);
	struct snd_pcm_substream *substream = priv->substream;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct dough_frame *tx_frame;
	struct dough_frame *rx_frame;
	u8 *timestamp_frame;
	unsigned long wakeup_min = ULONG_MAX;
	unsigned long wakeup_max = 0;
	unsigned long elapsed_us = 0;
	unsigned int flushed = 0;
	u32 previous_fpga_ts = 0;
	bool active_regdiag_dumped = false;
	ktime_t current_time;
	ktime_t previous_time;

	tx_frame = kzalloc(sizeof(*tx_frame), GFP_KERNEL | GFP_DMA);
	rx_frame = kzalloc(sizeof(*rx_frame), GFP_KERNEL | GFP_DMA);
	timestamp_frame = kmalloc(SPI_BYTES_PER_PERIOD, GFP_KERNEL);
	if (!tx_frame || !rx_frame || !timestamp_frame) {
		dev_err(&priv->spi->dev, "failed to allocate SPI frame buffers\n");
		goto out;
	}

	priv->elapsed = 0;
	priv->cur_write_offset = 0;
	priv->kernel_overruns = 0;
	priv->fpga_overruns = 0;
	amzn_spi_set_running(priv, true);
	sched_set_fifo(current);

	current_time = ktime_get_raw();
	previous_time = current_time;

	while (amzn_spi_is_running(priv)) {
		size_t frame_count;
		size_t bytes_left;
		size_t copied = 0;
		const u8 *source;
		int ret;

		ret = amzn_spi_txrx(priv, tx_frame, rx_frame,
				    sizeof(*rx_frame));
		if (ret) {
			dev_err(&priv->spi->dev, "SPI audio receive failed: %d\n",
				ret);
			break;
		}

		if (!active_regdiag_dumped) {
			/* Capture the controller state after a real audio transfer,
			 * not only after FPGA setup.  This is read-only diagnostic data
			 * for comparison with the 3.18 controller oracle. */
			amzn_spi_dump_regs(&priv->spi->dev);
			dev_info(&priv->spi->dev,
				 "FPGA active frame mode=%u frames=%u timestamp=%u overrun=%u rev=%u\n",
				 rx_frame->dsf.mode,
				 le16_to_cpu(rx_frame->dsf.num_audio_frames),
				 le32_to_cpu(rx_frame->dsf.timestamp_48mhz),
				 rx_frame->dsf.overrun,
				 rx_frame->dsf.fpga_rev);
			active_regdiag_dumped = true;
		}

		if (!amzn_spi_valid_fpga_rev(&priv->spi->dev,
					     rx_frame->dsf.fpga_rev))
			goto delay;

		if (rx_frame->dsf.overrun) {
			ktime_t after_spi = ktime_get_raw();
			unsigned long cycle_us;

			if (flushed < MAX_FLUSHED_CYCLES)
				goto delay;

			cycle_us = amzn_spi_ktime_diff_us(after_spi, previous_time);
			dev_err_ratelimited(&priv->spi->dev,
					    "FPGA overrun: mode=%u frames=%u timestamp=%u delta=%u cycle_us=%lu total=%zu\n",
					    rx_frame->dsf.mode,
					    le16_to_cpu(rx_frame->dsf.num_audio_frames),
					    le32_to_cpu(rx_frame->dsf.timestamp_48mhz),
					    le32_to_cpu(rx_frame->dsf.timestamp_48mhz) -
						previous_fpga_ts,
					    cycle_us,
					    ++priv->fpga_overruns);
		}

		if (!amzn_spi_is_running(priv))
			break;

		previous_fpga_ts =
			le32_to_cpu(rx_frame->dsf.timestamp_48mhz);
		frame_count = le16_to_cpu(rx_frame->dsf.num_audio_frames);
		if (frame_count > DOUGH_AUDIO_FRAME_BUF) {
			dev_err_ratelimited(&priv->spi->dev,
					    "invalid FPGA frame count %zu\n",
					    frame_count);
			goto delay;
		}

		if (transfer_timestamps) {
			/*
			 * Userspace expects one 27-byte pseudo audio frame carrying
			 * the 26-byte FPGA status header, followed by real frames.
			 * Build that representation explicitly instead of reading one
			 * byte past struct dough_frame at the maximum frame count.
			 */
			memset(timestamp_frame, 0, SPI_BYTES_PER_FRAME);
			memcpy(timestamp_frame, &rx_frame->dsf,
			       sizeof(rx_frame->dsf));
			memcpy(timestamp_frame + SPI_BYTES_PER_FRAME,
			       rx_frame->daf,
			       frame_count * SPI_BYTES_PER_FRAME);
			frame_count++;
			source = timestamp_frame;
		} else {
			source = (const u8 *)rx_frame->daf;
		}
		bytes_left = frame_count * SPI_BYTES_PER_FRAME;

		while (bytes_left) {
			unsigned long flags;
			size_t bytes;

			bytes = min(runtime->dma_bytes - priv->cur_write_offset,
				    bytes_left);
			memcpy(runtime->dma_area + priv->cur_write_offset,
			       source + copied, bytes);

			spin_lock_irqsave(&priv->write_lock, flags);
			priv->cur_write_offset =
				(priv->cur_write_offset + bytes) %
				runtime->dma_bytes;
			spin_unlock_irqrestore(&priv->write_lock, flags);

			bytes_left -= bytes;
			copied += bytes;
		}

		priv->elapsed += copied;
		while (priv->elapsed >= SPI_BYTES_PER_PERIOD) {
			priv->elapsed -= SPI_BYTES_PER_PERIOD;
			snd_pcm_period_elapsed(substream);
		}

delay:
		if (flushed < MAX_FLUSHED_CYCLES)
			flushed++;

		current_time = ktime_get_raw();
		elapsed_us = amzn_spi_ktime_diff_us(current_time,
						    previous_time);
		if (elapsed_us < SPI_READ_WAIT_MIN_US - MARGIN_US &&
		    !rx_frame->dsf.overrun) {
			usleep_range(SPI_READ_WAIT_MIN_US - elapsed_us,
				     SPI_READ_WAIT_MAX_US - elapsed_us);
			previous_time = ktime_get_raw();
		} else {
			previous_time = current_time;
		}

		wakeup_min = min(wakeup_min, elapsed_us);
		wakeup_max = max(wakeup_max, elapsed_us);
	}

out:
	kfree(timestamp_frame);
	kfree(rx_frame);
	kfree(tx_frame);
	amzn_spi_set_running(priv, false);
	dev_dbg(&priv->spi->dev, "capture stopped, cycle range %lu-%lu us\n",
		wakeup_min, wakeup_max);
}

static int amzn_spi_start_workqueue(struct amzn_spi_priv *priv,
				     struct snd_pcm_substream *substream)
{
	if (priv->spi_wq)
		return 0;

	priv->spi_wq = alloc_workqueue("amznspi",
				       WQ_HIGHPRI | WQ_MEM_RECLAIM, 1);
	if (!priv->spi_wq)
		return -ENOMEM;

	priv->cur_write_offset = 0;
	priv->elapsed = 0;
	priv->substream = substream;
	INIT_WORK(&priv->spi_work, amzn_spi_data_read);
	return 0;
}

static int amzn_mt_spi_pcm_open(struct snd_soc_component *component,
				struct snd_pcm_substream *substream)
{
	struct amzn_spi_priv *priv = snd_soc_component_get_drvdata(component);
	int ret;

	if (substream->stream != SNDRV_PCM_STREAM_CAPTURE)
		return -EINVAL;

	snd_soc_set_runtime_hwparams(substream, &amzn_mt_spi_pcm_hardware);
	ret = snd_pcm_hw_constraint_single(substream->runtime,
					   SNDRV_PCM_HW_PARAM_RATE,
					   SAMPLING_RATE);
	if (ret)
		return ret;

	ret = snd_pcm_hw_constraint_integer(substream->runtime,
					    SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret)
		return ret;

	return amzn_spi_start_workqueue(priv, substream);
}

static int amzn_mt_spi_pcm_close(struct snd_soc_component *component,
				 struct snd_pcm_substream *substream)
{
	struct amzn_spi_priv *priv = snd_soc_component_get_drvdata(component);

	amzn_spi_set_running(priv, false);
	if (priv->spi_wq) {
		destroy_workqueue(priv->spi_wq);
		priv->spi_wq = NULL;
	}
	priv->substream = NULL;

	return 0;
}

static int amzn_mt_spi_pcm_hw_params(struct snd_soc_component *component,
				     struct snd_pcm_substream *substream,
				     struct snd_pcm_hw_params *params)
{
	return snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(params));
}

static int amzn_mt_spi_pcm_hw_free(struct snd_soc_component *component,
				   struct snd_pcm_substream *substream)
{
	struct amzn_spi_priv *priv = snd_soc_component_get_drvdata(component);

	amzn_spi_set_running(priv, false);
	if (priv->spi_wq)
		flush_workqueue(priv->spi_wq);

	return snd_pcm_lib_free_pages(substream);
}

static int amzn_mt_spi_pcm_trigger(struct snd_soc_component *component,
				   struct snd_pcm_substream *substream, int cmd)
{
	struct amzn_spi_priv *priv = snd_soc_component_get_drvdata(component);

	dev_info(&priv->spi->dev,
		 "capture trigger cmd=%d stream=%d workqueue=%s\n", cmd,
		 substream->stream, priv->spi_wq ? "present" : "missing");

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		if (!priv->spi_wq)
			return -EPIPE;
		queue_work(priv->spi_wq, &priv->spi_work);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		amzn_spi_set_running(priv, false);
		return 0;
	default:
		return -EINVAL;
	}
}

static snd_pcm_uframes_t
amzn_mt_spi_pcm_pointer(struct snd_soc_component *component,
			struct snd_pcm_substream *substream)
{
	struct amzn_spi_priv *priv = snd_soc_component_get_drvdata(component);
	unsigned long flags;
	snd_pcm_uframes_t frames;

	spin_lock_irqsave(&priv->write_lock, flags);
	frames = bytes_to_frames(substream->runtime, priv->cur_write_offset);
	spin_unlock_irqrestore(&priv->write_lock, flags);

	if (frames >= substream->runtime->buffer_size)
		frames = 0;

	return frames;
}

static int amzn_mt_spi_pcm_copy_user(struct snd_soc_component *component,
				     struct snd_pcm_substream *substream,
				     int channel, unsigned long pos,
				     void __user *dst, unsigned long bytes)
{
	struct amzn_spi_priv *priv = snd_soc_component_get_drvdata(component);
	struct snd_pcm_runtime *runtime = substream->runtime;
	size_t end = pos + bytes;

	if (pos >= runtime->dma_bytes || bytes > runtime->dma_bytes - pos)
		return -EINVAL;

	if (priv->cur_write_offset > pos && priv->cur_write_offset < end) {
		priv->kernel_overruns++;
		dev_err_ratelimited(&priv->spi->dev,
				    "kernel capture overrun, total=%zu\n",
				    priv->kernel_overruns);
	}

	if (copy_to_user(dst, runtime->dma_area + pos, bytes))
		return -EFAULT;

	return 0;
}

static int amzn_mt_spi_pcm_construct(struct snd_soc_component *component,
				     struct snd_soc_pcm_runtime *rtd)
{
	int ret;

	if (!component->dev->dma_mask)
		component->dev->dma_mask = &component->dev->coherent_dma_mask;
	ret = dma_set_mask_and_coherent(component->dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(component->dev, ret,
				     "failed to configure PCM DMA mask\n");

	snd_pcm_set_managed_buffer_all(rtd->pcm, SNDRV_DMA_TYPE_DEV,
				       component->dev, SPI_DMA_BYTES_MAX,
				       SPI_DMA_BYTES_MAX);
	return 0;
}

static const struct snd_soc_component_driver amzn_mt_spi_component = {
	.name = AMZN_MT_SPI_PCM,
	.controls = amzn_mt_spi_controls,
	.num_controls = ARRAY_SIZE(amzn_mt_spi_controls),
	.open = amzn_mt_spi_pcm_open,
	.close = amzn_mt_spi_pcm_close,
	.hw_params = amzn_mt_spi_pcm_hw_params,
	.hw_free = amzn_mt_spi_pcm_hw_free,
	.trigger = amzn_mt_spi_pcm_trigger,
	.pointer = amzn_mt_spi_pcm_pointer,
	.copy_user = amzn_mt_spi_pcm_copy_user,
	.pcm_construct = amzn_mt_spi_pcm_construct,
	.use_dai_pcm_id = true,
	.legacy_dai_naming = 1,
};

static int amzn_mt_spi_dai_startup(struct snd_pcm_substream *substream,
				   struct snd_soc_dai *dai)
{
	return amzn_spi_start_workqueue(snd_soc_dai_get_drvdata(dai), substream);
}

static const struct snd_soc_dai_ops amzn_mt_spi_dai_ops = {
	.startup = amzn_mt_spi_dai_startup,
};

static struct snd_soc_dai_driver amzn_mt_spi_dai = {
	.name = AMZN_MT_SPI_PCM,
	.ops = &amzn_mt_spi_dai_ops,
	.capture = {
		.stream_name = "SPI Capture",
		.channels_min = SPI_N_CHANNELS,
		.channels_max = SPI_N_CHANNELS,
		.rates = SNDRV_PCM_RATE_16000,
		.formats = SNDRV_PCM_FMTBIT_S24_3LE,
	},
};

static int amzn_mt_spi_load_fpga(struct amzn_spi_priv *priv)
{
	const struct firmware *firmware;
	struct dough_frame *tx_frame;
	struct dough_frame *rx_frame;
	size_t transfer_bytes;
	u8 setup[SPI_SETUP_BUF_SIZE] = {};
	u8 *firmware_buf;
	int ret;

	tx_frame = kzalloc(sizeof(*tx_frame), GFP_KERNEL | GFP_DMA);
	rx_frame = kzalloc(sizeof(*rx_frame), GFP_KERNEL | GFP_DMA);
	firmware_buf = kzalloc(FIRMWARE_MAX_BYTES, GFP_KERNEL | GFP_DMA);
	if (!tx_frame || !rx_frame || !firmware_buf) {
		ret = -ENOMEM;
		goto out;
	}

	setup[0] = dough_fw_off;
	ret = amzn_spi_probe_txrx(priv, setup, NULL, sizeof(setup), "off");
	if (ret)
		goto out;

	ret = gpiod_direction_output(priv->reset_gpio, 0);
	if (ret) {
		dev_err(&priv->spi->dev, "failed to configure FPGA reset GPIO: %d\n",
			ret);
		goto out;
	}

	ret = pinctrl_select_state(priv->pinctrl, priv->i2s_mode0);
	if (ret) {
		dev_err(&priv->spi->dev, "failed to select audi2s1-mode0: %d\n",
			ret);
		goto out;
	}

	gpiod_set_value_cansleep(priv->reset_gpio, 0);
	msleep(FPGA_DELAY_MS);
	gpiod_set_value_cansleep(priv->reset_gpio, 1);
	msleep(FPGA_DELAY_MS);

	ret = request_firmware(&firmware, FPGA_FIRMWARE_NAME, &priv->spi->dev);
	if (ret) {
		dev_err(&priv->spi->dev, "failed to request FPGA firmware %s: %d\n",
			FPGA_FIRMWARE_NAME, ret);
		goto out;
	}

	transfer_bytes = roundup(firmware->size, 1024) + 1024;
	if (transfer_bytes > FIRMWARE_MAX_BYTES) {
		dev_err(&priv->spi->dev, "FPGA firmware is too large\n");
		ret = -EFBIG;
		goto release_firmware;
	}

	memcpy(firmware_buf, firmware->data, firmware->size);
	if (transfer_bytes - firmware->size == sizeof(amzn_fpga_oracle_tail)) {
		/* Experimental compatibility test against the exact 3.18 oracle
		 * transfer.  The legacy driver over-read adjacent .rodata here;
		 * use the recovered bytes explicitly rather than reproducing UB. */
		memcpy(firmware_buf + firmware->size, amzn_fpga_oracle_tail,
		       sizeof(amzn_fpga_oracle_tail));
		dev_info(&priv->spi->dev,
			 "FPGA probe diag using recovered oracle tail bytes=%zu\n",
			 sizeof(amzn_fpga_oracle_tail));
	} else if (transfer_bytes > firmware->size) {
		memset(firmware_buf + firmware->size, 0,
		       transfer_bytes - firmware->size);
	}
	dev_info(&priv->spi->dev,
		 "FPGA probe diag firmware_size=%zu transfer_bytes=%zu reset=%d\n",
		 firmware->size, transfer_bytes,
		 gpiod_get_value_cansleep(priv->reset_gpio));
	ret = amzn_spi_probe_txrx(priv, firmware_buf, NULL, transfer_bytes,
				    "firmware");
	if (ret)
		goto release_firmware;

	msleep(FPGA_DELAY_MS);
	/* Retry the status read with progressively longer delays: the FPGA may
	 * need extra time after the firmware load before it drives MISO. */
	{
		int attempt;
		static const int retry_delay_ms[] = { 100, 250, 500, 1000 };

		for (attempt = 0; attempt < ARRAY_SIZE(retry_delay_ms); attempt++) {
			if (attempt)
				msleep(retry_delay_ms[attempt]);
			ret = amzn_spi_probe_txrx(priv, tx_frame, rx_frame,
						  sizeof(*rx_frame), "status");
			if (ret)
				goto release_firmware;
			dev_info(&priv->spi->dev,
				 "FPGA status attempt=%d rev=%u\n", attempt,
				 rx_frame->dsf.fpga_rev);
			if (amzn_spi_valid_fpga_rev(&priv->spi->dev,
						    rx_frame->dsf.fpga_rev))
				break;
		}
	}
	amzn_spi_log_fpga_status(priv, rx_frame);
	amzn_spi_dump_regs(&priv->spi->dev);
	if (!amzn_spi_valid_fpga_rev(&priv->spi->dev,
				     rx_frame->dsf.fpga_rev)) {
		ret = -EINVAL;
		goto release_firmware;
	}

	dev_info(&priv->spi->dev, "FPGA revision %u\n",
		 rx_frame->dsf.fpga_rev);

	ret = pinctrl_select_state(priv->pinctrl, priv->mclk);
	if (ret) {
		dev_err(&priv->spi->dev, "failed to select cmmclk-mclk: %d\n",
			ret);
		goto release_firmware;
	}

	ret = pinctrl_select_state(priv->pinctrl, priv->i2s_mode1);
	if (ret) {
		dev_err(&priv->spi->dev, "failed to select audi2s1-mode1: %d\n",
			ret);
		goto release_firmware;
	}
	msleep(PINCTRL_DELAY_MS);

	memset(setup, 0, sizeof(setup));
	setup[0] = dough_fw_i2s;
	ret = amzn_spi_txrx(priv, setup, NULL, sizeof(setup));

release_firmware:
	release_firmware(firmware);
out:
	kfree(firmware_buf);
	kfree(rx_frame);
	kfree(tx_frame);
	return ret;
}

static int amzn_mt_spi_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct amzn_spi_priv *priv;
	int ret;

	dev_info(dev, "Radar-Puffin FPGA SPI probe entered\n");
	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->spi = spi;
	spin_lock_init(&priv->state_lock);
	spin_lock_init(&priv->write_lock);
	spi_set_drvdata(spi, priv);

	priv->reset_gpio = devm_gpiod_get(dev, NULL, GPIOD_ASIS);
	if (IS_ERR(priv->reset_gpio)) {
		ret = PTR_ERR(priv->reset_gpio);
		dev_err(dev, "failed to get FPGA reset GPIO: %d\n", ret);
		return ret;
	}
	dev_info(dev, "FPGA reset GPIO acquired\n");

	priv->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(priv->pinctrl)) {
		ret = PTR_ERR(priv->pinctrl);
		dev_err(dev, "failed to get audio pinctrl: %d\n", ret);
		return ret;
	}

	priv->i2s_mode0 = pinctrl_lookup_state(priv->pinctrl,
					       "audi2s1-mode0");
	if (IS_ERR(priv->i2s_mode0)) {
		ret = PTR_ERR(priv->i2s_mode0);
		dev_err(dev, "missing audi2s1-mode0 pinctrl state: %d\n", ret);
		return ret;
	}

	priv->i2s_mode1 = pinctrl_lookup_state(priv->pinctrl,
					       "audi2s1-mode1");
	if (IS_ERR(priv->i2s_mode1)) {
		ret = PTR_ERR(priv->i2s_mode1);
		dev_err(dev, "missing audi2s1-mode1 pinctrl state: %d\n", ret);
		return ret;
	}

	priv->mclk = pinctrl_lookup_state(priv->pinctrl, "cmmclk-mclk");
	if (IS_ERR(priv->mclk)) {
		ret = PTR_ERR(priv->mclk);
		dev_err(dev, "missing cmmclk-mclk pinctrl state: %d\n", ret);
		return ret;
	}
	dev_info(dev, "FPGA audio pinctrl states acquired\n");

	spi->mode = SPI_MODE_3;
	spi->bits_per_word = 8;
	spi->max_speed_hz = SPI_SPEED_HZ;
	ret = spi_setup(spi);
	if (ret) {
		dev_err(dev, "SPI setup failed: %d\n", ret);
		return ret;
	}

	dev_info(dev, "starting FPGA initialization\n");
	ret = amzn_mt_spi_load_fpga(priv);
	if (ret) {
		dev_err(dev, "FPGA initialization failed: %d\n", ret);
		return ret;
	}
	dev_info(dev, "FPGA initialization complete\n");

	/*
	 * The SPI child owns the shared audio pins only while bootstrapping the
	 * FPGA.  Hand them to the AFE after the final I2S state is programmed;
	 * otherwise the AFE cannot claim I2S_DATA_IN and its component never
	 * registers.
	 */
	devm_pinctrl_put(priv->pinctrl);
	priv->pinctrl = NULL;
	dev_info(dev, "FPGA boot pinctrl ownership handed to AFE\n");

	ret = devm_snd_soc_register_component(dev, &amzn_mt_spi_component,
					       &amzn_mt_spi_dai, 1);
	dev_info(dev, "FPGA SPI component registration returned %d\n", ret);
	return ret;
}

static const struct of_device_id amzn_mt_spi_of_match[] = {
	{ .compatible = "amzn-mtk,spi-audio-pltfm" },
	{ }
};
MODULE_DEVICE_TABLE(of, amzn_mt_spi_of_match);

static struct spi_driver amzn_mt_spi_driver = {
	.driver = {
		.name = "amzn-mt8163-spi-audio",
		.of_match_table = amzn_mt_spi_of_match,
	},
	.probe = amzn_mt_spi_probe,
};
module_spi_driver(amzn_mt_spi_driver);

MODULE_FIRMWARE(FPGA_FIRMWARE_NAME);
MODULE_AUTHOR("Amazon Lab126 Inc.");
MODULE_DESCRIPTION("Amazon Radar-Puffin FPGA SPI audio capture driver");
MODULE_LICENSE("GPL");
