/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __Q6APM_VOICE_BACKEND_H__
#define __Q6APM_VOICE_BACKEND_H__

#include <linux/cleanup.h>
#include <linux/firmware.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <sound/soc.h>

#define Q6APM_VOICE_RX_LINK "Voice Playback"
#define Q6APM_VOICE_TX_LINK "Voice Capture"

/*
 * A board has modem calls when its voice profile, the firmware with the graph
 * payloads and ordered commands generated from its stock ACDB, is installed:
 * qcom/<driver>/<card>-voice.bin, named as the card's topology. The profile's
 * header says what the board's voice links need.
 */
#define Q6APM_VOICE_PROFILE "qcom/%s/%s-voice.bin"
#define Q6APM_VOICE_PROFILE_MAGIC 0x4153564d /* MVSA */
#define Q6APM_VOICE_PROFILE_VERSION 2
#define Q6APM_VOICE_PROFILE_HEADER 40

struct q6apm_voice_board {
	/* Backend links the private voice links copy */
	unsigned int rx_port, tx_port;
	unsigned int rx_rate;
	snd_pcm_format_t rx_format;
	/* Decimators the TX graph's codec DMA endpoint reads */
	unsigned int tx_decimators;
	/* CAL_KEYS volume levels the ACDB calibrates; index 0 is loudest */
	unsigned int volume_levels;
};

/*
 * Header: magic, version, graph part size, command count, then the board:
 * RX port, TX port, RX rate, RX sample width, TX decimators, volume levels.
 */
static inline int q6apm_voice_board_read(const struct firmware *profile,
					 struct q6apm_voice_board *board)
{
	const u8 *data = profile->data;

	if (profile->size < Q6APM_VOICE_PROFILE_HEADER ||
	    get_unaligned_le32(data) != Q6APM_VOICE_PROFILE_MAGIC ||
	    get_unaligned_le32(data + 4) != Q6APM_VOICE_PROFILE_VERSION)
		return -EINVAL;
	board->rx_port = get_unaligned_le32(data + 16);
	board->tx_port = get_unaligned_le32(data + 20);
	board->rx_rate = get_unaligned_le32(data + 24);
	switch (get_unaligned_le32(data + 28)) {
	case 16:
		board->rx_format = SNDRV_PCM_FORMAT_S16_LE;
		break;
	case 24:
		board->rx_format = SNDRV_PCM_FORMAT_S24_LE;
		break;
	case 32:
		board->rx_format = SNDRV_PCM_FORMAT_S32_LE;
		break;
	default:
		return -EINVAL;
	}
	board->tx_decimators = get_unaligned_le32(data + 32);
	board->volume_levels = get_unaligned_le32(data + 36);
	if (!board->rx_rate || !board->tx_decimators ||
	    !board->volume_levels || board->volume_levels > 32)
		return -EINVAL;
	return 0;
}

/* The card's voice board; -ENOENT when no profile is installed. */
static inline int q6apm_voice_board_load(struct snd_soc_card *card,
					 struct q6apm_voice_board *board)
{
	char *name __free(kfree) = kasprintf(GFP_KERNEL, Q6APM_VOICE_PROFILE,
					     card->driver_name, card->name);
	const struct firmware *profile __free(firmware) = NULL;

	if (!name)
		return -ENOMEM;
	if (firmware_request_nowarn(&profile, name, card->dev))
		return -ENOENT;
	return q6apm_voice_board_read(profile, board);
}

static inline bool q6apm_voice_backend_link(const struct snd_soc_pcm_runtime *rtd)
{
	return !strcmp(rtd->dai_link->name, Q6APM_VOICE_RX_LINK) ||
		!strcmp(rtd->dai_link->name, Q6APM_VOICE_TX_LINK);
}

int q6apm_voice_backend_open(struct snd_soc_component *component,
			     const struct q6apm_voice_board *board, unsigned int graph,
			     struct snd_pcm_substream **stream);
void q6apm_voice_backend_close(struct snd_pcm_substream **stream);

#endif
