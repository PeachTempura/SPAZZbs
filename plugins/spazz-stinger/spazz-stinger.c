/******************************************************************************
    SPAZZbs - "SPAZZ Stinger": stinger transitions drawn live from presets
    and sliders instead of video files.
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
 * A transition with a fixed length (like OBS's video stinger) whose
 * animation is a shader: Matrix code, glitch, wipes and so on. Scene A is
 * shown until the cut point, scene B after it, and each style covers the
 * swap its own way. Styles, sliders and presets come from
 * tools/stinger-spec.js through spazz-stinger-spec.h.
 */

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/dstr.h>
#include <util/threading.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "spazz-stinger-spec.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("spazz-stinger", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "SPAZZbs live stinger transitions";
}

#define T_(s) obs_module_text(s)

#define SLOT_COUNT 12

struct stg_vals {
	int style;
	uint32_t duration_ms;
	float cut;
	bool audio_fade;
	float seed;
	double v[STG_PARAM_COUNT];
};

struct spazz_stinger {
	obs_source_t *source;
	gs_effect_t *effect;

	gs_eparam_t *p_tex_a, *p_tex_b, *p_has_a, *p_has_b, *p_t, *p_cut, *p_res, *p_time, *p_seed, *p_lin;
	gs_eparam_t *p_slot[SLOT_COUNT];

	pthread_mutex_t mutex;
	struct stg_vals vals;
	struct stg_vals render;
};

/* ------------------------------------------------------------------------ */
/* helpers                                                                   */

static long long obs_color_from_hex(uint32_t hex)
{
	uint32_t r = (hex >> 16) & 0xFF, g = (hex >> 8) & 0xFF, b = hex & 0xFF;
	return (long long)(0xFF000000u | (b << 16) | (g << 8) | r);
}

static uint32_t hex_from_obs_color(long long c)
{
	uint32_t r = (uint32_t)c & 0xFF, g = ((uint32_t)c >> 8) & 0xFF, b = ((uint32_t)c >> 16) & 0xFF;
	return (r << 16) | (g << 8) | b;
}

static int style_index(const char *id)
{
	for (int i = 0; id && i < STG_STYLE_COUNT; i++) {
		if (strcmp(stg_styles[i].id, id) == 0)
			return i;
	}
	return 0;
}

static int param_index(const char *key)
{
	for (int i = 0; key && i < STG_PARAM_COUNT; i++) {
		if (strcmp(stg_params[i].key, key) == 0)
			return i;
	}
	return -1;
}

static int preset_index(const char *name)
{
	for (int i = 0; name && i < STG_PRESET_COUNT; i++) {
		if (strcmp(stg_presets[i].name, name) == 0)
			return i;
	}
	return -1;
}

static void set_param(obs_data_t *s, int i, double value)
{
	const struct stg_param *p = &stg_params[i];
	switch (p->type) {
	case STG_SLIDER:
		obs_data_set_double(s, p->key, fmin(p->max, fmax(p->min, value)));
		break;
	case STG_COLOR:
		obs_data_set_int(s, p->key, obs_color_from_hex((uint32_t)value));
		break;
	case STG_TOGGLE:
		obs_data_set_bool(s, p->key, value != 0.0);
		break;
	case STG_SELECT:
		obs_data_set_int(s, p->key, (long long)value);
		break;
	}
}

/* defaults, then the preset's style, timing and values */
static void apply_preset(obs_data_t *s, int idx)
{
	if (idx < 0 || idx >= STG_PRESET_COUNT)
		return;
	const struct stg_preset *pr = &stg_presets[idx];

	for (int i = 0; i < STG_PARAM_COUNT; i++)
		set_param(s, i, stg_params[i].def);

	obs_data_set_string(s, "style", pr->style);
	obs_data_set_int(s, "duration", pr->duration_ms);
	obs_data_set_int(s, "cut", pr->cut_percent);
	for (int k = 0; k < pr->count; k++) {
		int i = param_index(pr->vals[k].key);
		if (i >= 0)
			set_param(s, i, pr->vals[k].value);
	}
}

/* ------------------------------------------------------------------------ */
/* source callbacks                                                          */

static const char *stg_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return T_("SpazzStinger");
}

static void stg_update(void *data, obs_data_t *settings)
{
	struct spazz_stinger *s = data;
	struct stg_vals v;

	v.style = style_index(obs_data_get_string(settings, "style"));
	long long dur = obs_data_get_int(settings, "duration");
	v.duration_ms = (uint32_t)(dur < 100 ? 100 : (dur > 20000 ? 20000 : dur));
	long long cut = obs_data_get_int(settings, "cut");
	v.cut = (float)(cut < 5 ? 5 : (cut > 95 ? 95 : cut)) / 100.0f;
	v.audio_fade = strcmp(obs_data_get_string(settings, "audio"), "fade") == 0;
	v.seed = (float)obs_data_get_int(settings, "variation");

	for (int i = 0; i < STG_PARAM_COUNT; i++) {
		const struct stg_param *p = &stg_params[i];
		switch (p->type) {
		case STG_SLIDER:
			v.v[i] = obs_data_get_double(settings, p->key);
			break;
		case STG_COLOR:
			v.v[i] = (double)hex_from_obs_color(obs_data_get_int(settings, p->key));
			break;
		case STG_TOGGLE:
			v.v[i] = obs_data_get_bool(settings, p->key) ? 1.0 : 0.0;
			break;
		case STG_SELECT:
			v.v[i] = (double)obs_data_get_int(settings, p->key);
			break;
		}
	}

	pthread_mutex_lock(&s->mutex);
	s->vals = v;
	pthread_mutex_unlock(&s->mutex);

	/* a stinger has its own length; the dock's duration box is ignored */
	obs_transition_enable_fixed(s->source, true, v.duration_ms);
}

static void stg_defaults(obs_data_t *settings)
{
	for (int i = 0; i < STG_PARAM_COUNT; i++) {
		const struct stg_param *p = &stg_params[i];
		switch (p->type) {
		case STG_SLIDER:
			obs_data_set_default_double(settings, p->key, p->def);
			break;
		case STG_COLOR:
			obs_data_set_default_int(settings, p->key, obs_color_from_hex((uint32_t)p->def));
			break;
		case STG_TOGGLE:
			obs_data_set_default_bool(settings, p->key, p->def != 0.0);
			break;
		case STG_SELECT:
			obs_data_set_default_int(settings, p->key, (long long)p->def);
			break;
		}
	}
	obs_data_set_default_string(settings, "style", stg_styles[0].id);
	obs_data_set_default_int(settings, "duration", 1800);
	obs_data_set_default_int(settings, "cut", 50);
	obs_data_set_default_string(settings, "audio", "cut");
	obs_data_set_default_int(settings, "variation", 0);
	obs_data_set_default_string(settings, "preset", "");
}

static void show_style_groups(obs_properties_t *props, int style)
{
	for (int i = 0; i < STG_STYLE_COUNT; i++) {
		char name[64];
		snprintf(name, sizeof(name), "grp_%s", stg_styles[i].id);
		obs_property_t *g = obs_properties_get(props, name);
		if (g)
			obs_property_set_visible(g, i == style);
	}
}

static bool style_modified(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);
	show_style_groups(props, style_index(obs_data_get_string(settings, "style")));
	return true;
}

static bool preset_modified(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);

	/* also called when the dialog opens: only apply a real change */
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
	show_style_groups(props, style_index(obs_data_get_string(settings, "style")));
	return true;
}

static void add_param(obs_properties_t *props, const struct stg_param *p)
{
	obs_property_t *prop = NULL;
	switch (p->type) {
	case STG_SLIDER:
		prop = obs_properties_add_float_slider(props, p->key, p->label, p->min, p->max, p->step);
		break;
	case STG_COLOR:
		prop = obs_properties_add_color(props, p->key, p->label);
		break;
	case STG_TOGGLE:
		prop = obs_properties_add_bool(props, p->key, p->label);
		break;
	case STG_SELECT:
		prop = obs_properties_add_list(props, p->key, p->label, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
		for (int o = 0; o < p->option_count; o++)
			obs_property_list_add_int(prop, p->options[o].label, p->options[o].value);
		break;
	}
	if (prop && p->tip)
		obs_property_set_long_description(prop, p->tip);
}

static obs_properties_t *stg_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	p = obs_properties_add_list(props, "preset", T_("Preset"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_("Preset.Custom"), "");
	for (int i = 0; i < STG_PRESET_COUNT; i++)
		obs_property_list_add_string(p, stg_presets[i].name, stg_presets[i].name);
	obs_property_set_modified_callback(p, preset_modified);

	p = obs_properties_add_list(props, "style", T_("Style"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	for (int i = 0; i < STG_STYLE_COUNT; i++)
		obs_property_list_add_string(p, stg_styles[i].name, stg_styles[i].id);
	obs_property_set_modified_callback(p, style_modified);

	p = obs_properties_add_int_slider(props, "duration", T_("Duration"), 200, 6000, 50);
	obs_property_int_set_suffix(p, " ms");
	p = obs_properties_add_int_slider(props, "cut", T_("Cut"), 10, 90, 1);
	obs_property_int_set_suffix(p, " %");
	obs_property_set_long_description(p, T_("Cut.Tip"));
	p = obs_properties_add_list(props, "audio", T_("Audio"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_("Audio.Cut"), "cut");
	obs_property_list_add_string(p, T_("Audio.Fade"), "fade");
	p = obs_properties_add_int_slider(props, "variation", T_("Variation"), 0, 99, 1);
	obs_property_set_long_description(p, T_("Variation.Tip"));

	for (int si = 0; si < STG_STYLE_COUNT; si++) {
		obs_properties_t *sub = obs_properties_create();
		for (int i = 0; i < STG_PARAM_COUNT; i++) {
			if (stg_params[i].style == si)
				add_param(sub, &stg_params[i]);
		}
		char name[64];
		snprintf(name, sizeof(name), "grp_%s", stg_styles[si].id);
		struct dstr title = {0};
		dstr_printf(&title, "%s %s", stg_styles[si].name, T_("StyleSettings"));
		obs_properties_add_group(props, name, title.array, OBS_GROUP_NORMAL, sub);
		dstr_free(&title);
	}

	return props;
}

static void stg_destroy(void *data)
{
	struct spazz_stinger *s = data;
	obs_enter_graphics();
	gs_effect_destroy(s->effect);
	obs_leave_graphics();
	pthread_mutex_destroy(&s->mutex);
	bfree(s);
}

static void *stg_create(obs_data_t *settings, obs_source_t *source)
{
	struct spazz_stinger *s = bzalloc(sizeof(*s));
	s->source = source;
	if (pthread_mutex_init(&s->mutex, NULL) != 0) {
		bfree(s);
		return NULL;
	}

	char *path = obs_module_file("spazz-stinger.effect");
	obs_enter_graphics();
	if (path) {
		char *errors = NULL;
		s->effect = gs_effect_create_from_file(path, &errors);
		if (!s->effect)
			blog(LOG_ERROR, "[spazz-stinger] could not load effect: %s", errors ? errors : "(no details)");
		bfree(errors);
	}
	obs_leave_graphics();
	bfree(path);

	if (s->effect) {
		gs_effect_t *e = s->effect;
		s->p_tex_a = gs_effect_get_param_by_name(e, "tex_a");
		s->p_tex_b = gs_effect_get_param_by_name(e, "tex_b");
		s->p_has_a = gs_effect_get_param_by_name(e, "has_a");
		s->p_has_b = gs_effect_get_param_by_name(e, "has_b");
		s->p_t = gs_effect_get_param_by_name(e, "t");
		s->p_cut = gs_effect_get_param_by_name(e, "cut");
		s->p_res = gs_effect_get_param_by_name(e, "res");
		s->p_time = gs_effect_get_param_by_name(e, "time_s");
		s->p_seed = gs_effect_get_param_by_name(e, "seed");
		s->p_lin = gs_effect_get_param_by_name(e, "lin_out");
		static const char *slots[SLOT_COUNT] = {"u_c1", "u_c2", "u_c3", "u_c4", "u_p1", "u_p2",
							"u_p3", "u_p4", "u_p5", "u_p6", "u_p7", "u_p8"};
		for (int i = 0; i < SLOT_COUNT; i++)
			s->p_slot[i] = gs_effect_get_param_by_name(e, slots[i]);
	}

	/* a brand new stinger starts as the classic Matrix look */
	if (!obs_data_has_user_value(settings, "preset_applied")) {
		apply_preset(settings, 0);
		obs_data_set_string(settings, "preset", stg_presets[0].name);
		obs_data_set_string(settings, "preset_applied", stg_presets[0].name);
	}

	stg_update(s, settings);
	return s;
}

/* ------------------------------------------------------------------------ */
/* rendering                                                                 */

static inline void setf(gs_eparam_t *p, float v)
{
	if (p)
		gs_effect_set_float(p, v);
}

static void stg_draw(void *data, gs_texture_t *a, gs_texture_t *b, float t, uint32_t cx, uint32_t cy)
{
	struct spazz_stinger *s = data;
	const struct stg_vals *v = &s->render;

	if (!s->effect)
		return;

	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);

	/* same colour handling as OBS's fade: blend what people see on SDR
	 * canvases, real light on linear / HDR ones */
	const bool srgb_space = gs_get_color_space() == GS_CS_SRGB;
	gs_texture_t *ta = a ? a : b;
	gs_texture_t *tb = b ? b : a;
	if (srgb_space) {
		gs_effect_set_texture(s->p_tex_a, ta);
		gs_effect_set_texture(s->p_tex_b, tb);
	} else {
		gs_effect_set_texture_srgb(s->p_tex_a, ta);
		gs_effect_set_texture_srgb(s->p_tex_b, tb);
	}
	setf(s->p_has_a, a ? 1.0f : 0.0f);
	setf(s->p_has_b, b ? 1.0f : 0.0f);
	setf(s->p_lin, srgb_space ? 0.0f : 1.0f);
	setf(s->p_t, t);
	setf(s->p_cut, v->cut);
	setf(s->p_time, t * (float)v->duration_ms / 1000.0f);
	setf(s->p_seed, v->seed);
	if (s->p_res) {
		struct vec2 r;
		vec2_set(&r, (float)cx, (float)cy);
		gs_effect_set_vec2(s->p_res, &r);
	}

	for (int i = 0; i < STG_PARAM_COUNT; i++) {
		const struct stg_param *p = &stg_params[i];
		if (p->style != v->style || p->slot < 0 || p->slot >= SLOT_COUNT || !s->p_slot[p->slot])
			continue;
		if (p->type == STG_COLOR) {
			uint32_t hex = (uint32_t)v->v[i];
			struct vec3 c;
			vec3_set(&c, (float)((hex >> 16) & 0xFF) / 255.0f, (float)((hex >> 8) & 0xFF) / 255.0f,
				 (float)(hex & 0xFF) / 255.0f);
			gs_effect_set_vec3(s->p_slot[p->slot], &c);
		} else {
			gs_effect_set_float(s->p_slot[p->slot], (float)v->v[i]);
		}
	}

	while (gs_effect_loop(s->effect, stg_styles[v->style].technique))
		gs_draw_sprite(NULL, 0, cx, cy);

	gs_enable_framebuffer_srgb(previous);
}

static void stg_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct spazz_stinger *s = data;

	pthread_mutex_lock(&s->mutex);
	s->render = s->vals;
	pthread_mutex_unlock(&s->mutex);

	const bool previous = gs_set_linear_srgb(true);
	obs_transition_video_render2(s->source, stg_draw, NULL);
	gs_set_linear_srgb(previous);
}

/* audio: switch at the cut (with a few ms of overlap), or crossfade */
static float stg_mix_a(void *data, float t)
{
	struct spazz_stinger *s = data;
	pthread_mutex_lock(&s->mutex);
	const bool fade = s->vals.audio_fade;
	const float c = s->vals.cut;
	pthread_mutex_unlock(&s->mutex);

	if (fade)
		return 1.0f - t;
	float x = (t - (c - 0.03f)) / 0.06f;
	x = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
	return 1.0f - x * x * (3.0f - 2.0f * x);
}

static float stg_mix_b(void *data, float t)
{
	return 1.0f - stg_mix_a(data, t);
}

static bool stg_audio_render(void *data, uint64_t *ts_out, struct obs_source_audio_mix *audio, uint32_t mixers,
			     size_t channels, size_t sample_rate)
{
	struct spazz_stinger *s = data;
	return obs_transition_audio_render(s->source, ts_out, audio, mixers, channels, sample_rate, stg_mix_a,
					   stg_mix_b);
}

static enum gs_color_space stg_get_color_space(void *data, size_t count, const enum gs_color_space *preferred_spaces)
{
	struct spazz_stinger *s = data;
	const enum gs_color_space transition_space = obs_transition_video_get_color_space(s->source);

	enum gs_color_space space = transition_space;
	for (size_t i = 0; i < count; ++i) {
		space = preferred_spaces[i];
		if (space == transition_space)
			break;
	}
	return space;
}

static struct obs_source_info spazz_stinger_info = {
	.id = "spazz_stinger",
	.type = OBS_SOURCE_TYPE_TRANSITION,
	.get_name = stg_get_name,
	.create = stg_create,
	.destroy = stg_destroy,
	.update = stg_update,
	.get_defaults = stg_defaults,
	.get_properties = stg_properties,
	.video_render = stg_video_render,
	.audio_render = stg_audio_render,
	.video_get_color_space = stg_get_color_space,
};

bool obs_module_load(void)
{
	obs_register_source(&spazz_stinger_info);
	return true;
}
