/******************************************************************************
    SPAZZbs - SPONSORCENTER: an animated sponsor spot as a scene source.
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
 * The animation itself is data/sponsor/index.html, drawn by a private
 * browser source this source owns. Settings reach it two ways:
 *
 *   - written to a small JSON file the page reads when it loads, and
 *   - pushed live with obs-browser's "javascript_event" proc, so changes
 *     show instantly without reloading the page.
 *
 * The page's address never changes, so the browser never reloads.
 */

#include <obs-module.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "spazz-sponsor-spec.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("spazz-sponsor", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "SPAZZbs SPONSORCENTER";
}

#define T_(s) obs_module_text(s)

struct spazz_sponsor {
	obs_source_t *source;
	obs_source_t *browser;
	char *config_path;
	volatile long width, height;
	bool preview;
	uint64_t last_write_ns;

	/* whether the wheel may turn: the Sponsors switch, and streaming /
	 * recording if this spot waits for them. Set from the UI thread. */
	volatile long st_switch, st_streaming, st_recording;
	volatile long run_when; /* 0 always, 1 streaming, 2 streaming or recording */
	volatile long running;
};

static bool compute_run(struct spazz_sponsor *sp)
{
	if (!os_atomic_load_long(&sp->st_switch)) {
		return false;
	}
	switch (os_atomic_load_long(&sp->run_when)) {
	case 1:
		return os_atomic_load_long(&sp->st_streaming) != 0;
	case 2:
		return os_atomic_load_long(&sp->st_streaming) || os_atomic_load_long(&sp->st_recording);
	default:
		return true;
	}
}

static long run_when_index(const char *v)
{
	if (v && strcmp(v, "streaming") == 0) {
		return 1;
	}
	if (v && strcmp(v, "live") == 0) {
		return 2;
	}
	return 0;
}

static const char *mask_path(const char *id)
{
	for (const struct spn_shape *m = spn_masks; id && m->id; m++) {
		if (strcmp(m->id, id) == 0) {
			return m->path;
		}
	}
	return "";
}

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

/* percent-encode for a query string value */
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
	for (int i = 0; name && i < SPN_PRESET_COUNT; i++) {
		if (strcmp(spn_presets[i].name, name) == 0) {
			return i;
		}
	}
	return -1;
}

static void apply_preset(obs_data_t *s, int idx)
{
	if (idx < 0 || idx >= SPN_PRESET_COUNT) {
		return;
	}
	/* the look and motion only: text, images and timing are yours */
	obs_data_set_string(s, "idle", "none");
	obs_data_set_string(s, "font", "");
	obs_data_set_double(s, "size", 1.0);
	obs_data_set_int(s, "margin_y", 60);
	const struct spn_preset *pr = &spn_presets[idx];
	for (int i = 0; i < pr->count; i++) {
		const struct spn_preset_val *v = &pr->vals[i];
		switch (v->kind) {
		case SPN_STR:
			obs_data_set_string(s, v->key, v->str);
			break;
		case SPN_COLOR:
			obs_data_set_int(s, v->key, obs_color_from_hex((uint32_t)v->num));
			break;
		case SPN_NUM:
			obs_data_set_double(s, v->key, v->num);
			break;
		case SPN_INT:
			obs_data_set_int(s, v->key, (long long)v->num);
			break;
		}
	}

	/* ... and each sponsor's own motion follows the preset */
	static const char *per_item[] = {"enter", "exit", "text_anim", "logo_anim"};
	obs_data_array_t *items = obs_data_get_array(s, "items");
	for (size_t i = 0; i < obs_data_array_count(items); i++) {
		obs_data_t *it = obs_data_array_item(items, i);
		for (size_t k = 0; k < sizeof(per_item) / sizeof(per_item[0]); k++) {
			obs_data_set_string(it, per_item[k], obs_data_get_string(s, per_item[k]));
		}
		obs_data_release(it);
	}
	obs_data_array_release(items);
}

/* a new sponsor, moving the way the preset says */
static obs_data_t *new_item(obs_data_t *s, const char *head, const char *sub, const char *image)
{
	obs_data_t *it = obs_data_create();
	obs_data_set_string(it, "image", image ? image : "");
	obs_data_set_string(it, "layout", "left");
	obs_data_set_double(it, "image_size", 1.0);
	obs_data_set_string(it, "head", head ? head : "");
	obs_data_set_string(it, "sub", sub ? sub : "");
	obs_data_set_string(it, "badge", "");
	obs_data_set_string(it, "text_anim", obs_data_get_string(s, "text_anim"));
	obs_data_set_string(it, "logo_anim", obs_data_get_string(s, "logo_anim"));
	obs_data_set_string(it, "enter", obs_data_get_string(s, "enter"));
	obs_data_set_string(it, "exit", obs_data_get_string(s, "exit"));
	obs_data_set_double(it, "show_sec", 8.0);
	obs_data_set_bool(it, "off", false);
	obs_data_set_string(it, "mask", "frame");
	obs_data_set_string(it, "aspect", "square");
	obs_data_set_string(it, "fit", "contain");
	obs_data_set_double(it, "zoom", 1.0);
	obs_data_set_double(it, "pan_x", 0.0);
	obs_data_set_double(it, "pan_y", 0.0);
	obs_data_set_double(it, "volume", 1.0);
	return it;
}

/* "Headline | small line" -> its two halves */
static void split_line(const char *line, struct dstr *head, struct dstr *sub)
{
	const char *bar = strchr(line, '|');
	if (bar) {
		dstr_ncopy(head, line, (size_t)(bar - line));
		dstr_copy(sub, bar + 1);
	} else {
		dstr_copy(head, line);
	}
	dstr_depad(head);
	dstr_depad(sub);
}

/* SPONSORCENTER v1 kept one message list and one image pool; turn them into
 * one sponsor per pairing so nothing set up before is lost */
static void migrate_items(obs_data_t *s)
{
	obs_data_array_t *msgs = obs_data_get_array(s, "messages");
	obs_data_array_t *imgs = obs_data_get_array(s, "images");
	DARRAY(char *) heads, subs, files;
	da_init(heads);
	da_init(subs);
	da_init(files);

	for (size_t i = 0; i < obs_data_array_count(msgs); i++) {
		obs_data_t *m = obs_data_array_item(msgs, i);
		const char *line = obs_data_get_string(m, "value");
		if (line && *line && !obs_data_get_bool(m, "hidden")) {
			struct dstr head = {0}, sub = {0};
			split_line(line, &head, &sub);
			char *h = bstrdup(head.array ? head.array : "");
			char *u = bstrdup(sub.array ? sub.array : "");
			da_push_back(heads, &h);
			da_push_back(subs, &u);
			dstr_free(&head);
			dstr_free(&sub);
		}
		obs_data_release(m);
	}
	for (size_t i = 0; i < obs_data_array_count(imgs); i++) {
		obs_data_t *m = obs_data_array_item(imgs, i);
		const char *file = obs_data_get_string(m, "value");
		if (file && *file && !obs_data_get_bool(m, "hidden")) {
			char *f = bstrdup(file);
			da_push_back(files, &f);
		}
		obs_data_release(m);
	}

	const size_t n = heads.num > files.num ? heads.num : files.num;
	const double each = heads.num > 1 ? obs_data_get_double(s, "message_sec") : obs_data_get_double(s, "show_sec");
	obs_data_array_t *items = obs_data_array_create();
	for (size_t i = 0; i < n; i++) {
		obs_data_t *it = new_item(s, heads.num ? heads.array[i % heads.num] : "",
					  heads.num ? subs.array[i % heads.num] : "",
					  files.num ? files.array[i % files.num] : "");
		obs_data_set_string(it, "badge", obs_data_get_string(s, "badge"));
		obs_data_set_double(it, "show_sec", each > 0.5 ? each : 8.0);
		obs_data_array_push_back(items, it);
		obs_data_release(it);
	}
	obs_data_set_array(s, "items", items);
	obs_data_array_release(items);

	for (size_t i = 0; i < heads.num; i++) {
		bfree(heads.array[i]);
		bfree(subs.array[i]);
	}
	for (size_t i = 0; i < files.num; i++) {
		bfree(files.array[i]);
	}
	da_free(heads);
	da_free(subs);
	da_free(files);
	obs_data_array_release(msgs);
	obs_data_array_release(imgs);
}

/* ------------------------------------------------------------------------ */
/* settings -> the page's config                                             */

static char *build_config(struct spazz_sponsor *sp, obs_data_t *s, bool preview)
{
	obs_data_t *c = obs_data_create();
	char hex[8];

	static const char *strs[][2] = {{"style", "style"},
					{"idle", "idle"},
					{"anchor", "anchor"},
					{"font", "font"},
					{"image_shape", "imageShape"},
					{"image_fit", "imageFit"},
					{"window_title", "windowTitle"},
					{"mode", "mode"}};
	for (size_t i = 0; i < sizeof(strs) / sizeof(strs[0]); i++) {
		obs_data_set_string(c, strs[i][1], obs_data_get_string(s, strs[i][0]));
	}

	static const char *colors[][2] = {{"accent", "accent"},
					  {"accent2", "accent2"},
					  {"text_color", "text"},
					  {"bg_color", "bg"}};
	for (size_t i = 0; i < sizeof(colors) / sizeof(colors[0]); i++) {
		hex_from_obs_color(obs_data_get_int(s, colors[i][0]), hex);
		obs_data_set_string(c, colors[i][1], hex);
	}

	obs_data_set_double(c, "bgOpacity", obs_data_get_double(s, "bg_opacity"));
	obs_data_set_double(c, "size", obs_data_get_double(s, "size"));
	obs_data_set_int(c, "marginX", obs_data_get_int(s, "margin_x"));
	obs_data_set_int(c, "marginY", obs_data_get_int(s, "margin_y"));
	obs_data_set_bool(c, "upper", obs_data_get_bool(s, "upper"));
	obs_data_set_double(c, "gapSec", obs_data_get_double(s, "gap_sec"));
	obs_data_set_double(c, "betweenSec", obs_data_get_double(s, "between_sec"));
	obs_data_set_double(c, "delaySec", obs_data_get_double(s, "delay_sec"));
	obs_data_set_int(c, "enterMs", obs_data_get_int(s, "enter_ms"));
	obs_data_set_int(c, "exitMs", obs_data_get_int(s, "exit_ms"));
	obs_data_set_int(c, "loops", obs_data_get_int(s, "loops"));
	obs_data_set_double(c, "everyMin", obs_data_get_double(s, "every_min"));
	obs_data_set_int(c, "cycles", obs_data_get_int(s, "cycles"));
	obs_data_set_bool(c, "run", compute_run(sp));
	if (preview) {
		obs_data_set_bool(c, "__preview", true);
		/* the studio holds the sponsor being edited on screen */
		obs_data_set_int(c, "__focus", obs_data_get_int(s, "studio_focus"));
	}

	/* the sponsors, in order. Images become addresses the page can load */
	static const char *item_strs[][2] = {
		{"layout", "layout"},      {"head", "head"},          {"sub", "sub"},     {"badge", "badge"},
		{"text_anim", "textAnim"}, {"logo_anim", "logoAnim"}, {"enter", "enter"}, {"exit", "exit"},
		{"mask", "mask"},          {"aspect", "aspect"},      {"fit", "fit"}};
	obs_data_array_t *out = obs_data_array_create();
	obs_data_array_t *items = obs_data_get_array(s, "items");
	for (size_t i = 0; i < obs_data_array_count(items); i++) {
		obs_data_t *it = obs_data_array_item(items, i);
		if (!obs_data_get_bool(it, "off")) {
			obs_data_t *o = obs_data_create();
			const char *file = obs_data_get_string(it, "image");
			if (file && *file) {
				struct dstr url = {0};
				absolute_url(&url, file);
				obs_data_set_string(o, "image", url.array);
				dstr_free(&url);
			}
			for (size_t k = 0; k < sizeof(item_strs) / sizeof(item_strs[0]); k++) {
				obs_data_set_string(o, item_strs[k][1], obs_data_get_string(it, item_strs[k][0]));
			}
			obs_data_set_double(o, "imageSize", obs_data_get_double(it, "image_size"));
			obs_data_set_double(o, "showSec", obs_data_get_double(it, "show_sec"));
			obs_data_set_string(o, "maskPath", mask_path(obs_data_get_string(it, "mask")));
			obs_data_set_double(o, "zoom", obs_data_get_double(it, "zoom"));
			obs_data_set_double(o, "panX", obs_data_get_double(it, "pan_x"));
			obs_data_set_double(o, "panY", obs_data_get_double(it, "pan_y"));
			/* sponsors saved before videos were possible play at full volume */
			obs_data_set_double(o, "volume",
					    obs_data_has_user_value(it, "volume") ? obs_data_get_double(it, "volume")
										  : 1.0);
			obs_data_array_push_back(out, o);
			obs_data_release(o);
		}
		obs_data_release(it);
	}
	obs_data_array_release(items);
	obs_data_set_array(c, "items", out);
	obs_data_array_release(out);

	char *json = bstrdup(obs_data_get_json(c));
	obs_data_release(c);
	return json;
}

static void send_event(struct spazz_sponsor *sp, const char *name, const char *json)
{
	if (!sp->browser) {
		return;
	}
	proc_handler_t *ph = obs_source_get_proc_handler(sp->browser);
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
/* source callbacks                                                          */

static const char *spn_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return T_("Sponsorcenter");
}

static void make_browser(struct spazz_sponsor *sp, uint32_t w, uint32_t h)
{
	char *page = obs_module_file("sponsor/index.html");
	if (!page) {
		blog(LOG_ERROR, "[sponsorcenter] sponsor/index.html is missing");
		return;
	}

	struct dstr url = {0}, cfg = {0};
	absolute_url(&url, page);
	absolute_url(&cfg, sp->config_path);
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
	/* video sound goes through this source onto the stream */
	obs_data_set_bool(bs, "reroute_audio", true);
	obs_data_set_string(bs, "css",
			    "body { background-color: rgba(0, 0, 0, 0); margin: 0px auto; overflow: hidden; }");

	sp->browser = obs_source_create_private("browser_source", "SPONSORCENTER page", bs);
	if (!sp->browser) {
		blog(LOG_WARNING, "[sponsorcenter] the browser source isn't available");
	} else {
		obs_source_add_active_child(sp->source, sp->browser);
		obs_source_set_audio_mixers(sp->browser, 0xFF);
	}

	obs_data_release(bs);
	dstr_free(&url);
	dstr_free(&cfg);
	bfree(page);
}

static void spn_update(void *data, obs_data_t *settings)
{
	struct spazz_sponsor *sp = data;

	long long w = obs_data_get_int(settings, "width");
	long long h = obs_data_get_int(settings, "height");
	w = w < 64 ? 64 : (w > 7680 ? 7680 : w);
	h = h < 64 ? 64 : (h > 4320 ? 4320 : h);
	const bool resized = os_atomic_load_long(&sp->width) != (long)w || os_atomic_load_long(&sp->height) != (long)h;
	os_atomic_set_long(&sp->width, (long)w);
	os_atomic_set_long(&sp->height, (long)h);
	sp->preview = obs_data_get_bool(settings, "studio_preview");
	os_atomic_set_long(&sp->run_when, run_when_index(obs_data_get_string(settings, "run_when")));
	os_atomic_set_long(&sp->running, compute_run(sp) ? 1 : 0);

	/* the page reads this when it (re)loads ... Updates run on the
	 * graphics thread, and the SPONSORCENTER preview updates as you type,
	 * so the preview's copy on disk is only refreshed now and then: the
	 * running page gets every change through the event below anyway. */
	char *json = build_config(sp, settings, sp->preview);
	const uint64_t now = os_gettime_ns();
	if (sp->config_path && (!sp->preview || !sp->browser || now - sp->last_write_ns > 1500000000ULL)) {
		os_quick_write_utf8_file_safe(sp->config_path, json, strlen(json), false, "tmp", NULL);
		sp->last_write_ns = now;
	}

	if (!sp->browser) {
		make_browser(sp, (uint32_t)w, (uint32_t)h);
	} else {
		if (resized) {
			obs_data_t *bs = obs_source_get_settings(sp->browser);
			obs_data_set_int(bs, "width", w);
			obs_data_set_int(bs, "height", h);
			obs_source_update(sp->browser, bs);
			obs_data_release(bs);
		}
		/* ... and gets it live while it's running */
		send_event(sp, "spazzSponsorConfig", json);
	}

	/* hear sponsor videos: always in the studio (it isn't on stream), and on
	 * a live spot if asked */
	if (sp->browser) {
		const bool mon = sp->preview || obs_data_get_bool(settings, "hear_video");
		if (obs_source_get_monitoring_enabled(sp->browser) != mon) {
			obs_source_set_monitoring_enabled(sp->browser, mon);
		}
	}
	bfree(json);
}

/* the Sponsors switch / streaming / recording changed (from SPAZZbs) */
static void apply_state(struct spazz_sponsor *sp, bool streaming, bool recording, bool on)
{
	os_atomic_set_long(&sp->st_streaming, streaming ? 1 : 0);
	os_atomic_set_long(&sp->st_recording, recording ? 1 : 0);
	os_atomic_set_long(&sp->st_switch, on ? 1 : 0);
	const long run = compute_run(sp) ? 1 : 0;
	if (os_atomic_set_long(&sp->running, run) != run && !sp->preview) {
		send_event(sp, "spazzSponsorRun", run ? "{\"run\":true}" : "{\"run\":false}");
	}
}

static void proc_set_state(void *data, calldata_t *cd)
{
	apply_state(data, calldata_bool(cd, "streaming"), calldata_bool(cd, "recording"), calldata_bool(cd, "enabled"));
}

/* ask SPAZZbs for the current state when a spot is made */
static void fetch_state(struct spazz_sponsor *sp)
{
	calldata_t cd;
	calldata_init(&cd);
	calldata_set_bool(&cd, "enabled", true);
	calldata_set_bool(&cd, "streaming", false);
	calldata_set_bool(&cd, "recording", false);
	proc_handler_call(obs_get_proc_handler(), "spazz_sponsor_state", &cd);
	os_atomic_set_long(&sp->st_switch, calldata_bool(&cd, "enabled") ? 1 : 0);
	os_atomic_set_long(&sp->st_streaming, calldata_bool(&cd, "streaming") ? 1 : 0);
	os_atomic_set_long(&sp->st_recording, calldata_bool(&cd, "recording") ? 1 : 0);
	calldata_free(&cd);
}

/* the browser's sound (sponsor videos) onto the stream */
static bool spn_audio_render(void *data, uint64_t *ts_out, struct obs_source_audio_mix *audio_output, uint32_t mixers,
			     size_t channels, size_t sample_rate)
{
	UNUSED_PARAMETER(sample_rate);
	struct spazz_sponsor *sp = data;
	obs_source_t *child = sp->browser;
	if (!child || obs_source_audio_pending(child)) {
		return false;
	}
	const uint64_t ts = obs_source_get_audio_timestamp(child);
	if (!ts) {
		return false;
	}

	struct obs_source_audio_mix child_audio;
	obs_source_get_audio_mix(child, &child_audio);
	for (size_t mix = 0; mix < MAX_AUDIO_MIXES; mix++) {
		if ((mixers & (1 << mix)) == 0) {
			continue;
		}
		for (size_t ch = 0; ch < channels; ch++) {
			float *out = audio_output->output[mix].data[ch];
			const float *in = child_audio.output[mix].data[ch];
			for (size_t i = 0; i < AUDIO_OUTPUT_FRAMES; i++) {
				out[i] += in[i];
			}
		}
	}
	*ts_out = ts;
	return true;
}

static void spn_defaults(obs_data_t *s)
{
	obs_data_set_default_string(s, "preset", "");
	obs_data_set_default_string(s, "style", "glass");
	obs_data_set_default_string(s, "enter", "slide");
	obs_data_set_default_string(s, "exit", "slide");
	obs_data_set_default_string(s, "text_anim", "rise");
	obs_data_set_default_string(s, "logo_anim", "none");
	obs_data_set_default_string(s, "idle", "float");
	obs_data_set_default_string(s, "anchor", "bl");
	obs_data_set_default_string(s, "font", "");
	obs_data_set_default_string(s, "image_shape", "style");
	obs_data_set_default_string(s, "image_fit", "contain");
	obs_data_set_default_string(s, "mode", "loop");
	obs_data_set_default_string(s, "window_title", "Sponsor.exe");
	obs_data_set_default_int(s, "accent", obs_color_from_hex(0x8a5cff));
	obs_data_set_default_int(s, "accent2", obs_color_from_hex(0xff5ad1));
	obs_data_set_default_int(s, "text_color", obs_color_from_hex(0xffffff));
	obs_data_set_default_int(s, "bg_color", obs_color_from_hex(0x140f22));
	obs_data_set_default_double(s, "bg_opacity", 0.82);
	obs_data_set_default_double(s, "size", 1.0);
	obs_data_set_default_int(s, "margin_x", 60);
	obs_data_set_default_int(s, "margin_y", 60);
	obs_data_set_default_bool(s, "upper", false);
	obs_data_set_default_double(s, "show_sec", 12.0);
	obs_data_set_default_double(s, "gap_sec", 45.0);
	obs_data_set_default_double(s, "every_min", 5.0);
	obs_data_set_default_int(s, "cycles", 1);
	obs_data_set_default_string(s, "run_when", "always");
	obs_data_set_default_bool(s, "hear_video", false);
	obs_data_set_default_double(s, "between_sec", 0.4);
	obs_data_set_default_double(s, "delay_sec", 0.0);
	obs_data_set_default_int(s, "enter_ms", 700);
	obs_data_set_default_int(s, "exit_ms", 600);
	obs_data_set_default_int(s, "loops", 0);
	obs_data_set_default_int(s, "width", 1920);
	obs_data_set_default_int(s, "height", 1080);
	obs_data_set_default_bool(s, "studio_preview", false);
	obs_data_set_default_int(s, "studio_focus", -1);
}

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
	int idx = preset_index(preset);
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

static bool style_modified(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);
	const char *style = obs_data_get_string(settings, "style");
	obs_property_t *t = obs_properties_get(props, "window_title");
	if (t) {
		obs_property_set_visible(t, style && strcmp(style, "y2k") == 0);
	}
	return true;
}

static bool mode_modified(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);
	const char *mode = obs_data_get_string(settings, "mode");
	const bool loop = mode && strcmp(mode, "loop") == 0;
	const bool always = mode && strcmp(mode, "always") == 0;
	obs_property_set_visible(obs_properties_get(props, "every_min"), loop);
	obs_property_set_visible(obs_properties_get(props, "cycles"), !always);
	obs_property_set_visible(obs_properties_get(props, "loops"), loop);
	obs_property_set_visible(obs_properties_get(props, "delay_sec"), loop || always);
	return true;
}

static bool play_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);
	if (data) {
		send_event(data, "spazzSponsorPlay", "{}");
	}
	return false;
}

static bool hide_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);
	if (data) {
		send_event(data, "spazzSponsorHide", "{}");
	}
	return false;
}

static bool edit_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);
	struct spazz_sponsor *sp = data;
	if (!sp) {
		return false;
	}
	calldata_t cd;
	calldata_init(&cd);
	calldata_set_string(&cd, "uuid", obs_source_get_uuid(sp->source));
	proc_handler_call(obs_get_proc_handler(), "spazz_sponsorcenter_open", &cd);
	calldata_free(&cd);
	return false;
}

static obs_property_t *add_list(obs_properties_t *props, const char *key, const char *label,
				const struct spn_option *opts)
{
	obs_property_t *p = obs_properties_add_list(props, key, label, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	for (const struct spn_option *o = opts; o->value; o++) {
		obs_property_list_add_string(p, o->label, o->value);
	}
	return p;
}

static obs_properties_t *spn_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	p = obs_properties_add_list(props, "preset", T_("Preset"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_("Preset.Custom"), "");
	for (int i = 0; i < SPN_PRESET_COUNT; i++) {
		obs_property_list_add_string(p, spn_presets[i].name, spn_presets[i].name);
	}
	obs_property_set_long_description(p, T_("Preset.Tip"));
	obs_property_set_modified_callback(p, preset_modified);

	obs_properties_add_button2(props, "play", T_("Play"), play_clicked, data);
	obs_properties_add_button2(props, "hide", T_("Hide"), hide_clicked, data);

	/* the sponsors themselves are edited as cards in Tools > SPONSORCENTER */
	int count = 0;
	const bool in_studio = data && ((struct spazz_sponsor *)data)->preview;
	if (data && !in_studio) {
		obs_data_t *cur = obs_source_get_settings(((struct spazz_sponsor *)data)->source);
		obs_data_array_t *items = obs_data_get_array(cur, "items");
		count = (int)obs_data_array_count(items);
		obs_data_array_release(items);
		obs_data_release(cur);
	}
	if (!in_studio) {
		struct dstr info = {0};
		dstr_printf(&info, T_("ItemsInfo"), count);
		obs_properties_add_text(props, "items_info", info.array, OBS_TEXT_INFO);
		dstr_free(&info);
		obs_properties_add_button2(props, "edit_items", T_("EditItems"), edit_clicked, data);
	}

	/* how it looks */
	obs_properties_t *look = obs_properties_create();
	p = add_list(look, "style", T_("Style"), spn_style);
	obs_property_set_modified_callback(p, style_modified);
	obs_properties_add_color(look, "accent", T_("Accent"));
	obs_properties_add_color(look, "accent2", T_("Accent2"));
	obs_properties_add_color(look, "text_color", T_("TextColor"));
	obs_properties_add_color(look, "bg_color", T_("BgColor"));
	obs_properties_add_float_slider(look, "bg_opacity", T_("BgOpacity"), 0.0, 1.0, 0.01);
	add_list(look, "font", T_("Font"), spn_font);
	obs_properties_add_bool(look, "upper", T_("Upper"));
	obs_properties_add_float_slider(look, "size", T_("Size"), 0.4, 3.0, 0.05);
	add_list(look, "anchor", T_("Anchor"), spn_anchor);
	obs_properties_add_int_slider(look, "margin_x", T_("MarginX"), 0, 600, 1);
	obs_properties_add_int_slider(look, "margin_y", T_("MarginY"), 0, 600, 1);
	add_list(look, "image_shape", T_("ImageShape"), spn_imageShape);
	add_list(look, "image_fit", T_("ImageFit"), spn_imageFit);
	obs_properties_add_text(look, "window_title", T_("WindowTitle"), OBS_TEXT_DEFAULT);
	add_list(look, "idle", T_("Idle"), spn_idle);
	p = obs_properties_add_int_slider(look, "enter_ms", T_("EnterMs"), 100, 3000, 50);
	obs_property_int_set_suffix(p, " ms");
	p = obs_properties_add_int_slider(look, "exit_ms", T_("ExitMs"), 100, 3000, 50);
	obs_property_int_set_suffix(p, " ms");
	p = obs_properties_add_bool(look, "hear_video", T_("HearVideo"));
	obs_property_set_long_description(p, T_("HearVideo.Tip"));
	obs_properties_add_group(props, "grp_look", T_("Look"), OBS_GROUP_NORMAL, look);

	/* when it shows */
	obs_properties_t *timing = obs_properties_create();
	p = add_list(timing, "run_when", T_("RunWhen"), spn_runWhen);
	obs_property_set_long_description(p, T_("RunWhen.Tip"));
	p = add_list(timing, "mode", T_("Mode"), spn_mode);
	obs_property_set_modified_callback(p, mode_modified);
	p = obs_properties_add_float(timing, "every_min", T_("EveryMin"), 0.5, 240.0, 0.5);
	obs_property_float_set_suffix(p, T_("Minutes"));
	obs_property_set_long_description(p, T_("EveryMin.Tip"));
	p = obs_properties_add_int(timing, "cycles", T_("Cycles"), 1, 50, 1);
	obs_property_set_long_description(p, T_("Cycles.Tip"));
	p = obs_properties_add_float_slider(timing, "between_sec", T_("BetweenSec"), 0.0, 10.0, 0.1);
	obs_property_float_set_suffix(p, " s");
	p = obs_properties_add_int(timing, "loops", T_("Loops"), 0, 1000, 1);
	obs_property_set_long_description(p, T_("Loops.Tip"));
	p = obs_properties_add_float_slider(timing, "delay_sec", T_("DelaySec"), 0.0, 600.0, 1.0);
	obs_property_float_set_suffix(p, " s");
	obs_properties_add_group(props, "grp_timing", T_("Timing"), OBS_GROUP_NORMAL, timing);

	obs_properties_t *canvas = obs_properties_create();
	obs_properties_add_int(canvas, "width", T_("Width"), 64, 7680, 1);
	obs_properties_add_int(canvas, "height", T_("Height"), 64, 4320, 1);
	p = obs_properties_add_text(canvas, "canvas_info", T_("CanvasInfo"), OBS_TEXT_INFO);
	obs_properties_add_group(props, "grp_canvas", T_("Canvas"), OBS_GROUP_NORMAL, canvas);

	return props;
}

static void hk_play(void *data, obs_hotkey_id id, obs_hotkey_t *hk, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hk);
	if (pressed) {
		send_event(data, "spazzSponsorPlay", "{}");
	}
}

static void hk_hide(void *data, obs_hotkey_id id, obs_hotkey_t *hk, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hk);
	if (pressed) {
		send_event(data, "spazzSponsorHide", "{}");
	}
}

/* "play" / "hide" for SPAZZbs itself (SPONSORCENTER's buttons) */
static void proc_play(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	send_event(data, "spazzSponsorPlay", "{}");
}

static void proc_hide(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	send_event(data, "spazzSponsorHide", "{}");
}

/* the page lives at a fixed address; the settings file is per source */
static char *config_path_for(obs_source_t *source)
{
	struct dstr name = {0};
	dstr_printf(&name, "sponsors/%s.json", obs_source_get_uuid(source));
	char *path = obs_module_config_path(name.array);
	dstr_free(&name);
	if (path) {
		char *dir = obs_module_config_path("sponsors");
		if (dir) {
			os_mkdirs(dir);
			bfree(dir);
		}
	}
	return path;
}

static void *spn_create(obs_data_t *settings, obs_source_t *source)
{
	struct spazz_sponsor *sp = bzalloc(sizeof(*sp));
	sp->source = source;
	sp->config_path = config_path_for(source);

	obs_data_array_t *items = obs_data_get_array(settings, "items");
	if (!items) {
		if (!obs_data_has_user_value(settings, "preset_applied")) {
			/* a brand new spot starts with the Glass Card look and a sample sponsor */
			apply_preset(settings, 0);
			obs_data_set_string(settings, "preset", spn_presets[0].name);
			obs_data_set_string(settings, "preset_applied", spn_presets[0].name);
			struct dstr head = {0}, sub = {0};
			split_line(T_("SampleMessage"), &head, &sub);
			obs_data_array_t *one = obs_data_array_create();
			obs_data_t *it = new_item(settings, head.array, sub.array, "");
			obs_data_array_push_back(one, it);
			obs_data_release(it);
			obs_data_set_array(settings, "items", one);
			obs_data_array_release(one);
			dstr_free(&head);
			dstr_free(&sub);
		} else {
			migrate_items(settings);
		}
	}
	obs_data_array_release(items);

	/* spots from before "every X minutes": keep roughly the same rhythm */
	if (!obs_data_has_user_value(settings, "every_min") && obs_data_has_user_value(settings, "preset_applied")) {
		double run = 0.0;
		obs_data_array_t *all = obs_data_get_array(settings, "items");
		const double move =
			(double)(obs_data_get_int(settings, "enter_ms") + obs_data_get_int(settings, "exit_ms")) /
			1000.0;
		for (size_t i = 0; i < obs_data_array_count(all); i++) {
			obs_data_t *it = obs_data_array_item(all, i);
			run += obs_data_get_double(it, "show_sec") + move +
			       obs_data_get_double(settings, "between_sec");
			obs_data_release(it);
		}
		obs_data_array_release(all);
		double every = (run + obs_data_get_double(settings, "gap_sec")) / 60.0;
		every = floor(every * 2.0 + 0.5) / 2.0;
		obs_data_set_double(settings, "every_min", every < 0.5 ? 0.5 : every);
	}

	obs_hotkey_register_source(source, "Sponsorcenter.Play", T_("Hotkey.Play"), hk_play, sp);
	obs_hotkey_register_source(source, "Sponsorcenter.Hide", T_("Hotkey.Hide"), hk_hide, sp);

	proc_handler_t *ph = obs_source_get_proc_handler(source);
	proc_handler_add(ph, "void play()", proc_play, sp);
	proc_handler_add(ph, "void hide()", proc_hide, sp);
	proc_handler_add(ph, "void set_state(in bool streaming, in bool recording, in bool enabled)", proc_set_state,
			 sp);
	os_atomic_set_long(&sp->st_switch, 1);
	fetch_state(sp);

	spn_update(sp, settings);
	return sp;
}

static void spn_destroy(void *data)
{
	struct spazz_sponsor *sp = data;

	if (sp->browser) {
		obs_source_remove_active_child(sp->source, sp->browser);
		obs_source_release(sp->browser);
	}
	/* a deleted sponsor doesn't need its settings file any more */
	if (sp->config_path && obs_source_removed(sp->source)) {
		os_unlink(sp->config_path);
	}
	bfree(sp->config_path);
	bfree(sp);
}

static void spn_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct spazz_sponsor *sp = data;
	if (sp->browser) {
		obs_source_video_render(sp->browser);
	}
}

static uint32_t spn_width(void *data)
{
	struct spazz_sponsor *sp = data;
	return (uint32_t)os_atomic_load_long(&sp->width);
}

static uint32_t spn_height(void *data)
{
	struct spazz_sponsor *sp = data;
	return (uint32_t)os_atomic_load_long(&sp->height);
}

static void spn_enum_sources(void *data, obs_source_enum_proc_t cb, void *param)
{
	struct spazz_sponsor *sp = data;
	if (sp->browser) {
		cb(sp->source, sp->browser, param);
	}
}

static enum gs_color_space spn_color_space(void *data, size_t count, const enum gs_color_space *preferred)
{
	struct spazz_sponsor *sp = data;
	if (sp->browser) {
		return obs_source_get_color_space(sp->browser, count, preferred);
	}
	return count ? preferred[0] : GS_CS_SRGB;
}

static struct obs_source_info spazz_sponsor_info = {
	.id = "spazz_sponsor",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_COMPOSITE,
	.get_name = spn_get_name,
	.create = spn_create,
	.destroy = spn_destroy,
	.update = spn_update,
	.get_defaults = spn_defaults,
	.get_properties = spn_properties,
	.video_render = spn_render,
	.get_width = spn_width,
	.get_height = spn_height,
	.enum_active_sources = spn_enum_sources,
	.enum_all_sources = spn_enum_sources,
	.audio_render = spn_audio_render,
	.video_get_color_space = spn_color_space,
	.icon_type = OBS_ICON_TYPE_SLIDESHOW,
};

bool obs_module_load(void)
{
	obs_register_source(&spazz_sponsor_info);
	return true;
}
