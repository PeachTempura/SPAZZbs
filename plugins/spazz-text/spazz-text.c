/******************************************************************************
    SPAZZbs - SPAZZ Text: animated text with neon, retro, glitch and CRT
    looks, as a scene source.
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
 * The text itself is drawn by data/text/index.html in a private browser
 * source this source owns (fonts, glows, gradients and per-letter animation
 * are what a browser does best). Settings reach the page two ways:
 *
 *   - written to a small JSON file the page reads when it loads, and
 *   - pushed live with obs-browser's "javascript_event" proc.
 *
 * The page's address never changes, so the browser never reloads. Text from
 * SPAZZamp or a text file is fetched here and pushed the same way.
 *
 * Reacting to sound: an audio capture callback on desktop audio or on
 * SPAZZamp's music source sums a few bands on the audio thread; the video
 * tick turns them into 0..1 levels (and beats) about 30 times a second and
 * sends them to the page. Nothing is attached while it's off or hidden.
 */

#include <obs-module.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "spazz-text-spec.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("spazz-text", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "SPAZZbs SPAZZ Text";
}

#define T_(s) obs_module_text(s)
#define FILE_MAX_BYTES (64 * 1024)

/* SPAZZamp keeps its music source on this output channel, marked with this
 * private setting (frontend/utility/SpazzMusicPlayer) */
#define MUSIC_CHANNEL 60
#define MUSIC_MARKER "spazz_music"
#define DESKTOP_CHANNEL 1
#define REACT_RATE (1.0f / 30.0f)

enum react_mode { REACT_OFF, REACT_DESKTOP, REACT_MUSIC };

/* what the audio thread hands the video tick */
struct band_sums {
	double level, bass, mid, treble;
	size_t frames;
};

struct spazz_text {
	obs_source_t *source;
	obs_source_t *browser;
	char *config_path;
	volatile long width, height;

	/* graphics thread (update and tick) */
	char kind[16];
	char *file_path;
	char *file_text;
	bool file_too_big; /* warned about it already */
	bool no_page;      /* the page or the browser isn't there: don't retry */
	float file_wait;
	float song_wait;
	char *song_json; /* last SPAZZamp song sent */

	/* reacting to sound */
	volatile long react;        /* enum react_mode */
	obs_weak_source_t *heard;   /* the source the capture callback is on */
	float find_wait, send_wait; /* graphics thread */
	float peak[4], bass_avg;    /* graphics thread: auto gain, beat */
	uint64_t last_beat_ns;
	pthread_mutex_t au_mutex; /* guards what follows */
	struct band_sums sums;
	float lp_bass1, lp_bass2, lp_mid; /* filter state */
	float k_bass, k_mid;              /* filter coefficients */

	obs_hotkey_id play_key, hide_key;
};

/* ------------------------------------------------------------------------ */
/* helpers                                                                   */

static long long obs_color_from_hex(uint32_t hex)
{
	uint32_t r = (hex >> 16) & 0xFF, g = (hex >> 8) & 0xFF, b = hex & 0xFF;
	return (long long)(0xFF000000u | (b << 16) | (g << 8) | r);
}

static void hex_from_obs_color(long long c, char out[8])
{
	uint32_t r = (uint32_t)c & 0xFF, g = ((uint32_t)c >> 8) & 0xFF, b = ((uint32_t)c >> 16) & 0xFF;
	snprintf(out, 8, "#%02x%02x%02x", r, g, b);
}

/* a local file as the browser's "http://absolute/" address, the same way
 * obs-browser does it for local files */
static void absolute_url(struct dstr *out, const char *path)
{
	static const char hexd[] = "0123456789ABCDEF";
	dstr_copy(out, "http://absolute/");
	for (const unsigned char *p = (const unsigned char *)path; p && *p; p++) {
		unsigned char ch = *p;
		if (ch == '\\') {
			ch = '/';
		}
		if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' ||
		    ch == '.' || ch == '_' || ch == '~' || ch == '/' || ch == ':') {
			dstr_ncat(out, (const char *)&ch, 1);
		} else {
			char esc[4] = {'%', hexd[ch >> 4], hexd[ch & 15], 0};
			dstr_cat(out, esc);
		}
	}
}

static void query_encode(struct dstr *out, const char *s)
{
	static const char hexd[] = "0123456789ABCDEF";
	for (const unsigned char *p = (const unsigned char *)s; p && *p; p++) {
		unsigned char ch = *p;
		if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' ||
		    ch == '.' || ch == '_' || ch == '~') {
			dstr_ncat(out, (const char *)&ch, 1);
		} else {
			char esc[4] = {'%', hexd[ch >> 4], hexd[ch & 15], 0};
			dstr_cat(out, esc);
		}
	}
}

static int preset_index(const char *name)
{
	for (int i = 0; name && i < STX_PRESET_COUNT; i++) {
		if (strcmp(stx_presets[i].name, name) == 0) {
			return i;
		}
	}
	return -1;
}

static bool key_is_preset(const char *setting)
{
	for (const struct stx_key *k = stx_keys; k->setting; k++) {
		if (strcmp(k->setting, setting) == 0) {
			return k->preset;
		}
	}
	return true;
}

/* a preset starts from a clean look: everything it governs goes back to its
 * default first, then the preset's own values. Your text, where it comes
 * from, its place on screen and its timing stay. */
static void apply_preset(obs_data_t *s, int idx)
{
	for (const struct stx_key *k = stx_keys; k->setting; k++) {
		if (k->preset) {
			obs_data_unset_user_value(s, k->setting);
		}
	}
	const struct stx_preset *pr = &stx_presets[idx];
	for (int i = 0; i < pr->count; i++) {
		const struct stx_preset_val *v = &pr->vals[i];
		/* your own settings: a preset only suggests one you haven't set */
		if (!key_is_preset(v->key) && obs_data_has_user_value(s, v->key)) {
			continue;
		}
		switch (v->kind) {
		case STX_STR:
			obs_data_set_string(s, v->key, v->str);
			break;
		case STX_COLOR:
			obs_data_set_int(s, v->key, obs_color_from_hex((uint32_t)v->num));
			break;
		case STX_NUM:
			obs_data_set_double(s, v->key, v->num);
			break;
		case STX_INT:
			obs_data_set_int(s, v->key, (long long)v->num);
			break;
		case STX_BOOL:
			obs_data_set_bool(s, v->key, v->num != 0.0);
			break;
		}
	}
}

static void send_event(struct spazz_text *st, const char *name, const char *json)
{
	if (!st || !st->browser) {
		return;
	}
	proc_handler_t *ph = obs_source_get_proc_handler(st->browser);
	if (!ph) {
		return;
	}
	calldata_t cd;
	calldata_init(&cd);
	calldata_set_string(&cd, "eventName", name);
	calldata_set_string(&cd, "jsonString", json ? json : "{}");
	proc_handler_call(ph, "javascript_event", &cd);
	calldata_free(&cd);
}

/* ------------------------------------------------------------------------ */
/* where the text comes from: SPAZZamp, a text file                          */

/* what SPAZZamp is playing, as {title, pos, dur, state} (state 1 = playing),
 * or NULL when SPAZZamp isn't there */
static char *song_json(void)
{
	calldata_t cd;
	calldata_init(&cd);
	char *json = NULL;
	if (proc_handler_call(obs_get_proc_handler(), "spazz_music_now", &cd)) {
		obs_data_t *d = obs_data_create();
		const char *title = calldata_string(&cd, "title");
		obs_data_set_string(d, "title", title ? title : "");
		obs_data_set_int(d, "pos", calldata_int(&cd, "pos_ms"));
		obs_data_set_int(d, "dur", calldata_int(&cd, "dur_ms"));
		obs_data_set_int(d, "state", calldata_int(&cd, "state"));
		json = bstrdup(obs_data_get_json(d));
		obs_data_release(d);
	}
	calldata_free(&cd);
	return json;
}

/* read every second: a file that's too big is reported once, not each time */
static char *read_text_file(const char *path, bool *too_big)
{
	if (!path || !*path) {
		return bstrdup("");
	}
	const int64_t size = os_get_file_size(path);
	if (size < 0) {
		return bstrdup("");
	}
	if (size > FILE_MAX_BYTES) {
		if (!*too_big) {
			blog(LOG_WARNING, "[SPAZZ Text] '%s' is over 64 KB; showing nothing", path);
		}
		*too_big = true;
		return bstrdup("");
	}
	*too_big = false;
	char *text = os_quick_read_utf8_file(path);
	return text ? text : bstrdup("");
}

/* ------------------------------------------------------------------------ */
/* reacting to sound                                                         */

static enum react_mode react_from(const char *v)
{
	if (v && strcmp(v, "desktop") == 0) {
		return REACT_DESKTOP;
	}
	if (v && strcmp(v, "music") == 0) {
		return REACT_MUSIC;
	}
	return REACT_OFF;
}

/* audio thread: a mono mix through two low-passes, summed per band. Bass is
 * under ~160 Hz, mid up to ~2.5 kHz, treble the rest. */
static void heard_audio(void *param, obs_source_t *source, const struct audio_data *audio, bool muted)
{
	UNUSED_PARAMETER(source);
	struct spazz_text *st = param;
	const float *c0 = (const float *)audio->data[0];
	if (!c0 || !audio->frames) {
		return;
	}
	const float *c1 = audio->data[1] ? (const float *)audio->data[1] : c0;

	pthread_mutex_lock(&st->au_mutex);
	float b1 = st->lp_bass1, b2 = st->lp_bass2, md = st->lp_mid;
	const float kb = st->k_bass, km = st->k_mid;
	double sl = 0.0, sb = 0.0, sm = 0.0, stt = 0.0;
	for (uint32_t i = 0; i < audio->frames; i++) {
		/* muted in the mixer: you can't hear it, so neither does the text */
		const float x = muted ? 0.0f : 0.5f * (c0[i] + c1[i]);
		b1 += kb * (x - b1);
		b2 += kb * (b1 - b2);
		md += km * (x - md);
		const float mid = md - b2, tre = x - md;
		sl += (double)(x * x);
		sb += (double)(b2 * b2);
		sm += (double)(mid * mid);
		stt += (double)(tre * tre);
	}
	st->lp_bass1 = b1;
	st->lp_bass2 = b2;
	st->lp_mid = md;
	st->sums.level += sl;
	st->sums.bass += sb;
	st->sums.mid += sm;
	st->sums.treble += stt;
	st->sums.frames += audio->frames;
	pthread_mutex_unlock(&st->au_mutex);
}

/* SPAZZamp's music source: on its channel, or anywhere by its marker */
static bool is_music(obs_source_t *src)
{
	obs_data_t *priv = src ? obs_source_get_private_settings(src) : NULL;
	const bool yes = priv && obs_data_get_bool(priv, MUSIC_MARKER);
	obs_data_release(priv);
	return yes;
}

static bool find_music_cb(void *param, obs_source_t *src)
{
	obs_source_t **found = param;
	if (is_music(src)) {
		*found = obs_source_get_ref(src);
		return false;
	}
	return true;
}

/* the source to listen to (a new reference), or NULL */
static obs_source_t *react_target(enum react_mode mode)
{
	if (mode == REACT_DESKTOP) {
		return obs_get_output_source(DESKTOP_CHANNEL);
	}
	if (mode == REACT_MUSIC) {
		obs_source_t *src = obs_get_output_source(MUSIC_CHANNEL);
		if (src && is_music(src)) {
			return src;
		}
		obs_source_release(src);
		obs_source_t *found = NULL;
		obs_enum_sources(find_music_cb, &found);
		return found;
	}
	return NULL;
}

static void stop_hearing(struct spazz_text *st)
{
	if (!st->heard) {
		return;
	}
	/* once this returns the callback won't run again: libobs holds the
	 * same lock while it calls capture callbacks */
	obs_source_t *src = obs_weak_source_get_source(st->heard);
	if (src) {
		obs_source_remove_audio_capture_callback(src, heard_audio, st);
		obs_source_release(src);
	}
	obs_weak_source_release(st->heard);
	st->heard = NULL;
	/* let the page settle at once */
	send_event(st, "spazzTextAudio", "{\"l\":0,\"b\":0,\"m\":0,\"t\":0,\"k\":0}");
}

static void start_hearing(struct spazz_text *st, obs_source_t *src)
{
	const uint32_t rate = audio_output_get_sample_rate(obs_get_audio());
	const float sr = rate ? (float)rate : 48000.0f;
	const float two_pi = 6.2831853f;
	pthread_mutex_lock(&st->au_mutex);
	st->k_bass = 1.0f - expf(-two_pi * 160.0f / sr);
	st->k_mid = 1.0f - expf(-two_pi * 2500.0f / sr);
	st->lp_bass1 = st->lp_bass2 = st->lp_mid = 0.0f;
	memset(&st->sums, 0, sizeof(st->sums));
	pthread_mutex_unlock(&st->au_mutex);
	for (int i = 0; i < 4; i++) {
		st->peak[i] = 0.0f;
	}
	st->bass_avg = 0.0f;
	st->heard = obs_source_get_weak_source(src);
	obs_source_add_audio_capture_callback(src, heard_audio, st);
}

/* RMS, auto-gained against the last few seconds so quiet and loud sources
 * both move the text; then a little curve for more contrast */
static float shape_level(float rms, float *peak, float dt)
{
	const float decay = powf(0.5f, dt / 3.0f); /* the peak halves in 3 s */
	*peak = rms > *peak * decay ? rms : *peak * decay;
	if (rms < 0.002f) {
		return 0.0f;
	}
	const float floor_ = 0.02f;
	float x = rms / (*peak > floor_ ? *peak : floor_);
	x = x > 1.0f ? 1.0f : x;
	return powf(x, 1.6f);
}

static void react_tick(struct spazz_text *st, float seconds)
{
	const enum react_mode mode = (enum react_mode)os_atomic_load_long(&st->react);
	if (mode == REACT_OFF || !st->browser || !obs_source_showing(st->source)) {
		stop_hearing(st);
		st->find_wait = 0.0f;
		return;
	}

	/* keep listening to the right source (it can come, go or change) */
	st->find_wait -= seconds;
	if (st->find_wait <= 0.0f) {
		st->find_wait = 1.0f;
		obs_source_t *want = react_target(mode);
		if (!want) {
			stop_hearing(st);
		} else if (!st->heard || !obs_weak_source_references_source(st->heard, want)) {
			stop_hearing(st);
			start_hearing(st, want);
		}
		obs_source_release(want);
	}
	if (!st->heard) {
		return;
	}

	st->send_wait += seconds;
	if (st->send_wait < REACT_RATE) {
		return;
	}
	const float dt = st->send_wait;
	st->send_wait = 0.0f;

	struct band_sums got;
	pthread_mutex_lock(&st->au_mutex);
	got = st->sums;
	memset(&st->sums, 0, sizeof(st->sums));
	pthread_mutex_unlock(&st->au_mutex);

	const double n = got.frames ? (double)got.frames : 1.0;
	const float rms[4] = {(float)sqrt(got.level / n), (float)sqrt(got.bass / n), (float)sqrt(got.mid / n),
			      (float)sqrt(got.treble / n)};
	float v[4];
	for (int i = 0; i < 4; i++) {
		v[i] = shape_level(rms[i], &st->peak[i], dt);
	}

	/* a beat: bass jumping well above its recent average */
	int beat = 0;
	const uint64_t now = os_gettime_ns();
	if (rms[1] > 0.01f && rms[1] > st->bass_avg * 1.4f && now - st->last_beat_ns > 200000000ULL) {
		beat = 1;
		st->last_beat_ns = now;
	}
	st->bass_avg += (rms[1] - st->bass_avg) * (1.0f - expf(-dt / 1.0f));

	char json[96];
	snprintf(json, sizeof(json), "{\"l\":%.3f,\"b\":%.3f,\"m\":%.3f,\"t\":%.3f,\"k\":%d}", v[0], v[1], v[2], v[3],
		 beat);
	send_event(st, "spazzTextAudio", json);
}

/* ------------------------------------------------------------------------ */
/* the page's settings                                                       */

static char *build_config(struct spazz_text *st, obs_data_t *s)
{
	obs_data_t *c = obs_data_create();
	for (const struct stx_key *k = stx_keys; k->setting; k++) {
		switch (k->kind) {
		case STX_STR:
			obs_data_set_string(c, k->page, obs_data_get_string(s, k->setting));
			break;
		case STX_COLOR: {
			char hex[8];
			hex_from_obs_color(obs_data_get_int(s, k->setting), hex);
			obs_data_set_string(c, k->page, hex);
			break;
		}
		case STX_NUM:
			obs_data_set_double(c, k->page, obs_data_get_double(s, k->setting));
			break;
		case STX_INT:
			obs_data_set_int(c, k->page, obs_data_get_int(s, k->setting));
			break;
		case STX_BOOL:
			obs_data_set_bool(c, k->page, obs_data_get_bool(s, k->setting));
			break;
		}
	}
	/* what a reloaded page should show right away */
	if (st->file_text) {
		obs_data_set_string(c, "fileText", st->file_text);
	}
	if (st->song_json) {
		obs_data_t *song = obs_data_create_from_json(st->song_json);
		if (song) {
			obs_data_set_obj(c, "song", song);
			obs_data_release(song);
		}
	}
	char *json = bstrdup(obs_data_get_json(c));
	obs_data_release(c);
	return json;
}

static void make_browser(struct spazz_text *st, uint32_t w, uint32_t h)
{
	char *page = obs_module_file("text/index.html");
	if (!page) {
		blog(LOG_ERROR, "[SPAZZ Text] text/index.html is missing");
		st->no_page = true;
		return;
	}

	struct dstr url = {0}, cfg = {0};
	absolute_url(&url, page);
	absolute_url(&cfg, st->config_path ? st->config_path : "");
	dstr_cat(&url, "?cfg=");
	query_encode(&url, cfg.array);

	obs_data_t *bs = obs_data_create();
	obs_data_set_bool(bs, "is_local_file", false);
	obs_data_set_string(bs, "url", url.array);
	obs_data_set_int(bs, "width", w);
	obs_data_set_int(bs, "height", h);
	obs_data_set_bool(bs, "fps_custom", true);
	obs_data_set_int(bs, "fps", 60);
	obs_data_set_bool(bs, "shutdown", false);
	obs_data_set_bool(bs, "restart_when_active", false);
	obs_data_set_bool(bs, "reroute_audio", true);
	obs_data_set_string(bs, "css",
			    "body { background-color: rgba(0, 0, 0, 0); margin: 0px auto; overflow: hidden; }");

	st->browser = obs_source_create_private("browser_source", "SPAZZ Text page", bs);
	if (!st->browser) {
		blog(LOG_WARNING, "[SPAZZ Text] the browser source isn't available");
		st->no_page = true;
	} else {
		obs_source_add_active_child(st->source, st->browser);
	}

	obs_data_release(bs);
	dstr_free(&url);
	dstr_free(&cfg);
	bfree(page);
}

/* ------------------------------------------------------------------------ */
/* source callbacks                                                          */

static const char *stx_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return T_("SpazzText");
}

static void stx_update(void *data, obs_data_t *settings)
{
	struct spazz_text *st = data;

	long long w = obs_data_get_int(settings, "width");
	long long h = obs_data_get_int(settings, "height");
	w = w < 32 ? 32 : (w > 7680 ? 7680 : w);
	h = h < 32 ? 32 : (h > 4320 ? 4320 : h);
	const bool resized = os_atomic_load_long(&st->width) != (long)w || os_atomic_load_long(&st->height) != (long)h;
	os_atomic_set_long(&st->width, (long)w);
	os_atomic_set_long(&st->height, (long)h);

	snprintf(st->kind, sizeof(st->kind), "%s", obs_data_get_string(settings, "source"));
	const long react = (long)react_from(obs_data_get_string(settings, "react"));
	if (os_atomic_set_long(&st->react, react) != react) {
		st->find_wait = 0.0f; /* look for the new source right away */
	}

	/* a text file: read it now (and on every change, in tick) */
	const char *path = obs_data_get_string(settings, "file_path");
	if (strcmp(st->kind, "file") == 0) {
		if (!st->file_path || strcmp(st->file_path, path) != 0 || !st->file_text) {
			bfree(st->file_path);
			st->file_path = bstrdup(path);
			bfree(st->file_text);
			st->file_too_big = false;
			st->file_text = read_text_file(path, &st->file_too_big);
			st->file_wait = 1.0f;
		}
	} else {
		bfree(st->file_text);
		st->file_text = NULL;
	}
	if (strcmp(st->kind, "nowplaying") == 0 && !st->song_json) {
		st->song_json = song_json();
	}

	char *json = build_config(st, settings);
	if (st->config_path) {
		os_quick_write_utf8_file_safe(st->config_path, json, strlen(json), false, "tmp", NULL);
	}

	if (!st->browser) {
		if (!st->no_page) {
			make_browser(st, (uint32_t)w, (uint32_t)h);
		}
	} else {
		if (resized) {
			obs_data_t *bs = obs_source_get_settings(st->browser);
			obs_data_set_int(bs, "width", w);
			obs_data_set_int(bs, "height", h);
			obs_source_update(st->browser, bs);
			obs_data_release(bs);
		}
		send_event(st, "spazzTextConfig", json);
	}
	bfree(json);
}

/* sound levels, SPAZZamp's song (twice a second) and the text file (every
 * second) */
static void stx_tick(void *data, float seconds)
{
	struct spazz_text *st = data;
	react_tick(st, seconds);
	if (!st->browser || !obs_source_showing(st->source)) {
		return;
	}

	if (strcmp(st->kind, "nowplaying") == 0) {
		st->song_wait -= seconds;
		if (st->song_wait <= 0.0f) {
			st->song_wait = 0.5f;
			char *json = song_json();
			if (json && (!st->song_json || strcmp(json, st->song_json) != 0)) {
				send_event(st, "spazzTextNow", json);
				bfree(st->song_json);
				st->song_json = json;
			} else {
				bfree(json);
			}
		}
	} else if (strcmp(st->kind, "file") == 0) {
		st->file_wait -= seconds;
		if (st->file_wait <= 0.0f) {
			st->file_wait = 1.0f;
			char *text = read_text_file(st->file_path, &st->file_too_big);
			if (!st->file_text || strcmp(text, st->file_text) != 0) {
				obs_data_t *d = obs_data_create();
				obs_data_set_string(d, "text", text);
				obs_data_set_bool(d, "replay", true);
				send_event(st, "spazzTextValue", obs_data_get_json(d));
				obs_data_release(d);
				bfree(st->file_text);
				st->file_text = text;
			} else {
				bfree(text);
			}
		}
	}
}

static void stx_defaults(obs_data_t *s)
{
	for (const struct stx_key *k = stx_keys; k->setting; k++) {
		switch (k->kind) {
		case STX_STR:
			obs_data_set_default_string(s, k->setting, k->str);
			break;
		case STX_COLOR:
			obs_data_set_default_int(s, k->setting, obs_color_from_hex((uint32_t)k->num));
			break;
		case STX_NUM:
			obs_data_set_default_double(s, k->setting, k->num);
			break;
		case STX_INT:
			obs_data_set_default_int(s, k->setting, (long long)k->num);
			break;
		case STX_BOOL:
			obs_data_set_default_bool(s, k->setting, k->num != 0.0);
			break;
		}
	}
	obs_data_set_default_string(s, "text", T_("DefaultText"));
	obs_data_set_default_string(s, "preset", "");
	obs_data_set_default_string(s, "file_path", "");

	struct obs_video_info ovi;
	const bool have = obs_get_video_info(&ovi);
	obs_data_set_default_int(s, "width", have ? ovi.base_width : 1920);
	obs_data_set_default_int(s, "height", have ? ovi.base_height : 1080);
}

/* ---- properties ---- */

static bool preset_modified(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);
	const char *preset = obs_data_get_string(settings, "preset");
	const char *applied = obs_data_get_string(settings, "preset_applied");
	if (!preset || !*preset) {
		obs_data_set_string(settings, "preset_applied", "");
		return false;
	}
	if (applied && strcmp(preset, applied) == 0) {
		return false;
	}
	const int idx = preset_index(preset);
	if (idx < 0) {
		return false;
	}
	char name[128];
	snprintf(name, sizeof(name), "%s", preset);
	apply_preset(settings, idx);
	obs_data_set_string(settings, "preset", name);
	obs_data_set_string(settings, "preset_applied", name);
	return true;
}

static void show(obs_properties_t *props, const char *key, bool on)
{
	obs_property_t *p = obs_properties_get(props, key);
	if (p) {
		obs_property_set_visible(p, on);
	}
}

static bool is(obs_data_t *s, const char *key, const char *value)
{
	const char *v = obs_data_get_string(s, key);
	return v && strcmp(v, value) == 0;
}

/* only what applies is shown */
static bool refresh_visible(obs_properties_t *props, obs_property_t *prop, obs_data_t *s)
{
	UNUSED_PARAMETER(prop);
	show(props, "text", is(s, "source", "typed"));
	show(props, "np_format", is(s, "source", "nowplaying"));
	show(props, "np_hide", is(s, "source", "nowplaying"));
	show(props, "clock_format", is(s, "source", "clock"));
	const bool cd = is(s, "source", "countdown");
	show(props, "countdown_kind", cd);
	show(props, "countdown_min", cd && is(s, "countdown_kind", "minutes"));
	show(props, "countdown_time", cd && is(s, "countdown_kind", "time"));
	show(props, "countdown_done", cd);
	show(props, "file_path", is(s, "source", "file"));

	const bool solid = is(s, "fill", "solid");
	const bool rainbow = is(s, "fill", "rainbow");
	show(props, "color1", !rainbow);
	show(props, "color2", !solid && !rainbow);
	show(props, "color3", !solid && !rainbow && !is(s, "fill", "grad2"));
	show(props, "grad_angle", is(s, "fill", "grad2") || is(s, "fill", "grad3"));

	const bool box = !is(s, "bg_shape", "none");
	show(props, "bg_color", box);
	show(props, "bg_opacity", box);
	show(props, "bg_padding", box);
	/* the shapes the page gives rounded corners or a border (a pill is
	 * always round; a window, a tape label and a burst draw their own edge) */
	const bool corners = is(s, "bg_shape", "box") || is(s, "bg_shape", "slant") || is(s, "bg_shape", "terminal") ||
			     is(s, "bg_shape", "tape") || is(s, "bg_shape", "bubble");
	const bool border = is(s, "bg_shape", "box") || is(s, "bg_shape", "pill") || is(s, "bg_shape", "bar") ||
			    is(s, "bg_shape", "slant") || is(s, "bg_shape", "terminal") || is(s, "bg_shape", "bubble");
	show(props, "bg_radius", corners);
	show(props, "bg_border", border);
	show(props, "bg_border_width", border);

	show(props, "glitch_every", is(s, "idle", "glitch"));

	const bool react = !is(s, "react", "off");
	static const char *react_keys[] = {"react_size",  "react_glow",  "react_bounce", "react_color",
					   "react_shake", "react_flash", "react_sens",   "react_smooth"};
	for (size_t i = 0; i < sizeof(react_keys) / sizeof(react_keys[0]); i++) {
		show(props, react_keys[i], react);
	}
	show(props, "rgb_speed", obs_data_get_bool(s, "rgb_cycle"));
	const bool cycle = obs_data_get_bool(s, "loop_cycle");
	show(props, "show_sec", cycle);
	show(props, "hide_sec", cycle);
	return true;
}

static bool play_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);
	send_event(data, "spazzTextPlay", "{}");
	return false;
}

static bool hide_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);
	send_event(data, "spazzTextHide", "{}");
	return false;
}

static obs_property_t *add_list(obs_properties_t *props, const char *key, const char *label,
				const struct stx_option *opts)
{
	obs_property_t *p = obs_properties_add_list(props, key, label, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	for (const struct stx_option *o = opts; o->value; o++) {
		obs_property_list_add_string(p, o->label, o->value);
	}
	return p;
}

static obs_property_t *slider(obs_properties_t *props, const char *key, const char *label, double lo, double hi,
			      double step, const char *suffix)
{
	obs_property_t *p = obs_properties_add_float_slider(props, key, label, lo, hi, step);
	if (suffix) {
		obs_property_float_set_suffix(p, suffix);
	}
	return p;
}

static obs_property_t *islider(obs_properties_t *props, const char *key, const char *label, int lo, int hi, int step,
			       const char *suffix)
{
	obs_property_t *p = obs_properties_add_int_slider(props, key, label, lo, hi, step);
	if (suffix) {
		obs_property_int_set_suffix(p, suffix);
	}
	return p;
}

static obs_properties_t *stx_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	/* preset first: it's the quickest way to a look */
	p = obs_properties_add_list(props, "preset", T_("Preset"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_("Preset.Custom"), "");
	for (int i = 0; i < STX_PRESET_COUNT; i++) {
		obs_property_list_add_string(p, stx_presets[i].name, stx_presets[i].name);
	}
	obs_property_set_long_description(p, T_("Preset.Tip"));
	obs_property_set_modified_callback(p, preset_modified);
	obs_properties_add_button2(props, "play", T_("Play"), play_clicked, data);
	obs_properties_add_button2(props, "hide", T_("Hide"), hide_clicked, data);

	/* text */
	obs_properties_t *text = obs_properties_create();
	p = add_list(text, "source", T_("Source"), stx_source);
	obs_property_set_modified_callback(p, refresh_visible);
	obs_properties_add_text(text, "text", T_("Text"), OBS_TEXT_MULTILINE);
	p = obs_properties_add_text(text, "np_format", T_("NpFormat"), OBS_TEXT_DEFAULT);
	obs_property_set_long_description(p, T_("NpFormat.Tip"));
	obs_properties_add_bool(text, "np_hide", T_("NpHide"));
	add_list(text, "clock_format", T_("ClockFormat"), stx_clock);
	p = add_list(text, "countdown_kind", T_("CountdownKind"), stx_cdKind);
	obs_property_set_modified_callback(p, refresh_visible);
	slider(text, "countdown_min", T_("CountdownMin"), 0.5, 600.0, 0.5, " min");
	p = obs_properties_add_text(text, "countdown_time", T_("CountdownTime"), OBS_TEXT_DEFAULT);
	obs_property_set_long_description(p, T_("CountdownTime.Tip"));
	obs_properties_add_text(text, "countdown_done", T_("CountdownDone"), OBS_TEXT_DEFAULT);
	obs_properties_add_path(text, "file_path", T_("FilePath"), OBS_PATH_FILE, T_("FileFilter"), NULL);
	obs_properties_add_group(props, "grp_text", T_("Group.Text"), OBS_GROUP_NORMAL, text);

	/* look */
	obs_properties_t *look = obs_properties_create();
	add_list(look, "font", T_("Font"), stx_font);
	islider(look, "size", T_("Size"), 8, 600, 1, " px");
	add_list(look, "weight", T_("Weight"), stx_weight);
	obs_properties_add_bool(look, "italic", T_("Italic"));
	obs_properties_add_bool(look, "upper", T_("Upper"));
	slider(look, "spacing", T_("Spacing"), -0.1, 1.0, 0.01, " em");
	slider(look, "line_height", T_("LineHeight"), 0.6, 3.0, 0.05, NULL);
	p = add_list(look, "fill", T_("Fill"), stx_fill);
	obs_property_set_modified_callback(p, refresh_visible);
	obs_properties_add_color(look, "color1", T_("Color1"));
	obs_properties_add_color(look, "color2", T_("Color2"));
	obs_properties_add_color(look, "color3", T_("Color3"));
	islider(look, "grad_angle", T_("GradAngle"), 0, 360, 5, "\xc2\xb0");
	obs_properties_add_color(look, "outline_color", T_("OutlineColor"));
	slider(look, "outline_width", T_("OutlineWidth"), 0.0, 20.0, 0.5, " px");
	slider(look, "opacity", T_("Opacity"), 0.0, 1.0, 0.01, NULL);
	obs_properties_add_group(props, "grp_look", T_("Group.Look"), OBS_GROUP_NORMAL, look);

	/* glow & effects */
	obs_properties_t *fx = obs_properties_create();
	obs_properties_add_color(fx, "glow_color", T_("GlowColor"));
	slider(fx, "glow", T_("Glow"), 0.0, 3.0, 0.05, NULL);
	islider(fx, "extrude", T_("Extrude"), 0, 40, 1, " px");
	obs_properties_add_color(fx, "extrude_color", T_("ExtrudeColor"));
	obs_properties_add_color(fx, "shadow_color", T_("ShadowColor"));
	slider(fx, "shadow_opacity", T_("ShadowOpacity"), 0.0, 1.0, 0.01, NULL);
	islider(fx, "shadow_x", T_("ShadowX"), -100, 100, 1, " px");
	islider(fx, "shadow_y", T_("ShadowY"), -100, 100, 1, " px");
	islider(fx, "shadow_blur", T_("ShadowBlur"), 0, 100, 1, " px");
	p = add_list(fx, "bg_shape", T_("BgShape"), stx_bgShape);
	obs_property_set_modified_callback(p, refresh_visible);
	obs_properties_add_color(fx, "bg_color", T_("BgColor"));
	slider(fx, "bg_opacity", T_("BgOpacity"), 0.0, 1.0, 0.01, NULL);
	islider(fx, "bg_padding", T_("BgPadding"), 0, 200, 1, " px");
	islider(fx, "bg_radius", T_("BgRadius"), 0, 200, 1, " px");
	obs_properties_add_color(fx, "bg_border", T_("BgBorder"));
	islider(fx, "bg_border_width", T_("BgBorderWidth"), 0, 20, 1, " px");
	p = obs_properties_add_bool(fx, "rgb_cycle", T_("RgbCycle"));
	obs_property_set_modified_callback(p, refresh_visible);
	slider(fx, "rgb_speed", T_("RgbSpeed"), 0.05, 5.0, 0.05, NULL);
	slider(fx, "scanlines", T_("Scanlines"), 0.0, 1.0, 0.01, NULL);
	slider(fx, "chroma", T_("Chroma"), 0.0, 20.0, 0.1, " px");
	obs_properties_add_group(props, "grp_fx", T_("Group.Effects"), OBS_GROUP_NORMAL, fx);

	/* animation */
	obs_properties_t *anim = obs_properties_create();
	p = add_list(anim, "anim_by", T_("AnimBy"), stx_animBy);
	obs_property_set_long_description(p, T_("AnimBy.Tip"));
	add_list(anim, "enter", T_("Enter"), stx_enter);
	p = add_list(anim, "idle", T_("Idle"), stx_idle);
	obs_property_set_modified_callback(p, refresh_visible);
	add_list(anim, "exit", T_("Exit"), stx_exit);
	slider(anim, "idle_speed", T_("IdleSpeed"), 0.1, 4.0, 0.05, NULL);
	slider(anim, "idle_amount", T_("IdleAmount"), 0.0, 3.0, 0.05, NULL);
	slider(anim, "glitch_every", T_("GlitchEvery"), 0.5, 30.0, 0.5, " s");
	obs_properties_add_group(props, "grp_anim", T_("Group.Animation"), OBS_GROUP_NORMAL, anim);

	/* reacting to sound */
	obs_properties_t *snd = obs_properties_create();
	p = add_list(snd, "react", T_("React"), stx_react);
	obs_property_set_long_description(p, T_("React.Tip"));
	obs_property_set_modified_callback(p, refresh_visible);
	obs_properties_add_bool(snd, "react_size", T_("ReactSize"));
	obs_properties_add_bool(snd, "react_glow", T_("ReactGlow"));
	p = obs_properties_add_bool(snd, "react_bounce", T_("ReactBounce"));
	obs_property_set_long_description(p, T_("ReactBounce.Tip"));
	obs_properties_add_bool(snd, "react_color", T_("ReactColor"));
	obs_properties_add_bool(snd, "react_shake", T_("ReactShake"));
	obs_properties_add_bool(snd, "react_flash", T_("ReactFlash"));
	slider(snd, "react_sens", T_("ReactSens"), 0.0, 3.0, 0.05, NULL);
	p = slider(snd, "react_smooth", T_("ReactSmooth"), 0.0, 1.0, 0.01, NULL);
	obs_property_set_long_description(p, T_("ReactSmooth.Tip"));
	obs_properties_add_group(props, "grp_react", T_("Group.React"), OBS_GROUP_NORMAL, snd);

	/* timing */
	obs_properties_t *timing = obs_properties_create();
	islider(timing, "enter_ms", T_("EnterMs"), 0, 5000, 50, " ms");
	islider(timing, "exit_ms", T_("ExitMs"), 0, 5000, 50, " ms");
	p = islider(timing, "stagger_ms", T_("Stagger"), 0, 500, 5, " ms");
	obs_property_set_long_description(p, T_("Stagger.Tip"));
	obs_properties_add_bool(timing, "replay_on_show", T_("ReplayOnShow"));
	p = obs_properties_add_bool(timing, "loop_cycle", T_("Cycle"));
	obs_property_set_long_description(p, T_("Cycle.Tip"));
	obs_property_set_modified_callback(p, refresh_visible);
	slider(timing, "show_sec", T_("ShowSec"), 0.5, 600.0, 0.5, " s");
	slider(timing, "hide_sec", T_("HideSec"), 0.0, 600.0, 0.5, " s");
	obs_properties_add_group(props, "grp_timing", T_("Group.Timing"), OBS_GROUP_NORMAL, timing);

	/* where it sits */
	obs_properties_t *place = obs_properties_create();
	add_list(place, "align_h", T_("AlignH"), stx_alignH);
	add_list(place, "align_v", T_("AlignV"), stx_alignV);
	islider(place, "margin", T_("Margin"), 0, 1000, 1, " px");
	obs_properties_add_bool(place, "wrap", T_("Wrap"));
	p = obs_properties_add_int(place, "width", T_("Width"), 32, 7680, 1);
	obs_property_set_long_description(p, T_("Size.Tip"));
	obs_properties_add_int(place, "height", T_("Height"), 32, 4320, 1);
	obs_properties_add_group(props, "grp_place", T_("Group.Place"), OBS_GROUP_NORMAL, place);

	return props;
}

/* ---- hotkeys / procs ---- */

static void hk_play(void *data, obs_hotkey_id id, obs_hotkey_t *hk, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hk);
	if (pressed) {
		send_event(data, "spazzTextPlay", "{}");
	}
}

static void hk_hide(void *data, obs_hotkey_id id, obs_hotkey_t *hk, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hk);
	if (pressed) {
		send_event(data, "spazzTextHide", "{}");
	}
}

static void proc_play(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	send_event(data, "spazzTextPlay", "{}");
}

static void proc_hide(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	send_event(data, "spazzTextHide", "{}");
}

/* the page lives at a fixed address; the settings file is per source */
static char *config_path_for(obs_source_t *source)
{
	struct dstr name = {0};
	dstr_printf(&name, "text/%s.json", obs_source_get_uuid(source));
	char *path = obs_module_config_path(name.array);
	dstr_free(&name);
	if (path) {
		char *dir = obs_module_config_path("text");
		if (dir) {
			os_mkdirs(dir);
			bfree(dir);
		}
	}
	return path;
}

static void *stx_create(obs_data_t *settings, obs_source_t *source)
{
	struct spazz_text *st = bzalloc(sizeof(*st));
	st->source = source;
	pthread_mutex_init(&st->au_mutex, NULL);
	st->config_path = config_path_for(source);

	/* a brand new text starts with the first preset's look */
	if (!obs_data_has_user_value(settings, "preset_applied")) {
		apply_preset(settings, 0);
		obs_data_set_string(settings, "preset", stx_presets[0].name);
		obs_data_set_string(settings, "preset_applied", stx_presets[0].name);
	}

	st->play_key = obs_hotkey_register_source(source, "SpazzText.Play", T_("Hotkey.Play"), hk_play, st);
	st->hide_key = obs_hotkey_register_source(source, "SpazzText.Hide", T_("Hotkey.Hide"), hk_hide, st);

	proc_handler_t *ph = obs_source_get_proc_handler(source);
	proc_handler_add(ph, "void play()", proc_play, st);
	proc_handler_add(ph, "void hide()", proc_hide, st);

	stx_update(st, settings);
	return st;
}

static void stx_destroy(void *data)
{
	struct spazz_text *st = data;
	/* libobs drops the hotkeys only after this returns; a key pressed in
	 * between would reach freed memory (unregistering waits for a running
	 * hotkey callback, which holds the same lock) */
	obs_hotkey_unregister(st->play_key);
	obs_hotkey_unregister(st->hide_key);
	/* after this the audio thread never calls us again */
	stop_hearing(st);
	if (st->browser) {
		obs_source_remove_active_child(st->source, st->browser);
		obs_source_release(st->browser);
	}
	/* a deleted text doesn't need its settings file any more */
	if (st->config_path && obs_source_removed(st->source)) {
		os_unlink(st->config_path);
	}
	bfree(st->config_path);
	bfree(st->file_path);
	bfree(st->file_text);
	bfree(st->song_json);
	pthread_mutex_destroy(&st->au_mutex);
	bfree(st);
}

static void stx_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct spazz_text *st = data;
	if (st->browser) {
		obs_source_video_render(st->browser);
	}
}

static uint32_t stx_width(void *data)
{
	struct spazz_text *st = data;
	return (uint32_t)os_atomic_load_long(&st->width);
}

static uint32_t stx_height(void *data)
{
	struct spazz_text *st = data;
	return (uint32_t)os_atomic_load_long(&st->height);
}

static void stx_enum_sources(void *data, obs_source_enum_proc_t cb, void *param)
{
	struct spazz_text *st = data;
	if (st->browser) {
		cb(st->source, st->browser, param);
	}
}

static enum gs_color_space stx_color_space(void *data, size_t count, const enum gs_color_space *preferred)
{
	struct spazz_text *st = data;
	if (st->browser) {
		return obs_source_get_color_space(st->browser, count, preferred);
	}
	return count ? preferred[0] : GS_CS_SRGB;
}

static struct obs_source_info spazz_text_info = {
	.id = "spazz_text",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = stx_get_name,
	.create = stx_create,
	.destroy = stx_destroy,
	.update = stx_update,
	.get_defaults = stx_defaults,
	.get_properties = stx_properties,
	.video_tick = stx_tick,
	.video_render = stx_render,
	.get_width = stx_width,
	.get_height = stx_height,
	.enum_active_sources = stx_enum_sources,
	.enum_all_sources = stx_enum_sources,
	.video_get_color_space = stx_color_space,
	.icon_type = OBS_ICON_TYPE_TEXT,
};

bool obs_module_load(void)
{
	obs_register_source(&spazz_text_info);
	return true;
}
