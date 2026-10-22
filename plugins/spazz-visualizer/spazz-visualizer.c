/******************************************************************************
    SPAZZbs - music visualizer source
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

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/bmem.h>
#include <math.h>
#include <string.h>

#include "spazz-analyzer.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("spazz-visualizer", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "SPAZZbs music visualizer";
}

#define T_(s) obs_module_text(s)

#define DEFAULT_TARGET "SPAZZ Music"
#define RETRY_SECONDS 1.0f
#define SILENCE_NS 250000000ULL

static const char *style_techniques[] = {"PrismRain", "PrismHalo", "SpectrumGlass"};
#define NUM_STYLES (sizeof(style_techniques) / sizeof(style_techniques[0]))

struct spazz_viz {
	obs_source_t *source;

	gs_effect_t *effect;
	gs_texture_t *band_tex;
	gs_eparam_t *p_bands, *p_res, *p_time, *p_flow, *p_bass, *p_mid, *p_treble, *p_level, *p_beat, *p_hue,
		*p_density, *p_bg;

	/* settings */
	uint32_t width, height;
	int style;
	float sensitivity;
	float density;
	float color_speed;
	bool midnight;
	char *target_name;

	/* audio input. "mutex" guards the analyzer and is taken by the audio
	 * thread; "target_mutex" guards which source we listen to and is never
	 * taken by the audio thread, so removing the callback can't deadlock. */
	pthread_mutex_t mutex;
	pthread_mutex_t target_mutex;
	obs_weak_source_t *target;
	bool retarget;
	float retry_timer;
	uint64_t last_audio_ns;
	struct spazz_analyzer *analyzer;
	float *mono_scratch;
	size_t mono_capacity;

	/* animation */
	float time, flow, hue;
	uint8_t tex_data[SPAZZ_NUM_BANDS * 4];
	bool tex_dirty;
};

/* ------------------------------------------------------------------------ */
/* audio                                                                     */

static void audio_capture(void *param, obs_source_t *src, const struct audio_data *audio, bool muted)
{
	struct spazz_viz *v = param;
	UNUSED_PARAMETER(src);

	if (!audio->frames)
		return;

	size_t channels = audio_output_get_channels(obs_get_audio());
	if (channels > MAX_AV_PLANES)
		channels = MAX_AV_PLANES;

	pthread_mutex_lock(&v->mutex);

	if (v->mono_capacity < audio->frames) {
		v->mono_scratch = brealloc(v->mono_scratch, audio->frames * sizeof(float));
		v->mono_capacity = audio->frames;
	}

	if (muted) {
		spazz_analyzer_push_silence(v->analyzer, audio->frames);
	} else {
		for (uint32_t i = 0; i < audio->frames; i++) {
			float sum = 0.0f;
			int used = 0;
			for (size_t c = 0; c < channels; c++) {
				if (audio->data[c]) {
					sum += ((const float *)audio->data[c])[i];
					used++;
				}
			}
			v->mono_scratch[i] = used ? sum / used : 0.0f;
		}
		spazz_analyzer_push(v->analyzer, v->mono_scratch, audio->frames);
	}

	v->last_audio_ns = os_gettime_ns();
	pthread_mutex_unlock(&v->mutex);
}

static void detach_target(struct spazz_viz *v)
{
	if (!v->target)
		return;

	obs_source_t *src = obs_weak_source_get_source(v->target);
	if (src) {
		obs_source_remove_audio_capture_callback(src, audio_capture, v);
		obs_source_release(src);
	}

	obs_weak_source_release(v->target);
	v->target = NULL;
}

static bool target_alive(struct spazz_viz *v)
{
	if (!v->target)
		return false;

	obs_source_t *src = obs_weak_source_get_source(v->target);
	if (!src)
		return false;

	bool alive = !obs_source_removed(src);
	obs_source_release(src);
	return alive;
}

static bool find_music_player(void *param, obs_source_t *src)
{
	obs_source_t **found = param;
	obs_data_t *priv = obs_source_get_private_settings(src);
	bool is_player = obs_data_get_bool(priv, "spazz_music");
	obs_data_release(priv);

	if (is_player) {
		*found = obs_source_get_ref(src);
		return false;
	}
	return true;
}

static obs_source_t *find_target(const char *name)
{
	/* "SPAZZ Music" means the music player's source, whatever OBS ended up
	 * naming it (e.g. "SPAZZ Music 2" if the name was taken) */
	if (strcmp(name, DEFAULT_TARGET) == 0) {
		obs_source_t *player = NULL;
		obs_enum_sources(find_music_player, &player);
		if (player)
			return player;
	}

	return obs_get_source_by_name(name);
}

static void attach_target(struct spazz_viz *v)
{
	detach_target(v);

	if (!v->target_name || !*v->target_name)
		return;

	obs_source_t *src = find_target(v->target_name);
	if (!src)
		return;

	if ((obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO) != 0 && src != v->source) {
		v->target = obs_source_get_weak_source(src);
		obs_source_add_audio_capture_callback(src, audio_capture, v);
	}

	obs_source_release(src);
}

/* ------------------------------------------------------------------------ */
/* source callbacks                                                          */

static const char *viz_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return T_("SpazzVisualizer");
}

static void viz_update(void *data, obs_data_t *settings)
{
	struct spazz_viz *v = data;

	int w = (int)obs_data_get_int(settings, "width");
	int h = (int)obs_data_get_int(settings, "height");
	v->width = (uint32_t)(w < 16 ? 16 : w);
	v->height = (uint32_t)(h < 16 ? 16 : h);

	const char *style = obs_data_get_string(settings, "style");
	v->style = 0;
	for (size_t i = 0; i < NUM_STYLES; i++) {
		if (strcmp(style, style_techniques[i]) == 0)
			v->style = (int)i;
	}

	v->sensitivity = (float)obs_data_get_double(settings, "sensitivity");
	v->density = (float)obs_data_get_double(settings, "density");
	v->color_speed = (float)obs_data_get_double(settings, "color_speed");
	v->midnight = strcmp(obs_data_get_string(settings, "background"), "midnight") == 0;

	const char *target = obs_data_get_string(settings, "audio_source");
	pthread_mutex_lock(&v->target_mutex);
	if (!v->target_name || strcmp(v->target_name, target) != 0) {
		bfree(v->target_name);
		v->target_name = bstrdup(target);
		v->retarget = true;
	}
	pthread_mutex_unlock(&v->target_mutex);
}

static void viz_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "style", "PrismRain");
	obs_data_set_default_string(settings, "audio_source", DEFAULT_TARGET);
	obs_data_set_default_double(settings, "sensitivity", 1.0);
	obs_data_set_default_double(settings, "density", 1.0);
	obs_data_set_default_double(settings, "color_speed", 1.0);
	obs_data_set_default_string(settings, "background", "transparent");
	obs_data_set_default_int(settings, "width", 1920);
	obs_data_set_default_int(settings, "height", 1080);
}

static bool add_audio_source(void *param, obs_source_t *src)
{
	obs_property_t *list = param;
	uint32_t flags = obs_source_get_output_flags(src);

	if ((flags & OBS_SOURCE_AUDIO) == 0)
		return true;

	const char *name = obs_source_get_name(src);
	if (name && *name && strcmp(name, DEFAULT_TARGET) != 0)
		obs_property_list_add_string(list, name, name);
	return true;
}

static obs_properties_t *viz_properties(void *data)
{
	struct spazz_viz *v = data;
	obs_properties_t *props = obs_properties_create();

	obs_property_t *p = obs_properties_add_list(props, "style", T_("Style"), OBS_COMBO_TYPE_LIST,
						    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_("Style.PrismRain"), "PrismRain");
	obs_property_list_add_string(p, T_("Style.PrismHalo"), "PrismHalo");
	obs_property_list_add_string(p, T_("Style.SpectrumGlass"), "SpectrumGlass");

	p = obs_properties_add_list(props, "audio_source", T_("AudioSource"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, DEFAULT_TARGET, DEFAULT_TARGET);
	obs_enum_sources(add_audio_source, p);

	/* keep a saved choice visible even if that source is gone right now */
	if (v) {
		pthread_mutex_lock(&v->target_mutex);
		if (v->target_name && *v->target_name) {
			bool found = false;
			size_t count = obs_property_list_item_count(p);
			for (size_t i = 0; i < count; i++) {
				if (strcmp(obs_property_list_item_string(p, i), v->target_name) == 0)
					found = true;
			}
			if (!found)
				obs_property_list_add_string(p, v->target_name, v->target_name);
		}
		pthread_mutex_unlock(&v->target_mutex);
	}

	p = obs_properties_add_list(props, "background", T_("Background"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_("Background.Transparent"), "transparent");
	obs_property_list_add_string(p, T_("Background.Midnight"), "midnight");

	obs_properties_add_float_slider(props, "sensitivity", T_("Sensitivity"), 0.3, 3.0, 0.05);
	obs_properties_add_float_slider(props, "density", T_("Density"), 0.3, 2.0, 0.05);
	obs_properties_add_float_slider(props, "color_speed", T_("ColorSpeed"), 0.0, 4.0, 0.05);

	obs_properties_add_int(props, "width", T_("Width"), 16, 8192, 1);
	obs_properties_add_int(props, "height", T_("Height"), 16, 8192, 1);

	return props;
}

static void viz_destroy(void *data)
{
	struct spazz_viz *v = data;

	detach_target(v);

	obs_enter_graphics();
	gs_effect_destroy(v->effect);
	gs_texture_destroy(v->band_tex);
	obs_leave_graphics();

	pthread_mutex_destroy(&v->mutex);
	pthread_mutex_destroy(&v->target_mutex);
	bfree(v->analyzer);
	bfree(v->mono_scratch);
	bfree(v->target_name);
	bfree(v);
}

static void *viz_create(obs_data_t *settings, obs_source_t *source)
{
	struct spazz_viz *v = bzalloc(sizeof(struct spazz_viz));
	v->source = source;

	if (pthread_mutex_init(&v->mutex, NULL) != 0) {
		bfree(v);
		return NULL;
	}
	if (pthread_mutex_init(&v->target_mutex, NULL) != 0) {
		pthread_mutex_destroy(&v->mutex);
		bfree(v);
		return NULL;
	}

	struct obs_audio_info oai;
	uint32_t rate = obs_get_audio_info(&oai) ? oai.samples_per_sec : 48000;
	v->analyzer = bzalloc(sizeof(struct spazz_analyzer));
	spazz_analyzer_init(v->analyzer, rate);

	char *path = obs_module_file("spazz-visualizer.effect");

	obs_enter_graphics();
	if (path) {
		char *errors = NULL;
		v->effect = gs_effect_create_from_file(path, &errors);
		if (!v->effect)
			blog(LOG_ERROR, "[spazz-visualizer] could not load effect: %s", errors ? errors : "(no details)");
		bfree(errors);
	}
	v->band_tex = gs_texture_create(SPAZZ_NUM_BANDS, 1, GS_RGBA, 1, NULL, GS_DYNAMIC);
	obs_leave_graphics();
	bfree(path);

	if (v->effect) {
		v->p_bands = gs_effect_get_param_by_name(v->effect, "bands");
		v->p_res = gs_effect_get_param_by_name(v->effect, "res");
		v->p_time = gs_effect_get_param_by_name(v->effect, "time");
		v->p_flow = gs_effect_get_param_by_name(v->effect, "flow");
		v->p_bass = gs_effect_get_param_by_name(v->effect, "bass");
		v->p_mid = gs_effect_get_param_by_name(v->effect, "mid");
		v->p_treble = gs_effect_get_param_by_name(v->effect, "treble");
		v->p_level = gs_effect_get_param_by_name(v->effect, "level");
		v->p_beat = gs_effect_get_param_by_name(v->effect, "beat");
		v->p_hue = gs_effect_get_param_by_name(v->effect, "hue");
		v->p_density = gs_effect_get_param_by_name(v->effect, "density");
		v->p_bg = gs_effect_get_param_by_name(v->effect, "bg");
	}

	viz_update(v, settings);
	return v;
}

static void viz_tick(void *data, float seconds)
{
	struct spazz_viz *v = data;

	/* the music source may be created after us, or replaced when the
	 * scene collection changes, so keep looking for it */
	pthread_mutex_lock(&v->target_mutex);
	v->retry_timer -= seconds;
	if (v->retarget) {
		v->retarget = false;
		v->retry_timer = RETRY_SECONDS;
		attach_target(v);
	} else if (v->retry_timer <= 0.0f) {
		v->retry_timer = RETRY_SECONDS;
		if (!target_alive(v))
			attach_target(v);
	}
	pthread_mutex_unlock(&v->target_mutex);

	/* not in any scene being shown: skip the analysis and animation */
	if (!obs_source_showing(v->source))
		return;

	pthread_mutex_lock(&v->mutex);
	uint64_t now = os_gettime_ns();
	if (now - v->last_audio_ns > SILENCE_NS) {
		/* paused or stopped: let everything settle down */
		spazz_analyzer_push_silence(v->analyzer, (size_t)(seconds * v->analyzer->sample_rate));
	}
	spazz_analyzer_update(v->analyzer, seconds, v->sensitivity);

	struct spazz_analyzer *a = v->analyzer;
	for (int i = 0; i < SPAZZ_NUM_BANDS; i++) {
		v->tex_data[i * 4 + 0] = (uint8_t)(a->bands[i] * 255.0f + 0.5f);
		v->tex_data[i * 4 + 1] = (uint8_t)(a->peaks[i] * 255.0f + 0.5f);
		v->tex_data[i * 4 + 2] = (uint8_t)(a->flash[i] * 255.0f + 0.5f);
		v->tex_data[i * 4 + 3] = 255;
	}
	float level = a->level;
	float beat = a->beat;
	pthread_mutex_unlock(&v->mutex);

	v->tex_dirty = true;
	v->time += seconds;
	v->flow += seconds * (0.3f + 1.1f * level + 0.5f * beat);
	v->hue += seconds * v->color_speed * 0.08f;

	/* keep floats small so the shader stays precise on long streams */
	if (v->time > 3600.0f)
		v->time -= 3600.0f;
	/* drop speeds are multiples of 1/16 in the shader, so wrapping at 64
	 * lands every drop exactly where it was: no jump, and full precision */
	if (v->flow >= 64.0f)
		v->flow -= 64.0f;
	v->hue -= floorf(v->hue);
}

static void viz_render(void *data, gs_effect_t *unused)
{
	struct spazz_viz *v = data;
	UNUSED_PARAMETER(unused);

	if (!v->effect || !v->band_tex)
		return;

	if (v->tex_dirty) {
		gs_texture_set_image(v->band_tex, v->tex_data, SPAZZ_NUM_BANDS * 4, false);
		v->tex_dirty = false;
	}

	struct vec2 res;
	vec2_set(&res, (float)v->width, (float)v->height);

	pthread_mutex_lock(&v->mutex);
	struct spazz_analyzer *a = v->analyzer;
	float bass = a->bass, mid = a->mid, treble = a->treble, level = a->level, beat = a->beat;
	pthread_mutex_unlock(&v->mutex);

	gs_effect_set_texture(v->p_bands, v->band_tex);
	gs_effect_set_vec2(v->p_res, &res);
	gs_effect_set_float(v->p_time, v->time);
	gs_effect_set_float(v->p_flow, v->flow);
	gs_effect_set_float(v->p_bass, bass);
	gs_effect_set_float(v->p_mid, mid);
	gs_effect_set_float(v->p_treble, treble);
	gs_effect_set_float(v->p_level, level);
	gs_effect_set_float(v->p_beat, beat);
	gs_effect_set_float(v->p_hue, v->hue);
	gs_effect_set_float(v->p_density, v->density);
	gs_effect_set_float(v->p_bg, v->midnight ? 1.0f : 0.0f);

	const char *tech = style_techniques[v->style];
	while (gs_effect_loop(v->effect, tech))
		gs_draw_sprite(NULL, 0, v->width, v->height);
}

static uint32_t viz_width(void *data)
{
	struct spazz_viz *v = data;
	return v->width;
}

static uint32_t viz_height(void *data)
{
	struct spazz_viz *v = data;
	return v->height;
}

static struct obs_source_info spazz_visualizer_info = {
	.id = "spazz_visualizer",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = viz_get_name,
	.create = viz_create,
	.destroy = viz_destroy,
	.update = viz_update,
	.get_defaults = viz_defaults,
	.get_properties = viz_properties,
	.video_tick = viz_tick,
	.video_render = viz_render,
	.get_width = viz_width,
	.get_height = viz_height,
	.icon_type = OBS_ICON_TYPE_MEDIA,
};

bool obs_module_load(void)
{
	obs_register_source(&spazz_visualizer_info);
	return true;
}
