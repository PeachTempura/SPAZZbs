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
		if (ch == '\\')
			ch = '/';
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
		if (strcmp(spn_presets[i].name, name) == 0)
			return i;
	}
	return -1;
}

static void apply_preset(obs_data_t *s, int idx)
{
	if (idx < 0 || idx >= SPN_PRESET_COUNT)
		return;
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
}

/* ------------------------------------------------------------------------ */
/* settings -> the page's config                                             */

static char *build_config(obs_data_t *s, bool preview)
{
	obs_data_t *c = obs_data_create();
	char hex[8];

	static const char *strs[][2] = {{"style", "style"},          {"enter", "enter"},         {"exit", "exit"},
					{"text_anim", "textAnim"},   {"image_anim", "imageAnim"}, {"idle", "idle"},
					{"anchor", "anchor"},        {"font", "font"},           {"image_shape", "imageShape"},
					{"image_fit", "imageFit"},   {"badge", "badge"},         {"window_title", "windowTitle"},
					{"mode", "mode"}};
	for (size_t i = 0; i < sizeof(strs) / sizeof(strs[0]); i++)
		obs_data_set_string(c, strs[i][1], obs_data_get_string(s, strs[i][0]));

	static const char *colors[][2] = {
		{"accent", "accent"}, {"accent2", "accent2"}, {"text_color", "text"}, {"bg_color", "bg"}};
	for (size_t i = 0; i < sizeof(colors) / sizeof(colors[0]); i++) {
		hex_from_obs_color(obs_data_get_int(s, colors[i][0]), hex);
		obs_data_set_string(c, colors[i][1], hex);
	}

	obs_data_set_double(c, "bgOpacity", obs_data_get_double(s, "bg_opacity"));
	obs_data_set_double(c, "size", obs_data_get_double(s, "size"));
	obs_data_set_int(c, "marginX", obs_data_get_int(s, "margin_x"));
	obs_data_set_int(c, "marginY", obs_data_get_int(s, "margin_y"));
	obs_data_set_bool(c, "upper", obs_data_get_bool(s, "upper"));
	obs_data_set_double(c, "showSec", obs_data_get_double(s, "show_sec"));
	obs_data_set_double(c, "gapSec", obs_data_get_double(s, "gap_sec"));
	obs_data_set_double(c, "imageSec", obs_data_get_double(s, "image_sec"));
	obs_data_set_double(c, "messageSec", obs_data_get_double(s, "message_sec"));
	obs_data_set_double(c, "delaySec", obs_data_get_double(s, "delay_sec"));
	obs_data_set_int(c, "enterMs", obs_data_get_int(s, "enter_ms"));
	obs_data_set_int(c, "exitMs", obs_data_get_int(s, "exit_ms"));
	obs_data_set_int(c, "loops", obs_data_get_int(s, "loops"));
	if (preview)
		obs_data_set_bool(c, "__preview", true);

	/* messages: "Headline | small line" per entry */
	obs_data_array_t *msgs_out = obs_data_array_create();
	obs_data_array_t *msgs = obs_data_get_array(s, "messages");
	for (size_t i = 0; i < obs_data_array_count(msgs); i++) {
		obs_data_t *item = obs_data_array_item(msgs, i);
		const char *line = obs_data_get_string(item, "value");
		if (line && *line && !obs_data_get_bool(item, "hidden")) {
			obs_data_t *m = obs_data_create();
			const char *bar = strchr(line, '|');
			struct dstr head = {0}, sub = {0};
			if (bar) {
				dstr_ncopy(&head, line, (size_t)(bar - line));
				dstr_copy(&sub, bar + 1);
			} else {
				dstr_copy(&head, line);
			}
			dstr_depad(&head);
			dstr_depad(&sub);
			obs_data_set_string(m, "head", head.array ? head.array : "");
			obs_data_set_string(m, "sub", sub.array ? sub.array : "");
			obs_data_array_push_back(msgs_out, m);
			obs_data_release(m);
			dstr_free(&head);
			dstr_free(&sub);
		}
		obs_data_release(item);
	}
	obs_data_array_release(msgs);
	obs_data_set_array(c, "messages", msgs_out);
	obs_data_array_release(msgs_out);

	/* the image pool, as addresses the page can load */
	obs_data_array_t *imgs_out = obs_data_array_create();
	obs_data_array_t *imgs = obs_data_get_array(s, "images");
	for (size_t i = 0; i < obs_data_array_count(imgs); i++) {
		obs_data_t *item = obs_data_array_item(imgs, i);
		const char *file = obs_data_get_string(item, "value");
		if (file && *file && !obs_data_get_bool(item, "hidden")) {
			struct dstr url = {0};
			absolute_url(&url, file);
			obs_data_t *u = obs_data_create();
			obs_data_set_string(u, "value", url.array);
			obs_data_array_push_back(imgs_out, u);
			obs_data_release(u);
			dstr_free(&url);
		}
		obs_data_release(item);
	}
	obs_data_array_release(imgs);

	/* the page wants a plain list of strings; obs_data arrays only hold
	 * objects, so splice it in as JSON text */
	struct dstr list = {0};
	dstr_copy(&list, "[");
	for (size_t i = 0; i < obs_data_array_count(imgs_out); i++) {
		obs_data_t *u = obs_data_array_item(imgs_out, i);
		const char *v = obs_data_get_string(u, "value");
		if (i)
			dstr_cat(&list, ",");
		dstr_cat(&list, "\"");
		for (const char *p = v; *p; p++) {
			if (*p == '"' || *p == '\\')
				dstr_cat(&list, "\\");
			dstr_ncat(&list, p, 1);
		}
		dstr_cat(&list, "\"");
		obs_data_release(u);
	}
	dstr_cat(&list, "]");
	obs_data_array_release(imgs_out);

	obs_data_set_string(c, "images", "__IMAGES__");
	struct dstr json = {0};
	dstr_copy(&json, obs_data_get_json(c));
	dstr_replace(&json, "\"__IMAGES__\"", list.array);
	obs_data_release(c);
	dstr_free(&list);
	return json.array;
}

static void send_event(struct spazz_sponsor *sp, const char *name, const char *json)
{
	if (!sp->browser)
		return;
	proc_handler_t *ph = obs_source_get_proc_handler(sp->browser);
	if (!ph)
		return;
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
	obs_data_set_bool(bs, "reroute_audio", false);
	obs_data_set_string(bs, "css", "body { background-color: rgba(0, 0, 0, 0); margin: 0px auto; overflow: hidden; }");

	sp->browser = obs_source_create_private("browser_source", "SPONSORCENTER page", bs);
	if (!sp->browser)
		blog(LOG_WARNING, "[sponsorcenter] the browser source isn't available");
	else
		obs_source_add_active_child(sp->source, sp->browser);

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

	/* the page reads this when it (re)loads ... */
	char *json = build_config(settings, sp->preview);
	if (sp->config_path)
		os_quick_write_utf8_file_safe(sp->config_path, json, strlen(json), false, "tmp", NULL);

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
	bfree(json);
}

static void spn_defaults(obs_data_t *s)
{
	obs_data_set_default_string(s, "preset", "");
	obs_data_set_default_string(s, "style", "glass");
	obs_data_set_default_string(s, "enter", "slide");
	obs_data_set_default_string(s, "exit", "slide");
	obs_data_set_default_string(s, "text_anim", "rise");
	obs_data_set_default_string(s, "image_anim", "crossfade");
	obs_data_set_default_string(s, "idle", "float");
	obs_data_set_default_string(s, "anchor", "bl");
	obs_data_set_default_string(s, "font", "");
	obs_data_set_default_string(s, "image_shape", "style");
	obs_data_set_default_string(s, "image_fit", "contain");
	obs_data_set_default_string(s, "mode", "loop");
	obs_data_set_default_string(s, "badge", "");
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
	obs_data_set_default_double(s, "image_sec", 4.0);
	obs_data_set_default_double(s, "message_sec", 6.0);
	obs_data_set_default_double(s, "delay_sec", 0.0);
	obs_data_set_default_int(s, "enter_ms", 700);
	obs_data_set_default_int(s, "exit_ms", 600);
	obs_data_set_default_int(s, "loops", 0);
	obs_data_set_default_int(s, "width", 1920);
	obs_data_set_default_int(s, "height", 1080);
	obs_data_set_default_bool(s, "studio_preview", false);
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
	if (applied && strcmp(preset, applied) == 0)
		return false;
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

static bool style_modified(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);
	const char *style = obs_data_get_string(settings, "style");
	obs_property_t *t = obs_properties_get(props, "window_title");
	if (t)
		obs_property_set_visible(t, style && strcmp(style, "y2k") == 0);
	return true;
}

static bool mode_modified(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);
	const char *mode = obs_data_get_string(settings, "mode");
	const bool loop = mode && strcmp(mode, "loop") == 0;
	const bool always = mode && strcmp(mode, "always") == 0;
	obs_property_set_visible(obs_properties_get(props, "gap_sec"), loop);
	obs_property_set_visible(obs_properties_get(props, "loops"), loop);
	obs_property_set_visible(obs_properties_get(props, "delay_sec"), loop || always);
	obs_property_set_visible(obs_properties_get(props, "show_sec"), !always);
	return true;
}

static bool play_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);
	if (data)
		send_event(data, "spazzSponsorPlay", "{}");
	return false;
}

static bool hide_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);
	if (data)
		send_event(data, "spazzSponsorHide", "{}");
	return false;
}

static obs_property_t *add_list(obs_properties_t *props, const char *key, const char *label,
				const struct spn_option *opts)
{
	obs_property_t *p = obs_properties_add_list(props, key, label, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	for (const struct spn_option *o = opts; o->value; o++)
		obs_property_list_add_string(p, o->label, o->value);
	return p;
}

static obs_properties_t *spn_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	p = obs_properties_add_list(props, "preset", T_("Preset"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_("Preset.Custom"), "");
	for (int i = 0; i < SPN_PRESET_COUNT; i++)
		obs_property_list_add_string(p, spn_presets[i].name, spn_presets[i].name);
	obs_property_set_long_description(p, T_("Preset.Tip"));
	obs_property_set_modified_callback(p, preset_modified);

	obs_properties_add_button2(props, "play", T_("Play"), play_clicked, data);
	obs_properties_add_button2(props, "hide", T_("Hide"), hide_clicked, data);

	/* what it says and shows */
	obs_properties_t *content = obs_properties_create();
	p = obs_properties_add_editable_list(content, "messages", T_("Messages"), OBS_EDITABLE_LIST_TYPE_STRINGS, NULL,
					     NULL);
	obs_property_set_long_description(p, T_("Messages.Tip"));
	p = obs_properties_add_text(content, "badge", T_("Badge"), OBS_TEXT_DEFAULT);
	obs_property_set_long_description(p, T_("Badge.Tip"));
	p = obs_properties_add_editable_list(content, "images", T_("Images"), OBS_EDITABLE_LIST_TYPE_FILES,
					     "Images (*.png *.jpg *.jpeg *.gif *.webp *.svg);;All files (*.*)", NULL);
	obs_property_set_long_description(p, T_("Images.Tip"));
	add_list(content, "image_fit", T_("ImageFit"), spn_imageFit);
	add_list(content, "image_shape", T_("ImageShape"), spn_imageShape);
	obs_properties_add_text(content, "window_title", T_("WindowTitle"), OBS_TEXT_DEFAULT);
	obs_properties_add_group(props, "grp_content", T_("Content"), OBS_GROUP_NORMAL, content);

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
	obs_properties_add_group(props, "grp_look", T_("Look"), OBS_GROUP_NORMAL, look);

	/* how it moves */
	obs_properties_t *motion = obs_properties_create();
	add_list(motion, "enter", T_("Enter"), spn_enter);
	add_list(motion, "exit", T_("Exit"), spn_enter);
	p = obs_properties_add_int_slider(motion, "enter_ms", T_("EnterMs"), 100, 3000, 50);
	obs_property_int_set_suffix(p, " ms");
	p = obs_properties_add_int_slider(motion, "exit_ms", T_("ExitMs"), 100, 3000, 50);
	obs_property_int_set_suffix(p, " ms");
	add_list(motion, "text_anim", T_("TextAnim"), spn_textAnim);
	add_list(motion, "image_anim", T_("ImageAnim"), spn_imageAnim);
	add_list(motion, "idle", T_("Idle"), spn_idle);
	obs_properties_add_group(props, "grp_motion", T_("Motion"), OBS_GROUP_NORMAL, motion);

	/* when it shows */
	obs_properties_t *timing = obs_properties_create();
	p = add_list(timing, "mode", T_("Mode"), spn_mode);
	obs_property_set_modified_callback(p, mode_modified);
	p = obs_properties_add_float_slider(timing, "show_sec", T_("ShowSec"), 1.0, 120.0, 0.5);
	obs_property_float_set_suffix(p, " s");
	p = obs_properties_add_float_slider(timing, "gap_sec", T_("GapSec"), 0.0, 900.0, 1.0);
	obs_property_float_set_suffix(p, " s");
	p = obs_properties_add_int(timing, "loops", T_("Loops"), 0, 1000, 1);
	obs_property_set_long_description(p, T_("Loops.Tip"));
	p = obs_properties_add_float_slider(timing, "delay_sec", T_("DelaySec"), 0.0, 600.0, 1.0);
	obs_property_float_set_suffix(p, " s");
	p = obs_properties_add_float_slider(timing, "image_sec", T_("ImageSec"), 0.5, 60.0, 0.5);
	obs_property_float_set_suffix(p, " s");
	p = obs_properties_add_float_slider(timing, "message_sec", T_("MessageSec"), 1.0, 60.0, 0.5);
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
	if (pressed)
		send_event(data, "spazzSponsorPlay", "{}");
}

static void hk_hide(void *data, obs_hotkey_id id, obs_hotkey_t *hk, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hk);
	if (pressed)
		send_event(data, "spazzSponsorHide", "{}");
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

	/* a brand new sponsor starts with the Glass Card look and a sample line */
	if (!obs_data_has_user_value(settings, "preset_applied")) {
		apply_preset(settings, 0);
		obs_data_set_string(settings, "preset", spn_presets[0].name);
		obs_data_set_string(settings, "preset_applied", spn_presets[0].name);
		obs_data_array_t *msgs = obs_data_array_create();
		obs_data_t *m = obs_data_create();
		obs_data_set_string(m, "value", T_("SampleMessage"));
		obs_data_array_push_back(msgs, m);
		obs_data_release(m);
		obs_data_set_array(settings, "messages", msgs);
		obs_data_array_release(msgs);
	}

	obs_hotkey_register_source(source, "Sponsorcenter.Play", T_("Hotkey.Play"), hk_play, sp);
	obs_hotkey_register_source(source, "Sponsorcenter.Hide", T_("Hotkey.Hide"), hk_hide, sp);

	proc_handler_t *ph = obs_source_get_proc_handler(source);
	proc_handler_add(ph, "void play()", proc_play, sp);
	proc_handler_add(ph, "void hide()", proc_hide, sp);

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
	if (sp->config_path && obs_source_removed(sp->source))
		os_unlink(sp->config_path);
	bfree(sp->config_path);
	bfree(sp);
}

static void spn_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct spazz_sponsor *sp = data;
	if (sp->browser)
		obs_source_video_render(sp->browser);
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
	if (sp->browser)
		cb(sp->source, sp->browser, param);
}

static enum gs_color_space spn_color_space(void *data, size_t count, const enum gs_color_space *preferred)
{
	struct spazz_sponsor *sp = data;
	if (sp->browser)
		return obs_source_get_color_space(sp->browser, count, preferred);
	return count ? preferred[0] : GS_CS_SRGB;
}

static struct obs_source_info spazz_sponsor_info = {
	.id = "spazz_sponsor",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
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
	.video_get_color_space = spn_color_space,
	.icon_type = OBS_ICON_TYPE_SLIDESHOW,
};

bool obs_module_load(void)
{
	obs_register_source(&spazz_sponsor_info);
	return true;
}
