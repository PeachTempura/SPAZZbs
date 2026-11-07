/******************************************************************************
    SPAZZbs - Movement: the motions and the enter / exit animations, as plain
    maths (no OBS calls), so they can be tested on their own.
    Copyright (C) 2026 SPAZZbs contributors

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <math.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define MO_TAU (2.0 * M_PI)
#define MO_DEG (M_PI / 180.0)

/* ------------------------------------------------------------------------ */
/* the lists (ids are what settings store)                                   */

struct mo_item {
	const char *id;
	const char *label; /* locale key */
};

enum mo_pivot { PIVOT_CENTER, PIVOT_TOP, PIVOT_BOTTOM, PIVOT_LEFT, PIVOT_RIGHT };

/* Motion: how the whole thing moves (SPONSORCENTER's idle / logo motion and
 * then some) */
static const struct mo_item mo_motions[] = {
	{"none", "Motion.none"},       {"float", "Motion.float"},         {"bob", "Motion.bob"},
	{"lilypad", "Motion.lilypad"}, {"hover", "Motion.hover"},         {"drift", "Motion.drift"},
	{"tilt", "Motion.tilt"},       {"swing", "Motion.swing"},         {"sway", "Motion.sway"},
	{"orbit", "Motion.orbit"},     {"figure8", "Motion.figure8"},     {"spin", "Motion.spin"},
	{"coin", "Motion.coin"},       {"wobble", "Motion.wobble"},       {"tada", "Motion.tada"},
	{"jelly", "Motion.jelly"},     {"rubber", "Motion.rubber"},       {"pulse", "Motion.pulse"},
	{"breathe", "Motion.breathe"}, {"heartbeat", "Motion.heartbeat"}, {"bounce", "Motion.bounce"},
	{"jump", "Motion.jump"},       {"shake", "Motion.shake"},         {"quake", "Motion.quake"},
};
#define MO_MOTIONS (sizeof(mo_motions) / sizeof(mo_motions[0]))

/* Distort: bends the picture (index = shader mode) */
static const struct mo_item mo_distorts[] = {
	{"none", "Distort.none"},     {"undulate", "Distort.undulate"}, {"flag", "Distort.flag"},
	{"ripple", "Distort.ripple"}, {"warp", "Distort.warp"},         {"haze", "Distort.haze"},
	{"glitch", "Distort.glitch"}, {"datamosh", "Distort.datamosh"}, {"vhs", "Distort.vhs"},
	{"crt", "Distort.crt"},       {"pixel", "Distort.pixel"},       {"chroma", "Distort.chroma"},
	{"swirl", "Distort.swirl"},   {"bulge", "Distort.bulge"},
};
#define MO_DISTORTS (sizeof(mo_distorts) / sizeof(mo_distorts[0]))

/* Look: restyles the colours (index = shader mode) */
static const struct mo_item mo_looks[] = {
	{"none", "Look.none"},           {"neon", "Look.neon"},       {"neonflicker", "Look.neonflicker"},
	{"outline", "Look.outline"},     {"rainbow", "Look.rainbow"}, {"shadow", "Look.shadow"},
	{"bloom", "Look.bloom"},         {"rgb", "Look.rgb"},         {"duotone", "Look.duotone"},
	{"synthwave", "Look.synthwave"}, {"chrome", "Look.chrome"},   {"hologram", "Look.hologram"},
	{"thermal", "Look.thermal"},     {"gameboy", "Look.gameboy"}, {"film", "Look.film"},
	{"noir", "Look.noir"},           {"popart", "Look.popart"},   {"scanlines", "Look.scanlines"},
	{"shine", "Look.shine"},         {"tint", "Look.tint"},
};
#define MO_LOOKS (sizeof(mo_looks) / sizeof(mo_looks[0]))

/* Enter / exit (the exit plays the same animation backwards) */
static const struct mo_item mo_enters[] = {
	{"none", "Enter.none"},       {"fade", "Enter.fade"},       {"pop", "Enter.pop"},
	{"zoom", "Enter.zoom"},       {"rise", "Enter.rise"},       {"slide_l", "Enter.slide_l"},
	{"slide_r", "Enter.slide_r"}, {"slide_u", "Enter.slide_u"}, {"slide_d", "Enter.slide_d"},
	{"drop", "Enter.drop"},       {"spin", "Enter.spin"},       {"flip", "Enter.flip"},
	{"unfold", "Enter.unfold"},   {"wipe", "Enter.wipe"},       {"glitch", "Enter.glitch"},
	{"crt", "Enter.crt"},         {"flicker", "Enter.flicker"}, {"vhs", "Enter.vhs"},
	{"pixel", "Enter.pixel"},
};
#define MO_ENTERS (sizeof(mo_enters) / sizeof(mo_enters[0]))

static const struct mo_item mo_pivots[] = {
	{"auto", "Pivot.auto"},     {"center", "Pivot.center"}, {"top", "Pivot.top"},
	{"bottom", "Pivot.bottom"}, {"left", "Pivot.left"},     {"right", "Pivot.right"},
};
#define MO_PIVOTS (sizeof(mo_pivots) / sizeof(mo_pivots[0]))

static inline int mo_index(const struct mo_item *list, int count, const char *id)
{
	if (!id) {
		return 0;
	}
	for (int i = 0; i < count; i++) {
		const char *a = list[i].id;
		const char *b = id;
		while (*a && *a == *b) {
			a++;
			b++;
		}
		if (*a == 0 && *b == 0) {
			return i;
		}
	}
	return 0;
}

/* ------------------------------------------------------------------------ */
/* one transform: move, turn, scale, lean                                    */

struct mo_xf {
	double tx, ty; /* pixels */
	double rot;    /* radians */
	double sx, sy;
	double skx;
	int pivot; /* motion's own pivot (enum mo_pivot) */
};

static inline void mo_xf_reset(struct mo_xf *x)
{
	x->tx = x->ty = 0.0;
	x->rot = 0.0;
	x->sx = x->sy = 1.0;
	x->skx = 0.0;
	x->pivot = PIVOT_CENTER;
}

/* smooth 1D value noise in -1..1 */
static inline double mo_hash(double n)
{
	double s = sin(n * 127.1 + 311.7) * 43758.5453;
	return (s - floor(s)) * 2.0 - 1.0;
}

static inline double mo_noise(double x)
{
	double i = floor(x);
	double f = x - i;
	double u = f * f * (3.0 - 2.0 * f);
	return mo_hash(i) * (1.0 - u) + mo_hash(i + 1.0) * u;
}

static inline double mo_wrap(double t, double period)
{
	return t - period * floor(t / period);
}

/* The motion at time t (seconds, already sped up and phase shifted).
 * amount 1 = the normal size; w / h = the source in pixels. */
static inline void mo_motion(int m, double t, double amount, double w, double h, struct mo_xf *o)
{
	const double u = 0.035 * fmin(w, h) * amount; /* a "step" in pixels */
	const double A = amount;
	mo_xf_reset(o);

	switch (m) {
	case 1: /* float */
		o->ty = -u * sin(MO_TAU * 0.22 * t);
		o->tx = u * 0.35 * sin(MO_TAU * 0.13 * t + 1.3);
		o->rot = 0.6 * MO_DEG * A * sin(MO_TAU * 0.17 * t);
		break;
	case 2: /* bob */
		o->ty = -u * 0.8 * sin(MO_TAU * 0.85 * t);
		break;
	case 3: /* lily pad: a lazy bob, a slow tilt, a little drift */
		o->ty = -u * 0.55 * sin(MO_TAU * 0.2 * t);
		o->tx = u * 0.25 * sin(MO_TAU * 0.11 * t + 0.7);
		o->rot = 2.2 * MO_DEG * A * sin(MO_TAU * 0.16 * t + 1.1);
		o->sx = o->sy = 1.0 + 0.008 * A * sin(MO_TAU * 0.2 * t + 1.6);
		break;
	case 4: /* hover */
		o->ty = -u * 0.7 * sin(MO_TAU * 0.3 * t);
		o->rot = 1.0 * MO_DEG * A * sin(MO_TAU * 0.15 * t);
		break;
	case 5: /* drift: wanders about */
		o->tx = u * (sin(MO_TAU * 0.07 * t) + 0.5 * sin(MO_TAU * 0.173 * t + 2.0)) * 0.75;
		o->ty = u * (sin(MO_TAU * 0.09 * t + 1.0) + 0.5 * sin(MO_TAU * 0.151 * t)) * 0.6;
		o->rot = 1.5 * MO_DEG * A * sin(MO_TAU * 0.05 * t);
		break;
	case 6: /* tilt */
		o->rot = 3.0 * MO_DEG * A * sin(MO_TAU * 0.22 * t);
		o->skx = 0.02 * A * sin(MO_TAU * 0.22 * t + 1.5);
		break;
	case 7: /* swing: hangs from the top */
		o->rot = 9.0 * MO_DEG * A * sin(MO_TAU * 0.45 * t);
		o->pivot = PIVOT_TOP;
		break;
	case 8: /* sway: stands on the bottom */
		o->rot = 4.0 * MO_DEG * A * sin(MO_TAU * 0.3 * t);
		o->pivot = PIVOT_BOTTOM;
		break;
	case 9: /* orbit */
		o->tx = u * cos(MO_TAU * 0.2 * t);
		o->ty = u * sin(MO_TAU * 0.2 * t);
		break;
	case 10: /* figure 8 */
		o->tx = u * 1.2 * sin(MO_TAU * 0.15 * t);
		o->ty = u * 0.6 * sin(MO_TAU * 0.3 * t);
		break;
	case 11: /* spin */
		o->rot = MO_TAU * 0.12 * A * t;
		break;
	case 12: { /* coin flip, every few seconds */
		const double ph = mo_wrap(t, 3.0);
		o->sx = ph < 1.0 ? cos(MO_TAU * (0.5 - 0.5 * cos(M_PI * ph))) : 1.0;
		break;
	}
	case 13: { /* wobble: a jiggle that settles, every 3 s */
		const double ph = mo_wrap(t, 3.0);
		o->rot = 7.0 * MO_DEG * A * exp(-3.5 * ph) * sin(22.0 * ph);
		break;
	}
	case 14: { /* tada: grow a little and wiggle, every 4 s */
		const double ph = mo_wrap(t, 4.0);
		if (ph < 1.0) {
			const double env = sin(M_PI * ph);
			o->sx = o->sy = 1.0 + 0.08 * A * env;
			o->rot = 4.0 * MO_DEG * A * sin(MO_TAU * 5.0 * ph) * env;
		}
		break;
	}
	case 15: { /* jelly: squash and stretch */
		const double k = sin(MO_TAU * 1.1 * t);
		o->sx = 1.0 + 0.05 * A * k;
		o->sy = 1.0 - 0.05 * A * k;
		o->pivot = PIVOT_BOTTOM;
		break;
	}
	case 16: { /* rubber band, every 3 s */
		const double ph = mo_wrap(t, 3.0);
		const double d = exp(-4.0 * ph) * sin(18.0 * ph) * 0.12 * A;
		o->sx = 1.0 + d;
		o->sy = 1.0 - d;
		break;
	}
	case 17: /* pulse */
		o->sx = o->sy = 1.0 + 0.045 * A * (0.5 + 0.5 * sin(MO_TAU * 0.9 * t));
		break;
	case 18: /* breathe */
		o->sx = o->sy = 1.0 + 0.025 * A * sin(MO_TAU * 0.18 * t);
		break;
	case 19: { /* heartbeat: lub-dub */
		const double ph = mo_wrap(t, 1.1);
		const double a = (ph - 0.1) / 0.05;
		const double b = (ph - 0.32) / 0.06;
		o->sx = o->sy = 1.0 + 0.07 * A * (exp(-a * a) + 0.6 * exp(-b * b));
		break;
	}
	case 20: { /* bounce: hop, squash on landing */
		const double ph = mo_wrap(t, 1.0);
		const double ht = 4.0 * ph * (1.0 - ph);
		const double g = fmax(0.0, 1.0 - ht * 6.0);
		o->ty = -u * 2.2 * ht;
		o->sx = 1.0 + 0.08 * A * g;
		o->sy = 1.0 - 0.08 * A * g;
		o->pivot = PIVOT_BOTTOM;
		break;
	}
	case 21: { /* jump: a big hop every 2.5 s */
		const double ph = mo_wrap(t, 2.5);
		o->pivot = PIVOT_BOTTOM;
		if (ph < 0.15) { /* crouch */
			const double c = sin(M_PI * ph / 0.15);
			o->sx = 1.0 + 0.07 * A * c;
			o->sy = 1.0 - 0.07 * A * c;
		} else if (ph < 0.75) {
			const double p = (ph - 0.15) / 0.6;
			o->ty = -u * 3.0 * 4.0 * p * (1.0 - p);
			o->sx = 1.0 - 0.04 * A * sin(M_PI * p);
			o->sy = 1.0 + 0.04 * A * sin(M_PI * p);
		} else if (ph < 0.9) { /* land */
			const double c = sin(M_PI * (ph - 0.75) / 0.15);
			o->sx = 1.0 + 0.06 * A * c;
			o->sy = 1.0 - 0.06 * A * c;
		}
		break;
	}
	case 22: /* shake */
		o->tx = u * 0.35 * mo_noise(t * 18.0);
		o->ty = u * 0.35 * mo_noise(t * 18.0 + 57.0);
		o->rot = 0.8 * MO_DEG * A * mo_noise(t * 14.0 + 13.0);
		break;
	case 23: /* earthquake */
		o->tx = u * 0.9 * mo_noise(t * 30.0);
		o->ty = u * 0.7 * mo_noise(t * 30.0 + 41.0);
		o->rot = 1.6 * MO_DEG * A * mo_noise(t * 24.0 + 7.0);
		break;
	default:
		break;
	}
}

/* ------------------------------------------------------------------------ */
/* enter / exit                                                              */

struct mo_fx {
	double opacity;
	double glitch; /* fx.x */
	double flash;  /* fx.y  white */
	double pixel;  /* fx.z  block size in px (<= 1: off) */
	double wipe;   /* fx.w  1 = whole picture */
	double vhs;    /* fx2.x */
};

static inline double mo_ease_out(double k)
{
	const double a = 1.0 - k;
	return 1.0 - a * a * a;
}

static inline double mo_back_out(double k)
{
	const double c1 = 1.70158;
	const double c3 = c1 + 1.0;
	const double a = k - 1.0;
	return 1.0 + c3 * a * a * a + c1 * a * a;
}

static inline double mo_bounce_out(double k)
{
	const double n1 = 7.5625;
	const double d1 = 2.75;
	if (k < 1.0 / d1) {
		return n1 * k * k;
	} else if (k < 2.0 / d1) {
		k -= 1.5 / d1;
		return n1 * k * k + 0.75;
	} else if (k < 2.5 / d1) {
		k -= 2.25 / d1;
		return n1 * k * k + 0.9375;
	}
	k -= 2.625 / d1;
	return n1 * k * k + 0.984375;
}

static inline double mo_clamp01(double v)
{
	return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

/* k: 0 = not there yet, 1 = fully in. Writes a centre-pivoted transform. */
static inline void mo_enter(int e, double k, double w, double h, struct mo_xf *o, struct mo_fx *f)
{
	k = mo_clamp01(k);
	const double eo = mo_ease_out(k);
	mo_xf_reset(o);
	f->opacity = 1.0;
	f->glitch = 0.0;
	f->flash = 0.0;
	f->pixel = 0.0;
	f->wipe = 1.0;
	f->vhs = 0.0;

	switch (e) {
	case 1: /* fade */
		f->opacity = k;
		break;
	case 2: /* pop */
		o->sx = o->sy = fmax(0.0, mo_back_out(k));
		f->opacity = mo_clamp01(k * 4.0);
		break;
	case 3: /* zoom */
		o->sx = o->sy = 1.0 + 1.5 * (1.0 - eo);
		f->opacity = k;
		break;
	case 4: /* rise */
		o->ty = (1.0 - eo) * 0.35 * h;
		f->opacity = k;
		break;
	case 5: /* slide from the left */
		o->tx = -(1.0 - eo) * w * 1.1;
		f->opacity = mo_clamp01(k * 2.0);
		break;
	case 6: /* from the right */
		o->tx = (1.0 - eo) * w * 1.1;
		f->opacity = mo_clamp01(k * 2.0);
		break;
	case 7: /* from above */
		o->ty = -(1.0 - eo) * h * 1.1;
		f->opacity = mo_clamp01(k * 2.0);
		break;
	case 8: /* from below */
		o->ty = (1.0 - eo) * h * 1.1;
		f->opacity = mo_clamp01(k * 2.0);
		break;
	case 9: /* drop and bounce */
		o->ty = -(1.0 - mo_bounce_out(k)) * h * 0.9;
		f->opacity = mo_clamp01(k * 4.0);
		break;
	case 10: /* spin */
		o->rot = -(1.0 - eo) * MO_TAU * 0.75;
		o->sx = o->sy = eo;
		f->opacity = k;
		break;
	case 11: /* flip */
		o->sx = eo;
		break;
	case 12: /* unfold */
		o->sy = eo;
		break;
	case 13: /* wipe */
		f->wipe = eo;
		break;
	case 14: /* glitch */
		f->glitch = (1.0 - k) * 1.2;
		f->opacity = k < 0.85 ? (mo_hash(floor(k * 20.0)) > -0.3 ? 1.0 : 0.15) * mo_clamp01(k * 3.0) : 1.0;
		break;
	case 15: /* CRT power on: a line, then the picture */
		if (k < 0.4) {
			const double a = k / 0.4;
			o->sx = mo_ease_out(a);
			o->sy = 0.012;
			f->flash = 1.0;
		} else {
			const double b = (k - 0.4) / 0.6;
			o->sy = 0.012 + 0.988 * mo_ease_out(b);
			f->flash = 1.0 - b;
		}
		break;
	case 16: { /* neon flicker on: sputters, catches, holds */
		static const unsigned char on[20] = {0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 1, 0, 1, 1, 1, 1, 0, 1};
		const int step = (int)(k * 20.0);
		if (step < 20) {
			f->opacity = on[step] ? 1.0 : 0.08;
		}
		break;
	}
	case 17: /* VHS */
		f->vhs = (1.0 - k) * 1.3;
		f->opacity = mo_clamp01(k * 3.0);
		break;
	case 18: /* pixels */
		f->pixel = 1.0 + (1.0 - k) * (1.0 - k) * 60.0;
		f->opacity = mo_clamp01(k * 3.0);
		break;
	default:
		break;
	}
}

/* ------------------------------------------------------------------------ */
/* putting it together: output pixel -> source pixel                          */

struct mo_affine {
	double a, b, c, d; /* 2x2, row major */
	double ex, ey;
};

static inline void mo_linear(const struct mo_xf *x, double *a, double *b, double *c, double *d)
{
	/* L = R(rot) * Skew(skx) * Scale(sx, sy) */
	const double cs = cos(x->rot);
	const double sn = sin(x->rot);
	const double sx = fabs(x->sx) < 0.0005 ? (x->sx < 0.0 ? -0.0005 : 0.0005) : x->sx;
	const double sy = fabs(x->sy) < 0.0005 ? (x->sy < 0.0 ? -0.0005 : 0.0005) : x->sy;
	/* Skew * Scale = [[sx, skx*sy], [0, sy]] */
	const double k00 = sx, k01 = x->skx * sy, k10 = 0.0, k11 = sy;
	*a = cs * k00 - sn * k10;
	*b = cs * k01 - sn * k11;
	*c = sn * k00 + cs * k10;
	*d = sn * k01 + cs * k11;
}

static inline void mo_pivot_px(int pivot, double w, double h, double *px, double *py)
{
	*px = w * 0.5;
	*py = h * 0.5;
	switch (pivot) {
	case PIVOT_TOP:
		*py = 0.0;
		break;
	case PIVOT_BOTTOM:
		*py = h;
		break;
	case PIVOT_LEFT:
		*px = 0.0;
		break;
	case PIVOT_RIGHT:
		*px = w;
		break;
	default:
		break;
	}
}

/* forward: out = room + M(motion about pivot) * E(enter about centre) * src.
 * Returns the inverse (out -> src); false if it collapsed to nothing. */
static inline int mo_compose(const struct mo_xf *motion, int pivot, const struct mo_xf *enter, double w, double h,
			     double room_x, double room_y, struct mo_affine *inv)
{
	double ea, eb, ec, ed, ma, mb, mc, md;
	mo_linear(enter, &ea, &eb, &ec, &ed);
	mo_linear(motion, &ma, &mb, &mc, &md);

	/* enter about the centre */
	const double cx = w * 0.5, cy = h * 0.5;
	const double bex = cx - (ea * cx + eb * cy) + enter->tx;
	const double bey = cy - (ec * cx + ed * cy) + enter->ty;

	/* motion about its pivot, then into the padded output */
	double px, py;
	mo_pivot_px(pivot, w, h, &px, &py);
	const double bmx = px - (ma * px + mb * py) + motion->tx + room_x;
	const double bmy = py - (mc * px + md * py) + motion->ty + room_y;

	/* total = M * E */
	const double a = ma * ea + mb * ec;
	const double b = ma * eb + mb * ed;
	const double c = mc * ea + md * ec;
	const double d = mc * eb + md * ed;
	const double bx = ma * bex + mb * bey + bmx;
	const double by = mc * bex + md * bey + bmy;

	const double det = a * d - b * c;
	if (fabs(det) < 1e-9) {
		return 0;
	}
	inv->a = d / det;
	inv->b = -b / det;
	inv->c = -c / det;
	inv->d = a / det;
	inv->ex = -(inv->a * bx + inv->b * by);
	inv->ey = -(inv->c * bx + inv->d * by);
	return 1;
}
