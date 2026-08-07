/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __SND_SOC_AMZN_MT8163_DOUGH_H
#define __SND_SOC_AMZN_MT8163_DOUGH_H

#include <linux/types.h>

#define DOUGH_AUDIO_SAMPLE_WIDTH	3
#define DOUGH_AUDIO_NUM_CHANNELS	9
#define DOUGH_AUDIO_FRAME_BUF		255
#define DOUGH_AUDIO_FRAME_BYTES		(DOUGH_AUDIO_SAMPLE_WIDTH * \
					 DOUGH_AUDIO_NUM_CHANNELS)
#define DOUGH_FPGA_REV_MIN		30
#define DOUGH_FPGA_REV_MAX		251

enum dough_fw_cmd {
	dough_fw_nop = 0,
	dough_fw_off = 0x80,
	dough_fw_i2s = 0x81,
	dough_fw_tpg = 0x83,
};

struct dough_audio_frame {
	u8 audio_data[DOUGH_AUDIO_FRAME_BYTES];
};

struct dough_status_frame {
	u8 rsvd0[15];
	__le32 timestamp_48mhz;
	__le16 num_audio_frames;
	u8 rsvd1;
	u8 mode;
	u8 dac_inactive;
	u8 i2s_inactive;
	u8 overrun;
	u8 fpga_rev;
} __packed;

struct dough_frame {
	struct dough_status_frame dsf;
	struct dough_audio_frame daf[DOUGH_AUDIO_FRAME_BUF];
} __packed;

#endif
