/******************************************************************************
    SPAZZbs - "SPAZZ CRT": a box that shows everything under it through a
    simulated CRT / LCD tube. Port of the Phosphor tube renderer.
    Copyright (C) 2026 SPAZZbs contributors

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

/*
 * How it works
 * ------------
 * The source is drawn like any other box in a scene, but instead of drawing
 * its own picture it looks at what has already been drawn underneath it:
 *
 *   1. work out where the box's corners land in the current render target
 *      (world matrix x projection x viewport),
 *   2. copy just that part of the render target (gs_copy_from_target),
 *   3. resample it to the tube's line count  -> "page"      (Capture)
 *   4. run Phosphor's pipeline on it: Signal -> Persist -> Beam -> Bloom/Halo
 *   5. draw the Final pass into the scene over the box.
 *
 * The same source can be drawn several times per frame (program, Studio Mode
 * preview, multiview tiles). Each differently sized appearance gets its own
 * "view" with its own targets, and the afterglow feedback only advances once
 * per frame per view.
 */

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/matrix4.h>
#include <graphics/vec4.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/bmem.h>
#include <util/dstr.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spazz-crt-params.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("spazz-crt", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "SPAZZbs CRT tube box";
}

#define T_(s) obs_module_text(s)

#define MAX_VIEWS 4
#define VIEW_EXPIRE_FRAMES 180
#define MIN_DIM 16u
#define MAX_BEAM 4096u
#define MAX_PAGE_H 2160u
#define MAX_PAGE_W 4096u

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------------ */
/* effect uniforms                                                          */

#define CRT_UNIFORMS(X)                                                                                         \
	X(image) X(image2) X(image3) X(in_size) X(out_size) X(uTime) X(uFrame) X(box_o) X(box_x) X(box_y)       \
	X(cap_valid) X(lin_canvas) X(uLumaBlur) X(uChromaBlur) X(uChromaShift) X(uArtifacts) X(uGhost)          \
	X(uGhostOff) X(uRinging) X(uNoise) X(uInterf) X(uJitter) X(uWobble) X(uTracking) X(uDropouts)           \
	X(uRollOff) X(uStatic) X(uBright) X(uContrast) X(uSat) X(uHue) X(uBlack) X(uGammaIn) X(uTemp)           \
	X(uMono) X(uMonoLevels) X(uMonoDark) X(uMonoLight) X(uDecay) X(uMode) X(uTint) X(uRect) X(uCurv)        \
	X(uOverscan) X(uPower) X(uScan) X(uBeamMin) X(uBeamMax) X(uBeamShape) X(uSharp) X(uInterlace)          \
	X(uAntiMoire) X(uConvR) X(uConvB) X(uConvEdge) X(uDegauss) X(uMaskType) X(uMaskStr) X(uMaskSize)        \
	X(uMaskCurve) X(uMaskBoost) X(uLcdGap) X(uLcdGapLight) X(uLcdSub) X(uLcdBGR) X(uLcdShadow)              \
	X(uLcdShadowOff) X(uGapColor) X(uInTexel) X(uThresh) X(uDir) X(uCorner) X(uEdgeSoft)                    \
	X(uBezelReflect) X(uBloomAmt) X(uHalation) X(uVignette) X(uAmbient) X(uGlare) X(uFlicker)               \
	X(uLcdBleed) X(uTN) X(uGammaOut) X(uPowerGain) X(uBezelColor) X(uClearOutside)

struct crt_uniforms {
#define X(n) gs_eparam_t *n;
	CRT_UNIFORMS(X)
#undef X
};

static inline void setf(gs_eparam_t *p, float v)
{
	if (p)
		gs_effect_set_float(p, v);
}

static inline void set2(gs_eparam_t *p, float x, float y)
{
	if (p) {
		struct vec2 v;
		vec2_set(&v, x, y);
		gs_effect_set_vec2(p, &v);
	}
}

static inline void set3(gs_eparam_t *p, const float c[3])
{
	if (p) {
		struct vec3 v;
		vec3_set(&v, c[0], c[1], c[2]);
		gs_effect_set_vec3(p, &v);
	}
}

static inline void set4(gs_eparam_t *p, const float c[4])
{
	if (p) {
		struct vec4 v;
		vec4_set(&v, c[0], c[1], c[2], c[3]);
		gs_effect_set_vec4(p, &v);
	}
}

static inline void settex(gs_eparam_t *p, gs_texrender_t *tr)
{
	if (p)
		gs_effect_set_texture(p, gs_texrender_get_texture(tr));
}

/* ------------------------------------------------------------------------ */
/* data                                                                      */

/* everything the renderer needs from the settings, groups already applied */
struct crt_vals {
	double v[CP_COUNT];
	double aspect; /* width / height, 0 = fill the box */
	int lines;     /* 0 = native */
	int fps;
	int mask_type;
	bool clear_outside;
	bool off_passthrough;
};

struct crt_view {
	bool used;
	uint32_t bw, bh; /* beam (screen) resolution */
	uint32_t pw, ph; /* page (picture) resolution */
	gs_texrender_t *page, *signal, *persist[2], *persist_tmp, *beam, *bloom_a, *bloom_b, *halo_a, *halo_b;
	int persist_idx;
	int frame_prev_idx;
	bool page_valid;
	bool persist_clear;
	uint64_t last_frame;
	double last_capture;
	float rect[4];
};

enum crt_on_show { SHOW_NOTHING, SHOW_POWER_ON, SHOW_STATIC };

struct spazz_crt {
	obs_source_t *source;
	gs_effect_t *effect;
	struct crt_uniforms u;

	/* settings, written by update() on the UI thread */
	pthread_mutex_t mutex;
	struct crt_vals pending;
	volatile long width, height;
	volatile long on_show;
	volatile bool sounds;

	/* requests from hotkeys, buttons and activate() */
	volatile bool req_power, req_static, req_degauss, req_power_on;

	/* animation state, graphics thread only */
	struct crt_vals P;
	double clock;
	float time, dt, roll_off, flicker;
	uint64_t frame;
	float degauss_t, static_t, power_t;
	int power_target;
	uint32_t roll_h;

	/* render state, graphics thread only */
	struct crt_view views[MAX_VIEWS];
	struct crt_view *last_view;
	gs_texture_t *backdrop;
	uint32_t back_w, back_h;
	enum gs_color_format back_fmt;
	bool warned_effect;

	/* sound */
	uint32_t sample_rate;
	float *mix;
	size_t mix_len, mix_cap;
	uint64_t next_ts;
	bool streaming;
	uint32_t rng;
};

/* ------------------------------------------------------------------------ */
/* helpers                                                                   */

static inline float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static inline uint32_t clampu(uint32_t v, uint32_t lo, uint32_t hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static uint32_t hex_from_obs_color(long long c)
{
	uint32_t r = (uint32_t)c & 0xFF, g = ((uint32_t)c >> 8) & 0xFF, b = ((uint32_t)c >> 16) & 0xFF;
	return (r << 16) | (g << 8) | b;
}

static long long obs_color_from_hex(uint32_t hex)
{
	uint32_t r = (hex >> 16) & 0xFF, g = (hex >> 8) & 0xFF, b = hex & 0xFF;
	return (long long)(0xFF000000u | (b << 16) | (g << 8) | r);
}

static void rgb_from_hex(uint32_t hex, float out[3])
{
	out[0] = (float)((hex >> 16) & 0xFF) / 255.0f;
	out[1] = (float)((hex >> 8) & 0xFF) / 255.0f;
	out[2] = (float)(hex & 0xFF) / 255.0f;
}

static void lin_from_hex(uint32_t hex, float g, float out[3])
{
	rgb_from_hex(hex, out);
	for (int i = 0; i < 3; i++)
		out[i] = powf(out[i], g);
}

static bool parse_hex_color(const char *s, uint32_t *out)
{
	if (!s)
		return false;
	if (*s == '#')
		s++;
	if (strlen(s) != 6)
		return false;
	char *end = NULL;
	unsigned long v = strtoul(s, &end, 16);
	if (!end || *end)
		return false;
	*out = (uint32_t)v;
	return true;
}

static void kelvin_rgb(double k, float out[3])
{
	double t = k / 100.0, r, g, b;
	if (t <= 66.0) {
		r = 255.0;
		g = 99.4708025861 * log(t) - 161.1195681661;
	} else {
		r = 329.698727446 * pow(t - 60.0, -0.1332047592);
		g = 288.1221695283 * pow(t - 60.0, -0.0755148492);
	}
	if (t >= 66.0)
		b = 255.0;
	else if (t <= 19.0)
		b = 0.0;
	else
		b = 138.5177312231 * log(t - 10.0) - 305.0447927307;
	out[0] = (float)(fmin(255.0, fmax(0.0, r)) / 255.0);
	out[1] = (float)(fmin(255.0, fmax(0.0, g)) / 255.0);
	out[2] = (float)(fmin(255.0, fmax(0.0, b)) / 255.0);
}

static int param_index(const char *id)
{
	for (int i = 0; i < CP_COUNT; i++) {
		if (strcmp(crt_params[i].id, id) == 0)
			return i;
	}
	return -1;
}

static double decode_select(enum crt_param_id id, const char *s)
{
	if (!s || !*s)
		s = crt_params[id].def_str;

	switch (id) {
	case CP_lines:
		return strcmp(s, "native") == 0 ? 0.0 : atof(s);
	case CP_aspect: {
		int a = 0, b = 0;
		if (strcmp(s, "fill") == 0 || sscanf(s, "%d:%d", &a, &b) != 2 || a <= 0 || b <= 0)
			return 0.0;
		return (double)a / (double)b;
	}
	default:
		return atof(s);
	}
}

/* ------------------------------------------------------------------------ */
/* settings                                                                  */

static void read_vals(obs_data_t *settings, struct crt_vals *out)
{
	bool group_on[CRT_G_COUNT];
	for (int g = 0; g < CRT_G_COUNT; g++)
		group_on[g] = g == CRT_G_PICTURE ? true : obs_data_get_bool(settings, crt_group_ids[g]);

	for (int i = 0; i < CP_COUNT; i++) {
		const struct crt_param *p = &crt_params[i];
		bool bypass = p->has_bypass && !group_on[p->group];

		switch (p->type) {
		case CRT_SLIDER: {
			double v = bypass ? p->bypass : obs_data_get_double(settings, p->id);
			if (!bypass)
				v = fmin(p->max, fmax(p->min, v));
			out->v[i] = v;
			break;
		}
		case CRT_TOGGLE:
			out->v[i] = bypass ? p->bypass : (obs_data_get_bool(settings, p->id) ? 1.0 : 0.0);
			break;
		case CRT_COLOR:
			out->v[i] = (double)hex_from_obs_color(obs_data_get_int(settings, p->id));
			break;
		case CRT_SELECT:
			out->v[i] = decode_select((enum crt_param_id)i,
						  bypass ? p->bypass_str : obs_data_get_string(settings, p->id));
			break;
		}
	}

	out->lines = (int)out->v[CP_lines];
	out->aspect = out->v[CP_aspect];
	out->fps = (int)out->v[CP_fps];
	if (out->fps <= 0)
		out->fps = 60;
	out->mask_type = (int)out->v[CP_maskType];
	out->clear_outside = obs_data_get_bool(settings, "clear_outside");
	out->off_passthrough = strcmp(obs_data_get_string(settings, "off_mode"), "passthrough") == 0;
}

/* write one Phosphor value into the settings */
static void set_param(obs_data_t *s, int i, double num, const char *str)
{
	const struct crt_param *p = &crt_params[i];

	switch (p->type) {
	case CRT_SLIDER:
		obs_data_set_double(s, p->id, fmin(p->max, fmax(p->min, num)));
		break;
	case CRT_TOGGLE:
		obs_data_set_bool(s, p->id, num != 0.0);
		break;
	case CRT_COLOR:
		obs_data_set_int(s, p->id, obs_color_from_hex((uint32_t)num));
		break;
	case CRT_SELECT: {
		/* only accept values the list actually has */
		const char *pick = NULL;
		char buf[32];
		if (!str) {
			snprintf(buf, sizeof(buf), "%g", num);
			str = buf;
		}
		for (const char *const *o = p->options; *o; o += 2) {
			if (strcmp(*o, str) == 0)
				pick = *o;
		}
		obs_data_set_string(s, p->id, pick ? pick : p->def_str);
		break;
	}
	}
}

static void reset_params(obs_data_t *s)
{
	for (int i = 0; i < CP_COUNT; i++)
		set_param(s, i, crt_params[i].def, crt_params[i].def_str);
	for (int g = 0; g < CRT_G_COUNT; g++)
		obs_data_set_bool(s, crt_group_ids[g], true);
}

static void apply_preset(obs_data_t *s, int idx)
{
	if (idx < 0 || idx >= CRT_PRESET_COUNT)
		return;

	/* keep the user's performance choice and picture refresh */
	double scale = obs_data_get_double(s, "renderScale");
	const char *fps = obs_data_get_string(s, "fps");
	char fps_buf[16];
	snprintf(fps_buf, sizeof(fps_buf), "%s", fps && *fps ? fps : "60");

	reset_params(s);
	const struct crt_preset *pr = &crt_presets[idx];
	for (int k = 0; k < pr->count; k++)
		set_param(s, pr->vals[k].param, pr->vals[k].num, pr->vals[k].str);

	obs_data_set_double(s, "renderScale", scale > 0.0 ? scale : 1.0);
	obs_data_set_string(s, "fps", fps_buf);
}

static int preset_index(const char *name)
{
	for (int i = 0; name && i < CRT_PRESET_COUNT; i++) {
		if (strcmp(crt_presets[i].name, name) == 0)
			return i;
	}
	return -1;
}

/* Phosphor's export format: {"name": ..., "p": {id: value, ...}} with only
 * the values that differ from the defaults. Also takes {"params": ...} or a
 * bare object of values. */
static bool import_phosphor_json(obs_data_t *s, const char *json)
{
	if (!json || !*json)
		return false;

	obs_data_t *root = obs_data_create_from_json(json);
	if (!root)
		return false;

	obs_data_t *p = obs_data_get_obj(root, "p");
	if (!p)
		p = obs_data_get_obj(root, "params");
	if (!p) {
		p = root;
		obs_data_addref(p);
	}

	int matched = 0;
	for (obs_data_item_t *it = obs_data_first(p); it; obs_data_item_next(&it)) {
		int i = param_index(obs_data_item_get_name(it));
		if (i >= 0)
			matched++;
	}

	if (matched == 0) {
		obs_data_release(p);
		obs_data_release(root);
		return false;
	}

	double scale = obs_data_get_double(s, "renderScale");
	reset_params(s);
	obs_data_set_double(s, "renderScale", scale > 0.0 ? scale : 1.0);

	for (obs_data_item_t *it = obs_data_first(p); it; obs_data_item_next(&it)) {
		int i = param_index(obs_data_item_get_name(it));
		if (i < 0)
			continue;

		const struct crt_param *d = &crt_params[i];
		enum obs_data_type type = obs_data_item_gettype(it);

		if (d->type == CRT_COLOR) {
			uint32_t hex;
			if (type == OBS_DATA_STRING && parse_hex_color(obs_data_item_get_string(it), &hex))
				set_param(s, i, (double)hex, NULL);
		} else if (type == OBS_DATA_STRING) {
			const char *str = obs_data_item_get_string(it);
			if (d->type == CRT_SELECT)
				set_param(s, i, 0.0, str);
			else if (d->type == CRT_TOGGLE)
				set_param(s, i, strcmp(str, "true") == 0 ? 1.0 : 0.0, NULL);
			else
				set_param(s, i, atof(str), NULL);
		} else if (type == OBS_DATA_NUMBER) {
			set_param(s, i, obs_data_item_get_double(it), NULL);
		} else if (type == OBS_DATA_BOOLEAN) {
			set_param(s, i, obs_data_item_get_bool(it) ? 1.0 : 0.0, NULL);
		}
	}

	const char *name = obs_data_get_string(root, "name");
	obs_data_set_string(s, "preset", "");
	obs_data_set_string(s, "preset_applied", "");
	obs_data_set_string(s, "imported_name", name ? name : "");

	obs_data_release(p);
	obs_data_release(root);
	return true;
}

static void export_phosphor_json(obs_data_t *s)
{
	struct crt_vals v;
	read_vals(s, &v);

	obs_data_t *p = obs_data_create();
	for (int i = 0; i < CP_COUNT; i++) {
		const struct crt_param *d = &crt_params[i];
		if (d->hidden)
			continue;

		bool group_on = d->group == CRT_G_PICTURE || obs_data_get_bool(s, crt_group_ids[d->group]);
		bool bypass = d->has_bypass && !group_on;

		switch (d->type) {
		case CRT_SLIDER:
			if (fabs(v.v[i] - d->def) > 1e-9)
				obs_data_set_double(p, d->id, v.v[i]);
			break;
		case CRT_TOGGLE:
			if ((v.v[i] != 0.0) != (d->def != 0.0))
				obs_data_set_bool(p, d->id, v.v[i] != 0.0);
			break;
		case CRT_COLOR:
			if ((uint32_t)v.v[i] != (uint32_t)d->def) {
				char hex[8];
				snprintf(hex, sizeof(hex), "#%06x", (uint32_t)v.v[i]);
				obs_data_set_string(p, d->id, hex);
			}
			break;
		case CRT_SELECT: {
			const char *str = bypass ? d->bypass_str : obs_data_get_string(s, d->id);
			if (str && *str && strcmp(str, d->def_str) != 0)
				obs_data_set_string(p, d->id, str);
			break;
		}
		}
	}

	obs_data_t *root = obs_data_create();
	const char *preset = obs_data_get_string(s, "preset");
	obs_data_set_string(root, "name", preset && *preset ? preset : "SPAZZ CRT");
	obs_data_set_obj(root, "p", p);
	obs_data_set_string(s, "phosphor_json", obs_data_get_json_pretty(root));
	obs_data_release(root);
	obs_data_release(p);
}

/* ------------------------------------------------------------------------ */
/* sounds: Phosphor's WebAudio effects, synthesised offline into clips      */

static inline float rnd(struct spazz_crt *c)
{
	c->rng = c->rng * 1664525u + 1013904223u;
	return (float)(c->rng >> 8) / 8388608.0f - 1.0f;
}

/* WebAudio's exponential ramps: value v0 at t0 to v1 at t1 */
static inline float exp_ramp(float t, float t0, float v0, float t1, float v1)
{
	if (t <= t0)
		return v0;
	if (t >= t1)
		return v1;
	return v0 * powf(v1 / v0, (t - t0) / (t1 - t0));
}

struct biquad {
	float b0, b1, b2, a1, a2, z1, z2;
};

/* RBJ cookbook, as used by WebAudio's BiquadFilterNode (Q is linear here) */
static void biquad_init(struct biquad *f, int type, float freq, float q, float rate)
{
	float w = 2.0f * (float)M_PI * freq / rate, cw = cosf(w), sw = sinf(w), alpha = sw / (2.0f * q);
	float b0, b1, b2, a0 = 1.0f + alpha, a1 = -2.0f * cw, a2 = 1.0f - alpha;
	if (type == 0) { /* low-pass */
		b0 = (1.0f - cw) * 0.5f;
		b1 = 1.0f - cw;
		b2 = b0;
	} else if (type == 1) { /* high-pass */
		b0 = (1.0f + cw) * 0.5f;
		b1 = -(1.0f + cw);
		b2 = b0;
	} else { /* band-pass, constant 0 dB peak */
		b0 = alpha;
		b1 = 0.0f;
		b2 = -alpha;
	}
	f->b0 = b0 / a0;
	f->b1 = b1 / a0;
	f->b2 = b2 / a0;
	f->a1 = a1 / a0;
	f->a2 = a2 / a0;
	f->z1 = f->z2 = 0.0f;
}

static inline float biquad_run(struct biquad *f, float x)
{
	float y = f->b0 * x + f->z1;
	f->z1 = f->b1 * x - f->a1 * y + f->z2;
	f->z2 = f->b2 * x - f->a2 * y;
	return y;
}

static float *mix_reserve(struct spazz_crt *c, size_t frames)
{
	if (c->mix_len < frames) {
		if (c->mix_cap < frames) {
			c->mix = brealloc(c->mix, frames * sizeof(float));
			c->mix_cap = frames;
		}
		memset(c->mix + c->mix_len, 0, (frames - c->mix_len) * sizeof(float));
		c->mix_len = frames;
	}
	return c->mix;
}

enum crt_sound { SND_POWER_ON, SND_POWER_OFF, SND_CHANNEL, SND_DEGAUSS };

static void play_sound(struct spazz_crt *c, enum crt_sound which)
{
	if (!os_atomic_load_bool(&c->sounds))
		return;

	const float rate = (float)c->sample_rate;
	float len = 0.0f;
	switch (which) {
	case SND_POWER_ON:
		len = 2.6f;
		break;
	case SND_POWER_OFF:
		len = 0.6f;
		break;
	case SND_CHANNEL:
		len = 0.3f;
		break;
	case SND_DEGAUSS:
		len = 1.9f;
		break;
	}

	size_t n = (size_t)(len * rate);
	float *out = mix_reserve(c, n);
	struct biquad f;

	switch (which) {
	case SND_DEGAUSS: {
		/* mains hum through the degauss coil, plus a thunk */
		biquad_init(&f, 0, 400.0f, 0.7071f, rate);
		const float freqs[4] = {60.0f, 120.0f, 180.0f, 240.0f}, amps[4] = {1.0f, 0.6f, 0.35f, 0.2f};
		for (size_t i = 0; i < n; i++) {
			float t = (float)i / rate;
			float g = t < 0.02f ? exp_ramp(t, 0.0f, 0.0001f, 0.02f, 0.35f)
					    : exp_ramp(t, 0.02f, 0.35f, 1.8f, 0.0001f);
			float s = 0.0f;
			for (int k = 0; k < 4; k++) {
				float ph = t * freqs[k];
				s += (2.0f * (ph - floorf(ph)) - 1.0f) * amps[k] * 0.25f;
			}
			float v = s * g;
			if (t < 0.08f)
				v += biquad_run(&f, rnd(c)) * 0.9f;
			out[i] += v;
		}
		break;
	}
	case SND_POWER_ON: {
		/* relay click, rising high-voltage hiss, 15.7 kHz flyback whine */
		biquad_init(&f, 1, 3000.0f, 0.7071f, rate);
		for (size_t i = 0; i < n; i++) {
			float t = (float)i / rate;
			float v = 0.0f;
			if (t < 0.05f)
				v += rnd(c) * 0.5f;
			if (t >= 0.05f && t < 1.25f) {
				float g = t < 0.15f ? exp_ramp(t, 0.05f, 0.0001f, 0.15f, 0.06f)
						    : exp_ramp(t, 0.15f, 0.06f, 1.1f, 0.0001f);
				v += biquad_run(&f, rnd(c)) * g;
			}
			float wg = t < 0.3f ? exp_ramp(t, 0.0f, 0.0001f, 0.3f, 0.012f)
					    : exp_ramp(t, 0.3f, 0.012f, 2.5f, 0.0001f);
			v += sinf(2.0f * (float)M_PI * 15734.0f * t) * wg;
			out[i] += v;
		}
		break;
	}
	case SND_POWER_OFF: {
		/* falling "pew" as the deflection collapses */
		float phase = 0.0f;
		for (size_t i = 0; i < n; i++) {
			float t = (float)i / rate;
			float fr = exp_ramp(t, 0.0f, 900.0f, 0.5f, 40.0f);
			phase += fr / rate;
			phase -= floorf(phase);
			float v = sinf(2.0f * (float)M_PI * phase) * exp_ramp(t, 0.0f, 0.08f, 0.55f, 0.0001f);
			if (t < 0.04f)
				v += rnd(c) * 0.4f;
			out[i] += v;
		}
		break;
	}
	case SND_CHANNEL:
		/* tuning static */
		biquad_init(&f, 2, 2500.0f, 0.5f, rate);
		for (size_t i = 0; i < n; i++) {
			float t = (float)i / rate;
			out[i] += biquad_run(&f, rnd(c)) * exp_ramp(t, 0.0f, 0.12f, 0.3f, 0.0001f);
		}
		break;
	}
}

/* feed the mixed clips to OBS in small chunks with continuous timestamps,
 * staying a little ahead of real time so nothing gaps */
static void pump_audio(struct spazz_crt *c)
{
	if (!c->mix_len) {
		c->streaming = false;
		return;
	}

	const uint64_t now = os_gettime_ns();
	if (!c->streaming || c->next_ts + 200000000ULL < now) {
		c->next_ts = now;
		c->streaming = true;
	}

	const size_t chunk = c->sample_rate / 100; /* 10 ms */
	while (c->mix_len && c->next_ts < now + 60000000ULL) {
		size_t frames = c->mix_len < chunk ? c->mix_len : chunk;

		struct obs_source_audio a = {0};
		a.data[0] = (const uint8_t *)c->mix;
		a.frames = (uint32_t)frames;
		a.speakers = SPEAKERS_MONO;
		a.format = AUDIO_FORMAT_FLOAT;
		a.samples_per_sec = c->sample_rate;
		a.timestamp = c->next_ts;
		obs_source_output_audio(c->source, &a);

		c->mix_len -= frames;
		memmove(c->mix, c->mix + frames, c->mix_len * sizeof(float));
		c->next_ts += util_mul_div64(frames, 1000000000ULL, c->sample_rate);
	}
}

/* ------------------------------------------------------------------------ */
/* power animation (Phosphor's _powerScale)                                  */

static void power_scale(const struct spazz_crt *c, float out[3])
{
	float t = c->power_t < 1.0f ? c->power_t : 1.0f;

	if (c->power_target) {
		if (t >= 1.0f) {
			out[0] = out[1] = out[2] = 1.0f;
			return;
		}
		/* turn on: a bright line opens up, with a little overshoot */
		float a = fminf(1.0f, t / 0.25f), b = fmaxf(0.0f, (t - 0.15f) / 0.85f);
		float sx = 0.02f + 0.98f * (1.0f - powf(1.0f - a, 3.0f));
		float ease = 1.0f - powf(1.0f - b, 3.0f);
		float sy = fmaxf(0.004f, ease * (1.0f + 0.04f * sinf(b * 9.0f) * (1.0f - b)));
		float gain = fminf(6.0f, 1.0f / sqrtf(sx * sy)) * (0.5f + 0.5f * a);
		out[0] = sx;
		out[1] = sy;
		out[2] = fmaxf(1.0f, gain * 0.6f);
		return;
	}

	if (t >= 1.0f) {
		out[0] = out[1] = 0.002f;
		out[2] = 0.0f;
	} else if (t < 0.45f) {
		float e = t / 0.45f;
		float sy = fmaxf(0.004f, 1.0f - e * e * (3.0f - 2.0f * e));
		out[0] = 1.0f;
		out[1] = sy;
		out[2] = fminf(5.0f, 1.0f / sqrtf(sy));
	} else if (t < 0.75f) {
		float e = (t - 0.45f) / 0.3f;
		float sx = fmaxf(0.004f, 1.0f - e);
		out[0] = sx;
		out[1] = 0.004f;
		out[2] = fminf(9.0f, 6.0f / sqrtf(sx) * 0.4f);
	} else {
		float e = (t - 0.75f) / 0.25f;
		out[0] = out[1] = 0.004f;
		out[2] = 8.0f * (1.0f - e) * (1.0f - e);
	}
}

static inline bool fully_off(const struct spazz_crt *c)
{
	return !c->power_target && c->power_t >= 1.0f;
}

/* ------------------------------------------------------------------------ */
/* views                                                                     */

static void view_free(struct crt_view *v)
{
	gs_texrender_t **all[] = {&v->page,    &v->signal, &v->persist[0], &v->persist[1], &v->persist_tmp,
				  &v->beam,    &v->bloom_a, &v->bloom_b,   &v->halo_a,     &v->halo_b};
	for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
		gs_texrender_destroy(*all[i]);
		*all[i] = NULL;
	}
	memset(v, 0, sizeof(*v));
}

static void view_init(struct crt_view *v, uint32_t bw, uint32_t bh, uint32_t pw, uint32_t ph)
{
	view_free(v);
	gs_texrender_t **all[] = {&v->page,    &v->signal, &v->persist[0], &v->persist[1], &v->persist_tmp,
				  &v->beam,    &v->bloom_a, &v->bloom_b,   &v->halo_a,     &v->halo_b};
	for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++)
		*all[i] = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);

	v->used = true;
	v->bw = bw;
	v->bh = bh;
	v->pw = pw;
	v->ph = ph;
	v->persist_clear = true;
	v->last_capture = -1e9;
}

/* Phosphor's layout(): the picture rect inside the box, in beam pixels */
static void tube_layout(const struct crt_vals *P, uint32_t bw, uint32_t bh, float rect[4])
{
	float cw = (float)bw, ch = (float)bh;
	float inset = (float)P->v[CP_bezelSize] * fminf(cw, ch);
	float aw = fmaxf(1.0f, cw - inset * 2.0f), ah = fmaxf(1.0f, ch - inset * 2.0f);
	float w = aw, h = ah;

	if (P->aspect > 0.0) {
		float r = (float)P->aspect;
		if (aw / ah > r) {
			h = ah;
			w = h * r;
		} else {
			w = aw;
			h = w / r;
		}
	}

	rect[0] = (cw - w) * 0.5f;
	rect[1] = (ch - h) * 0.5f;
	rect[2] = w;
	rect[3] = h;
}

static void page_size(const struct crt_vals *P, const float rect[4], uint32_t *pw, uint32_t *ph)
{
	float scale = (float)P->v[CP_renderScale];
	float h = P->lines > 0 ? (float)P->lines : rect[3] / fmaxf(scale, 0.05f);
	uint32_t hh = clampu((uint32_t)lroundf(h), MIN_DIM, MAX_PAGE_H);
	uint32_t ww = clampu((uint32_t)lroundf((float)hh * rect[2] / fmaxf(rect[3], 1.0f)), MIN_DIM, MAX_PAGE_W);
	*pw = ww;
	*ph = hh;
}

static struct crt_view *get_view(struct spazz_crt *c, uint32_t bw, uint32_t bh, uint32_t pw, uint32_t ph)
{
	struct crt_view *free_slot = NULL, *oldest = NULL;

	for (int i = 0; i < MAX_VIEWS; i++) {
		struct crt_view *v = &c->views[i];
		if (!v->used) {
			if (!free_slot)
				free_slot = v;
			continue;
		}
		if (v->bw == bw && v->bh == bh && v->pw == pw && v->ph == ph)
			return v;
		if (!oldest || v->last_frame < oldest->last_frame)
			oldest = v;
	}

	struct crt_view *v = free_slot ? free_slot : oldest;
	if (c->last_view == v)
		c->last_view = NULL;
	view_init(v, bw, bh, pw, ph);
	return v;
}

static void expire_views(struct spazz_crt *c)
{
	for (int i = 0; i < MAX_VIEWS; i++) {
		struct crt_view *v = &c->views[i];
		if (v->used && v->last_frame + VIEW_EXPIRE_FRAMES < c->frame && v != c->last_view)
			view_free(v);
	}
}

/* ------------------------------------------------------------------------ */
/* where the box is on the render target                                    */

struct screen_map {
	uint32_t rt_w, rt_h;
	enum gs_color_format fmt;
	/* box corners in render-target pixels (rows from the top of memory) */
	float o[2], x[2], y[2];
};

/* Both backends: gs_projection_get() is the projection as OBS set it (OpenGL
 * flips it only when it uploads it for a texture target), so ndc +1 is the
 * top row of the target's memory either way. */
static void to_target_px(const struct matrix4 *world, const struct matrix4 *proj, const struct gs_rect *vp, float bx,
			 float by, float out[2])
{
	struct vec4 p, w, clip;
	vec4_set(&p, bx, by, 0.0f, 1.0f);
	vec4_transform(&w, &p, world);
	vec4_transform(&clip, &w, proj);

	float iw = fabsf(clip.w) > 1e-8f ? 1.0f / clip.w : 1.0f;
	float nx = clip.x * iw, ny = clip.y * iw;

	out[0] = (float)vp->x + (nx + 1.0f) * 0.5f * (float)vp->cx;
	out[1] = (float)vp->y + (1.0f - ny) * 0.5f * (float)vp->cy;
}

static bool map_box(uint32_t w, uint32_t h, struct screen_map *m)
{
	if (!gs_get_target_info(&m->rt_w, &m->rt_h, &m->fmt))
		return false;

	switch (m->fmt) {
	case GS_RGBA:
	case GS_BGRA:
	case GS_BGRX:
	case GS_R10G10B10A2:
	case GS_RGBA16F:
		break;
	default:
		return false;
	}

	struct matrix4 world, proj;
	struct gs_rect vp;
	gs_matrix_get(&world);
	if (!gs_projection_get(&proj))
		return false;
	gs_get_viewport(&vp);
	if (vp.cx <= 0 || vp.cy <= 0)
		return false;

	float px[2], py[2];
	to_target_px(&world, &proj, &vp, 0.0f, 0.0f, m->o);
	to_target_px(&world, &proj, &vp, (float)w, 0.0f, px);
	to_target_px(&world, &proj, &vp, 0.0f, (float)h, py);
	m->x[0] = px[0] - m->o[0];
	m->x[1] = px[1] - m->o[1];
	m->y[0] = py[0] - m->o[0];
	m->y[1] = py[1] - m->o[1];
	return true;
}

static bool ensure_backdrop(struct spazz_crt *c, uint32_t w, uint32_t h, enum gs_color_format fmt)
{
	if (c->backdrop && c->back_fmt == fmt && c->back_w >= w && c->back_h >= h)
		return true;

	/* round up so a box that moves or scales doesn't realloc every frame */
	uint32_t nw = (w + 127u) & ~127u, nh = (h + 127u) & ~127u;
	if (c->backdrop && c->back_fmt == fmt) {
		nw = nw > c->back_w ? nw : c->back_w;
		nh = nh > c->back_h ? nh : c->back_h;
	}

	gs_texture_destroy(c->backdrop);
	c->backdrop = gs_texture_create(nw, nh, fmt, 1, NULL, 0);
	c->back_w = c->backdrop ? nw : 0;
	c->back_h = c->backdrop ? nh : 0;
	c->back_fmt = fmt;
	return c->backdrop != NULL;
}

/* ------------------------------------------------------------------------ */
/* passes                                                                    */

static bool pass_begin(gs_texrender_t *tr, uint32_t w, uint32_t h)
{
	gs_texrender_reset(tr);
	if (!gs_texrender_begin(tr, w, h))
		return false;
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
	return true;
}

static void pass_run(struct spazz_crt *c, const char *tech, gs_texrender_t *tr, uint32_t w, uint32_t h)
{
	if (!pass_begin(tr, w, h))
		return;
	while (gs_effect_loop(c->effect, tech))
		gs_draw_sprite(NULL, 0, w, h);
	gs_texrender_end(tr);
}

static void pass_clear(gs_texrender_t *tr, uint32_t w, uint32_t h)
{
	if (!pass_begin(tr, w, h))
		return;
	struct vec4 black;
	vec4_set(&black, 0.0f, 0.0f, 0.0f, 1.0f);
	gs_clear(GS_CLEAR_COLOR, &black, 0.0f, 0);
	gs_texrender_end(tr);
}

static void blur_chain(struct spazz_crt *c, gs_texrender_t *input, uint32_t in_w, uint32_t in_h, gs_texrender_t *a,
		       gs_texrender_t *b, uint32_t w, uint32_t h, float radius, float thresh)
{
	settex(c->u.image, input);
	set2(c->u.uInTexel, 1.0f / (float)in_w, 1.0f / (float)in_h);
	setf(c->u.uThresh, thresh);
	pass_run(c, "Down", a, w, h);

	for (int it = 0; it < 2; it++) {
		float r = radius * (float)(it + 1) * 0.75f;
		settex(c->u.image, a);
		set2(c->u.uDir, r / (float)w, 0.0f);
		pass_run(c, "Blur", b, w, h);
		settex(c->u.image, b);
		set2(c->u.uDir, 0.0f, r / (float)h);
		pass_run(c, "Blur", a, w, h);
	}
}

/* Capture: the backdrop under the picture rect -> page */
static bool capture_page(struct spazz_crt *c, struct crt_view *v, const struct screen_map *m)
{
	/* picture rect corners in render-target pixels */
	float fx = v->rect[0] / (float)v->bw, fy = v->rect[1] / (float)v->bh;
	float sx = v->rect[2] / (float)v->bw, sy = v->rect[3] / (float)v->bh;
	float q0[2] = {m->o[0] + fx * m->x[0] + fy * m->y[0], m->o[1] + fx * m->x[1] + fy * m->y[1]};
	float qx[2] = {sx * m->x[0], sx * m->x[1]};
	float qy[2] = {sy * m->y[0], sy * m->y[1]};

	float xs[4] = {q0[0], q0[0] + qx[0], q0[0] + qy[0], q0[0] + qx[0] + qy[0]};
	float ys[4] = {q0[1], q0[1] + qx[1], q0[1] + qy[1], q0[1] + qx[1] + qy[1]};
	float x0 = xs[0], x1 = xs[0], y0 = ys[0], y1 = ys[0];
	for (int i = 1; i < 4; i++) {
		x0 = fminf(x0, xs[i]);
		x1 = fmaxf(x1, xs[i]);
		y0 = fminf(y0, ys[i]);
		y1 = fmaxf(y1, ys[i]);
	}

	/* margin for the area taps around each page texel */
	float foot = fmaxf(hypotf(qx[0], qx[1]) / (float)v->pw, hypotf(qy[0], qy[1]) / (float)v->ph);
	float margin = ceilf(foot) + 2.0f;
	x0 = fmaxf(0.0f, floorf(x0 - margin));
	y0 = fmaxf(0.0f, floorf(y0 - margin));
	x1 = fminf((float)m->rt_w, ceilf(x1 + margin));
	y1 = fminf((float)m->rt_h, ceilf(y1 + margin));
	if (x1 - x0 < 1.0f || y1 - y0 < 1.0f)
		return false; /* entirely off the canvas */

	uint32_t rx = (uint32_t)x0, ry = (uint32_t)y0, rw = (uint32_t)(x1 - x0), rh = (uint32_t)(y1 - y0);
	if (!ensure_backdrop(c, rw, rh, m->fmt))
		return false;
	if (!gs_copy_from_target(c->backdrop, rx, ry, rw, rh))
		return false;

	const float bw = (float)c->back_w, bh = (float)c->back_h;
	set2(c->u.box_o, (q0[0] - x0) / bw, (q0[1] - y0) / bh);
	set2(c->u.box_x, qx[0] / bw, qx[1] / bh);
	set2(c->u.box_y, qy[0] / bw, qy[1] / bh);
	const float valid[4] = {0.0f, 0.0f, (float)rw / bw, (float)rh / bh};
	set4(c->u.cap_valid, valid);
	setf(c->u.lin_canvas, m->fmt == GS_RGBA16F ? 1.0f : 0.0f);
	set2(c->u.out_size, (float)v->pw, (float)v->ph);
	if (c->u.image)
		gs_effect_set_texture(c->u.image, c->backdrop);

	pass_run(c, "Capture", v->page, v->pw, v->ph);
	return true;
}

/* Signal -> Persist -> Beam -> Bloom/Halo. Returns false if nothing usable. */
static void run_tube(struct spazz_crt *c, struct crt_view *v, bool first_this_frame)
{
	const struct crt_vals *P = &c->P;
	const double *p = P->v;
	const float pw = (float)v->pw, ph = (float)v->ph;

	if (v->persist_clear) {
		pass_clear(v->persist[0], v->pw, v->ph);
		pass_clear(v->persist[1], v->pw, v->ph);
		v->persist_clear = false;
	}

	if (!fully_off(c)) {
		/* SIGNAL */
		settex(c->u.image, v->page);
		set2(c->u.in_size, pw, ph);
		set2(c->u.out_size, pw, ph);
		setf(c->u.uTime, c->time);
		setf(c->u.uFrame, (float)(c->frame % 100000));
		setf(c->u.uLumaBlur, (float)p[CP_lumaBlur]);
		setf(c->u.uChromaBlur, (float)p[CP_chromaBlur]);
		setf(c->u.uChromaShift, (float)p[CP_chromaShift]);
		setf(c->u.uArtifacts, (float)p[CP_artifacts]);
		setf(c->u.uGhost, (float)p[CP_ghost]);
		setf(c->u.uGhostOff, (float)p[CP_ghostOffset]);
		setf(c->u.uRinging, (float)p[CP_ringing]);
		setf(c->u.uNoise, (float)p[CP_noise]);
		setf(c->u.uInterf, (float)p[CP_interference]);
		setf(c->u.uJitter, (float)p[CP_jitter]);
		setf(c->u.uWobble, (float)p[CP_wobble]);
		setf(c->u.uTracking, (float)p[CP_tracking]);
		setf(c->u.uDropouts, (float)p[CP_dropouts]);
		setf(c->u.uRollOff, c->roll_off);
		setf(c->u.uStatic, c->static_t > 0.0f ? fminf(1.0f, c->static_t * 1.3f) : 0.0f);
		setf(c->u.uBright, (float)p[CP_brightness]);
		setf(c->u.uContrast, (float)p[CP_contrast]);
		setf(c->u.uSat, (float)p[CP_saturation]);
		setf(c->u.uHue, (float)(p[CP_hue] * M_PI / 180.0));
		setf(c->u.uBlack, (float)p[CP_blackLevel]);
		setf(c->u.uGammaIn, (float)p[CP_gammaIn]);
		float kt[3], k65[3], temp[3];
		kelvin_rgb(p[CP_temperature], kt);
		kelvin_rgb(6500.0, k65);
		for (int i = 0; i < 3; i++)
			temp[i] = kt[i] / k65[i];
		set3(c->u.uTemp, temp);
		setf(c->u.uMono, p[CP_mono] != 0.0 ? 1.0f : 0.0f);
		setf(c->u.uMonoLevels, (float)p[CP_monoLevels]);
		float md[3], ml[3];
		rgb_from_hex((uint32_t)p[CP_monoDark], md);
		rgb_from_hex((uint32_t)p[CP_monoLight], ml);
		set3(c->u.uMonoDark, md);
		set3(c->u.uMonoLight, ml);
		pass_run(c, "Signal", v->signal, v->pw, v->ph);
	}

	/* PERSIST: the feedback only moves once per frame for this view */
	gs_texrender_t *phosphor = v->signal;
	const bool lcd = P->mask_type >= 4;
	if (p[CP_persistence] > 0.001 && !fully_off(c)) {
		int prev_idx;
		gs_texrender_t *out;
		if (first_this_frame) {
			prev_idx = v->persist_idx;
			out = v->persist[prev_idx ^ 1];
			v->frame_prev_idx = prev_idx;
			v->persist_idx ^= 1;
		} else {
			prev_idx = v->frame_prev_idx;
			out = v->persist_tmp;
		}
		settex(c->u.image, v->signal);
		settex(c->u.image2, v->persist[prev_idx]);
		setf(c->u.uDecay, powf((float)p[CP_persistence], c->dt * 60.0f));
		setf(c->u.uMode, lcd ? 1.0f : 0.0f);
		float t = (float)p[CP_persistTint];
		float tint[3];
		if (t >= 0.0f) {
			tint[0] = 1.0f - 0.15f * t;
			tint[1] = 1.0f;
			tint[2] = 1.0f - 0.5f * t;
		} else {
			tint[0] = 1.0f + 0.5f * t;
			tint[1] = 1.0f + 0.2f * t;
			tint[2] = 1.0f;
		}
		set3(c->u.uTint, tint);
		pass_run(c, "Persist", out, v->pw, v->ph);
		phosphor = out;
	}

	/* BEAM */
	float pwr[3];
	power_scale(c, pwr);
	float degauss = c->degauss_t > 0.0f ? c->degauss_t * c->degauss_t * (0.6f + 0.4f * sinf(c->time * 40.0f))
					    : 0.0f;
	settex(c->u.image, phosphor);
	set2(c->u.in_size, pw, ph);
	set2(c->u.out_size, (float)v->bw, (float)v->bh);
	set4(c->u.uRect, v->rect);
	set2(c->u.uCurv, (float)p[CP_curvatureX], (float)p[CP_curvatureY]);
	setf(c->u.uOverscan, (float)p[CP_overscan]);
	set2(c->u.uPower, pwr[0], pwr[1]);
	setf(c->u.uTime, c->time);
	setf(c->u.uFrame, (float)(c->frame % 100000));
	setf(c->u.uScan, (float)p[CP_scanStrength]);
	setf(c->u.uBeamMin, (float)fmin(p[CP_beamMin], p[CP_beamMax]));
	setf(c->u.uBeamMax, (float)fmax(p[CP_beamMin], p[CP_beamMax]));
	setf(c->u.uBeamShape, (float)p[CP_beamShape]);
	setf(c->u.uSharp, (float)p[CP_sharpness]);
	setf(c->u.uInterlace, (float)p[CP_interlace]);
	setf(c->u.uAntiMoire, p[CP_antiMoire] != 0.0 ? 1.0f : 0.0f);
	set2(c->u.uConvR, (float)p[CP_convRx], (float)p[CP_convRy]);
	set2(c->u.uConvB, (float)p[CP_convBx], (float)p[CP_convBy]);
	setf(c->u.uConvEdge, (float)p[CP_convEdge]);
	setf(c->u.uDegauss, degauss);
	setf(c->u.uMaskType, (float)P->mask_type);
	setf(c->u.uMaskStr, (float)p[CP_maskStrength]);
	setf(c->u.uMaskSize, (float)p[CP_maskSize]);
	setf(c->u.uMaskCurve, p[CP_maskCurve] != 0.0 ? 1.0f : 0.0f);
	setf(c->u.uMaskBoost, (float)p[CP_maskBoost]);
	setf(c->u.uLcdGap, (float)p[CP_lcdGap]);
	setf(c->u.uLcdGapLight, (float)p[CP_lcdGapLight]);
	setf(c->u.uLcdSub, (float)p[CP_lcdSubpix]);
	setf(c->u.uLcdBGR, p[CP_lcdBGR] != 0.0 ? 1.0f : 0.0f);
	setf(c->u.uLcdShadow, (float)p[CP_lcdShadow]);
	setf(c->u.uLcdShadowOff, (float)p[CP_lcdShadowOff]);
	float gap[3] = {0.8f, 0.8f, 0.8f};
	if (p[CP_mono] != 0.0)
		lin_from_hex((uint32_t)p[CP_monoLight], (float)p[CP_gammaIn], gap);
	set3(c->u.uGapColor, gap);
	pass_run(c, "Beam", v->beam, v->bw, v->bh);

	/* BLOOM (tight) + HALO (wide) */
	uint32_t bloom_w = (v->bw + 3) / 4, bloom_h = (v->bh + 3) / 4;
	uint32_t halo_w = (v->bw + 15) / 16, halo_h = (v->bh + 15) / 16;
	if (p[CP_bloom] > 0.0 || p[CP_halation] > 0.0 || p[CP_bezelReflect] > 0.0) {
		blur_chain(c, v->beam, v->bw, v->bh, v->bloom_a, v->bloom_b, bloom_w, bloom_h, (float)p[CP_bloomRadius],
			   (float)p[CP_bloomThreshold]);
		blur_chain(c, v->bloom_a, bloom_w, bloom_h, v->halo_a, v->halo_b, halo_w, halo_h, 1.6f, 0.0f);
	} else {
		pass_clear(v->bloom_a, bloom_w, bloom_h);
		pass_clear(v->halo_a, halo_w, halo_h);
	}
}

/* FINAL: straight into the scene over the box */
static void draw_final(struct spazz_crt *c, struct crt_view *v, uint32_t w, uint32_t h, bool lin_canvas)
{
	const struct crt_vals *P = &c->P;
	const double *p = P->v;
	float pwr[3];
	power_scale(c, pwr);
	float degauss = c->degauss_t > 0.0f ? c->degauss_t * c->degauss_t * (0.6f + 0.4f * sinf(c->time * 40.0f))
					    : 0.0f;

	settex(c->u.image, v->beam);
	settex(c->u.image2, v->bloom_a);
	settex(c->u.image3, v->halo_a);
	set2(c->u.out_size, (float)v->bw, (float)v->bh);
	set4(c->u.uRect, v->rect);
	set2(c->u.uCurv, (float)p[CP_curvatureX], (float)p[CP_curvatureY]);
	setf(c->u.uOverscan, (float)p[CP_overscan]);
	set2(c->u.uPower, 1.0f, 1.0f);
	setf(c->u.uTime, c->time);
	float min_dim = fminf(v->rect[2], v->rect[3]) / fmaxf(v->rect[3], 1.0f);
	setf(c->u.uCorner, fminf((float)p[CP_cornerRadius], 0.49f * min_dim));
	setf(c->u.uEdgeSoft, (float)p[CP_edgeSoft]);
	setf(c->u.uBezelReflect, (float)p[CP_bezelReflect]);
	float bezel[3];
	lin_from_hex((uint32_t)p[CP_bezelColor], 2.2f, bezel);
	set3(c->u.uBezelColor, bezel);
	setf(c->u.uBloomAmt, (float)p[CP_bloom]);
	setf(c->u.uHalation, (float)p[CP_halation] * 1.5f);
	setf(c->u.uVignette, (float)p[CP_vignette]);
	float amb = (float)p[CP_ambient];
	setf(c->u.uAmbient, amb * amb * 4.0f + amb * 0.2f);
	setf(c->u.uGlare, (float)p[CP_glare]);
	setf(c->u.uFlicker, c->flicker);
	setf(c->u.uLcdBleed, (float)p[CP_lcdBleed]);
	setf(c->u.uTN, (float)p[CP_tnAngle]);
	setf(c->u.uGammaOut, (float)p[CP_gammaOut]);
	setf(c->u.uPowerGain, pwr[2]);
	setf(c->u.uDegauss, degauss);
	setf(c->u.lin_canvas, lin_canvas ? 1.0f : 0.0f);
	setf(c->u.uClearOutside, P->clear_outside ? 1.0f : 0.0f);

	const bool prev_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	while (gs_effect_loop(c->effect, "Final"))
		gs_draw_sprite(NULL, 0, w, h);
	gs_enable_framebuffer_srgb(prev_srgb);
}

/* ------------------------------------------------------------------------ */
/* source callbacks                                                          */

static const char *crt_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return T_("SpazzCRT");
}

static void crt_update(void *data, obs_data_t *settings)
{
	struct spazz_crt *c = data;
	struct crt_vals v;
	read_vals(settings, &v);

	long w = (long)obs_data_get_int(settings, "width");
	long h = (long)obs_data_get_int(settings, "height");
	os_atomic_set_long(&c->width, w < 16 ? 16 : w);
	os_atomic_set_long(&c->height, h < 16 ? 16 : h);

	const char *show = obs_data_get_string(settings, "on_show");
	os_atomic_set_long(&c->on_show, strcmp(show, "power") == 0    ? SHOW_POWER_ON
					: strcmp(show, "static") == 0 ? SHOW_STATIC
								      : SHOW_NOTHING);
	os_atomic_set_bool(&c->sounds, obs_data_get_bool(settings, "sounds"));

	pthread_mutex_lock(&c->mutex);
	c->pending = v;
	pthread_mutex_unlock(&c->mutex);
}

static void crt_defaults(obs_data_t *settings)
{
	for (int i = 0; i < CP_COUNT; i++) {
		const struct crt_param *p = &crt_params[i];
		switch (p->type) {
		case CRT_SLIDER:
			obs_data_set_default_double(settings, p->id, p->def);
			break;
		case CRT_TOGGLE:
			obs_data_set_default_bool(settings, p->id, p->def != 0.0);
			break;
		case CRT_COLOR:
			obs_data_set_default_int(settings, p->id, obs_color_from_hex((uint32_t)p->def));
			break;
		case CRT_SELECT:
			obs_data_set_default_string(settings, p->id, p->def_str);
			break;
		}
	}
	for (int g = 0; g < CRT_G_COUNT; g++)
		obs_data_set_default_bool(settings, crt_group_ids[g], true);

	obs_data_set_default_string(settings, "preset", "");
	obs_data_set_default_int(settings, "width", 640);
	obs_data_set_default_int(settings, "height", 480);
	obs_data_set_default_string(settings, "on_show", "nothing");
	obs_data_set_default_string(settings, "off_mode", "dark");
	obs_data_set_default_bool(settings, "clear_outside", false);
	obs_data_set_default_bool(settings, "sounds", true);
}

/* -- property callbacks -- */

static bool preset_modified(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);

	/* OBS calls this when the dialog opens too: only apply a real change */
	const char *preset = obs_data_get_string(settings, "preset");
	const char *applied = obs_data_get_string(settings, "preset_applied");
	if (!preset || !*preset) {
		/* "Custom": forget the last preset so picking it again re-applies */
		obs_data_set_string(settings, "preset_applied", "");
		return false;
	}
	if (applied && strcmp(preset, applied) == 0)
		return false;
	obs_data_set_string(settings, "import_status", "");

	int idx = preset_index(preset);
	if (idx < 0)
		return false;

	char name[128];
	snprintf(name, sizeof(name), "%s", preset);
	apply_preset(settings, idx);
	obs_data_set_string(settings, "preset", name);
	obs_data_set_string(settings, "preset_applied", name);
	return true;
}

static bool reapply_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);
	struct spazz_crt *c = data;
	if (!c)
		return false;

	obs_data_t *s = obs_source_get_settings(c->source);
	const char *preset = obs_data_get_string(s, "preset");
	int idx = preset_index(preset);
	if (idx >= 0) {
		char name[128];
		snprintf(name, sizeof(name), "%s", preset);
		apply_preset(s, idx);
		obs_data_set_string(s, "preset", name);
		obs_data_set_string(s, "preset_applied", name);
	} else {
		double scale = obs_data_get_double(s, "renderScale");
		reset_params(s);
		obs_data_set_double(s, "renderScale", scale > 0.0 ? scale : 1.0);
	}
	obs_source_update(c->source, s);
	obs_data_release(s);
	return true;
}

static bool import_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);
	struct spazz_crt *c = data;
	if (!c)
		return false;

	obs_data_t *s = obs_source_get_settings(c->source);
	char *json = bstrdup(obs_data_get_string(s, "phosphor_json"));
	if (import_phosphor_json(s, json)) {
		obs_data_set_string(s, "import_status", T_("Import.Done"));
		os_atomic_set_bool(&c->req_static, true);
	} else {
		obs_data_set_string(s, "import_status", T_("Import.Failed"));
	}
	bfree(json);
	obs_source_update(c->source, s);
	obs_data_release(s);
	return true;
}

static bool export_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);
	struct spazz_crt *c = data;
	if (!c)
		return false;

	obs_data_t *s = obs_source_get_settings(c->source);
	export_phosphor_json(s);
	obs_data_set_string(s, "import_status", T_("Export.Done"));
	obs_source_update(c->source, s);
	obs_data_release(s);
	return true;
}

static bool power_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);
	struct spazz_crt *c = data;
	if (!c)
		return false;
	os_atomic_set_bool(&c->req_power, true);
	return false;
}

static bool static_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);
	struct spazz_crt *c = data;
	if (!c)
		return false;
	os_atomic_set_bool(&c->req_static, true);
	return false;
}

static bool degauss_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);
	struct spazz_crt *c = data;
	if (!c)
		return false;
	os_atomic_set_bool(&c->req_degauss, true);
	return false;
}

static void add_param_property(obs_properties_t *props, const struct crt_param *p)
{
	obs_property_t *prop = NULL;

	switch (p->type) {
	case CRT_SLIDER:
		prop = obs_properties_add_float_slider(props, p->id, p->label, p->min, p->max, p->step);
		break;
	case CRT_TOGGLE:
		prop = obs_properties_add_bool(props, p->id, p->label);
		break;
	case CRT_COLOR:
		prop = obs_properties_add_color(props, p->id, p->label);
		break;
	case CRT_SELECT:
		prop = obs_properties_add_list(props, p->id, p->label, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
		for (const char *const *o = p->options; *o; o += 2)
			obs_property_list_add_string(prop, o[1], o[0]);
		break;
	}

	if (prop && p->tip)
		obs_property_set_long_description(prop, p->tip);
}

static obs_properties_t *crt_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	/* presets */
	p = obs_properties_add_list(props, "preset", T_("Preset"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_("Preset.Custom"), "");
	for (int i = 0; i < CRT_PRESET_COUNT; i++)
		obs_property_list_add_string(p, crt_presets[i].name, crt_presets[i].name);
	obs_property_set_long_description(p, T_("Preset.Tip"));
	obs_property_set_modified_callback(p, preset_modified);
	obs_properties_add_button2(props, "reapply", T_("Preset.Reapply"), reapply_clicked, data);

	/* power & behaviour */
	obs_properties_t *pw = obs_properties_create();
	obs_properties_add_button2(pw, "btn_power", T_("Power"), power_clicked, data);
	obs_properties_add_button2(pw, "btn_static", T_("Static"), static_clicked, data);
	obs_properties_add_button2(pw, "btn_degauss", T_("Degauss"), degauss_clicked, data);
	p = obs_properties_add_list(pw, "on_show", T_("OnShow"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_("OnShow.Nothing"), "nothing");
	obs_property_list_add_string(p, T_("OnShow.Power"), "power");
	obs_property_list_add_string(p, T_("OnShow.Static"), "static");
	obs_property_set_long_description(p, T_("OnShow.Tip"));
	p = obs_properties_add_list(pw, "off_mode", T_("OffMode"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_("OffMode.Dark"), "dark");
	obs_property_list_add_string(p, T_("OffMode.Passthrough"), "passthrough");
	p = obs_properties_add_bool(pw, "clear_outside", T_("ClearOutside"));
	obs_property_set_long_description(p, T_("ClearOutside.Tip"));
	p = obs_properties_add_bool(pw, "sounds", T_("Sounds"));
	obs_property_set_long_description(p, T_("Sounds.Tip"));
	obs_properties_add_group(props, "grp_power", T_("PowerGroup"), OBS_GROUP_NORMAL, pw);

	/* picture: always on */
	obs_properties_t *pic = obs_properties_create();
	for (int i = 0; i < CP_COUNT; i++) {
		if (crt_params[i].group == CRT_G_PICTURE && !crt_params[i].hidden)
			add_param_property(pic, &crt_params[i]);
	}
	obs_properties_add_int(pic, "width", T_("Width"), 16, 8192, 1);
	obs_properties_add_int(pic, "height", T_("Height"), 16, 8192, 1);
	obs_properties_add_group(props, "grp_picture", crt_group_titles[CRT_G_PICTURE], OBS_GROUP_NORMAL, pic);

	/* the look, one switchable group per Phosphor panel */
	for (int g = CRT_G_PICTURE + 1; g < CRT_G_COUNT; g++) {
		obs_properties_t *sub = obs_properties_create();
		for (int i = 0; i < CP_COUNT; i++) {
			if ((int)crt_params[i].group == g && !crt_params[i].hidden)
				add_param_property(sub, &crt_params[i]);
		}
		obs_properties_add_group(props, crt_group_ids[g], crt_group_titles[g], OBS_GROUP_CHECKABLE, sub);
	}

	/* Phosphor preset files */
	obs_properties_t *io = obs_properties_create();
	p = obs_properties_add_text(io, "phosphor_json", T_("Json"), OBS_TEXT_MULTILINE);
	obs_property_set_long_description(p, T_("Json.Tip"));
	obs_properties_add_button2(io, "btn_import", T_("Import"), import_clicked, data);
	obs_properties_add_button2(io, "btn_export", T_("Export"), export_clicked, data);
	p = obs_properties_add_text(io, "import_status", "", OBS_TEXT_INFO);
	obs_properties_add_group(props, "grp_io", T_("JsonGroup"), OBS_GROUP_NORMAL, io);

	obs_properties_add_text(props, "limits", T_("Limits"), OBS_TEXT_INFO);
	return props;
}

/* -- hotkeys -- */

static void hk_power(void *data, obs_hotkey_id id, obs_hotkey_t *hk, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hk);
	struct spazz_crt *c = data;
	if (pressed)
		os_atomic_set_bool(&c->req_power, true);
}

static void hk_static(void *data, obs_hotkey_id id, obs_hotkey_t *hk, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hk);
	struct spazz_crt *c = data;
	if (pressed)
		os_atomic_set_bool(&c->req_static, true);
}

static void hk_degauss(void *data, obs_hotkey_id id, obs_hotkey_t *hk, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hk);
	struct spazz_crt *c = data;
	if (pressed)
		os_atomic_set_bool(&c->req_degauss, true);
}

/* -- lifetime -- */

static void crt_destroy(void *data)
{
	struct spazz_crt *c = data;

	obs_enter_graphics();
	for (int i = 0; i < MAX_VIEWS; i++)
		view_free(&c->views[i]);
	gs_texture_destroy(c->backdrop);
	gs_effect_destroy(c->effect);
	obs_leave_graphics();

	pthread_mutex_destroy(&c->mutex);
	bfree(c->mix);
	bfree(c);
}

static void *crt_create(obs_data_t *settings, obs_source_t *source)
{
	struct spazz_crt *c = bzalloc(sizeof(struct spazz_crt));
	c->source = source;
	c->power_target = 1;
	c->power_t = 1.0f;
	c->flicker = 1.0f;
	c->dt = 1.0f / 60.0f;
	c->rng = 0x5eed1234u;

	if (pthread_mutex_init(&c->mutex, NULL) != 0) {
		bfree(c);
		return NULL;
	}

	struct obs_audio_info oai;
	c->sample_rate = obs_get_audio_info(&oai) ? oai.samples_per_sec : 48000;

	char *path = obs_module_file("spazz-crt.effect");
	obs_enter_graphics();
	if (path) {
		char *errors = NULL;
		c->effect = gs_effect_create_from_file(path, &errors);
		if (!c->effect)
			blog(LOG_ERROR, "[spazz-crt] could not load effect: %s", errors ? errors : "(no details)");
		bfree(errors);
	}
	obs_leave_graphics();
	bfree(path);

	if (c->effect) {
#define X(n)                                                                              \
	c->u.n = gs_effect_get_param_by_name(c->effect, #n);                              \
	if (!c->u.n)                                                                      \
		blog(LOG_WARNING, "[spazz-crt] effect has no uniform '%s'", #n);
		CRT_UNIFORMS(X)
#undef X
	}

	obs_hotkey_register_source(source, "SpazzCRT.Power", T_("Hotkey.Power"), hk_power, c);
	obs_hotkey_register_source(source, "SpazzCRT.Static", T_("Hotkey.Static"), hk_static, c);
	obs_hotkey_register_source(source, "SpazzCRT.Degauss", T_("Hotkey.Degauss"), hk_degauss, c);

	/* new boxes: let the user hear the TV through their monitor too */
	if (!obs_data_get_bool(settings, "monitor_init")) {
		obs_source_set_monitoring_enabled(source, true);
		obs_data_set_bool(settings, "monitor_init", true);
	}

	/* a brand new box starts from the living-room preset */
	if (!obs_data_has_user_value(settings, "preset_applied")) {
		apply_preset(settings, 0);
		obs_data_set_string(settings, "preset", crt_presets[0].name);
		obs_data_set_string(settings, "preset_applied", crt_presets[0].name);
	}

	crt_update(c, settings);
	c->P = c->pending;
	return c;
}

static void crt_activate(void *data)
{
	struct spazz_crt *c = data;
	long mode = os_atomic_load_long(&c->on_show);
	if (mode == SHOW_POWER_ON)
		os_atomic_set_bool(&c->req_power_on, true);
	else if (mode == SHOW_STATIC)
		os_atomic_set_bool(&c->req_static, true);
}

static void crt_tick(void *data, float seconds)
{
	struct spazz_crt *c = data;

	pthread_mutex_lock(&c->mutex);
	c->P = c->pending;
	pthread_mutex_unlock(&c->mutex);

	float dt = fminf(0.1f, fmaxf(0.0f, seconds));
	c->dt = dt > 0.0f ? dt : 1.0f / 60.0f;
	c->clock += dt;
	c->time = fmodf(c->time + dt, 3600.0f);
	c->frame++;

	/* requests */
	if (os_atomic_exchange_bool(&c->req_power_on, false)) {
		/* "power on when shown": snap off, then warm up */
		c->power_target = 1;
		c->power_t = 0.0f;
		play_sound(c, SND_POWER_ON);
	}
	if (os_atomic_exchange_bool(&c->req_power, false)) {
		c->power_target = !c->power_target;
		c->power_t = 0.0f;
		play_sound(c, c->power_target ? SND_POWER_ON : SND_POWER_OFF);
	}
	if (os_atomic_exchange_bool(&c->req_static, false) && c->power_target) {
		c->static_t = 1.0f;
		play_sound(c, SND_CHANNEL);
	}
	if (os_atomic_exchange_bool(&c->req_degauss, false)) {
		c->degauss_t = 1.0f;
		play_sound(c, SND_DEGAUSS);
	}

	c->degauss_t = fmaxf(0.0f, c->degauss_t - dt * 0.6f);
	c->static_t = fmaxf(0.0f, c->static_t - dt * 3.2f);
	c->power_t = fminf(2.0f, c->power_t + dt / (c->power_target ? 0.9f : 0.75f));

	float h = (float)(c->roll_h ? c->roll_h : 480);
	c->roll_off = fmodf(c->roll_off + (float)c->P.v[CP_roll] * dt * h * 0.9f, h * 1.07f);
	if (c->roll_off < 0.0f)
		c->roll_off += h * 1.07f;

	float fl = (float)c->P.v[CP_flicker];
	float r = (rnd(c) + 1.0f) * 0.5f;
	c->flicker = 1.0f - fl * (0.06f * r + 0.05f * (0.5f + 0.5f * sinf(c->time * 2.0f * (float)M_PI * 7.3f)));

	pump_audio(c);
}

static void crt_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct spazz_crt *c = data;

	if (!c->effect)
		return;

	/* switched off with "let the scene through": nothing to draw */
	if (fully_off(c) && c->P.off_passthrough)
		return;

	const uint32_t w = (uint32_t)os_atomic_load_long(&c->width);
	const uint32_t h = (uint32_t)os_atomic_load_long(&c->height);
	const struct crt_vals *P = &c->P;

	expire_views(c);

	struct screen_map m;
	struct crt_view *v = NULL;
	bool mapped = map_box(w, h, &m);

	gs_blend_state_push();
	gs_enable_blending(false);
	const bool prev_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);

	if (mapped) {
		float scale = clampf((float)P->v[CP_renderScale], 0.1f, 1.0f);
		uint32_t bw = clampu((uint32_t)lroundf(hypotf(m.x[0], m.x[1]) * scale), MIN_DIM, MAX_BEAM);
		uint32_t bh = clampu((uint32_t)lroundf(hypotf(m.y[0], m.y[1]) * scale), MIN_DIM, MAX_BEAM);
		float rect[4];
		uint32_t pw, ph;
		tube_layout(P, bw, bh, rect);
		page_size(P, rect, &pw, &ph);

		v = get_view(c, bw, bh, pw, ph);
		memcpy(v->rect, rect, sizeof(rect));

		const bool first = v->last_frame != c->frame;
		v->last_frame = c->frame;

		/* the picture behind the tube only refreshes at the chosen rate */
		const double period = 1.0 / (double)P->fps - 0.25 / 60.0;
		bool want_capture = !fully_off(c) && (!first || !v->page_valid || c->clock - v->last_capture >= period);
		if (want_capture) {
			if (capture_page(c, v, &m)) {
				v->page_valid = true;
				if (first)
					v->last_capture = c->clock;
			} else if (!v->page_valid) {
				pass_clear(v->page, v->pw, v->ph);
				v->page_valid = true;
			}
		}

		run_tube(c, v, first);
		c->last_view = v;
		c->roll_h = ph;
	} else {
		/* can't see what's underneath (e.g. an OpenGL window): show the
		 * most recent picture so the box isn't just missing */
		v = c->last_view;
	}

	gs_enable_framebuffer_srgb(prev_srgb);
	gs_blend_state_pop();

	if (v && v->used)
		draw_final(c, v, w, h, mapped && m.fmt == GS_RGBA16F);
}

static uint32_t crt_width(void *data)
{
	struct spazz_crt *c = data;
	return (uint32_t)os_atomic_load_long(&c->width);
}

static uint32_t crt_height(void *data)
{
	struct spazz_crt *c = data;
	return (uint32_t)os_atomic_load_long(&c->height);
}

static struct obs_source_info spazz_crt_info = {
	.id = "spazz_crt",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_AUDIO,
	.get_name = crt_get_name,
	.create = crt_create,
	.destroy = crt_destroy,
	.update = crt_update,
	.get_defaults = crt_defaults,
	.get_properties = crt_properties,
	.activate = crt_activate,
	.video_tick = crt_tick,
	.video_render = crt_render,
	.get_width = crt_width,
	.get_height = crt_height,
	.icon_type = OBS_ICON_TYPE_CUSTOM,
};

bool obs_module_load(void)
{
	obs_register_source(&spazz_crt_info);
	return true;
}
