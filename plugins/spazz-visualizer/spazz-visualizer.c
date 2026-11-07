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
#include <stddef.h>
#include <string.h>

#include "spazz-analyzer.h"
#include "spazz-pixel-font.h"

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

static const char *style_techniques[] = {
	"PrismRain", "PrismHalo", "PrismPetals", "PrismAurora", "PrismTunnel", "PrismWaves", "PrismKaleido",
	"SpectrumGlass", "CandyBars", "Bubbles", "Confetti", "BounceBuddies", "DiscoFloor", "LavaLamp", "GroovySwirl",
	"StarWarp", "Fireflies", "Fireworks", "Galaxy", "Synthwave", "RainbowRipples", "NeonHeartbeat", "FlowerGarden",
	"DiscoBall", "PixelEQ", "DNAHelix", "PlasmaGlobe", "OceanSunset",
	/* the SPAZZamp player on stream, in a few skins (keep these together,
	 * in this order: the skin number is the offset from PlayerSpazz) */
	"PlayerSpazz", "PlayerClassic", "PlayerNeon", "PlayerSynthwave", "PlayerCRT", "PlayerGlitch",
	/* retro screens */
	"NeonScope", "CRTSpectrum", "GlitchBars", "VHSTape"};
#define NUM_STYLES (sizeof(style_techniques) / sizeof(style_techniques[0]))
#define NUM_SKINS 6

/* the player's text: a font atlas (95 glyphs, 6 pixels apart, 8 tall) and
 * a little texture of character codes, one string per row */
#define FONT_TEX_W (96 * 6) /* 95 glyphs; 576 keeps rows 4-byte aligned for OpenGL */
#define FONT_TEX_H 8
#define TEXT_TEX_W 256
#define TEXT_TEX_H 4
#define MARQUEE_CELLS 25 /* characters the marquee shows at once */
/* row 3: fixed labels, at the offsets the shader uses */
static const char player_labels[] = "SPAZZAMP kbps khz mono stereo shuf rep EQ PL 1 PLAY";

/* each skin's own accent colour ("skin colours"); 0 means it cycles like
 * the SPAZZamp dock */
static const uint32_t skin_accents[NUM_SKINS] = {0, 0x19ff4a, 0xff2bd6, 0xff3fb4, 0x39ff6a, 0x00ffd0};

static int player_skin(int style)
{
	static int first = -1;
	if (first < 0) {
		for (size_t i = 0; i < NUM_STYLES; i++) {
			if (strcmp(style_techniques[i], "PlayerSpazz") == 0) {
				first = (int)i;
			}
		}
	}
	const int skin = style - first;
	return skin >= 0 && skin < NUM_SKINS ? skin : -1;
}

/* colour palettes: three colours the styles loop through ("prism" is the
 * original rainbow) */
struct viz_palette {
	const char *id;
	uint32_t a, b, c;
};
static const struct viz_palette palettes[] = {
	{"prism", 0, 0, 0},
	{"sunset", 0xff4e7a, 0xff9a3c, 0xffd84d},
	{"ocean", 0x00c6ff, 0x2a5bff, 0x6ef0c8},
	{"neon", 0xff2bd6, 0x00e5ff, 0xb6ff3b},
	{"vapor", 0xff71ce, 0x01cdfe, 0xb967ff},
	{"candy", 0xff8ad8, 0x9b8cff, 0x7fe8ff},
	{"fire", 0xff3d00, 0xff9100, 0xffe066},
	{"ice", 0xe0f7ff, 0x7ad7ff, 0x9f8cff},
	{"forest", 0x3ddc84, 0xb8f04a, 0x1fb6a6},
	{"gold", 0xffcf40, 0xfff1b8, 0xd98c1a},
	{"mono", 0xffffff, 0xc8d2ff, 0xffffff},
	{"custom", 0, 0, 0},
};
#define NUM_PALETTES (sizeof(palettes) / sizeof(palettes[0]))

struct spazz_viz {
	obs_source_t *source;

	gs_effect_t *effect;
	gs_texture_t *band_tex;
	gs_eparam_t *p_bands, *p_res, *p_time, *p_flow, *p_bass, *p_mid, *p_treble, *p_level, *p_beat, *p_hue,
		*p_density, *p_bg, *p_pal_a, *p_pal_b, *p_pal_c, *p_pal_mix, *p_grade, *p_bg_col, *p_bg_flat, *p_view,
		*p_flip_x, *p_font_tex, *p_text_tex, *p_pl_state, *p_pl_digits, *p_pl_info, *p_pl_look, *p_pl_accent,
		*p_pl_show;

	/* settings */
	uint32_t width, height;
	int style;
	float sensitivity;
	float density;
	float color_speed;
	float speed;
	float bass_boost, treble_boost, beat_punch, smoothing;
	float hue_shift, brightness, saturation;
	float zoom, mirror_x, mirror_y, flip_y, flip_x;
	struct vec3 pal_a, pal_b, pal_c;
	float pal_mix;
	float bg_opacity, bg_flat;
	struct vec3 bg_col;
	float auto_secs, fade_secs;
	bool auto_shuffle;
	char *target_name;

	/* which style is on screen, and the one fading out */
	int shown, prev_shown;
	float fade, auto_timer;
	bool style_changed;
	uint32_t rng;

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

	/* what the shader gets, after the boosts and extra smoothing */
	float bands[SPAZZ_NUM_BANDS], peaks[SPAZZ_NUM_BANDS];
	float bass, mid, treble, level, beat;

	/* animation */
	float time, flow, hue;
	uint8_t tex_data[SPAZZ_NUM_BANDS * 4];
	bool tex_dirty;

	obs_hotkey_id hk_next, hk_prev, hk_random;

	/* the SPAZZamp player styles */
	gs_texture_t *font_tex, *text_tex;
	uint8_t text_data[TEXT_TEX_W * TEXT_TEX_H];
	bool text_dirty;
	int player_viz, player_strip, player_color;
	struct vec3 player_custom;
	bool player_rgb, player_remaining, show_title, show_seek, show_buttons;
	float rgb_speed, marquee_speed;
	float skin_hue;
	/* what's playing, from SPAZZamp (spazz_music_now) */
	float now_poll;
	char *now_title;
	int now_state, now_kbps, now_track, now_repeat;
	bool now_shuffle;
	double now_pos_ms, now_dur_ms, now_volume;
	char marquee[TEXT_TEX_W];
	int marquee_len;
	float marquee_pos;
	bool stereo;
	int khz;
};

/* ------------------------------------------------------------------------ */
/* audio                                                                     */

static void audio_capture(void *param, obs_source_t *src, const struct audio_data *audio, bool muted)
{
	struct spazz_viz *v = param;
	if (!audio->frames) {
		return;
	}

	size_t channels = audio_output_get_channels(obs_get_audio());
	if (channels > MAX_AV_PLANES) {
		channels = MAX_AV_PLANES;
	}

	pthread_mutex_lock(&v->mutex);

	if (v->mono_capacity < audio->frames) {
		v->mono_scratch = brealloc(v->mono_scratch, audio->frames * sizeof(float));
		v->mono_capacity = audio->frames;
	}

	/* a media source (the music player) only counts while it's playing, so
	 * the visuals move to the music and nothing else */
	const bool media = (obs_source_get_output_flags(src) & OBS_SOURCE_CONTROLLABLE_MEDIA) != 0;
	if (muted || (media && obs_source_media_get_state(src) != OBS_MEDIA_STATE_PLAYING)) {
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
	if (!v->target) {
		return;
	}

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
	if (!v->target) {
		return false;
	}

	obs_source_t *src = obs_weak_source_get_source(v->target);
	if (!src) {
		return false;
	}

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
		if (player) {
			return player;
		}
	}

	return obs_get_source_by_name(name);
}

static void attach_target(struct spazz_viz *v)
{
	detach_target(v);

	if (!v->target_name || !*v->target_name) {
		return;
	}

	obs_source_t *src = find_target(v->target_name);
	if (!src) {
		return;
	}

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

static void color_from_hex(struct vec3 *out, uint32_t hex)
{
	vec3_set(out, (float)((hex >> 16) & 0xFF) / 255.0f, (float)((hex >> 8) & 0xFF) / 255.0f,
		 (float)(hex & 0xFF) / 255.0f);
}

/* OBS colour properties are 0xAABBGGRR */
static void color_from_obs(struct vec3 *out, long long c)
{
	const uint32_t v = (uint32_t)c;
	vec3_set(out, (float)(v & 0xFF) / 255.0f, (float)((v >> 8) & 0xFF) / 255.0f,
		 (float)((v >> 16) & 0xFF) / 255.0f);
}

static long long obs_color(uint32_t hex)
{
	const uint32_t r = (hex >> 16) & 0xFF, g = (hex >> 8) & 0xFF, b = hex & 0xFF;
	return (long long)(0xFF000000u | (b << 16) | (g << 8) | r);
}

static int style_index(const char *name)
{
	for (size_t i = 0; name && i < NUM_STYLES; i++) {
		if (strcmp(name, style_techniques[i]) == 0) {
			return (int)i;
		}
	}
	return 0;
}

static bool is(const char *a, const char *b)
{
	return a && strcmp(a, b) == 0;
}

static void viz_update(void *data, obs_data_t *settings)
{
	struct spazz_viz *v = data;

	int w = (int)obs_data_get_int(settings, "width");
	int h = (int)obs_data_get_int(settings, "height");
	v->width = (uint32_t)(w < 16 ? 16 : w);
	v->height = (uint32_t)(h < 16 ? 16 : h);

	const int style = style_index(obs_data_get_string(settings, "style"));
	if (style != v->style) {
		v->style = style;
		v->style_changed = true;
	}

	v->sensitivity = (float)obs_data_get_double(settings, "sensitivity");
	v->density = (float)obs_data_get_double(settings, "density");
	v->color_speed = (float)obs_data_get_double(settings, "color_speed");
	v->speed = (float)obs_data_get_double(settings, "speed");
	v->bass_boost = (float)obs_data_get_double(settings, "bass_boost");
	v->treble_boost = (float)obs_data_get_double(settings, "treble_boost");
	v->beat_punch = (float)obs_data_get_double(settings, "beat_punch");
	v->smoothing = (float)obs_data_get_double(settings, "smoothing");
	v->hue_shift = (float)obs_data_get_double(settings, "hue_shift");
	v->brightness = (float)obs_data_get_double(settings, "brightness");
	v->saturation = (float)obs_data_get_double(settings, "saturation");
	v->zoom = (float)obs_data_get_double(settings, "zoom");

	const char *mirror = obs_data_get_string(settings, "mirror");
	v->mirror_x = is(mirror, "lr") || is(mirror, "both") ? 1.0f : 0.0f;
	v->mirror_y = is(mirror, "tb") || is(mirror, "both") ? 1.0f : 0.0f;
	const char *flip = obs_data_get_string(settings, "flip");
	v->flip_y = is(flip, "v") || is(flip, "both") ? 1.0f : 0.0f;
	v->flip_x = is(flip, "h") || is(flip, "both") ? 1.0f : 0.0f;

	const char *pal = obs_data_get_string(settings, "palette");
	v->pal_mix = 0.0f;
	if (is(pal, "custom")) {
		color_from_obs(&v->pal_a, obs_data_get_int(settings, "color_1"));
		color_from_obs(&v->pal_b, obs_data_get_int(settings, "color_2"));
		color_from_obs(&v->pal_c, obs_data_get_int(settings, "color_3"));
		v->pal_mix = 1.0f;
	} else {
		for (size_t i = 1; i + 1 < NUM_PALETTES; i++) {
			if (is(pal, palettes[i].id)) {
				color_from_hex(&v->pal_a, palettes[i].a);
				color_from_hex(&v->pal_b, palettes[i].b);
				color_from_hex(&v->pal_c, palettes[i].c);
				v->pal_mix = 1.0f;
			}
		}
	}

	const char *bg = obs_data_get_string(settings, "background");
	const float opacity = (float)obs_data_get_double(settings, "bg_opacity");
	v->bg_opacity = is(bg, "transparent") ? 0.0f : opacity;
	v->bg_flat = is(bg, "midnight") ? 0.0f : 1.0f;
	if (is(bg, "custom")) {
		color_from_obs(&v->bg_col, obs_data_get_int(settings, "bg_color"));
	} else {
		vec3_zero(&v->bg_col);
	}

	/* the SPAZZamp player */
	const char *pv = obs_data_get_string(settings, "player_viz");
	v->player_viz = is(pv, "scope") ? 1 : is(pv, "aurora") ? 2 : is(pv, "peaks") ? 3 : 0;
	const char *ps = obs_data_get_string(settings, "player_strip");
	v->player_strip = is(ps, "bars") ? 0 : is(ps, "scope") ? 1 : is(ps, "aurora") ? 2 : is(ps, "peaks") ? 3 : -1;
	const char *pc = obs_data_get_string(settings, "player_color");
	v->player_color = is(pc, "palette") ? 2 : is(pc, "custom") ? 1 : 0;
	color_from_obs(&v->player_custom, obs_data_get_int(settings, "player_accent"));
	v->player_rgb = obs_data_get_bool(settings, "player_rgb");
	v->rgb_speed = (float)obs_data_get_double(settings, "player_rgb_speed");
	v->player_remaining = is(obs_data_get_string(settings, "player_time"), "remaining");
	v->show_title = obs_data_get_bool(settings, "player_titlebar");
	v->show_seek = obs_data_get_bool(settings, "player_seek");
	v->show_buttons = obs_data_get_bool(settings, "player_buttons");
	v->marquee_speed = (float)obs_data_get_double(settings, "marquee_speed");

	v->auto_secs = (float)obs_data_get_double(settings, "auto_switch");
	v->auto_shuffle = obs_data_get_bool(settings, "auto_shuffle");
	v->fade_secs = (float)obs_data_get_double(settings, "crossfade");

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
	obs_data_set_default_double(settings, "speed", 1.0);
	obs_data_set_default_double(settings, "bass_boost", 1.0);
	obs_data_set_default_double(settings, "treble_boost", 1.0);
	obs_data_set_default_double(settings, "beat_punch", 1.0);
	obs_data_set_default_double(settings, "smoothing", 0.0);
	obs_data_set_default_string(settings, "palette", "prism");
	obs_data_set_default_int(settings, "color_1", obs_color(0xff3f8e));
	obs_data_set_default_int(settings, "color_2", obs_color(0x8a5cff));
	obs_data_set_default_int(settings, "color_3", obs_color(0x2bd9ff));
	obs_data_set_default_double(settings, "hue_shift", 0.0);
	obs_data_set_default_double(settings, "brightness", 1.0);
	obs_data_set_default_double(settings, "saturation", 1.0);
	obs_data_set_default_double(settings, "zoom", 1.0);
	obs_data_set_default_string(settings, "mirror", "none");
	obs_data_set_default_string(settings, "flip", "none");
	obs_data_set_default_string(settings, "background", "transparent");
	obs_data_set_default_int(settings, "bg_color", obs_color(0x0b0a14));
	obs_data_set_default_double(settings, "bg_opacity", 1.0);
	obs_data_set_default_double(settings, "auto_switch", 0.0);
	obs_data_set_default_bool(settings, "auto_shuffle", false);
	obs_data_set_default_double(settings, "crossfade", 1.5);
	obs_data_set_default_int(settings, "width", 1920);
	obs_data_set_default_int(settings, "height", 1080);
	obs_data_set_default_string(settings, "player_viz", "bars");
	obs_data_set_default_string(settings, "player_strip", "scope");
	obs_data_set_default_string(settings, "player_color", "skin");
	obs_data_set_default_int(settings, "player_accent", obs_color(0x2bd9ff));
	obs_data_set_default_bool(settings, "player_rgb", false);
	obs_data_set_default_double(settings, "player_rgb_speed", 1.0);
	obs_data_set_default_string(settings, "player_time", "elapsed");
	obs_data_set_default_bool(settings, "player_titlebar", true);
	obs_data_set_default_bool(settings, "player_seek", true);
	obs_data_set_default_bool(settings, "player_buttons", true);
	obs_data_set_default_double(settings, "marquee_speed", 1.0);
}

static bool add_audio_source(void *param, obs_source_t *src)
{
	obs_property_t *list = param;
	uint32_t flags = obs_source_get_output_flags(src);

	if ((flags & OBS_SOURCE_AUDIO) == 0) {
		return true;
	}

	const char *name = obs_source_get_name(src);
	if (name && *name && strcmp(name, DEFAULT_TARGET) != 0) {
		obs_property_list_add_string(list, name, name);
	}
	return true;
}

/* ---- changing the style: properties buttons, hotkeys, SPAZZamp ---- */

static uint32_t next_rand(struct spazz_viz *v)
{
	/* xorshift: good enough to pick a style */
	uint32_t x = v->rng ? v->rng : 0x9e3779b9u;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	v->rng = x;
	return x;
}

static int random_other_style(struct spazz_viz *v, int not_this)
{
	int idx = (int)(next_rand(v) % (uint32_t)(NUM_STYLES - 1));
	return idx >= not_this ? idx + 1 : idx;
}

/* stored in the settings, so it sticks and the properties show it */
static void set_style(struct spazz_viz *v, int idx)
{
	idx = (idx % (int)NUM_STYLES + (int)NUM_STYLES) % (int)NUM_STYLES;
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "style", style_techniques[idx]);
	obs_source_update(v->source, s);
	obs_data_release(s);
}

static void step_style(struct spazz_viz *v, int dir)
{
	if (!v) {
		return;
	}
	obs_data_t *s = obs_source_get_settings(v->source);
	const int cur = style_index(obs_data_get_string(s, "style"));
	obs_data_release(s);
	set_style(v, dir == 0 ? random_other_style(v, cur) : cur + dir);
}

static bool next_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);
	step_style(data, 1);
	return true;
}

static bool prev_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);
	step_style(data, -1);
	return true;
}

static bool random_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);
	step_style(data, 0);
	return true;
}

static void hk_next(void *data, obs_hotkey_id id, obs_hotkey_t *hk, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hk);
	if (pressed) {
		step_style(data, 1);
	}
}

static void hk_prev(void *data, obs_hotkey_id id, obs_hotkey_t *hk, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hk);
	if (pressed) {
		step_style(data, -1);
	}
}

static void hk_random(void *data, obs_hotkey_id id, obs_hotkey_t *hk, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hk);
	if (pressed) {
		step_style(data, 0);
	}
}

/* show only what applies */
static bool palette_modified(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);
	const bool custom = is(obs_data_get_string(settings, "palette"), "custom");
	obs_property_set_visible(obs_properties_get(props, "color_1"), custom);
	obs_property_set_visible(obs_properties_get(props, "color_2"), custom);
	obs_property_set_visible(obs_properties_get(props, "color_3"), custom);
	return true;
}

static bool background_modified(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);
	const char *bg = obs_data_get_string(settings, "background");
	obs_property_set_visible(obs_properties_get(props, "bg_color"), is(bg, "custom"));
	obs_property_set_visible(obs_properties_get(props, "bg_opacity"), !is(bg, "transparent"));
	return true;
}

static bool auto_modified(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);
	const bool on = obs_data_get_double(settings, "auto_switch") > 0.0;
	obs_property_set_visible(obs_properties_get(props, "auto_shuffle"), on);
	return true;
}

static bool style_modified(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);
	const bool player = player_skin(style_index(obs_data_get_string(settings, "style"))) >= 0;
	obs_property_set_visible(obs_properties_get(props, "grp_player"), player);
	return true;
}

static bool player_color_modified(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(prop);
	obs_property_set_visible(obs_properties_get(props, "player_accent"),
				 is(obs_data_get_string(settings, "player_color"), "custom"));
	obs_property_set_visible(obs_properties_get(props, "player_rgb_speed"),
				 obs_data_get_bool(settings, "player_rgb") ||
					 is(obs_data_get_string(settings, "player_color"), "skin"));
	return true;
}

static obs_property_t *add_list(obs_properties_t *props, const char *key, const char *label)
{
	return obs_properties_add_list(props, key, label, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
}

static obs_properties_t *viz_properties(void *data)
{
	struct spazz_viz *v = data;
	obs_properties_t *props = obs_properties_create();
	char key[64];

	obs_property_t *p = add_list(props, "style", T_("Style"));
	for (size_t i = 0; i < NUM_STYLES; i++) {
		snprintf(key, sizeof(key), "Style.%s", style_techniques[i]);
		obs_property_list_add_string(p, T_(key), style_techniques[i]);
	}
	obs_property_set_modified_callback(p, style_modified);
	obs_properties_add_button2(props, "style_prev", T_("PrevStyle"), prev_clicked, v);
	obs_properties_add_button2(props, "style_next", T_("NextStyle"), next_clicked, v);
	obs_properties_add_button2(props, "style_random", T_("RandomStyle"), random_clicked, v);

	p = add_list(props, "audio_source", T_("AudioSource"));
	obs_property_list_add_string(p, DEFAULT_TARGET, DEFAULT_TARGET);
	obs_enum_sources(add_audio_source, p);

	/* keep a saved choice visible even if that source is gone right now */
	if (v) {
		pthread_mutex_lock(&v->target_mutex);
		if (v->target_name && *v->target_name) {
			bool found = false;
			size_t count = obs_property_list_item_count(p);
			for (size_t i = 0; i < count; i++) {
				if (strcmp(obs_property_list_item_string(p, i), v->target_name) == 0) {
					found = true;
				}
			}
			if (!found) {
				obs_property_list_add_string(p, v->target_name, v->target_name);
			}
		}
		pthread_mutex_unlock(&v->target_mutex);
	}

	/* the SPAZZamp player skins */
	obs_properties_t *pl = obs_properties_create();
	static const char *viz_kinds[] = {"bars", "scope", "aurora", "peaks"};
	p = add_list(pl, "player_viz", T_("PlayerViz"));
	for (size_t i = 0; i < 4; i++) {
		snprintf(key, sizeof(key), "PlayerViz.%s", viz_kinds[i]);
		obs_property_list_add_string(p, T_(key), viz_kinds[i]);
	}
	p = add_list(pl, "player_strip", T_("PlayerStrip"));
	obs_property_list_add_string(p, T_("PlayerStrip.off"), "off");
	for (size_t i = 0; i < 4; i++) {
		snprintf(key, sizeof(key), "PlayerViz.%s", viz_kinds[i]);
		obs_property_list_add_string(p, T_(key), viz_kinds[i]);
	}
	p = add_list(pl, "player_color", T_("PlayerColor"));
	obs_property_list_add_string(p, T_("PlayerColor.skin"), "skin");
	obs_property_list_add_string(p, T_("PlayerColor.palette"), "palette");
	obs_property_list_add_string(p, T_("PlayerColor.custom"), "custom");
	obs_property_set_modified_callback(p, player_color_modified);
	obs_properties_add_color(pl, "player_accent", T_("PlayerAccent"));
	p = obs_properties_add_bool(pl, "player_rgb", T_("PlayerRGB"));
	obs_property_set_modified_callback(p, player_color_modified);
	p = obs_properties_add_float_slider(pl, "player_rgb_speed", T_("PlayerRGBSpeed"), 0.05, 4.0, 0.05);
	obs_property_float_set_suffix(p, "x");
	p = add_list(pl, "player_time", T_("PlayerTime"));
	obs_property_list_add_string(p, T_("PlayerTime.elapsed"), "elapsed");
	obs_property_list_add_string(p, T_("PlayerTime.remaining"), "remaining");
	obs_properties_add_bool(pl, "player_titlebar", T_("PlayerTitlebar"));
	obs_properties_add_bool(pl, "player_seek", T_("PlayerSeek"));
	obs_properties_add_bool(pl, "player_buttons", T_("PlayerButtons"));
	p = obs_properties_add_float_slider(pl, "marquee_speed", T_("MarqueeSpeed"), 0.0, 4.0, 0.05);
	obs_property_float_set_suffix(p, "x");
	p = obs_properties_add_group(props, "grp_player", T_("Group.Player"), OBS_GROUP_NORMAL, pl);
	obs_property_set_long_description(p, T_("Group.Player.Tip"));

	/* how it moves with the music */
	obs_properties_t *motion = obs_properties_create();
	p = obs_properties_add_float_slider(motion, "speed", T_("Speed"), 0.1, 3.0, 0.05);
	obs_property_float_set_suffix(p, "x");
	obs_properties_add_float_slider(motion, "sensitivity", T_("Sensitivity"), 0.3, 3.0, 0.05);
	obs_properties_add_float_slider(motion, "bass_boost", T_("BassBoost"), 0.0, 3.0, 0.05);
	obs_properties_add_float_slider(motion, "treble_boost", T_("TrebleBoost"), 0.0, 3.0, 0.05);
	obs_properties_add_float_slider(motion, "beat_punch", T_("BeatPunch"), 0.0, 3.0, 0.05);
	p = obs_properties_add_float_slider(motion, "smoothing", T_("Smoothing"), 0.0, 1.0, 0.01);
	obs_property_set_long_description(p, T_("Smoothing.Tip"));
	obs_properties_add_float_slider(motion, "density", T_("Density"), 0.3, 2.0, 0.05);
	obs_properties_add_group(props, "grp_motion", T_("Group.Motion"), OBS_GROUP_NORMAL, motion);

	/* colour */
	obs_properties_t *color = obs_properties_create();
	p = add_list(color, "palette", T_("Palette"));
	for (size_t i = 0; i < NUM_PALETTES; i++) {
		snprintf(key, sizeof(key), "Palette.%s", palettes[i].id);
		obs_property_list_add_string(p, T_(key), palettes[i].id);
	}
	obs_property_set_modified_callback(p, palette_modified);
	obs_properties_add_color(color, "color_1", T_("Color1"));
	obs_properties_add_color(color, "color_2", T_("Color2"));
	obs_properties_add_color(color, "color_3", T_("Color3"));
	obs_properties_add_float_slider(color, "hue_shift", T_("HueShift"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(color, "color_speed", T_("ColorSpeed"), 0.0, 4.0, 0.05);
	obs_properties_add_float_slider(color, "brightness", T_("Brightness"), 0.2, 3.0, 0.05);
	obs_properties_add_float_slider(color, "saturation", T_("Saturation"), 0.0, 2.0, 0.05);
	obs_properties_add_group(props, "grp_color", T_("Group.Color"), OBS_GROUP_NORMAL, color);

	/* framing and backdrop */
	obs_properties_t *frame = obs_properties_create();
	p = obs_properties_add_float_slider(frame, "zoom", T_("Zoom"), 0.4, 3.0, 0.05);
	obs_property_float_set_suffix(p, "x");
	p = add_list(frame, "mirror", T_("Mirror"));
	obs_property_list_add_string(p, T_("Mirror.None"), "none");
	obs_property_list_add_string(p, T_("Mirror.LR"), "lr");
	obs_property_list_add_string(p, T_("Mirror.TB"), "tb");
	obs_property_list_add_string(p, T_("Mirror.Both"), "both");
	p = add_list(frame, "flip", T_("Flip"));
	obs_property_list_add_string(p, T_("Flip.None"), "none");
	obs_property_list_add_string(p, T_("Flip.V"), "v");
	obs_property_list_add_string(p, T_("Flip.H"), "h");
	obs_property_list_add_string(p, T_("Flip.Both"), "both");
	p = add_list(frame, "background", T_("Background"));
	obs_property_list_add_string(p, T_("Background.Transparent"), "transparent");
	obs_property_list_add_string(p, T_("Background.Midnight"), "midnight");
	obs_property_list_add_string(p, T_("Background.Black"), "black");
	obs_property_list_add_string(p, T_("Background.Custom"), "custom");
	obs_property_set_modified_callback(p, background_modified);
	obs_properties_add_color(frame, "bg_color", T_("BgColor"));
	obs_properties_add_float_slider(frame, "bg_opacity", T_("BgOpacity"), 0.0, 1.0, 0.01);
	obs_properties_add_int(frame, "width", T_("Width"), 16, 8192, 1);
	obs_properties_add_int(frame, "height", T_("Height"), 16, 8192, 1);
	obs_properties_add_group(props, "grp_frame", T_("Group.Frame"), OBS_GROUP_NORMAL, frame);

	/* switching styles on its own */
	obs_properties_t *aut = obs_properties_create();
	p = obs_properties_add_float(aut, "auto_switch", T_("AutoSwitch"), 0.0, 3600.0, 5.0);
	obs_property_float_set_suffix(p, " s");
	obs_property_set_long_description(p, T_("AutoSwitch.Tip"));
	obs_property_set_modified_callback(p, auto_modified);
	obs_properties_add_bool(aut, "auto_shuffle", T_("AutoShuffle"));
	p = obs_properties_add_float_slider(aut, "crossfade", T_("Crossfade"), 0.0, 5.0, 0.1);
	obs_property_float_set_suffix(p, " s");
	obs_properties_add_group(props, "grp_auto", T_("Group.Auto"), OBS_GROUP_NORMAL, aut);

	return props;
}

static void viz_destroy(void *data)
{
	struct spazz_viz *v = data;

	detach_target(v);

	obs_enter_graphics();
	gs_effect_destroy(v->effect);
	gs_texture_destroy(v->band_tex);
	gs_texture_destroy(v->font_tex);
	gs_texture_destroy(v->text_tex);
	obs_leave_graphics();
	bfree(v->now_title);

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
		if (!v->effect) {
			blog(LOG_ERROR, "[spazz-visualizer] could not load effect: %s",
			     errors ? errors : "(no details)");
		}
		bfree(errors);
	}
	v->band_tex = gs_texture_create(SPAZZ_NUM_BANDS, 1, GS_RGBA, 1, NULL, GS_DYNAMIC);
	{
		uint8_t *atlas = bzalloc(FONT_TEX_W * FONT_TEX_H);
		for (int g = 0; g < 95; g++) {
			for (int x = 0; x < 5; x++) {
				for (int y = 0; y < FONT_TEX_H; y++) {
					if (spazz_pixel_font[g][x] & (1 << y)) {
						atlas[y * FONT_TEX_W + g * 6 + x] = 255;
					}
				}
			}
		}
		const uint8_t *levels[] = {atlas};
		v->font_tex = gs_texture_create(FONT_TEX_W, FONT_TEX_H, GS_R8, 1, levels, 0);
		bfree(atlas);
	}
	v->text_tex = gs_texture_create(TEXT_TEX_W, TEXT_TEX_H, GS_R8, 1, NULL, GS_DYNAMIC);
	obs_leave_graphics();
	bfree(path);

	if (v->effect) {
		static const struct {
			size_t offset;
			const char *name;
		} params[] = {
#define P(field, name) {offsetof(struct spazz_viz, field), name}
			P(p_bands, "bands"),         P(p_res, "res"),
			P(p_time, "time"),           P(p_flow, "flow"),
			P(p_bass, "bass"),           P(p_mid, "mid"),
			P(p_treble, "treble"),       P(p_level, "level"),
			P(p_beat, "beat"),           P(p_hue, "hue"),
			P(p_density, "density"),     P(p_bg, "bg"),
			P(p_pal_a, "pal_a"),         P(p_pal_b, "pal_b"),
			P(p_pal_c, "pal_c"),         P(p_pal_mix, "pal_mix"),
			P(p_grade, "grade"),         P(p_bg_col, "bg_col"),
			P(p_bg_flat, "bg_flat"),     P(p_view, "view"),
			P(p_flip_x, "flip_x"),       P(p_font_tex, "font_tex"),
			P(p_text_tex, "text_tex"),   P(p_pl_state, "pl_state"),
			P(p_pl_digits, "pl_digits"), P(p_pl_info, "pl_info"),
			P(p_pl_look, "pl_look"),     P(p_pl_accent, "pl_accent"),
			P(p_pl_show, "pl_show"),
#undef P
		};
		for (size_t i = 0; i < sizeof(params) / sizeof(params[0]); i++) {
			gs_eparam_t **slot = (gs_eparam_t **)((char *)v + params[i].offset);
			*slot = gs_effect_get_param_by_name(v->effect, params[i].name);
		}
	}

	/* the player's fixed labels, and an empty marquee until something plays */
	for (size_t i = 0; player_labels[i] && i < TEXT_TEX_W; i++) {
		v->text_data[3 * TEXT_TEX_W + i] = (uint8_t)(player_labels[i] - 32);
	}
	v->text_dirty = true;
	v->now_poll = 0.0f;
	v->now_volume = 1.0;

	v->rng = (uint32_t)os_gettime_ns() | 1u;
	v->style = -1;
	viz_update(v, settings);
	v->shown = v->prev_shown = v->style;
	v->fade = 1.0f;
	v->style_changed = false;

	v->hk_next = obs_hotkey_register_source(source, "SpazzViz.Next", T_("Hotkey.Next"), hk_next, v);
	v->hk_prev = obs_hotkey_register_source(source, "SpazzViz.Prev", T_("Hotkey.Prev"), hk_prev, v);
	v->hk_random = obs_hotkey_register_source(source, "SpazzViz.Random", T_("Hotkey.Random"), hk_random, v);
	return v;
}

/* start fading to another style */
static void show_style(struct spazz_viz *v, int idx)
{
	if (idx == v->shown) {
		return;
	}
	v->prev_shown = v->shown;
	v->shown = idx;
	v->fade = v->fade_secs > 0.01f ? 0.0f : 1.0f;
	v->auto_timer = 0.0f;
}

/* ---- the SPAZZamp player: what's playing ---- */

/* titles can be any language; the pixel font is ASCII, so accented Latin
 * letters lose their accents and anything else becomes '?' */
static size_t to_ascii(char *out, size_t cap, const char *in)
{
	static const char latin1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
	size_t n = 0;
	const unsigned char *s = (const unsigned char *)in;
	while (s && *s && n + 1 < cap) {
		uint32_t cp;
		int len;
		if (*s < 0x80) {
			cp = *s;
			len = 1;
		} else if ((*s & 0xE0) == 0xC0) {
			cp = ((uint32_t)(*s & 0x1F) << 6) | (s[1] & 0x3F);
			len = 2;
		} else if ((*s & 0xF0) == 0xE0) {
			cp = 0xFFFF;
			len = 3;
		} else {
			cp = 0xFFFF;
			len = 4;
		}
		for (int i = 1; i < len; i++) {
			if (!s[i]) {
				len = i;
				break;
			}
		}
		char c = '?';
		if (cp >= 32 && cp < 127) {
			c = (char)cp;
		} else if (cp == '\t') {
			c = ' ';
		} else if (cp >= 0xC0 && cp <= 0xFF) {
			c = latin1[cp - 0xC0];
		} else if (cp < 32) {
			c = 0;
		}
		if (c) {
			out[n++] = c;
		}
		s += len;
	}
	out[n] = 0;
	return n;
}

static void put_row(struct spazz_viz *v, int row, const char *s, size_t len)
{
	uint8_t *dst = v->text_data + row * TEXT_TEX_W;
	for (size_t i = 0; i < TEXT_TEX_W; i++) {
		const char c = i < len ? s[i] : ' ';
		dst[i] = (uint8_t)(c >= 32 && c < 127 ? c - 32 : 0);
	}
	v->text_dirty = true;
}

static void format_time(char *out, size_t cap, double ms)
{
	const long long secs = (long long)(ms / 1000.0);
	snprintf(out, cap, "%lld:%02lld", secs / 60, secs % 60);
}

static void player_poll(struct spazz_viz *v)
{
	calldata_t cd;
	calldata_init(&cd);
	const bool found = proc_handler_call(obs_get_proc_handler(), "spazz_music_now", &cd);
	const char *title = NULL;
	if (found) {
		calldata_get_string(&cd, "title", &title);
		v->now_pos_ms = (double)calldata_int(&cd, "pos_ms");
		v->now_dur_ms = (double)calldata_int(&cd, "dur_ms");
		v->now_state = (int)calldata_int(&cd, "state");
		v->now_shuffle = calldata_bool(&cd, "shuffle");
		v->now_repeat = (int)calldata_int(&cd, "repeat_mode");
		v->now_kbps = (int)calldata_int(&cd, "kbps");
		v->now_track = (int)calldata_int(&cd, "track");
		v->now_volume = calldata_float(&cd, "volume");
	} else {
		v->now_pos_ms = v->now_dur_ms = 0.0;
		v->now_state = OBS_MEDIA_STATE_NONE;
		v->now_kbps = v->now_track = 0;
	}

	/* the marquee: "3. Artist - Song (3:45)", or the player's name */
	char name[200];
	char line[TEXT_TEX_W];
	size_t n = to_ascii(name, sizeof(name), title);
	if (!n) {
		snprintf(line, sizeof(line), "SPAZZamp");
	} else {
		char len_txt[24] = "";
		if (v->now_dur_ms > 0.0) {
			char t[16];
			format_time(t, sizeof(t), v->now_dur_ms);
			snprintf(len_txt, sizeof(len_txt), " (%s)", t);
		}
		if (v->now_track > 0) {
			snprintf(line, sizeof(line), "%d. %s%s", v->now_track, name, len_txt);
		} else {
			snprintf(line, sizeof(line), "%s%s", name, len_txt);
		}
	}
	size_t len = strlen(line);
	if (len > MARQUEE_CELLS) {
		/* too long to fit: it scrolls round, with a gap */
		const char *gap = "  ***  ";
		if (len + strlen(gap) >= TEXT_TEX_W) {
			len = TEXT_TEX_W - 1 - strlen(gap);
			line[len] = 0;
		}
		strcat(line, gap);
		len = strlen(line);
	}
	if (len != (size_t)v->marquee_len || memcmp(line, v->marquee, len) != 0) {
		const bool same_song = title && v->now_title && strcmp(title, v->now_title) == 0;
		if (!same_song) {
			v->marquee_pos = 0.0f;
		}
		memcpy(v->marquee, line, len);
		v->marquee_len = (int)len;
		put_row(v, 0, line, len);
	}
	if (!(title && v->now_title && strcmp(title, v->now_title) == 0)) {
		bfree(v->now_title);
		v->now_title = title ? bstrdup(title) : NULL;
	}

	/* kbps and kHz readouts, right-aligned */
	char num[16];
	const bool going = v->now_state == OBS_MEDIA_STATE_PLAYING || v->now_state == OBS_MEDIA_STATE_PAUSED;
	if (going && v->now_kbps > 0) {
		snprintf(num, sizeof(num), "%3d", v->now_kbps > 999 ? 999 : v->now_kbps);
	} else {
		snprintf(num, sizeof(num), "   ");
	}
	put_row(v, 1, num, 3);
	struct obs_audio_info oai;
	const bool have = obs_get_audio_info(&oai);
	v->khz = have ? (int)((oai.samples_per_sec + 500) / 1000) : 48;
	v->stereo = !have || oai.speakers != SPEAKERS_MONO;
	snprintf(num, sizeof(num), "%2d", v->khz > 99 ? 99 : v->khz);
	put_row(v, 2, num, 2);

	calldata_free(&cd);
}

/* styles that show what's playing: the player skins, and VHS Tape's clock */
static bool uses_music_info(int style)
{
	return player_skin(style) >= 0 || (style >= 0 && strcmp(style_techniques[style], "VHSTape") == 0);
}

static void player_tick(struct spazz_viz *v, float seconds)
{
	const bool wanted = uses_music_info(v->shown) || (v->fade < 1.0f && uses_music_info(v->prev_shown));
	if (!wanted) {
		return;
	}

	v->skin_hue += seconds * v->rgb_speed * 0.12f;
	v->skin_hue -= floorf(v->skin_hue);

	/* the clock runs on between polls */
	if (v->now_state == OBS_MEDIA_STATE_PLAYING) {
		v->now_pos_ms += (double)seconds * 1000.0;
		if (v->now_dur_ms > 0.0 && v->now_pos_ms > v->now_dur_ms) {
			v->now_pos_ms = v->now_dur_ms;
		}
	}
	v->now_poll -= seconds;
	if (v->now_poll <= 0.0f) {
		v->now_poll = 0.25f;
		player_poll(v);
	}

	if (v->marquee_len > MARQUEE_CELLS) {
		v->marquee_pos += seconds * 3.2f * v->marquee_speed;
		v->marquee_pos = fmodf(v->marquee_pos, (float)v->marquee_len);
	} else {
		v->marquee_pos = 0.0f;
	}
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
		if (!target_alive(v)) {
			attach_target(v);
		}
	}
	pthread_mutex_unlock(&v->target_mutex);

	/* a style picked by hand, or the auto switch's turn */
	if (v->style_changed) {
		v->style_changed = false;
		show_style(v, v->style);
	}
	if (v->auto_secs > 0.0f) {
		v->auto_timer += seconds;
		if (v->auto_timer >= v->auto_secs) {
			show_style(v, v->auto_shuffle ? random_other_style(v, v->shown)
						      : (v->shown + 1) % (int)NUM_STYLES);
		}
	}
	if (v->fade < 1.0f) {
		v->fade += seconds / (v->fade_secs > 0.01f ? v->fade_secs : 0.01f);
		if (v->fade > 1.0f) {
			v->fade = 1.0f;
		}
	}

	/* not in any scene being shown: skip the analysis and animation */
	if (!obs_source_showing(v->source)) {
		return;
	}
	player_tick(v, seconds);

	pthread_mutex_lock(&v->mutex);
	uint64_t now = os_gettime_ns();
	if (now - v->last_audio_ns > SILENCE_NS) {
		/* paused or stopped: let everything settle down */
		spazz_analyzer_push_silence(v->analyzer, (size_t)(seconds * v->analyzer->sample_rate));
	}
	spazz_analyzer_update(v->analyzer, seconds, v->sensitivity);

	/* bass and treble boost tilt the spectrum; smoothing calms it down */
	const float keep = v->smoothing > 0.0f ? powf(v->smoothing * 0.96f, seconds * 30.0f) : 0.0f;
	struct spazz_analyzer *a = v->analyzer;
	for (int i = 0; i < SPAZZ_NUM_BANDS; i++) {
		const float x = (float)i / (float)(SPAZZ_NUM_BANDS - 1);
		const float lo = 1.0f - 2.0f * x > 0.0f ? 1.0f - 2.0f * x : 0.0f;
		const float hi = 2.0f * x - 1.0f > 0.0f ? 2.0f * x - 1.0f : 0.0f;
		const float gain = 1.0f + (v->bass_boost - 1.0f) * lo + (v->treble_boost - 1.0f) * hi;
		const float band = fminf(a->bands[i] * gain, 1.0f);
		const float peak = fminf(a->peaks[i] * gain, 1.0f);
		v->bands[i] = band + (v->bands[i] - band) * keep;
		v->peaks[i] = fmaxf(peak, v->peaks[i] * keep);
		v->tex_data[i * 4 + 0] = (uint8_t)(v->bands[i] * 255.0f + 0.5f);
		v->tex_data[i * 4 + 1] = (uint8_t)(v->peaks[i] * 255.0f + 0.5f);
		v->tex_data[i * 4 + 2] = (uint8_t)(fminf(a->flash[i] * v->beat_punch, 1.0f) * 255.0f + 0.5f);
		v->tex_data[i * 4 + 3] = 255;
	}
	const float bass = fminf(a->bass * v->bass_boost, 1.5f);
	const float treble = fminf(a->treble * v->treble_boost, 1.5f);
	v->bass = bass + (v->bass - bass) * keep;
	v->mid = a->mid + (v->mid - a->mid) * keep;
	v->treble = treble + (v->treble - treble) * keep;
	v->level = a->level + (v->level - a->level) * keep;
	v->beat = fminf(a->beat * v->beat_punch, 1.5f);
	const float level = a->level;
	const float beat = a->beat;
	pthread_mutex_unlock(&v->mutex);

	v->tex_dirty = true;
	const float dt = seconds * v->speed;
	v->time += dt;
	v->flow += dt * (0.3f + 1.1f * level + 0.5f * beat);
	v->hue += seconds * v->color_speed * 0.08f;

	/* keep floats small so the shader stays precise on long streams */
	if (v->time > 3600.0f) {
		v->time -= 3600.0f;
	}
	/* drop speeds are multiples of 1/16 in the shader, so wrapping at 64
	 * lands every drop exactly where it was: no jump, and full precision */
	if (v->flow >= 64.0f) {
		v->flow -= 64.0f;
	}
	v->hue -= floorf(v->hue);
}

static void draw_style(struct spazz_viz *v, int idx, float alpha)
{
	struct vec4 grade;
	vec4_set(&grade, v->brightness, v->saturation, alpha, 0.0f);
	gs_effect_set_vec4(v->p_grade, &grade);

	/* the player's accent: fixed, cycling like the SPAZZamp dock (1) or
	 * following the palette (2) */
	const int skin = player_skin(idx);
	struct vec3 acc;
	float mode = 0.0f;
	vec3_set(&acc, 1.0f, 1.0f, 1.0f);
	if (v->player_rgb) {
		mode = 1.0f;
	} else if (v->player_color == 2) {
		mode = 2.0f;
	} else if (v->player_color == 1) {
		acc = v->player_custom;
	} else if (skin >= 0 && skin_accents[skin]) {
		color_from_hex(&acc, skin_accents[skin]);
	} else {
		mode = 1.0f;
	}
	struct vec4 look;
	vec4_set(&look, (float)v->player_viz, (float)v->player_strip, v->skin_hue, mode);
	gs_effect_set_vec4(v->p_pl_look, &look);
	gs_effect_set_vec3(v->p_pl_accent, &acc);

	const char *tech = style_techniques[idx];
	while (gs_effect_loop(v->effect, tech)) {
		gs_draw_sprite(NULL, 0, v->width, v->height);
	}
}

static void viz_render(void *data, gs_effect_t *unused)
{
	struct spazz_viz *v = data;
	UNUSED_PARAMETER(unused);

	if (!v->effect || !v->band_tex) {
		return;
	}

	if (v->tex_dirty) {
		gs_texture_set_image(v->band_tex, v->tex_data, SPAZZ_NUM_BANDS * 4, false);
		v->tex_dirty = false;
	}

	struct vec2 res;
	vec2_set(&res, (float)v->width, (float)v->height);
	struct vec4 view;
	vec4_set(&view, v->zoom, v->mirror_x, v->mirror_y, v->flip_y);

	gs_effect_set_texture(v->p_bands, v->band_tex);
	gs_effect_set_vec2(v->p_res, &res);
	gs_effect_set_float(v->p_time, v->time);
	gs_effect_set_float(v->p_flow, v->flow);
	gs_effect_set_float(v->p_bass, v->bass);
	gs_effect_set_float(v->p_mid, v->mid);
	gs_effect_set_float(v->p_treble, v->treble);
	gs_effect_set_float(v->p_level, v->level);
	gs_effect_set_float(v->p_beat, v->beat);
	gs_effect_set_float(v->p_hue, v->hue + v->hue_shift);
	gs_effect_set_float(v->p_density, v->density);
	gs_effect_set_float(v->p_bg, v->bg_opacity);
	gs_effect_set_vec3(v->p_pal_a, &v->pal_a);
	gs_effect_set_vec3(v->p_pal_b, &v->pal_b);
	gs_effect_set_vec3(v->p_pal_c, &v->pal_c);
	gs_effect_set_float(v->p_pal_mix, v->pal_mix);
	gs_effect_set_vec3(v->p_bg_col, &v->bg_col);
	gs_effect_set_float(v->p_bg_flat, v->bg_flat);
	gs_effect_set_vec4(v->p_view, &view);
	gs_effect_set_float(v->p_flip_x, v->flip_x);

	/* the SPAZZamp player (every draw sets these, used or not) */
	if (v->text_dirty && v->text_tex) {
		gs_texture_set_image(v->text_tex, v->text_data, TEXT_TEX_W, false);
		v->text_dirty = false;
	}
	gs_effect_set_texture(v->p_font_tex, v->font_tex);
	gs_effect_set_texture(v->p_text_tex, v->text_tex);
	const int st = v->now_state;
	const bool playing = st == OBS_MEDIA_STATE_PLAYING;
	const bool paused = st == OBS_MEDIA_STATE_PAUSED;
	const bool going = playing || paused;
	const float progress = going && v->now_dur_ms > 0.0 ? (float)(v->now_pos_ms / v->now_dur_ms) : 0.0f;
	struct vec4 pstate;
	vec4_set(&pstate, fminf(fmaxf(progress, 0.0f), 1.0f),
		 playing        ? 1.0f
		 : paused       ? 2.0f
		 : v->now_title ? 3.0f
				: 0.0f,
		 v->marquee_pos, v->marquee_len > MARQUEE_CELLS ? (float)v->marquee_len : 0.0f);
	gs_effect_set_vec4(v->p_pl_state, &pstate);
	const bool minus = v->player_remaining && going && v->now_dur_ms > 0.0;
	double ms = going ? (minus ? v->now_dur_ms - v->now_pos_ms : v->now_pos_ms) : 0.0;
	if (ms < 0.0) {
		ms = 0.0;
	}
	const long long secs = (long long)(ms / 1000.0);
	const long long mins = (secs / 60) % 100;
	struct vec4 digits;
	vec4_set(&digits, (float)(mins / 10), (float)(mins % 10), (float)((secs % 60) / 10), (float)(secs % 10));
	gs_effect_set_vec4(v->p_pl_digits, &digits);
	struct vec4 info;
	vec4_set(&info, (float)v->now_volume, v->now_shuffle ? 1.0f : 0.0f, (float)v->now_repeat, minus ? 1.0f : 0.0f);
	gs_effect_set_vec4(v->p_pl_info, &info);
	struct vec4 show;
	vec4_set(&show, v->show_title ? 1.0f : 0.0f, v->show_seek ? 1.0f : 0.0f, v->show_buttons ? 1.0f : 0.0f,
		 v->stereo ? 1.0f : 0.0f);
	gs_effect_set_vec4(v->p_pl_show, &show);
	/* pl_look and pl_accent depend on the skin: draw_style sets them */

	/* crossfading: the old style fades out under the new one */
	if (v->fade < 1.0f && v->prev_shown != v->shown) {
		draw_style(v, v->prev_shown, 1.0f - v->fade);
		draw_style(v, v->shown, v->fade);
	} else {
		draw_style(v, v->shown, 1.0f);
	}
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
