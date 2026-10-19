/******************************************************************************
    SPAZZbs - music analysis for the visualizer
    Copyright (C) 2026 SPAZZbs contributors

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "spazz-analyzer.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define BAND_MIN_HZ 35.0
#define BAND_MAX_HZ 16000.0
#define DB_RANGE 48.0f

static inline float clamp01(float v)
{
	return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static inline float approach(float cur, float target, float rate, float dt)
{
	float k = rate * dt;
	if (k > 1.0f)
		k = 1.0f;
	return cur + (target - cur) * k;
}

void spazz_analyzer_init(struct spazz_analyzer *a, uint32_t sample_rate)
{
	memset(a, 0, sizeof(*a));
	a->sample_rate = sample_rate ? sample_rate : 48000;
	a->loudness_ref = -40.0f;

	const int n = SPAZZ_FFT_SIZE;
	for (int i = 0; i < n; i++)
		a->window[i] = (float)(0.5 - 0.5 * cos(2.0 * M_PI * i / (n - 1)));

	for (int i = 0; i < n / 2; i++) {
		a->cos_table[i] = (float)cos(2.0 * M_PI * i / n);
		a->sin_table[i] = (float)-sin(2.0 * M_PI * i / n);
	}

	double fmax = BAND_MAX_HZ;
	if (fmax > a->sample_rate * 0.45)
		fmax = a->sample_rate * 0.45;

	const double bin_hz = (double)a->sample_rate / n;
	int prev_hi = 1;
	for (int b = 0; b < SPAZZ_NUM_BANDS; b++) {
		double f_lo = BAND_MIN_HZ * pow(fmax / BAND_MIN_HZ, (double)b / SPAZZ_NUM_BANDS);
		double f_hi = BAND_MIN_HZ * pow(fmax / BAND_MIN_HZ, (double)(b + 1) / SPAZZ_NUM_BANDS);

		int lo = (int)floor(f_lo / bin_hz);
		int hi = (int)ceil(f_hi / bin_hz);

		/* low bands are narrower than one bin; let them share */
		if (lo < 1)
			lo = 1;
		if (lo > prev_hi)
			lo = prev_hi;
		if (hi <= lo)
			hi = lo + 1;
		if (hi > n / 2)
			hi = n / 2;
		if (lo >= hi)
			lo = hi - 1;

		a->band_lo[b] = lo;
		a->band_hi[b] = hi;
		prev_hi = hi;

		/* music has much less energy up high: tilt +3 dB/octave around 1 kHz */
		double fc = sqrt(f_lo * f_hi);
		a->band_tilt[b] = (float)(3.0 * log2(fc / 1000.0));
	}
}

void spazz_analyzer_push(struct spazz_analyzer *a, const float *mono, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		a->ring[a->ring_pos] = mono[i];
		a->ring_pos = (a->ring_pos + 1) % SPAZZ_RING_SIZE;
	}
}

void spazz_analyzer_push_silence(struct spazz_analyzer *a, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		a->ring[a->ring_pos] = 0.0f;
		a->ring_pos = (a->ring_pos + 1) % SPAZZ_RING_SIZE;
	}
}

static void fft(struct spazz_analyzer *a)
{
	const int n = SPAZZ_FFT_SIZE;
	float *re = a->re;
	float *im = a->im;

	/* bit reversal */
	for (int i = 1, j = 0; i < n; i++) {
		int bit = n >> 1;
		for (; j & bit; bit >>= 1)
			j ^= bit;
		j ^= bit;
		if (i < j) {
			float t = re[i];
			re[i] = re[j];
			re[j] = t;
			t = im[i];
			im[i] = im[j];
			im[j] = t;
		}
	}

	for (int len = 2; len <= n; len <<= 1) {
		int half = len >> 1;
		int step = n / len;
		for (int i = 0; i < n; i += len) {
			for (int k = 0; k < half; k++) {
				float wr = a->cos_table[k * step];
				float wi = a->sin_table[k * step];
				int u = i + k;
				int v = u + half;
				float xr = re[v] * wr - im[v] * wi;
				float xi = re[v] * wi + im[v] * wr;
				re[v] = re[u] - xr;
				im[v] = im[u] - xi;
				re[u] += xr;
				im[u] += xi;
			}
		}
	}
}

void spazz_analyzer_update(struct spazz_analyzer *a, float dt, float sensitivity)
{
	const int n = SPAZZ_FFT_SIZE;

	if (dt <= 0.0f)
		dt = 1.0f / 60.0f;
	if (dt > 0.25f)
		dt = 0.25f;
	if (sensitivity < 0.1f)
		sensitivity = 0.1f;

	size_t start = (a->ring_pos + SPAZZ_RING_SIZE - n) % SPAZZ_RING_SIZE;
	for (int i = 0; i < n; i++) {
		a->re[i] = a->ring[(start + i) % SPAZZ_RING_SIZE] * a->window[i];
		a->im[i] = 0.0f;
	}

	fft(a);

	/* a full-scale sine reads 1.0 (Hann window halves the peak) */
	const float norm = 4.0f / n;
	float db[SPAZZ_NUM_BANDS];
	float max_db = -200.0f;

	for (int b = 0; b < SPAZZ_NUM_BANDS; b++) {
		float peak = 0.0f;
		float power = 0.0f;
		int count = 0;
		for (int k = a->band_lo[b]; k < a->band_hi[b]; k++) {
			float m = sqrtf(a->re[k] * a->re[k] + a->im[k] * a->im[k]) * norm;
			if (m > peak)
				peak = m;
			power += m * m;
			count++;
		}
		float rms = count ? sqrtf(power / count) : 0.0f;
		float v = 0.5f * peak + 0.5f * rms;
		db[b] = 20.0f * log10f(v + 1e-7f) + a->band_tilt[b];
		if (db[b] > max_db)
			max_db = db[b];
	}

	/* follow the loudest band so quiet and loud songs both fill the range */
	a->loudness_ref -= 8.0f * dt;
	if (max_db > a->loudness_ref)
		a->loudness_ref = max_db;
	if (a->loudness_ref < -40.0f)
		a->loudness_ref = -40.0f;

	const float top = a->loudness_ref + 2.0f;
	const float bottom = top - DB_RANGE;
	const float contrast = 1.8f / sensitivity;

	float sum = 0.0f, bass = 0.0f, mid = 0.0f, treble = 0.0f;
	float low_flux = 0.0f;

	for (int b = 0; b < SPAZZ_NUM_BANDS; b++) {
		float x = clamp01((db[b] - bottom) / DB_RANGE);
		x = clamp01(powf(x, contrast));

		/* how far the low end jumps above where it just was (drum hits);
		 * compared to a fast average so it works at any frame rate */
		if (b < 12 && x > a->prev_raw[b])
			low_flux += x - a->prev_raw[b];
		a->prev_raw[b] = approach(a->prev_raw[b], x, 12.0f, dt);

		float cur = a->bands[b];
		cur = approach(cur, x, x > cur ? 28.0f : 7.0f, dt);
		a->bands[b] = cur;

		/* onset flash: jump above the slow average */
		a->band_slow[b] = approach(a->band_slow[b], x, 2.5f, dt);
		float onset = clamp01((x - a->band_slow[b]) * 2.5f);
		a->flash[b] -= 3.0f * dt;
		if (onset > a->flash[b])
			a->flash[b] = onset;
		if (a->flash[b] < 0.0f)
			a->flash[b] = 0.0f;

		/* falling peak caps */
		if (cur >= a->peaks[b]) {
			a->peaks[b] = cur;
			a->peak_hold[b] = 0.35f;
		} else if (a->peak_hold[b] > 0.0f) {
			a->peak_hold[b] -= dt;
		} else {
			a->peaks[b] -= 0.55f * dt;
			if (a->peaks[b] < cur)
				a->peaks[b] = cur;
		}

		sum += cur;
		if (b < 10)
			bass += cur;
		else if (b < 38)
			mid += cur;
		else
			treble += cur;
	}

	a->bass = bass / 10.0f;
	a->mid = mid / 28.0f;
	a->treble = treble / (SPAZZ_NUM_BANDS - 38);
	a->level = sum / SPAZZ_NUM_BANDS;

	/* beat: the low end jumps much more than it usually does */
	low_flux /= 12.0f;
	a->beat_cooldown -= dt;
	a->beat *= expf(-4.0f * dt);
	if (a->beat_cooldown <= 0.0f && low_flux > a->flux_avg * 1.5f + 0.045f) {
		a->beat = 1.0f;
		a->beat_cooldown = 0.2f;
	}
	a->flux_avg = approach(a->flux_avg, low_flux, 2.0f, dt);
}
