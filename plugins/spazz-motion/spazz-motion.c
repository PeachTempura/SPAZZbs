/******************************************************************************
    SPAZZbs - "Movement": a filter that makes any source float, bob, swing,
    bounce, glitch, glow like neon, play on an old CRT ... and animate in and
    out, with the same motions SPONSORCENTER uses.
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
 * Three layers, each picked from a list, so presets are just combinations:
 *   Motion   moves the whole picture (float, lily pad, swing, bounce ...)
 *   Distort  bends it (waves, glitch, VHS, CRT, pixels ...)
 *   Look     restyles its colours (neon glow, hologram, synthwave ...)
 * plus an enter animation when the source is shown and an exit you can play
 * (button, hotkey, or an automatic on / off loop).
 *
 * The output is the source with "room to move" around it, so it can float
 * and glow past its own edges. One pass: the C side works out where each
 * output pixel comes from (spazz-motion-anim.h), the shader does the rest.
 */

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>
#include <util/platform.h>
#include <util/threading.h>
#include <string.h>

#include "spazz-motion-anim.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("spazz-motion", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "SPAZZbs Movement filter";
}

#define T_(s) obs_module_text(s)

/* ------------------------------------------------------------------------ */
/* presets                                                                   */

struct mo_preset {
	const char *name;
	const char *motion;
	double m_speed, m_amount;
	const char *distort;
	double d_amount, d_size, d_speed;
	const char *look;
	double l_amount;
	uint32_t col_a, col_b; /* 0xRRGGBB */
	double glow, cycle;
	const char *enter, *exit;
};

/* clang-format off */
static const struct mo_preset mo_presets[] = {
	/* name                         motion       spd  amt   distort     amt  size  spd   look           amt  colour A  colour B  glow cyc  enter      exit */
	{"Float · Lilypad",            "lilypad",   1.0, 1.0,  "undulate", 0.35, 1.0, 1.0,  "shadow",      0.5, 0x000000, 0x000000, 14, 0.0,  "rise",    "fade"},
	{"Float · Balloon",            "float",     1.0, 1.3,  "none",     0.0,  1.0, 1.0,  "shadow",      0.6, 0x000000, 0x000000, 18, 0.0,  "rise",    "slide_u"},
	{"Float · Hover",              "hover",     1.0, 1.0,  "none",     0.0,  1.0, 1.0,  "shadow",      0.7, 0x000000, 0x000000, 16, 0.0,  "pop",     "fade"},
	{"Float · Drift",              "drift",     1.0, 1.0,  "none",     0.0,  1.0, 1.0,  "none",        1.0, 0x000000, 0x000000, 12, 0.0,  "fade",    "fade"},
	{"Float · Lazy River",         "sway",      0.8, 0.8,  "flag",     0.5,  1.2, 0.6,  "none",        1.0, 0x000000, 0x000000, 12, 0.0,  "slide_l", "slide_r"},
	{"Float · Underwater",         "float",     0.7, 0.9,  "warp",     0.7,  1.0, 0.8,  "tint",        0.35, 0x3fc8ff, 0x002244, 12, 0.0, "rise",    "fade"},
	{"Float · Dreamy",             "breathe",   1.0, 1.0,  "swirl",    0.25, 1.0, 0.5,  "bloom",       0.8, 0xffffff, 0x000000, 10, 0.0,  "zoom",    "fade"},
	{"Bob · Gentle",               "bob",       0.8, 0.6,  "none",     0.0,  1.0, 1.0,  "none",        1.0, 0x000000, 0x000000, 12, 0.0,  "fade",    "fade"},
	{"Bob · Happy",                "bob",       1.4, 1.2,  "none",     0.0,  1.0, 1.0,  "shadow",      0.5, 0x000000, 0x000000, 12, 0.0,  "pop",     "pop"},
	{"Bounce · Ball",              "bounce",    1.0, 1.0,  "none",     0.0,  1.0, 1.0,  "shadow",      0.5, 0x000000, 0x000000, 12, 0.0,  "drop",    "slide_d"},
	{"Bounce · Jelly",             "jelly",     1.0, 1.0,  "none",     0.0,  1.0, 1.0,  "none",        1.0, 0x000000, 0x000000, 12, 0.0,  "pop",     "pop"},
	{"Bounce · Rubber Band",       "rubber",    1.0, 1.0,  "none",     0.0,  1.0, 1.0,  "none",        1.0, 0x000000, 0x000000, 12, 0.0,  "pop",     "fade"},
	{"Bounce · Jump",              "jump",      1.0, 1.0,  "none",     0.0,  1.0, 1.0,  "shadow",      0.5, 0x000000, 0x000000, 12, 0.0,  "drop",    "slide_u"},
	{"Swing · Pendulum",           "swing",     1.0, 1.0,  "none",     0.0,  1.0, 1.0,  "shadow",      0.5, 0x000000, 0x000000, 14, 0.0,  "drop",    "slide_u"},
	{"Swing · Hanging Sign",       "swing",     0.6, 0.6,  "none",     0.0,  1.0, 1.0,  "shadow",      0.8, 0x000000, 0x000000, 18, 0.0,  "drop",    "slide_u"},
	{"Swing · Sway",               "sway",      1.0, 1.0,  "none",     0.0,  1.0, 1.0,  "none",        1.0, 0x000000, 0x000000, 12, 0.0,  "rise",    "fade"},
	{"Swing · Tilt",               "tilt",      1.0, 1.0,  "none",     0.0,  1.0, 1.0,  "none",        1.0, 0x000000, 0x000000, 12, 0.0,  "flip",    "flip"},
	{"Spin · Coin Flip",           "coin",      1.0, 1.0,  "none",     0.0,  1.0, 1.0,  "shine",       0.8, 0x000000, 0x000000, 12, 0.0,  "flip",    "flip"},
	{"Spin · Record",              "spin",      1.0, 1.0,  "none",     0.0,  1.0, 1.0,  "none",        1.0, 0x000000, 0x000000, 12, 0.0,  "spin",    "spin"},
	{"Spin · Orbit",               "orbit",     1.0, 1.0,  "none",     0.0,  1.0, 1.0,  "none",        1.0, 0x000000, 0x000000, 12, 0.0,  "zoom",    "zoom"},
	{"Spin · Figure 8",            "figure8",   1.0, 1.0,  "none",     0.0,  1.0, 1.0,  "none",        1.0, 0x000000, 0x000000, 12, 0.0,  "fade",    "fade"},
	{"Pulse · Heartbeat",          "heartbeat", 1.0, 1.0,  "none",     0.0,  1.0, 1.0,  "neon",        0.8, 0xff2050, 0x000000, 16, 0.0,  "pop",     "fade"},
	{"Pulse · Breathe",            "breathe",   1.0, 1.0,  "none",     0.0,  1.0, 1.0,  "bloom",       0.6, 0xffffff, 0x000000, 10, 0.0,  "fade",    "fade"},
	{"Pulse · Pop",                "pulse",     1.0, 1.0,  "none",     0.0,  1.0, 1.0,  "none",        1.0, 0x000000, 0x000000, 12, 0.0,  "pop",     "pop"},
	{"Wobble · Wiggle",            "wobble",    1.0, 1.0,  "none",     0.0,  1.0, 1.0,  "none",        1.0, 0x000000, 0x000000, 12, 0.0,  "pop",     "pop"},
	{"Wobble · Tada",              "tada",      1.0, 1.0,  "none",     0.0,  1.0, 1.0,  "shine",       0.7, 0x000000, 0x000000, 12, 0.0,  "pop",     "zoom"},
	{"Shake · Nervous",            "shake",     1.0, 0.6,  "none",     0.0,  1.0, 1.0,  "none",        1.0, 0x000000, 0x000000, 12, 0.0,  "fade",    "fade"},
	{"Shake · Earthquake",         "quake",     1.0, 1.0,  "chroma",   0.6,  1.0, 1.0,  "none",        1.0, 0x000000, 0x000000, 12, 0.0,  "glitch",  "glitch"},
	{"Neon · Pink Glow",           "breathe",   1.0, 0.6,  "none",     0.0,  1.0, 1.0,  "neon",        1.0, 0xff2bd6, 0x000000, 18, 0.0,  "flicker", "fade"},
	{"Neon · Cyan Glow",           "breathe",   1.0, 0.6,  "none",     0.0,  1.0, 1.0,  "neon",        1.0, 0x18e8ff, 0x000000, 18, 0.0,  "flicker", "fade"},
	{"Neon · Green Sign",          "none",      1.0, 1.0,  "none",     0.0,  1.0, 1.0,  "neonflicker", 1.0, 0x39ff5a, 0x000000, 18, 0.0,  "flicker", "flicker"},
	{"Neon · Purple Haze",         "float",     0.8, 0.8,  "haze",     0.4,  1.0, 0.8,  "neon",        1.0, 0x9b4dff, 0x000000, 24, 0.0,  "fade",    "fade"},
	{"Neon · RGB Cycle",           "float",     1.0, 0.6,  "none",     0.0,  1.0, 1.0,  "neon",        1.0, 0xff0040, 0x000000, 18, 0.15, "flicker", "fade"},
	{"Neon · Outline",             "none",      1.0, 1.0,  "none",     0.0,  1.0, 1.0,  "outline",     1.0, 0xff2bd6, 0x000000, 6,  0.0,  "pop",     "pop"},
	{"Neon · Rainbow Edge",        "float",     1.0, 0.5,  "none",     0.0,  1.0, 1.0,  "rainbow",     1.0, 0xffffff, 0x000000, 7,  0.0,  "pop",     "fade"},
	{"Neon · Flicker Sign",        "none",      1.0, 1.0,  "none",     0.0,  1.0, 1.0,  "neonflicker", 1.0, 0xff3a2a, 0x000000, 20, 0.0,  "flicker", "flicker"},
	{"Neon · Electric Blue",       "hover",     1.0, 0.7,  "chroma",   0.25, 1.0, 1.0,  "neon",        1.2, 0x3a6bff, 0x000000, 22, 0.0,  "glitch",  "fade"},
	{"Retro · Synthwave",          "float",     0.8, 0.7,  "none",     0.0,  1.0, 1.0,  "synthwave",   1.0, 0xff2bd6, 0x000000, 16, 0.0,  "rise",    "fade"},
	{"Retro · Chrome 80s",         "tilt",      0.8, 0.6,  "none",     0.0,  1.0, 1.0,  "chrome",      0.8, 0x000000, 0x000000, 12, 0.0,  "zoom",    "zoom"},
	{"Retro · Pop Art",            "pulse",     1.0, 0.6,  "none",     0.0,  1.0, 1.0,  "popart",      1.0, 0x000000, 0x000000, 12, 0.0,  "pop",     "pop"},
	{"Retro · Old Film",           "shake",     1.0, 0.15, "none",     0.0,  1.0, 1.0,  "film",        1.0, 0x000000, 0x000000, 12, 0.0,  "flicker", "fade"},
	{"Retro · Noir",               "none",      1.0, 1.0,  "none",     0.0,  1.0, 1.0,  "noir",        1.0, 0x000000, 0x000000, 12, 0.0,  "fade",    "fade"},
	{"Retro · Sepia Sway",         "sway",      0.7, 0.7,  "none",     0.0,  1.0, 1.0,  "film",        0.8, 0x000000, 0x000000, 12, 0.0,  "fade",    "fade"},
	{"Retro · Duotone",            "breathe",   1.0, 0.5,  "none",     0.0,  1.0, 1.0,  "duotone",     1.0, 0xffd23a, 0x5a1a8a, 12, 0.0,  "wipe",    "wipe"},
	{"Arcade · 8-bit",             "jump",      1.0, 0.7,  "pixel",    1.0,  1.0, 1.0,  "none",        1.0, 0x000000, 0x000000, 12, 0.0,  "pixel",   "pixel"},
	{"Arcade · Game Boy",          "bob",       1.0, 0.5,  "pixel",    0.8,  1.0, 1.0,  "gameboy",     1.0, 0x000000, 0x000000, 12, 0.0,  "pixel",   "pixel"},
	{"Arcade · Pixel Bounce",      "bounce",    1.0, 0.8,  "pixel",    0.7,  1.0, 1.0,  "scanlines",   0.6, 0x000000, 0x000000, 12, 0.0,  "pixel",   "pixel"},
	{"Glitch · Light",             "none",      1.0, 1.0,  "glitch",   0.4,  1.0, 1.0,  "none",        1.0, 0x000000, 0x000000, 12, 0.0,  "glitch",  "glitch"},
	{"Glitch · Heavy",             "shake",     1.0, 0.5,  "glitch",   1.2,  1.0, 1.0,  "none",        1.0, 0x000000, 0x000000, 12, 0.0,  "glitch",  "glitch"},
	{"Glitch · Datamosh",          "none",      1.0, 1.0,  "datamosh", 1.0,  1.0, 1.0,  "none",        1.0, 0x000000, 0x000000, 12, 0.0,  "glitch",  "glitch"},
	{"Glitch · RGB Split",         "shake",     1.0, 0.25, "chroma",   1.0,  1.0, 1.0,  "none",        1.0, 0x000000, 0x000000, 12, 0.0,  "glitch",  "glitch"},
	{"Glitch · Corrupted",         "none",      1.0, 1.0,  "datamosh", 0.7,  1.0, 1.0,  "rgb",         0.7, 0x000000, 0x000000, 12, 0.3,  "glitch",  "pixel"},
	{"Glitch · Neon Error",        "shake",     1.0, 0.2,  "glitch",   0.7,  1.0, 1.0,  "neon",        1.0, 0x18e8ff, 0x000000, 16, 0.0,  "glitch",  "glitch"},
	{"CRT · Old TV",               "none",      1.0, 1.0,  "crt",      1.0,  1.0, 1.0,  "none",        1.0, 0x000000, 0x000000, 12, 0.0,  "crt",     "crt"},
	{"CRT · Arcade Cabinet",       "none",      1.0, 1.0,  "crt",      0.8,  1.0, 1.0,  "neon",        0.6, 0xff2bd6, 0x000000, 16, 0.0,  "crt",     "crt"},
	{"CRT · Green Terminal",       "none",      1.0, 1.0,  "crt",      1.0,  1.0, 1.0,  "duotone",     1.0, 0x5aff7a, 0x001a06, 12, 0.0,  "crt",     "crt"},
	{"CRT · Amber Monitor",        "none",      1.0, 1.0,  "crt",      1.0,  1.0, 1.0,  "duotone",     1.0, 0xffb02a, 0x1a0a00, 12, 0.0,  "crt",     "crt"},
	{"CRT · Floating TV",          "lilypad",   1.0, 0.8,  "crt",      0.9,  1.0, 1.0,  "shadow",      0.6, 0x000000, 0x000000, 16, 0.0,  "crt",     "crt"},
	{"VHS · Tracking",             "none",      1.0, 1.0,  "vhs",      1.0,  1.0, 1.0,  "none",        1.0, 0x000000, 0x000000, 12, 0.0,  "vhs",     "vhs"},
	{"VHS · Home Video",           "shake",     1.0, 0.12, "vhs",      0.5,  1.0, 1.0,  "scanlines",   0.5, 0x000000, 0x000000, 12, 0.0,  "vhs",     "vhs"},
	{"VHS · Rewind",               "none",      1.0, 1.0,  "vhs",      1.4,  1.0, 3.0,  "none",        1.0, 0x000000, 0x000000, 12, 0.0,  "vhs",     "vhs"},
	{"Hologram · Cyan",            "hover",     1.0, 0.7,  "chroma",   0.2,  1.0, 1.0,  "hologram",    1.0, 0x30e0ff, 0x000000, 12, 0.0,  "flicker", "flicker"},
	{"Hologram · Princess",        "float",     0.6, 0.5,  "glitch",   0.25, 1.0, 1.0,  "hologram",    1.0, 0x5aa8ff, 0x000000, 12, 0.0,  "crt",     "flicker"},
	{"Hologram · Matrix Green",    "hover",     1.0, 0.6,  "none",     0.0,  1.0, 1.0,  "hologram",    1.0, 0x39ff5a, 0x000000, 12, 0.0,  "pixel",   "pixel"},
	{"Thermal · Predator",         "none",      1.0, 1.0,  "haze",     0.6,  1.0, 1.0,  "thermal",     1.0, 0x000000, 0x000000, 12, 0.0,  "glitch",  "fade"},
	{"Water · Ripple",             "float",     0.8, 0.6,  "ripple",   0.8,  1.0, 1.0,  "none",        1.0, 0x000000, 0x000000, 12, 0.0,  "rise",    "fade"},
	{"Water · Deep Wave",          "lilypad",   1.0, 1.0,  "undulate", 0.9,  0.8, 1.0,  "tint",        0.3, 0x3fa0ff, 0x000000, 12, 0.0,  "rise",    "fade"},
	{"Dream · Swirl",              "breathe",   1.0, 1.0,  "swirl",    0.45,  1.0, 1.0,  "bloom",       0.6, 0xffffff, 0x000000, 10, 0.0,  "spin",    "spin"},
	{"Dream · Heat Haze",          "float",     0.6, 0.5,  "haze",     0.8,  1.0, 1.0,  "bloom",       0.5, 0xffffff, 0x000000, 10, 0.0,  "fade",    "fade"},
	{"Dream · Fisheye",            "breathe",   1.0, 1.0,  "bulge",    0.7,  1.0, 1.0,  "none",        1.0, 0x000000, 0x000000, 12, 0.0,  "zoom",    "zoom"},
	{"Shine · Sparkle Card",       "tilt",      1.0, 0.7,  "none",     0.0,  1.0, 1.0,  "shine",       1.0, 0x000000, 0x000000, 12, 0.0,  "pop",     "pop"},
	{"Shadow · Drop Shadow",       "float",     0.8, 0.6,  "none",     0.0,  1.0, 1.0,  "shadow",      0.9, 0x000000, 0x000000, 20, 0.0,  "rise",    "fade"},
	{"Party · Disco",              "bob",       1.6, 0.8,  "none",     0.0,  1.0, 1.0,  "rgb",         1.0, 0x000000, 0x000000, 12, 0.4,  "pop",     "spin"},
	{"Party · Rave",               "pulse",     2.0, 0.8,  "chroma",   0.5,  1.0, 1.0,  "neon",        1.2, 0xff00aa, 0x000000, 20, 0.6,  "glitch",  "glitch"},
	{"Clean · Fade In & Out",      "none",      1.0, 1.0,  "none",     0.0,  1.0, 1.0,  "none",        1.0, 0x000000, 0x000000, 12, 0.0,  "fade",    "fade"},
	{"Clean · Pop In",             "none",      1.0, 1.0,  "none",     0.0,  1.0, 1.0,  "none",        1.0, 0x000000, 0x000000, 12, 0.0,  "pop",     "pop"},
	{"Clean · Slide In",           "none",      1.0, 1.0,  "none",     0.0,  1.0, 1.0,  "none",        1.0, 0x000000, 0x000000, 12, 0.0,  "slide_l", "slide_r"},
};
/* clang-format on */
#define MO_PRESETS (sizeof(mo_presets) / sizeof(mo_presets[0]))
#define MO_DEFAULT_PRESET 0

/* ------------------------------------------------------------------------ */

enum phase { PH_SHOWN, PH_ENTER, PH_EXIT, PH_GONE };

struct spazz_motion {
	obs_source_t *source;
	gs_effect_t *effect;
	gs_eparam_t *p_out_size, *p_src_size, *p_xf, *p_xo, *p_time, *p_opacity, *p_dist, *p_look, *p_col_a, *p_col_b,
		*p_fx, *p_fx2;

	/* settings */
	int motion, distort, look, enter, exit, pivot;
	double m_speed, m_amount, m_phase;
	double d_amount, d_size, d_speed;
	double l_amount, glow, cycle;
	struct vec3 col_a, col_b;
	double anim_sec;
	bool enter_on_live; /* play the enter when it goes live, not when shown */
	bool loop;
	double stay_sec, gap_sec;
	int room_pct;

	/* clocks */
	double clock;  /* raw seconds */
	double mclock; /* motion, scaled by its speed */
	double dclock; /* distort, scaled by its speed */
	double seed;

	/* enter / exit */
	enum phase phase;
	double anim_t; /* seconds into the enter / exit */
	double hold_t; /* seconds shown / gone (loop) */
	volatile bool req_enter;
	volatile bool req_exit;

	obs_hotkey_id hk_enter, hk_exit;
};

static const char *mo_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return T_("Movement");
}

static struct vec3 color_from_int(long long v)
{
	struct vec3 c;
	const uint32_t u = (uint32_t)v; /* OBS stores 0xAABBGGRR */
	vec3_set(&c, (float)(u & 0xff) / 255.0f, (float)((u >> 8) & 0xff) / 255.0f, (float)((u >> 16) & 0xff) / 255.0f);
	return c;
}

static long long rgb_to_obs(uint32_t rgb)
{
	const uint32_t r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
	return (long long)(0xff000000u | (b << 16) | (g << 8) | r);
}

static int list_index(obs_data_t *s, const char *key, const struct mo_item *list, size_t count)
{
	return mo_index(list, (int)count, obs_data_get_string(s, key));
}

static void apply_preset(obs_data_t *s, const struct mo_preset *p)
{
	obs_data_set_string(s, "motion", p->motion);
	obs_data_set_double(s, "m_speed", p->m_speed);
	obs_data_set_double(s, "m_amount", p->m_amount);
	obs_data_set_string(s, "distort", p->distort);
	obs_data_set_double(s, "d_amount", p->d_amount);
	obs_data_set_double(s, "d_size", p->d_size);
	obs_data_set_double(s, "d_speed", p->d_speed);
	obs_data_set_string(s, "look", p->look);
	obs_data_set_double(s, "l_amount", p->l_amount);
	obs_data_set_int(s, "col_a", rgb_to_obs(p->col_a));
	obs_data_set_int(s, "col_b", rgb_to_obs(p->col_b));
	obs_data_set_double(s, "glow", p->glow);
	obs_data_set_double(s, "cycle", p->cycle);
	obs_data_set_string(s, "enter", p->enter);
	obs_data_set_string(s, "exit", p->exit);
	obs_data_set_string(s, "pivot", "auto");
	obs_data_set_string(s, "preset_applied", p->name);
}

static const struct mo_preset *find_preset(const char *name)
{
	for (size_t i = 0; i < MO_PRESETS; i++) {
		if (name && strcmp(mo_presets[i].name, name) == 0) {
			return &mo_presets[i];
		}
	}
	return NULL;
}

static void mo_update(void *data, obs_data_t *s)
{
	struct spazz_motion *mo = data;

	mo->motion = list_index(s, "motion", mo_motions, MO_MOTIONS);
	mo->distort = list_index(s, "distort", mo_distorts, MO_DISTORTS);
	mo->look = list_index(s, "look", mo_looks, MO_LOOKS);
	mo->enter = list_index(s, "enter", mo_enters, MO_ENTERS);
	mo->exit = list_index(s, "exit", mo_enters, MO_ENTERS);
	mo->pivot = list_index(s, "pivot", mo_pivots, MO_PIVOTS);

	mo->m_speed = obs_data_get_double(s, "m_speed");
	mo->m_amount = obs_data_get_double(s, "m_amount");
	mo->m_phase = obs_data_get_double(s, "m_phase");
	mo->d_amount = obs_data_get_double(s, "d_amount");
	mo->d_size = obs_data_get_double(s, "d_size");
	mo->d_speed = obs_data_get_double(s, "d_speed");
	mo->l_amount = obs_data_get_double(s, "l_amount");
	mo->glow = obs_data_get_double(s, "glow");
	mo->cycle = obs_data_get_double(s, "cycle");
	mo->col_a = color_from_int(obs_data_get_int(s, "col_a"));
	mo->col_b = color_from_int(obs_data_get_int(s, "col_b"));

	mo->anim_sec = (double)obs_data_get_int(s, "anim_ms") / 1000.0;
	if (mo->anim_sec < 0.05) {
		mo->anim_sec = 0.05;
	}
	mo->enter_on_live = strcmp(obs_data_get_string(s, "enter_when"), "live") == 0;
	mo->loop = obs_data_get_bool(s, "loop");
	mo->stay_sec = obs_data_get_double(s, "stay_sec");
	mo->gap_sec = obs_data_get_double(s, "gap_sec");
	mo->room_pct = (int)obs_data_get_int(s, "room");

	/* switched the loop off while it was away: bring it back */
	if (!mo->loop && mo->phase == PH_GONE) {
		mo->phase = PH_SHOWN;
	}
}

static void mo_defaults(obs_data_t *s)
{
	const struct mo_preset *p = &mo_presets[MO_DEFAULT_PRESET];
	obs_data_set_default_string(s, "preset", p->name);
	obs_data_set_default_string(s, "motion", p->motion);
	obs_data_set_default_double(s, "m_speed", p->m_speed);
	obs_data_set_default_double(s, "m_amount", p->m_amount);
	obs_data_set_default_double(s, "m_phase", 0.0);
	obs_data_set_default_string(s, "pivot", "auto");
	obs_data_set_default_string(s, "distort", p->distort);
	obs_data_set_default_double(s, "d_amount", p->d_amount);
	obs_data_set_default_double(s, "d_size", p->d_size);
	obs_data_set_default_double(s, "d_speed", p->d_speed);
	obs_data_set_default_string(s, "look", p->look);
	obs_data_set_default_double(s, "l_amount", p->l_amount);
	obs_data_set_default_int(s, "col_a", rgb_to_obs(p->col_a));
	obs_data_set_default_int(s, "col_b", rgb_to_obs(p->col_b));
	obs_data_set_default_double(s, "glow", p->glow);
	obs_data_set_default_double(s, "cycle", p->cycle);
	obs_data_set_default_string(s, "enter", p->enter);
	obs_data_set_default_string(s, "exit", p->exit);
	obs_data_set_default_int(s, "anim_ms", 700);
	obs_data_set_default_string(s, "enter_when", "show");
	obs_data_set_default_bool(s, "loop", false);
	obs_data_set_default_double(s, "stay_sec", 20.0);
	obs_data_set_default_double(s, "gap_sec", 10.0);
	obs_data_set_default_int(s, "room", 12);
}

/* ------------------------------------------------------------------------ */
/* playing the enter / exit                                                  */

static void play_enter(struct spazz_motion *mo)
{
	os_atomic_set_bool(&mo->req_enter, true);
}

static void play_exit(struct spazz_motion *mo)
{
	os_atomic_set_bool(&mo->req_exit, true);
}

static void mo_show(void *data)
{
	struct spazz_motion *mo = data;
	if (!mo->enter_on_live) {
		play_enter(mo);
	}
}

static void mo_activate(void *data)
{
	struct spazz_motion *mo = data;
	if (mo->enter_on_live) {
		play_enter(mo);
	}
}

static void hotkey_enter(void *data, obs_hotkey_id id, obs_hotkey_t *hk, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hk);
	if (pressed) {
		play_enter(data);
	}
}

static void hotkey_exit(void *data, obs_hotkey_id id, obs_hotkey_t *hk, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hk);
	if (pressed) {
		play_exit(data);
	}
}

static void proc_enter(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	play_enter(data);
}

static void proc_exit(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	play_exit(data);
}

static void mo_tick(void *data, float seconds)
{
	struct spazz_motion *mo = data;
	const double dt = seconds;

	mo->clock += dt;
	mo->mclock += dt * mo->m_speed;
	mo->dclock += dt * mo->d_speed;
	/* keep the clocks small enough for float precision in the shader */
	if (mo->clock > 36000.0) {
		mo->clock -= 36000.0;
	}
	if (mo->dclock > 36000.0) {
		mo->dclock -= 36000.0;
	}

	if (os_atomic_set_bool(&mo->req_enter, false)) {
		mo->phase = mo->enter ? PH_ENTER : PH_SHOWN;
		mo->anim_t = 0.0;
		mo->hold_t = 0.0;
		mo->seed = mo->clock;
	}
	if (os_atomic_set_bool(&mo->req_exit, false) && mo->phase != PH_GONE) {
		mo->phase = mo->exit ? PH_EXIT : PH_GONE;
		mo->anim_t = 0.0;
		mo->hold_t = 0.0;
		mo->seed = mo->clock;
	}

	switch (mo->phase) {
	case PH_ENTER:
		mo->anim_t += dt;
		if (mo->anim_t >= mo->anim_sec) {
			mo->phase = PH_SHOWN;
			mo->hold_t = 0.0;
		}
		break;
	case PH_EXIT:
		mo->anim_t += dt;
		if (mo->anim_t >= mo->anim_sec) {
			mo->phase = PH_GONE;
			mo->hold_t = 0.0;
		}
		break;
	case PH_SHOWN:
		mo->hold_t += dt;
		if (mo->loop && mo->hold_t >= mo->stay_sec) {
			mo->phase = mo->exit ? PH_EXIT : PH_GONE;
			mo->anim_t = 0.0;
			mo->hold_t = 0.0;
		}
		break;
	case PH_GONE:
		mo->hold_t += dt;
		if (mo->loop && mo->hold_t >= mo->gap_sec) {
			mo->phase = mo->enter ? PH_ENTER : PH_SHOWN;
			mo->anim_t = 0.0;
			mo->hold_t = 0.0;
		}
		break;
	}
}

/* ------------------------------------------------------------------------ */
/* size and drawing                                                          */

static void source_size(struct spazz_motion *mo, uint32_t *w, uint32_t *h)
{
	obs_source_t *target = obs_filter_get_target(mo->source);
	*w = target ? obs_source_get_base_width(target) : 0;
	*h = target ? obs_source_get_base_height(target) : 0;
}

static uint32_t room_px(const struct spazz_motion *mo, uint32_t w, uint32_t h)
{
	const uint32_t m = w < h ? w : h;
	return (uint32_t)((double)m * (double)mo->room_pct / 100.0 + 0.5);
}

static uint32_t mo_width(void *data)
{
	struct spazz_motion *mo = data;
	uint32_t w, h;
	source_size(mo, &w, &h);
	return w ? w + 2 * room_px(mo, w, h) : 0;
}

static uint32_t mo_height(void *data)
{
	struct spazz_motion *mo = data;
	uint32_t w, h;
	source_size(mo, &w, &h);
	return h ? h + 2 * room_px(mo, w, h) : 0;
}

static void hue_rotate(struct vec3 *c, double a)
{
	const double cs = cos(a), sn = sin(a);
	const double k = 0.57735;
	const double r = c->x, g = c->y, b = c->z;
	const double d = k * (r + g + b) * k * (1.0 - cs);
	/* c*cos + cross(k, c)*sin + k*dot(k, c)*(1 - cos) */
	const double cx = k * (b - g), cy = k * (r - b), cz = k * (g - r);
	c->x = (float)(r * cs + cx * sn + d);
	c->y = (float)(g * cs + cy * sn + d);
	c->z = (float)(b * cs + cz * sn + d);
}

static void mo_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct spazz_motion *mo = data;

	uint32_t w, h;
	source_size(mo, &w, &h);
	if (!w || !h || !mo->effect) {
		obs_source_skip_video_filter(mo->source);
		return;
	}

	/* away (exit played): draw nothing at all */
	if (mo->phase == PH_GONE) {
		return;
	}

	const uint32_t room = room_px(mo, w, h);
	const bool still = mo->motion == 0 && mo->distort == 0 && mo->look == 0 && mo->phase == PH_SHOWN && room == 0;
	if (still) {
		obs_source_skip_video_filter(mo->source);
		return;
	}

	/* where the picture is this frame */
	struct mo_xf motion, enter;
	struct mo_fx fx;
	const double t = mo->mclock + mo->m_phase * 10.0;
	mo_motion(mo->motion, t, mo->m_amount, (double)w, (double)h, &motion);
	int pivot = motion.pivot;
	if (mo->pivot > 0) { /* not "auto" */
		static const int map[] = {PIVOT_CENTER, PIVOT_CENTER, PIVOT_TOP, PIVOT_BOTTOM, PIVOT_LEFT, PIVOT_RIGHT};
		pivot = map[mo->pivot];
	}

	double k = 1.0;
	int anim = 0;
	if (mo->phase == PH_ENTER) {
		k = mo->anim_t / mo->anim_sec;
		anim = mo->enter;
	} else if (mo->phase == PH_EXIT) {
		k = 1.0 - mo->anim_t / mo->anim_sec;
		anim = mo->exit;
	}
	mo_enter(anim, k, (double)w, (double)h, &enter, &fx);

	struct mo_affine inv;
	if (!mo_compose(&motion, pivot, &enter, (double)w, (double)h, (double)room, (double)room, &inv) ||
	    fx.opacity <= 0.001) {
		return;
	}

	if (!obs_source_process_filter_begin(mo->source, GS_RGBA, OBS_NO_DIRECT_RENDERING)) {
		return;
	}

	const uint32_t ow = w + 2 * room, oh = h + 2 * room;
	struct vec2 v2;
	struct vec4 v4;
	struct vec3 ca = mo->col_a, cb = mo->col_b;
	const double hue = MO_TAU * mo->cycle * mo->clock;
	if (mo->cycle > 0.0) {
		hue_rotate(&ca, hue);
		hue_rotate(&cb, hue);
	}

	vec2_set(&v2, (float)ow, (float)oh);
	gs_effect_set_vec2(mo->p_out_size, &v2);
	vec2_set(&v2, (float)w, (float)h);
	gs_effect_set_vec2(mo->p_src_size, &v2);
	vec4_set(&v4, (float)inv.a, (float)inv.b, (float)inv.c, (float)inv.d);
	gs_effect_set_vec4(mo->p_xf, &v4);
	vec2_set(&v2, (float)inv.ex, (float)inv.ey);
	gs_effect_set_vec2(mo->p_xo, &v2);
	gs_effect_set_float(mo->p_time, (float)mo->clock);
	gs_effect_set_float(mo->p_opacity, (float)fx.opacity);
	vec4_set(&v4, (float)mo->distort, (float)mo->d_amount, (float)mo->d_size, (float)mo->dclock);
	gs_effect_set_vec4(mo->p_dist, &v4);
	vec4_set(&v4, (float)mo->look, (float)mo->l_amount, (float)mo->glow, (float)fmod(hue, MO_TAU * 64.0));
	gs_effect_set_vec4(mo->p_look, &v4);
	gs_effect_set_vec3(mo->p_col_a, &ca);
	gs_effect_set_vec3(mo->p_col_b, &cb);
	vec4_set(&v4, (float)fx.glitch, (float)fx.flash, (float)fx.pixel, (float)fx.wipe);
	gs_effect_set_vec4(mo->p_fx, &v4);
	vec4_set(&v4, (float)fx.vhs, (float)fmod(mo->seed, 97.0), 0.0f, 0.0f);
	gs_effect_set_vec4(mo->p_fx2, &v4);

	obs_source_process_filter_end(mo->source, mo->effect, ow, oh);
}

/* ------------------------------------------------------------------------ */
/* properties                                                                */

static void add_list(obs_properties_t *props, const char *key, const char *label, const struct mo_item *list,
		     size_t count)
{
	obs_property_t *p = obs_properties_add_list(props, key, label, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	for (size_t i = 0; i < count; i++) {
		obs_property_list_add_string(p, T_(list[i].label), list[i].id);
	}
}

static bool preset_modified(obs_properties_t *props, obs_property_t *prop, obs_data_t *s)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);
	const char *name = obs_data_get_string(s, "preset");
	if (strcmp(name, obs_data_get_string(s, "preset_applied")) == 0) {
		return false;
	}
	const struct mo_preset *p = find_preset(name);
	if (!p) {
		return false;
	}
	apply_preset(s, p);
	return true;
}

static bool loop_modified(obs_properties_t *props, obs_property_t *prop, obs_data_t *s)
{
	UNUSED_PARAMETER(prop);
	const bool on = obs_data_get_bool(s, "loop");
	obs_property_set_visible(obs_properties_get(props, "stay_sec"), on);
	obs_property_set_visible(obs_properties_get(props, "gap_sec"), on);
	return true;
}

static bool button_enter(obs_properties_t *props, obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);
	play_enter(data);
	return false;
}

static bool button_exit(obs_properties_t *props, obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);
	play_exit(data);
	return false;
}

static obs_properties_t *mo_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	p = obs_properties_add_list(props, "preset", T_("Preset"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	for (size_t i = 0; i < MO_PRESETS; i++) {
		obs_property_list_add_string(p, mo_presets[i].name, mo_presets[i].name);
	}
	obs_property_set_long_description(p, T_("Preset.Tip"));
	obs_property_set_modified_callback(p, preset_modified);

	/* motion */
	obs_properties_t *mg = obs_properties_create();
	add_list(mg, "motion", T_("Motion"), mo_motions, MO_MOTIONS);
	p = obs_properties_add_float_slider(mg, "m_speed", T_("Speed"), 0.1, 4.0, 0.05);
	obs_property_float_set_suffix(p, "x");
	obs_properties_add_float_slider(mg, "m_amount", T_("Amount"), 0.0, 4.0, 0.05);
	p = obs_properties_add_float_slider(mg, "m_phase", T_("Phase"), 0.0, 1.0, 0.01);
	obs_property_set_long_description(p, T_("Phase.Tip"));
	add_list(mg, "pivot", T_("Pivot"), mo_pivots, MO_PIVOTS);
	obs_properties_add_group(props, "grp_motion", T_("Group.Motion"), OBS_GROUP_NORMAL, mg);

	/* distort */
	obs_properties_t *dg = obs_properties_create();
	add_list(dg, "distort", T_("Distort"), mo_distorts, MO_DISTORTS);
	obs_properties_add_float_slider(dg, "d_amount", T_("Amount"), 0.0, 3.0, 0.05);
	obs_properties_add_float_slider(dg, "d_size", T_("Size"), 0.2, 4.0, 0.05);
	p = obs_properties_add_float_slider(dg, "d_speed", T_("Speed"), 0.0, 5.0, 0.05);
	obs_property_float_set_suffix(p, "x");
	obs_properties_add_group(props, "grp_distort", T_("Group.Distort"), OBS_GROUP_NORMAL, dg);

	/* look */
	obs_properties_t *lg = obs_properties_create();
	add_list(lg, "look", T_("Look"), mo_looks, MO_LOOKS);
	obs_properties_add_float_slider(lg, "l_amount", T_("Amount"), 0.0, 2.0, 0.05);
	obs_properties_add_color(lg, "col_a", T_("ColorA"));
	obs_properties_add_color(lg, "col_b", T_("ColorB"));
	p = obs_properties_add_float_slider(lg, "glow", T_("Glow"), 1.0, 80.0, 0.5);
	obs_property_float_set_suffix(p, " px");
	p = obs_properties_add_float_slider(lg, "cycle", T_("Cycle"), 0.0, 2.0, 0.01);
	obs_property_set_long_description(p, T_("Cycle.Tip"));
	obs_properties_add_group(props, "grp_look", T_("Group.Look"), OBS_GROUP_NORMAL, lg);

	/* enter & exit */
	obs_properties_t *eg = obs_properties_create();
	add_list(eg, "enter", T_("Enter"), mo_enters, MO_ENTERS);
	add_list(eg, "exit", T_("Exit"), mo_enters, MO_ENTERS);
	p = obs_properties_add_int_slider(eg, "anim_ms", T_("AnimMs"), 100, 4000, 50);
	obs_property_int_set_suffix(p, " ms");
	p = obs_properties_add_list(eg, "enter_when", T_("EnterWhen"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_("EnterWhen.show"), "show");
	obs_property_list_add_string(p, T_("EnterWhen.live"), "live");
	p = obs_properties_add_bool(eg, "loop", T_("Loop"));
	obs_property_set_long_description(p, T_("Loop.Tip"));
	obs_property_set_modified_callback(p, loop_modified);
	p = obs_properties_add_float(eg, "stay_sec", T_("StaySec"), 1.0, 3600.0, 1.0);
	obs_property_float_set_suffix(p, " s");
	p = obs_properties_add_float(eg, "gap_sec", T_("GapSec"), 0.0, 3600.0, 1.0);
	obs_property_float_set_suffix(p, " s");
	obs_properties_add_button2(eg, "btn_enter", T_("ReplayEnter"), button_enter, data);
	obs_properties_add_button2(eg, "btn_exit", T_("PlayExit"), button_exit, data);
	obs_properties_add_group(props, "grp_enter", T_("Group.Enter"), OBS_GROUP_NORMAL, eg);

	p = obs_properties_add_int_slider(props, "room", T_("Room"), 0, 50, 1);
	obs_property_int_set_suffix(p, "%");
	obs_property_set_long_description(p, T_("Room.Tip"));

	return props;
}

/* ------------------------------------------------------------------------ */
/* create / destroy                                                          */

static void *mo_create(obs_data_t *settings, obs_source_t *source)
{
	struct spazz_motion *mo = bzalloc(sizeof(*mo));
	mo->source = source;
	mo->phase = PH_SHOWN;

	/* a new filter starts as its preset: write it so the lists show it */
	if (!obs_data_has_user_value(settings, "preset_applied")) {
		const struct mo_preset *p = find_preset(obs_data_get_string(settings, "preset"));
		apply_preset(settings, p ? p : &mo_presets[MO_DEFAULT_PRESET]);
	}

	char *path = obs_module_file("spazz-motion.effect");
	obs_enter_graphics();
	mo->effect = gs_effect_create_from_file(path, NULL);
	obs_leave_graphics();
	bfree(path);
	if (mo->effect) {
		mo->p_out_size = gs_effect_get_param_by_name(mo->effect, "out_size");
		mo->p_src_size = gs_effect_get_param_by_name(mo->effect, "src_size");
		mo->p_xf = gs_effect_get_param_by_name(mo->effect, "xf");
		mo->p_xo = gs_effect_get_param_by_name(mo->effect, "xo");
		mo->p_time = gs_effect_get_param_by_name(mo->effect, "time");
		mo->p_opacity = gs_effect_get_param_by_name(mo->effect, "opacity");
		mo->p_dist = gs_effect_get_param_by_name(mo->effect, "dist");
		mo->p_look = gs_effect_get_param_by_name(mo->effect, "look");
		mo->p_col_a = gs_effect_get_param_by_name(mo->effect, "col_a");
		mo->p_col_b = gs_effect_get_param_by_name(mo->effect, "col_b");
		mo->p_fx = gs_effect_get_param_by_name(mo->effect, "fx");
		mo->p_fx2 = gs_effect_get_param_by_name(mo->effect, "fx2");
	} else {
		blog(LOG_WARNING, "[Movement] couldn't load spazz-motion.effect");
	}

	mo->hk_enter = obs_hotkey_register_source(source, "SpazzMotion.Enter", T_("Hotkey.Enter"), hotkey_enter, mo);
	mo->hk_exit = obs_hotkey_register_source(source, "SpazzMotion.Exit", T_("Hotkey.Exit"), hotkey_exit, mo);

	proc_handler_t *ph = obs_source_get_proc_handler(source);
	proc_handler_add(ph, "void play_enter()", proc_enter, mo);
	proc_handler_add(ph, "void play_exit()", proc_exit, mo);

	mo_update(mo, settings);
	return mo;
}

static void mo_destroy(void *data)
{
	struct spazz_motion *mo = data;
	obs_hotkey_unregister(mo->hk_enter);
	obs_hotkey_unregister(mo->hk_exit);
	if (mo->effect) {
		obs_enter_graphics();
		gs_effect_destroy(mo->effect);
		obs_leave_graphics();
	}
	bfree(mo);
}

static struct obs_source_info spazz_motion_info = {
	.id = "spazz_motion",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = mo_get_name,
	.create = mo_create,
	.destroy = mo_destroy,
	.update = mo_update,
	.get_defaults = mo_defaults,
	.get_properties = mo_properties,
	.get_width = mo_width,
	.get_height = mo_height,
	.video_tick = mo_tick,
	.video_render = mo_render,
	.show = mo_show,
	.activate = mo_activate,
};

bool obs_module_load(void)
{
	obs_register_source(&spazz_motion_info);
	return true;
}
