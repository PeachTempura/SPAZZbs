/******************************************************************************
    SPAZZbs - music analysis for the visualizer
    Copyright (C) 2026 SPAZZbs contributors

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPAZZ_FFT_SIZE 2048
#define SPAZZ_RING_SIZE 8192
#define SPAZZ_NUM_BANDS 64

/* Pure analysis state: no OBS types, so it can be tested on its own.
 * Feed samples with spazz_analyzer_push() (any thread, caller locks),
 * then call spazz_analyzer_update() once per video frame. */
struct spazz_analyzer {
	/* input */
	float ring[SPAZZ_RING_SIZE];
	size_t ring_pos;
	uint32_t sample_rate;

	/* fft scratch */
	float window[SPAZZ_FFT_SIZE];
	float re[SPAZZ_FFT_SIZE];
	float im[SPAZZ_FFT_SIZE];
	float cos_table[SPAZZ_FFT_SIZE / 2];
	float sin_table[SPAZZ_FFT_SIZE / 2];
	int band_lo[SPAZZ_NUM_BANDS];
	int band_hi[SPAZZ_NUM_BANDS];
	float band_tilt[SPAZZ_NUM_BANDS];

	/* outputs, all 0..1 */
	float bands[SPAZZ_NUM_BANDS];
	float peaks[SPAZZ_NUM_BANDS];
	float flash[SPAZZ_NUM_BANDS];
	float bass, mid, treble, level, beat;

	/* follow-up state */
	float band_slow[SPAZZ_NUM_BANDS];
	float peak_hold[SPAZZ_NUM_BANDS];
	float loudness_ref;
	float prev_raw[SPAZZ_NUM_BANDS];
	float flux_avg;
	float beat_cooldown;
};

void spazz_analyzer_init(struct spazz_analyzer *a, uint32_t sample_rate);
void spazz_analyzer_push(struct spazz_analyzer *a, const float *mono, size_t count);
void spazz_analyzer_push_silence(struct spazz_analyzer *a, size_t count);

/* sensitivity: 1.0 is normal, higher makes everything react more */
void spazz_analyzer_update(struct spazz_analyzer *a, float dt, float sensitivity);

#ifdef __cplusplus
}
#endif
