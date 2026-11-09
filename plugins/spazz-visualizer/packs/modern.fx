/* ================================================================== */
/* Modern pack                                                          */
/* (styles listed in pack-modern.h; labels in packs/locale)             */
/*                                                                      */
/* Every style here follows react_mix: x how much things MOVE with the  */
/* music, y how much they BRIGHTEN, z how much the COLOURS shift, w the */
/* beat pulses. The calm ones (VZ_CALM) read calm_in, the slow-smoothed */
/* music, and never flash.                                              */
/* ================================================================== */

/* ---- helpers (md_ = modern pack) ---------------------------------- */

float3 md3(float x)
{
	return float3(x, x, x);
}

/* beat pulse, scaled by react_mix.w */
float md_beat()
{
	return saturate(beat) * react_mix.w;
}

/* distance from p to the segment a-b */
float md_seg(float2 p, float2 a, float2 b)
{
	float2 pa = p - a;
	float2 ba = b - a;
	float h = saturate(dot(pa, ba) / max(dot(ba, ba), 0.000001));
	return length(pa - ba * h);
}

/* signed distance to a box of half size b */
float md_box(float2 p, float2 b)
{
	float2 d = abs(p) - b;
	return length(max(d, float2(0.0, 0.0))) + min(max(d.x, d.y), 0.0);
}

/* a field of small twinkling stars; p in aspect units, scale = cells per unit */
float md_stars(float2 p, float scale, float seed)
{
	float2 q = p * scale;
	float2 id = floor(q);
	float2 f = frac(q) - 0.5;
	float h = hash21(id + seed);
	float2 o = float2(hash21(id + seed + 3.7), hash21(id + seed + 9.1)) - 0.5;
	float d = length(f - o * 0.7) / scale;
	float tw = 0.55 + 0.45 * sin(time * (0.8 + 2.0 * h) + h * 40.0);
	float px = 1.0 / res.y;
	return step(0.86, h) * exp(-d * d / (px * px * 1.3)) * tw * (0.4 + 0.8 * hash21(id + seed + 1.3));
}

/* palette colour for the style's own warm/cool tint: base when the rainbow
 * palette is on, the chosen palette otherwise */
float3 md_tint(float3 base, float h)
{
	return lerp(base, prism(h), 0.25 + 0.75 * pal_mix);
}

/* ================================================================== */
/* CALM                                                                 */
/* ================================================================== */

/* ------------------------------------------------------------------ */
/* Calm · Soft Orbs: glowing orbs at three depths, the far ones soft    */
/* and dim, the near ones crisp with a faint lens ring. Each swells     */
/* with its own band. Amount = how many (up to 16).                    */
float4 PSModernSoftOrbs(VertData v_in) : TARGET
{
	float2 uv = view_uv(v_in.uv);
	float aspect = res.x / res.y;
	float2 p = float2(uv.x * aspect, uv.y);
	float px = 1.0 / res.y;
	float n = clamp(floor(6.0 + 6.0 * density), 1.0, 16.0);

	float3 light = float3(0.0, 0.0, 0.0);
	for (int i = 0; i < 16; i++) {
		float fi = float(i);
		if (fi < n) {
			float h1 = hash11(fi * 7.31 + 1.0);
			float h2 = hash11(fi * 3.17 + 5.0);
			float depth = hash11(fi * 1.93 + 8.0); /* 0 far .. 1 near */
			float2 c = float2((0.5 + 0.44 * sin(time * (0.05 + 0.04 * h1) * (0.6 + depth) + fi * 2.1)) * aspect,
					  0.5 + 0.4 * cos(time * (0.045 + 0.035 * h2) * (0.6 + depth) + fi * 1.3));
			float e = calm_e(fi / max(n - 1.0, 1.0), 0.55);
			float rad = (0.03 + 0.05 * h2) * (0.6 + 0.8 * depth) * (1.0 + 0.35 * react_mix.x * e);
			float d = length(p - c);
			/* far orbs are out of focus: a wider, softer edge */
			float soft = px * 1.5 + rad * (0.45 - 0.4 * depth);
			float core = 1.0 - smoothstep(rad - soft, rad + soft, d);
			float glow = exp(-d / (rad * (1.4 + 0.8 * (1.0 - depth))));
			float ring = exp(-pow((d - rad * 0.92) / (rad * 0.06 + px), 2.0)) * depth * 0.35;
			float h = cm_hue(hue + fi * 0.071 + react_mix.z * 0.06 * calm_in.z, uv, fi / max(n, 1.0), fi);
			float bright = (0.25 + 0.55 * depth) * (0.55 + 0.9 * react_mix.y * e);
			light += prism(h) * (core * 0.55 + ring + glow * 0.32) * bright;
		}
	}
	return finish_dither(light, uv, v_in.uv * res);
}

/* ------------------------------------------------------------------ */
/* Calm · Jellyfish: translucent jellyfish drifting up through deep    */
/* water, bells breathing, frilled rims and long trailing tentacles,   */
/* soft light from the surface and drifting marine snow.               */
/* Amount = how many (up to 7).                                         */
float4 PSModernJellyfish(VertData v_in) : TARGET
{
	float2 uv = view_uv(v_in.uv);
	float aspect = res.x / res.y;
	float2 p = float2(uv.x * aspect, uv.y);
	float px = 1.0 / res.y;
	float e = calm_in.x;

	/* deep water, lighter towards the surface */
	float3 water = md_tint(float3(0.03, 0.16, 0.24), hue + 0.5);
	float3 light = lerp(water * 0.55, water * 0.06, smoothstep(0.0, 1.0, uv.y)) * (0.8 + 0.4 * react_mix.y * e);
	/* slow shafts of light from above */
	float rx = p.x + uv.y * 0.3;
	float shafts = fbm4(float2(rx * 3.2 + time * 0.02, time * 0.04));
	light += water * 0.9 * pow(saturate(shafts * 1.25 - 0.15), 3.0) * (1.0 - uv.y) * (1.0 + 0.5 * calm_in.w);

	/* marine snow, two depths */
	for (int k = 0; k < 2; k++) {
		float fk = float(k);
		float s = 16.0 + 20.0 * fk;
		float2 q = p * s + float2(sin(time * 0.1 + fk) * 0.5, -time * (0.25 + 0.2 * fk));
		float2 id = floor(q);
		float2 f = frac(q) - 0.5;
		float h = hash21(id + fk * 13.0);
		float2 o = float2(hash21(id + 3.1), hash21(id + 7.7)) - 0.5;
		float d = length(f - o * 0.7) / s;
		light += md3(0.22 + 0.15 * fk) * step(0.72, h) * exp(-d * d / (px * px * (1.2 + 1.6 * (1.0 - fk))));
	}

	float n = clamp(floor(3.0 + 2.5 * density), 1.0, 7.0);
	for (int i = 0; i < 7; i++) {
		float fi = float(i);
		if (fi < n) {
			float h1 = hash11(fi * 3.7 + 0.3);
			float h2 = hash11(fi * 9.1 + 2.0);
			float size = 0.06 + 0.075 * h2; /* bell radius; bigger = nearer */
			float rise = 0.008 + 0.01 * h1;
			float y = 1.5 - frac(time * rise + h1 * 1.7) * 2.3;
			float x = (0.1 + 0.8 * hash11(fi * 5.3 + 1.1)) * aspect + 0.06 * sin(time * 0.17 + fi * 2.0);
			float ph = time * (1.1 + 0.7 * react_mix.x * calm_in.y) * (0.7 + 0.5 * h2) + fi * 1.9;
			float pulse = 0.5 + 0.5 * sin(ph);
			float2 q = (p - float2(x, y)) / size;
			float upx = px / size;

			/* the bell: a dome that squeezes as it swims */
			float sx = 1.0 + 0.13 * pulse;
			float sy = 1.0 - 0.1 * pulse;
			float2 b = float2(q.x / sx, q.y / sy);
			float r = length(float2(b.x, b.y * 1.18));
			float cut = 0.22 + 0.04 * sin(b.x * 16.0 + ph * 1.5); /* the frilled open rim */
			float inside = (1.0 - smoothstep(1.0 - 1.5 * upx, 1.0 + 1.5 * upx, r)) *
				       (1.0 - smoothstep(cut - 1.5 * upx, cut + 1.5 * upx, b.y));
			float rimlight = smoothstep(0.55, 1.0, r);           /* edges glow (seen edge-on) */
			float frill = exp(-pow((b.y - cut) / 0.05, 2.0)) * (1.0 - smoothstep(0.95, 1.05, abs(b.x)));
			float core = exp(-length(b - float2(0.0, -0.2)) * 3.2);
			float lobes = pow(0.5 + 0.5 * cos(atan2(b.x, b.y + 0.15) * 4.0), 4.0) * exp(-r * r * 5.0);
			float bell = inside * (0.18 + 0.9 * rimlight + 0.5 * lobes) + frill * 0.9 + core * 0.35 * inside;

			/* tentacles, swaying in the bell's wake */
			float tent = 0.0;
			float len = 2.4 + 1.6 * h1;
			for (int t = 0; t < 7; t++) {
				float ft = float(t);
				float yy = q.y - 0.24;
				if (yy > 0.0 && yy < len) {
					float tx = (ft / 6.0 - 0.5) * 1.5 * sx;
					float k = yy / len;
					float xx = tx * (1.0 - 0.35 * k) + (0.1 + 0.08 * hash11(ft + fi)) * sin(yy * 3.2 - ph * 1.2 + ft * 1.7) * yy;
					float w = (0.035 + 0.02 * hash11(ft * 2.0 + fi)) * (1.0 - 0.6 * k) + upx;
					float dd = (q.x - xx) / w;
					tent += exp(-dd * dd) * (1.0 - k) * (1.0 - k) * (0.5 + 0.5 * hash11(ft + 3.0 * fi));
				}
			}
			/* two long oral arms in the middle */
			float yy2 = q.y - 0.1;
			if (yy2 > 0.0 && yy2 < len * 0.6) {
				float k2 = yy2 / (len * 0.6);
				float xx2 = 0.12 * sin(yy2 * 2.2 - ph + fi) * (0.5 + k2);
				float w2 = 0.09 * (1.0 - 0.7 * k2) + upx;
				float d2 = (q.x - xx2) / w2;
				tent += exp(-d2 * d2) * (1.0 - k2) * 0.6 * (0.6 + 0.4 * sin(yy2 * 14.0 - ph * 2.0));
			}

			float hcol = cm_hue(hue + h1 * 0.55 + react_mix.z * 0.12 * calm_in.z, uv, saturate(q.y * 0.25 + 0.3), fi);
			float3 col = prism(hcol);
			float near = smoothstep(0.06, 0.135, size);
			float glow = (0.35 + 0.65 * near) * (0.55 + 0.8 * react_mix.y * calm_e(h1, 0.6));
			light += col * (bell + tent * 0.55) * glow;
			/* a soft halo in the water around the bell */
			light += col * exp(-max(r - 0.9, 0.0) * 2.5) * 0.05 * glow;
		}
	}
	return finish_dither(light, uv, v_in.uv * res);
}

/* ------------------------------------------------------------------ */
/* Calm · Glowing Tide: a night beach where the breaking waves glow     */
/* blue-green (bioluminescence) as they roll in and wash up the sand,   */
/* under a starry sky. Amount = how much sparkle in the water.          */
float4 PSModernGlowTide(VertData v_in) : TARGET
{
	float2 uv = view_uv(v_in.uv);
	float aspect = res.x / res.y;
	float2 p = float2(uv.x * aspect, uv.y);
	float px = 1.0 / res.y;
	float horizon = 0.38;
	float shore = 0.76;
	float3 glowc = prism(cm_hue(hue + 0.5 + react_mix.z * 0.1 * calm_in.z, uv, uv.x, 0.0));
	float3 light = float3(0.0, 0.0, 0.0);

	/* sky */
	float sky = 1.0 - smoothstep(horizon - px, horizon + px, uv.y);
	light += lerp(float3(0.008, 0.012, 0.035), float3(0.03, 0.05, 0.09), saturate(uv.y / horizon)) * sky;
	light += (md_stars(p, 70.0, 1.0) + md_stars(p, 140.0, 7.0) * 0.6) * sky * (0.4 + 0.6 * (1.0 - uv.y / horizon));
	light += glowc * 0.05 * exp(-abs(uv.y - horizon) * 30.0);

	/* sea, in perspective */
	float sea = step(horizon, uv.y) * (1.0 - smoothstep(shore - 0.01, shore + 0.02, uv.y));
	float dy = max(uv.y - horizon, 0.0005);
	float z = 0.22 / dy;                        /* distance */
	float wx = (p.x - aspect * 0.5) * z;
	float swell = 0.6 + 0.9 * react_mix.x * calm_in.y;
	float3 water = float3(0.006, 0.014, 0.03) * (0.6 + 0.5 * vnoise(float2(wx * 2.0, z * 6.0 - time * 0.3)));
	light += water * sea;
	/* the wave crests: rows that roll towards us and break near the shore */
	float rows = z * 1.6 + time * 0.22;
	float crest = pow(0.5 + 0.5 * cos(6.2831853 * rows), 10.0 + 6.0 * saturate(z / 6.0));
	float breakn = fbm4(float2(wx * 2.5 + time * 0.1, floor(rows) * 3.1));
	float nearness = 1.0 - smoothstep(0.0, 1.0, (z - 0.35) / 2.5); /* close rows break harder */
	float glowwave = crest * smoothstep(0.35, 0.75, breakn) * nearness * swell;
	light += glowc * glowwave * sea * (0.8 + 0.8 * react_mix.y * calm_in.x);
	/* sparkling plankton where the water is stirred */
	float2 sq = float2(wx * 6.0, z * 14.0 + time * 0.4);
	float sh = hash21(floor(sq));
	float spark = step(1.0 - 0.06 * clamp(density, 0.0, 3.0), sh) *
		      (0.5 + 0.5 * sin(time * (2.0 + 4.0 * sh) + sh * 60.0));
	light += glowc * spark * (0.3 + glowwave * 2.0) * sea * nearness * 0.9;

	/* the wash running up the sand, with its glowing edge */
	float wash = shore + 0.06 + 0.05 * sin(time * 0.35) * swell + 0.015 * sin(p.x * 9.0 + time * 0.7);
	float wet = step(shore, uv.y) * (1.0 - smoothstep(wash - px, wash + px, uv.y));
	float edge = exp(-pow((uv.y - wash) / (0.004 + px), 2.0)) * step(shore, uv.y) *
		     (0.6 + 0.4 * vnoise(float2(p.x * 30.0, time)));
	float3 sand = float3(0.035, 0.03, 0.04) * (0.7 + 0.3 * vnoise(p * 140.0));
	float sandm = step(shore, uv.y);
	light += sand * sandm * (1.0 - wet * 0.5);
	light += glowc * (edge * 1.2 + wet * 0.12 * (1.0 + calm_in.w)) * swell;
	/* reflected glow on the wet sand */
	light += glowc * wet * 0.15 * exp(-(wash - uv.y) * 12.0);
	return finish_dither(light, uv, v_in.uv * res);
}

/* ------------------------------------------------------------------ */
/* Calm · Sky Lanterns: paper lanterns rising slowly from a still lake, */
/* their warm light mirrored in the rippling water.                     */
/* Amount = how many (up to 14).                                         */
float3 md_lanterns(float2 p, float aspect, float px, float n, float3 warm)
{
	float3 light = float3(0.0, 0.0, 0.0);
	for (int i = 0; i < 14; i++) {
		float fi = float(i);
		if (fi < n) {
			float h1 = hash11(fi * 4.13 + 0.7);
			float h2 = hash11(fi * 8.71 + 3.3);
			float depth = h2;                         /* 0 far, 1 near */
			float size = 0.012 + 0.03 * depth;
			float speed = 0.006 + 0.012 * depth;
			float y = 0.72 - frac(time * speed + h1 * 3.0) * 0.95;
			float x = (0.05 + 0.9 * hash11(fi * 2.9 + 5.5)) * aspect + 0.03 * sin(time * 0.15 + fi * 1.7);
			float2 q = (p - float2(x, y)) / size;
			/* a slightly tapered paper lantern, wider at the top */
			float w = 0.62 - 0.12 * q.y;
			float d = md_box(float2(q.x * (0.6 / w), q.y), float2(0.6, 0.85));
			float body = 1.0 - smoothstep(-px / size, px / size * 1.5, d);
			/* the flame glows through the paper, brightest near the bottom */
			float inner = exp(-length(q - float2(0.0, 0.55)) * 1.4);
			float flick = 0.85 + 0.15 * sin(time * (6.0 + 3.0 * h1) + fi);
			float3 col = lerp(warm, warm * float3(1.0, 0.85, 0.7), h1);
			float fade = smoothstep(0.72, 0.6, y) * smoothstep(-0.25, -0.05, y);
			float lum = (0.35 + 0.65 * depth) * fade;
			light += col * body * (0.55 + 1.3 * inner) * flick * lum;
			light += col * exp(-length(q) / 2.4) * 0.12 * lum * flick;
		}
	}
	return light;
}

float4 PSModernSkyLanterns(VertData v_in) : TARGET
{
	float2 uv = view_uv(v_in.uv);
	float aspect = res.x / res.y;
	float2 p = float2(uv.x * aspect, uv.y);
	float px = 1.0 / res.y;
	float horizon = 0.7;
	float n = clamp(floor(7.0 + 5.0 * density), 1.0, 14.0);
	float3 warm = md_tint(float3(1.0, 0.55, 0.2), cm_hue(hue + 0.08 + react_mix.z * 0.06 * calm_in.z, uv, uv.y, 0.0));
	float glow = 0.75 + 0.6 * react_mix.y * calm_in.x + 0.15 * calm_in.w;
	float3 light = float3(0.0, 0.0, 0.0);

	/* dusk sky: deep blue fading to a warm haze at the water */
	float sky = 1.0 - step(horizon, uv.y);
	float3 skyc = lerp(float3(0.01, 0.015, 0.05), float3(0.09, 0.05, 0.07), pow(saturate(uv.y / horizon), 2.0));
	light += skyc * sky + warm * 0.05 * exp(-abs(uv.y - horizon) * 18.0);
	light += md_stars(p, 90.0, 2.0) * sky * (1.0 - uv.y / horizon) * 0.8;
	light += md_lanterns(p, aspect, px, n, warm) * glow * sky;

	/* far shore */
	float ridge = horizon - 0.012 - 0.01 * vnoise(float2(p.x * 9.0, 1.0));
	float land = step(ridge, uv.y) * step(uv.y, horizon);
	light *= 1.0 - land;
	light += float3(0.004, 0.004, 0.008) * land;

	/* the lake: a mirror, broken into slow ripples */
	float under = step(horizon, uv.y);
	float dy = uv.y - horizon;
	float rip = sin(dy * 260.0 / (0.3 + dy * 6.0) - time * 1.2 + vnoise(float2(p.x * 6.0, time * 0.2)) * 4.0);
	float2 rp = float2(p.x + rip * 0.004 * (0.3 + dy * 4.0) * (0.6 + 0.6 * react_mix.x * calm_in.y), horizon - dy * 1.05);
	float3 refl = md_lanterns(rp, aspect, px * 1.8, n, warm) * glow;
	float3 skyr = lerp(float3(0.09, 0.05, 0.07), float3(0.01, 0.015, 0.05), saturate(dy * 3.0));
	float fres = 0.35 + 0.35 * saturate(1.0 - dy * 3.0);
	light += (refl * fres * (0.75 + 0.25 * rip) + skyr * 0.6) * under;
	return finish_dither(light, uv, v_in.uv * res);
}

/* ------------------------------------------------------------------ */
/* Calm · Sunbeams Below: underwater, looking along the sea floor:      */
/* beams of light slanting down from a rippling surface, caustics       */
/* dancing on the sand, drifting motes. Amount = motes.                  */
float md_caustic(float2 p, float t)
{
	float2 q = p;
	float c = 0.0;
	for (int i = 0; i < 3; i++) {
		float fi = float(i);
		q = float2(q.x + sin(q.y * 1.7 + t * (0.6 + 0.2 * fi)) * 0.6, q.y + cos(q.x * 1.5 + t * (0.5 + 0.15 * fi)) * 0.6);
		c += 1.0 / (1.0 + 18.0 * abs(sin(q.x * 1.3) * sin(q.y * 1.3)));
	}
	return c / 3.0;
}

float4 PSModernSunbeams(VertData v_in) : TARGET
{
	float2 uv = view_uv(v_in.uv);
	float aspect = res.x / res.y;
	float2 p = float2(uv.x * aspect, uv.y);
	float px = 1.0 / res.y;
	float3 sea = md_tint(float3(0.05, 0.3, 0.42), cm_hue(hue + 0.52 + react_mix.z * 0.08 * calm_in.z, uv, uv.x, 0.0));
	float3 sun = md_tint(float3(0.85, 1.0, 0.95), hue + 0.4);
	float e = 0.75 + 0.5 * react_mix.y * calm_in.x + 0.2 * calm_in.w;

	/* the water column: bright under the surface, deep below */
	float3 light = lerp(sea * 0.5, sea * 0.03, smoothstep(0.0, 0.95, uv.y));
	/* the surface seen from below: a bright, wobbling sheet at the top */
	float surf = 0.08 + 0.015 * sin(p.x * 7.0 + time * 0.8) + 0.01 * sin(p.x * 17.0 - time * 1.3);
	float above = 1.0 - smoothstep(surf - 2.0 * px, surf + 2.0 * px, uv.y);
	light += (sea * 0.8 + sun * 0.4 * pow(md_caustic(p * 6.0, time * 0.7), 2.0)) * above;

	/* god rays: slanted shafts, flickering slowly as the surface moves */
	float slant = 0.32;
	float rx = p.x + uv.y * slant;
	float ray = 0.0;
	ray += pow(saturate(fbm4(float2(rx * 4.0, time * 0.06)) * 1.4 - 0.25), 2.5);
	ray += 0.6 * pow(saturate(fbm4(float2(rx * 9.0 + 3.0, time * 0.09)) * 1.4 - 0.3), 3.0);
	float raydepth = exp(-max(uv.y - surf, 0.0) * 2.3);
	light += sun * ray * raydepth * 0.55 * e * (1.0 - above);

	/* the sea floor: sand in perspective with caustics */
	float floory = 0.7 + 0.03 * sin(p.x * 2.0 + 1.0);
	float ground = smoothstep(floory - 0.01, floory + 0.03, uv.y);
	float dz = max(uv.y - floory, 0.002);
	float2 gp = float2((p.x - aspect * 0.5) / (dz * 3.0 + 0.1), 1.0 / (dz * 3.0 + 0.1));
	float caus = md_caustic(gp * 1.6, time * (0.5 + 0.4 * react_mix.x * calm_in.y));
	float3 sandc = float3(0.12, 0.13, 0.11) * (0.7 + 0.3 * vnoise(gp * 4.0));
	light = lerp(light, sandc * sea * 2.0 + sun * pow(caus, 3.0) * 0.5 * e, ground * 0.95);
	/* haze over the far floor */
	light += sea * 0.12 * exp(-abs(uv.y - floory) * 14.0);

	/* drifting motes catching the light */
	float nm = 0.5 + 0.5 * clamp(density, 0.0, 3.0);
	for (int k = 0; k < 2; k++) {
		float fk = float(k);
		float s = 22.0 + 18.0 * fk;
		float2 q = p * s + float2(time * 0.15 * (1.0 + fk), sin(time * 0.2 + fk) * 0.4 + time * 0.05);
		float2 id = floor(q);
		float2 f = frac(q) - 0.5;
		float h = hash21(id + fk * 31.0);
		float2 o = float2(hash21(id + 1.7), hash21(id + 5.3)) - 0.5;
		float d = length(f - o * 0.7) / s;
		float on = step(1.0 - 0.18 * nm, h);
		light += sun * on * exp(-d * d / (px * px * (1.0 + 2.0 * fk))) * (0.25 + 0.6 * ray) * 0.9;
	}
	return finish_dither(light, uv, v_in.uv * res);
}

/* ------------------------------------------------------------------ */
/* Calm · Blossom Breeze: cherry blossom petals tumbling on the wind at */
/* four depths over a soft dusk with out-of-focus lights.               */
/* Amount = how many petals.                                             */
float4 PSModernBlossoms(VertData v_in) : TARGET
{
	float2 uv = view_uv(v_in.uv);
	float aspect = res.x / res.y;
	float2 p = float2(uv.x * aspect, uv.y);
	float px = 1.0 / res.y;
	float3 pink = md_tint(float3(1.0, 0.62, 0.75), cm_hue(hue + 0.92 + react_mix.z * 0.08 * calm_in.z, uv, uv.x, 0.0));
	float3 skyA = md_tint(float3(0.16, 0.1, 0.22), hue + 0.75);
	float3 skyB = md_tint(float3(0.32, 0.17, 0.25), hue + 0.95);
	float3 light = lerp(skyA * 0.6, skyB * 0.8, smoothstep(0.0, 1.0, uv.y));

	/* soft bokeh lights far behind */
	for (int b = 0; b < 8; b++) {
		float fb = float(b);
		float2 c = float2(hash11(fb * 3.3 + 1.0) * aspect, 0.25 + 0.6 * hash11(fb * 7.7 + 2.0));
		c.x += 0.03 * sin(time * 0.05 + fb);
		float r = 0.05 + 0.07 * hash11(fb * 1.9);
		float d = length(p - c);
		light += pink * 0.12 * (1.0 - smoothstep(r * 0.8, r, d)) * (0.6 + 0.4 * calm_e(fb / 8.0, 0.6));
	}

	float wind = 0.06 + 0.05 * react_mix.x * calm_in.y + 0.02 * sin(time * 0.13);
	float amount = clamp(density, 0.2, 3.0);
	for (int l = 0; l < 4; l++) {
		float fl = float(l);
		float depth = (fl + 1.0) / 4.0;             /* far .. near */
		float s = 9.0 - 5.0 * depth;                /* cells per unit: near cells are bigger */
		float2 q = p * s + float2(-time * wind * s * (0.6 + depth), -time * 0.05 * s * (0.5 + depth));
		q.y += sin(q.x * 0.7 + time * 0.3 + fl) * 0.3;
		float2 id = floor(q);
		float2 f = frac(q) - 0.5;
		float h = hash21(id + fl * 17.0);
		if (h < 0.35 * amount) {
			float2 o = (float2(hash21(id + 2.1), hash21(id + 4.7)) - 0.5) * 0.5;
			float ang = time * (0.4 + 1.2 * hash21(id + 9.3)) + h * 30.0;
			float2 lq = rot2(f - o, ang);
			/* flutter: the petal turns, so it squashes */
			float flip = 0.35 + 0.65 * abs(sin(time * (0.7 + h) + h * 12.0));
			lq.y /= flip;
			float sz = 0.18 + 0.08 * hash21(id + 6.6);
			/* petal: an ellipse with a small notch at its tip */
			float d = length(float2(lq.x * 1.6, lq.y)) - sz;
			d = max(d, -(length(lq - float2(0.0, sz * 1.05)) - sz * 0.22));
			float upx = px * s;
			float blur = upx * (1.0 + 7.0 * pow(depth, 3.0) + 3.0 * (1.0 - depth));
			float m = 1.0 - smoothstep(-blur, blur, d);
			float shade = 0.75 + 0.25 * lq.y / sz + 0.2 * flip;
			float3 pc = lerp(pink, float3(1.0, 0.95, 0.97), 0.25 * (1.0 - abs(lq.x) / sz));
			light = lerp(light, pc * shade * (0.45 + 0.45 * depth) * (0.85 + 0.35 * react_mix.y * calm_in.x),
				     m * (0.55 + 0.4 * depth));
		}
	}
	return finish_dither(light, uv, v_in.uv * res);
}

/* ------------------------------------------------------------------ */
/* Calm · Winter Night: snow falling at four depths, near flakes soft   */
/* and large, past street lamps glowing in the dark, a snowy ground     */
/* glinting. Amount = how heavy the snow.                                */
float4 PSModernSnowfall(VertData v_in) : TARGET
{
	float2 uv = view_uv(v_in.uv);
	float aspect = res.x / res.y;
	float2 p = float2(uv.x * aspect, uv.y);
	float px = 1.0 / res.y;
	float3 lamp = md_tint(float3(1.0, 0.72, 0.4), cm_hue(hue + 0.1 + react_mix.z * 0.06 * calm_in.z, uv, uv.x, 0.0));
	float3 night = md_tint(float3(0.03, 0.05, 0.11), hue + 0.62);
	float3 light = lerp(night * 0.6, night * 1.3, smoothstep(0.0, 0.8, uv.y));
	float glow = 0.8 + 0.5 * react_mix.y * calm_in.x + 0.15 * calm_in.w;

	/* street lamps along a far path */
	float groundy = 0.8;
	for (int i = 0; i < 4; i++) {
		float fi = float(i);
		float2 c = float2((0.12 + 0.26 * fi) * aspect, 0.48 + 0.04 * fi * (1.0 - fi / 3.0));
		float d = length(p - c);
		light += lamp * (exp(-d * 9.0) * 0.5 + (1.0 - smoothstep(0.006, 0.01, d)) * 1.5) * glow * (0.7 + 0.3 * hash11(fi));
		/* the post */
		float post = step(abs(p.x - c.x), 0.0025) * step(c.y, uv.y) * step(uv.y, groundy);
		light = lerp(light, float3(0.01, 0.01, 0.015), post * 0.9);
		/* the pool of light on the snow */
		float2 pg = float2((p.x - c.x) * 1.0, (uv.y - groundy) * 6.0);
		light += lamp * 0.25 * exp(-dot(pg, pg) * 30.0) * glow * step(groundy, uv.y);
	}

	/* snowy ground, glinting */
	float ground = smoothstep(groundy - px, groundy + px, uv.y);
	float3 snow = float3(0.16, 0.18, 0.24) * (0.8 + 0.2 * vnoise(p * 20.0));
	light = lerp(light, snow + light * 0.3, ground);
	float glint = step(0.985, hash21(floor(p * 260.0))) * (0.5 + 0.5 * sin(time * 3.0 + hash21(floor(p * 260.0)) * 50.0));
	light += md3(0.8) * glint * ground;

	/* falling snow, far to near */
	float heavy = clamp(density, 0.2, 3.0);
	float sway = 0.15 + 0.15 * react_mix.x * calm_in.y;
	for (int l = 0; l < 4; l++) {
		float fl = float(l);
		float depth = (fl + 1.0) / 4.0;
		float s = 34.0 - 26.0 * depth;
		float2 q = p * s;
		q.y -= time * (0.25 + 0.6 * depth) * s * 0.3;
		q.x += sin(q.y * 0.4 + time * 0.5 + fl * 2.0) * sway * s * 0.05 + time * 0.05 * s * 0.2;
		float2 id = floor(q);
		float2 f = frac(q) - 0.5;
		float h = hash21(id + fl * 23.0);
		float on = step(1.0 - 0.45 * heavy * (0.6 + 0.4 * depth), h);
		float2 o = (float2(hash21(id + 1.3), hash21(id + 8.1)) - 0.5) * 0.6;
		float d = length(f - o) / s;
		float r = (0.0025 + 0.004 * depth * depth) * (0.7 + 0.6 * hash21(id + 3.3));
		float blur = px * (1.0 + 6.0 * depth * depth);
		float flake = 1.0 - smoothstep(r - blur, r + blur, d);
		/* flakes near a lamp catch its light */
		light += lerp(md3(0.6), lamp, 0.25) * flake * on * (0.35 + 0.55 * depth);
	}
	return finish_dither(light, uv, v_in.uv * res);
}

/* ------------------------------------------------------------------ */
/* Calm · Crystal Cave: glowing crystal clusters rising from the floor  */
/* and hanging from the roof, faceted and lit from inside, with motes   */
/* in the misty dark. Each crystal glows with its band.                 */
float4 PSModernCrystalCave(VertData v_in) : TARGET
{
	float2 uv = view_uv(v_in.uv);
	float aspect = res.x / res.y;
	float2 p = float2(uv.x * aspect, uv.y);
	float px = 1.0 / res.y;
	float3 light = md_tint(float3(0.02, 0.015, 0.04), hue + 0.7) * (0.6 + 0.5 * fbm4(p * 3.0 + time * 0.02));

	for (int i = 0; i < 12; i++) {
		float fi = float(i);
		float h1 = hash11(fi * 2.71 + 0.4);
		float h2 = hash11(fi * 6.13 + 1.9);
		float top = step(0.62, hash11(fi * 4.4 + 7.0)); /* hanging from the roof */
		float2 base = float2((0.04 + 0.92 * h1) * aspect, lerp(1.04, -0.04, top));
		float ang = (h2 - 0.5) * 0.9 + lerp(0.0, 3.14159265, top);
		float len = 0.25 + 0.35 * hash11(fi * 9.3);
		float wid = 0.035 + 0.035 * hash11(fi * 3.9 + 4.0);
		float e = calm_e(h1, 0.5);
		/* local frame: u along the crystal (up), v across */
		float2 dir = float2(sin(ang), -cos(ang));
		float2 rel = p - base;
		float u = dot(rel, dir);
		float v = dot(rel, float2(-dir.y, dir.x));
		float tipk = smoothstep(len * 0.72, len, u);
		float halfw = wid * (1.0 - tipk);
		float d = max(abs(v) - halfw, max(-u, u - len));
		float body = 1.0 - smoothstep(-px, px, d);
		/* facets: one side lit, a bright ridge down the middle */
		float side = v / max(halfw, 0.0001);
		float facet = lerp(0.55, 1.0, step(0.0, side + 0.15 * sin(u * 30.0)));
		float ridge = exp(-pow(v / (wid * 0.08 + px), 2.0)) * (1.0 - tipk * 0.5);
		float edge = exp(-pow(d / (px * 1.5), 2.0)) * step(-0.5 * wid, u);
		float inner = 0.35 + 0.65 * (1.0 - u / len);
		float h = cm_hue(hue + h2 * 0.35 + react_mix.z * 0.1 * calm_in.z, uv, u / len, fi);
		float3 col = prism(h);
		float bright = 0.45 + 0.9 * react_mix.y * e + 0.12 * calm_in.w;
		light += col * body * (facet * inner * 0.5 + ridge * 0.6) * bright;
		light += lerp(col, md3(1.0), 0.5) * edge * 0.35 * bright;
		/* light spilling into the mist around it */
		float dc = length(rel - dir * len * 0.4);
		light += col * exp(-dc / (0.12 + 0.05 * e)) * 0.08 * bright;
	}
	/* motes */
	float2 q = p * 30.0 + float2(time * 0.08, -time * 0.12);
	float2 id = floor(q);
	float2 f = frac(q) - 0.5;
	float hh = hash21(id);
	float dm = length(f - (float2(hash21(id + 2.0), hash21(id + 5.0)) - 0.5) * 0.7) / 30.0;
	light += md3(0.5) * step(0.9, hh) * exp(-dm * dm / (px * px * 1.5)) * (0.5 + 0.5 * sin(time * 2.0 + hh * 30.0));
	return finish_dither(light, uv, v_in.uv * res);
}

/* ================================================================== */
/* LIVELY                                                               */
/* ================================================================== */

/* ------------------------------------------------------------------ */
/* Neon · City Rain: a wet street at night, neon signs on dark          */
/* buildings pulsing with the music, their glow smeared across the      */
/* puddles, rain slanting down. Amount = rain.                           */
float3 md_signs(float2 p, float aspect, float px, float stretch)
{
	float3 col = float3(0.0, 0.0, 0.0);
	for (int i = 0; i < 8; i++) {
		float fi = float(i);
		float h1 = hash11(fi * 3.17 + 0.2);
		float h2 = hash11(fi * 7.91 + 1.3);
		float2 c = float2((0.07 + 0.86 * (fi + 0.5 * h1) / 8.0) * aspect, 0.18 + 0.32 * h2);
		float2 sz = float2(0.03 + 0.05 * hash11(fi * 5.5), 0.012 + 0.025 * hash11(fi * 2.2 + 9.0));
		if (hash11(fi * 1.1 + 4.0) > 0.5) {
			sz = sz.yx * float2(1.0, 1.6); /* some signs hang vertically */
		}
		float2 d2 = p - c;
		d2.y *= stretch;
		float d = abs(md_box(d2, sz));
		float e = band_level(fi / 7.0);
		float on = 0.55 + 0.45 * react_mix.y * e + 0.5 * md_beat() * step(0.6, h1);
		/* the odd sign buzzes */
		on *= 1.0 - 0.6 * step(0.86, h2) * step(0.7, hash11(floor(time * 12.0) + fi));
		float tube = exp(-d * d / ((px * 1.5) * (px * 1.5))) + exp(-d / (0.012 + 0.006 * e)) * 0.35;
		float3 c3 = prism(cm_hue(hue + fi * 0.13 + react_mix.z * 0.15 * mid, p / float2(aspect, 1.0), fi / 8.0, fi));
		col += c3 * tube * on;
	}
	return col;
}

float4 PSModernCityRain(VertData v_in) : TARGET
{
	float2 uv = view_uv(v_in.uv);
	float aspect = res.x / res.y;
	float2 p = float2(uv.x * aspect, uv.y);
	float px = 1.0 / res.y;
	float street = 0.66;
	float3 light = float3(0.0, 0.0, 0.0);

	/* sky glow and building silhouettes */
	float3 haze = prism(hue + 0.75) * 0.06;
	light += lerp(float3(0.01, 0.01, 0.025), haze, smoothstep(0.0, street, uv.y)) * step(uv.y, street);
	float bx = floor(p.x * 9.0);
	float bh = 0.05 + 0.22 * hash11(bx * 1.7 + 3.0);
	float bld = step(bh, uv.y) * step(uv.y, street);
	light *= 1.0 - bld * 0.8;
	/* lit windows */
	float2 wq = float2(p.x * 90.0, uv.y * 60.0);
	float2 wid = floor(wq);
	float2 wf = frac(wq);
	float win = step(0.72, hash21(wid + bx)) * step(0.25, wf.x) * step(wf.x, 0.75) * step(0.3, wf.y) * step(wf.y, 0.7);
	light += float3(0.25, 0.2, 0.12) * win * bld * 0.25 * (0.7 + 0.3 * hash21(wid));

	light += md_signs(p, aspect, px, 1.0) * step(uv.y, street);

	/* the wet street: the signs mirrored and smeared down into it */
	float under = step(street, uv.y);
	float dy = uv.y - street;
	float wob = vnoise(float2(p.x * 40.0, dy * 25.0 + time * 0.5)) - 0.5;
	float2 rp = float2(p.x + wob * 0.012 * (1.0 + dy * 6.0), street - dy * 0.9);
	float3 rcol = md_signs(rp, aspect, px * 3.0, 0.35);
	float puddle = smoothstep(0.35, 0.65, vnoise(float2(p.x * 3.0, dy * 9.0 + 2.0)));
	light += rcol * under * (0.25 + 0.45 * puddle) * (1.0 - smoothstep(0.0, 0.35, dy) * 0.5);
	light += float3(0.012, 0.012, 0.02) * under;
	/* ripples in the puddles, more on the beat */
	float2 rq = float2(p.x * 14.0, dy * 70.0);
	float2 rid = floor(rq);
	float rh = hash21(rid);
	float rt = frac(time * 0.7 + rh * 7.0);
	float rr = length((frac(rq) - 0.5) * float2(1.0, 0.25));
	float ring = exp(-pow((rr - rt * 0.5) / 0.03, 2.0)) * (1.0 - rt) * step(0.8 - 0.3 * md_beat(), rh);
	light += md3(0.25) * ring * under * puddle;

	/* slanting rain */
	float heavy = clamp(density, 0.2, 3.0);
	for (int l = 0; l < 2; l++) {
		float fl = float(l);
		float2 q = float2(p.x + uv.y * 0.18, uv.y) * float2(110.0 - 40.0 * fl, 3.5 - 1.2 * fl);
		q.y -= time * (5.0 + 2.0 * fl);
		float2 id = floor(q);
		float2 f = frac(q);
		float h = hash21(id + fl * 7.0);
		float drop = step(1.0 - 0.18 * heavy, h) * smoothstep(0.5, 0.0, abs(f.x - 0.5)) * smoothstep(0.0, 0.3, f.y) *
			     smoothstep(1.0, 0.6, f.y);
		light += md3(0.18 + 0.08 * fl) * drop;
	}
	return finish_dither(light, uv, v_in.uv * res);
}

/* ------------------------------------------------------------------ */
/* Neon · Hex Pulse: a honeycomb of neon cells, each lit by its band,    */
/* with rings rippling out from the centre on the beat.                  */
/* Amount = cell size (smaller cells at higher amounts).                 */
float4 PSModernHexPulse(VertData v_in) : TARGET
{
	float2 uv = view_uv(v_in.uv);
	float aspect = res.x / res.y;
	float2 p = float2((uv.x - 0.5) * aspect, uv.y - 0.5);
	float px = 1.0 / res.y;
	float scale = 7.0 + 5.0 * clamp(density, 0.2, 3.0);
	float2 q = p * scale;
	/* hex grid */
	float2 r = float2(1.0, 1.7320508);
	float2 hr = r * 0.5;
	float2 a = float2(glmod(q.x, r.x), glmod(q.y, r.y)) - hr;
	float2 b = float2(glmod(q.x - hr.x, r.x), glmod(q.y - hr.y, r.y)) - hr;
	float2 gv = dot(a, a) < dot(b, b) ? a : b;
	float2 id = q - gv;
	float2 ag = abs(gv);
	float hd = max(dot(ag, normalize(float2(1.0, 1.7320508))), ag.x); /* 0 centre .. 0.5 edge */
	float dist = length(id) / scale;
	float ang = atan2(id.y, id.x) / 6.2831853 + 0.5;
	/* a cell's band: by angle round the centre, so the spectrum wraps */
	float bandx = abs(ang * 2.0 - 1.0);
	float e = band_level(bandx);
	/* rings from the centre */
	float wave = sin(dist * 22.0 - time * 3.0 * (0.6 + 0.4 * react_mix.x));
	float pulse = exp(-pow((dist - frac(time * 0.5) * 1.2) / 0.05, 2.0)) * md_beat();
	float lightup = saturate(e * 1.3 * react_mix.y + 0.1 + 0.2 * wave * react_mix.x + pulse);
	float h = cm_hue(hue + dist * 0.6 + bandx * 0.25 + react_mix.z * 0.2 * e, uv, bandx, 0.0);
	float3 col = prism(h);
	float upx = px * scale;
	float rim = exp(-pow((hd - 0.45) / (0.02 + upx), 2.0));
	float fill = (1.0 - smoothstep(0.43, 0.45, hd)) * 0.18;
	float3 light = col * (rim * (0.25 + 1.3 * lightup) + fill * lightup);
	light += col * exp(-(0.5 - hd) * 9.0) * 0.15 * lightup;
	light *= 1.0 - smoothstep(0.55, 0.9, dist);
	return finish_dither(light, uv, v_in.uv * res);
}

/* ------------------------------------------------------------------ */
/* Neon · Tube EQ: an equaliser of glass tubes filled with glowing      */
/* gas, the gas column rising with each band, reflected in a glossy    */
/* floor. Amount = how many tubes (8..32).                               */
float4 PSModernTubeEQ(VertData v_in) : TARGET
{
	float2 uv = view_uv(v_in.uv);
	float aspect = res.x / res.y;
	float px = 1.0 / res.y;
	float n = clamp(floor(12.0 + 8.0 * density), 8.0, 32.0);
	float floory = 0.82;
	float3 light = float3(0.0, 0.0, 0.0);

	float y = uv.y;
	float refl = step(floory, y);
	if (y > floory) {
		y = floory - (y - floory) * 1.6;
	}
	float xs = (uv.x - 0.06) / 0.88 * n;
	float i = floor(xs);
	float fx = frac(xs) - 0.5;
	float inside = step(0.0, i) * step(i, n - 1.0);
	float4 bf = band_full((i + 0.5) / n);
	float lvl = saturate(bf.r * (0.6 + 0.6 * react_mix.x) + 0.04);
	float top = floory - 0.66 * lvl;
	float tubew = 0.32;
	float upx = px * n / (0.88 * aspect);
	/* glass: faint walls and a highlight */
	float wall = exp(-pow((abs(fx) - tubew) / (upx * 1.2), 2.0)) * step(0.14, y) * step(y, floory);
	float cap = exp(-pow((y - 0.14) / (px * 1.5), 2.0)) * step(abs(fx), tubew);
	/* the gas column */
	float gas = step(abs(fx), tubew * 0.75) * smoothstep(top - px * 2.0, top + px * 2.0, y) * step(y, floory);
	float core = exp(-pow(fx / (0.07 + upx), 2.0)) * smoothstep(top - 0.01, top + 0.02, y) * step(y, floory);
	float h = cm_hue(hue + i / n * 0.8 + react_mix.z * 0.15 * bf.b, uv, i / n, i);
	float3 col = prism(h);
	float flick = 0.92 + 0.08 * sin(time * 40.0 + i * 3.0);
	float bright = (0.6 + 0.6 * react_mix.y * bf.r + 0.4 * md_beat() * bf.b) * flick;
	light += col * (gas * 0.55 + core * 1.1) * bright;
	light += col * exp(-abs(fx) * 6.0) * smoothstep(top - 0.08, top + 0.05, y) * step(y, floory) * 0.25 * bright;
	light += md3(0.12) * (wall + cap) + col * wall * 0.15;
	/* the peak marker floating above */
	float peak = floory - 0.66 * saturate(bf.g * (0.6 + 0.6 * react_mix.x) + 0.04);
	light += lerp(col, md3(1.0), 0.4) * exp(-pow((y - peak + 0.012) / (px * 1.6), 2.0)) * step(abs(fx), tubew) * 0.8;
	light *= inside;
	/* the glossy floor: a dimmer, rippled reflection */
	float fade = refl * 0.32 * exp(-(uv.y - floory) * 6.0);
	light = lerp(light, light * fade, refl);
	light += prism(hue) * 0.03 * exp(-abs(uv.y - floory) * 40.0);
	return finish_dither(light, uv, v_in.uv * res);
}

/* ------------------------------------------------------------------ */
/* Modern · Liquid Metal: blobs of mercury merging and splitting,       */
/* reflecting a coloured studio. Bass swells them; beats ripple them.  */
/* Amount = how many blobs.                                              */
float4 PSModernLiquidMetal(VertData v_in) : TARGET
{
	float2 uv = view_uv(v_in.uv);
	float aspect = res.x / res.y;
	float2 p = float2((uv.x - 0.5) * aspect, uv.y - 0.5);
	float px = 1.0 / res.y;
	float n = clamp(floor(4.0 + 2.0 * density), 3.0, 9.0);
	float field = 0.0;
	float2 grad = float2(0.0, 0.0);
	for (int i = 0; i < 9; i++) {
		float fi = float(i);
		if (fi < n) {
			float h1 = hash11(fi * 1.7 + 0.3);
			float h2 = hash11(fi * 5.9 + 2.1);
			float2 c = float2(0.55 * aspect * 0.5 * sin(time * (0.21 + 0.13 * h1) + fi * 2.3),
					  0.32 * cos(time * (0.17 + 0.11 * h2) + fi * 1.4));
			float e = band_level(fi / max(n - 1.0, 1.0));
			float rr = (0.07 + 0.04 * h2) * (1.0 + 0.45 * react_mix.x * (0.6 * bass + 0.4 * e));
			float2 d = p - c;
			float d2 = dot(d, d) + 0.0001;
			float f = rr * rr / d2;
			field += f;
			grad += -2.0 * f / d2 * d;
		}
	}
	/* a beat sends a ripple across the surface */
	field += 0.06 * md_beat() * sin(length(p) * 60.0 - time * 12.0);
	float th = 1.0;
	float gl = length(grad);
	float sd = (field - th) / max(gl, 0.0001); /* ~ distance inside the surface */
	float m = smoothstep(-px * 1.2, px * 1.2, sd);
	/* a normal from the field's slope: steep at the rim, flat in the middle */
	float2 nxy = -grad / (gl + 6.0);
	float3 nrm = normalize(float3(nxy * 1.6, 1.0));
	/* the studio it reflects: a dark room with coloured light strips */
	float h = cm_hue(hue + react_mix.z * 0.2 * mid, uv, nrm.x * 0.5 + 0.5, 0.0);
	float3 ca = prism(h);
	float3 cb = prism(h + 0.45);
	float sky = nrm.y * 0.5 + 0.5;
	float3 env = lerp(ca * 0.9, cb * 0.5, smoothstep(0.2, 0.8, sky));
	env = lerp(env, md3(0.05), smoothstep(0.45, 0.55, sky) * 0.85);
	float strip = exp(-pow((nrm.x - 0.35) / 0.06, 2.0)) + exp(-pow((nrm.y + 0.55) / 0.05, 2.0)) * 0.8;
	env += md3(1.2) * strip;
	float fres = pow(1.0 - nrm.z, 3.0);
	float3 metal = env * (0.35 + 0.65 * fres) + md3(0.06);
	float3 spec = md3(1.0) * pow(saturate(dot(nrm, normalize(float3(-0.4, -0.5, 0.75)))), 60.0);
	float bright = 0.8 + 0.5 * react_mix.y * level;
	float3 surf = (metal + spec) * bright;
	/* glow of the coloured light around the blobs */
	float halo = saturate(field) * (1.0 - m);
	float3 light = surf * m + ca * pow(halo, 3.0) * 0.25;
	return finish_dither(light, uv, v_in.uv * res);
}

/* ------------------------------------------------------------------ */
/* Modern · Ferrofluid: a pool of black magnetic fluid that stands up   */
/* in glossy spikes, each spike a band of the music, rimmed in colour. */
/* Amount = how many spikes.                                             */
float md_ferro_r(float a, float n, float t)
{
	float ai = a * n / 6.2831853;
	float k = floor(ai);
	float fk = frac(ai) - 0.5;
	float e = band_level(abs(glmod(k, n) / n * 2.0 - 1.0));
	float spike = pow(saturate(1.0 - abs(fk) * 2.0), 2.2) * (0.05 + 0.32 * e * (0.5 + 0.7 * react_mix.x));
	return 0.2 + 0.012 * sin(a * 3.0 + t) + spike;
}

float4 PSModernFerrofluid(VertData v_in) : TARGET
{
	float2 uv = view_uv(v_in.uv);
	float aspect = res.x / res.y;
	float2 p = float2((uv.x - 0.5) * aspect, uv.y - 0.5);
	float px = 1.0 / res.y;
	float n = clamp(floor(16.0 + 10.0 * density), 8.0, 48.0);
	float rot = time * 0.08;
	float a = atan2(p.y, p.x) + rot;
	float r = length(p);
	float R = md_ferro_r(a, n, time) * (1.0 + 0.06 * md_beat());
	float da = 0.004;
	float Rp = md_ferro_r(a + da, n, time);
	float Rm = md_ferro_r(a - da, n, time);
	float slope = (Rp - Rm) / (2.0 * da);
	float sd = r - R;
	float m = 1.0 - smoothstep(-px * 1.2, px * 1.2, sd / sqrt(1.0 + slope * slope / max(r * r, 0.0001)));
	/* the surface leans out along the spikes: shade with a fake normal */
	float inner = saturate(r / max(R, 0.0001));
	float2 radial = p / max(r, 0.0001);
	float2 tang = float2(-radial.y, radial.x);
	float2 n2 = radial * pow(inner, 4.0) - tang * slope / max(r, 0.05) * 0.4;
	float3 nrm = normalize(float3(n2, 0.7));
	float h = cm_hue(hue + react_mix.z * 0.15 * mid, uv, a / 6.2831853, 0.0);
	float3 c1 = prism(h);
	float3 c2 = prism(h + 0.33);
	float spec = pow(saturate(dot(nrm, normalize(float3(-0.5, -0.6, 0.6)))), 40.0);
	float rim = pow(saturate(dot(nrm.xy, normalize(float2(0.4, 0.6))) * 0.5 + 0.5), 6.0);
	float3 fluid = md3(0.015) + c1 * rim * 0.6 + c2 * pow(1.0 - nrm.z, 2.0) * 0.25 + md3(1.0) * spec;
	float bright = 0.8 + 0.5 * react_mix.y * level;
	float3 light = fluid * m * bright;
	/* coloured light behind it and its reflection on the dish */
	light += c1 * exp(-max(sd, 0.0) * 9.0) * 0.18 * (1.0 - m) * bright;
	light += c2 * 0.06 * exp(-abs(r - 0.42) * 30.0) * (1.0 - m);
	return finish_dither(light, uv, v_in.uv * res);
}

/* ------------------------------------------------------------------ */
/* Modern · Sound Map: a glowing contour map of hills and valleys that  */
/* rise with the music, lines brightening near the peaks.               */
/* Amount = how many contour lines.                                      */
float md_terrain(float2 q)
{
	float h = fbm4(q * 1.4 + float2(time * 0.04, 0.0));
	float x = saturate(q.x / 6.0 + 0.5);
	h += 0.45 * band_level(x) * react_mix.x * exp(-pow(q.y * 0.6, 2.0));
	h += 0.15 * bass * react_mix.x * exp(-dot(q, q) * 0.15);
	return h;
}

float4 PSModernSoundMap(VertData v_in) : TARGET
{
	float2 uv = view_uv(v_in.uv);
	float aspect = res.x / res.y;
	float2 q = float2((uv.x - 0.5) * aspect, uv.y - 0.5) * 5.0;
	float px = 5.0 / res.y;
	float lines = 10.0 + 8.0 * clamp(density, 0.2, 3.0);
	float h = md_terrain(q);
	float hx = md_terrain(q + float2(px, 0.0));
	float hy = md_terrain(q + float2(0.0, px));
	float2 g = float2(hx - h, hy - h) / px;
	float gl = length(g) + 0.0001;
	float v = h * lines;
	float fv = abs(frac(v) - 0.5);
	/* line width in pixels, steady however steep the hill */
	float dpx = (0.5 - fv) / (gl * lines * px); /* pixels to the nearest contour */
	float cline = 1.0 - smoothstep(0.5, 1.5, dpx);
	float major = step(glmod(floor(v + 0.5), 5.0), 0.01);
	float hgt = saturate(h);
	float col_h = cm_hue(hue + hgt * 0.6 + react_mix.z * 0.15 * mid, uv, hgt, 0.0);
	float3 col = prism(col_h);
	float bright = (0.35 + 1.0 * pow(hgt, 2.0)) * (0.7 + 0.6 * react_mix.y * level) + 0.4 * md_beat() * pow(hgt, 3.0);
	float3 light = col * cline * (0.7 + 0.6 * major) * bright;
	/* soft hill shading and glow on the peaks */
	float shade = saturate(dot(normalize(float3(-g * 0.4, 1.0)), normalize(float3(-0.5, -0.6, 0.6))));
	light += col * 0.05 * shade + col * pow(hgt, 4.0) * 0.15;
	return finish_dither(light, uv, v_in.uv * res);
}

/* ------------------------------------------------------------------ */
/* Modern · Holo Foil: crinkled holographic foil catching the light,   */
/* its rainbow sheen rolling as the music moves it, with glints.       */
/* Amount = how crinkled.                                                */
float4 PSModernHoloFoil(VertData v_in) : TARGET
{
	float2 uv = view_uv(v_in.uv);
	float aspect = res.x / res.y;
	float2 p = float2(uv.x * aspect, uv.y);
	float px = 1.0 / res.y;
	float k = 2.0 + 3.0 * clamp(density, 0.2, 3.0);
	float t = time * 0.12;
	/* height of the foil: big soft folds plus sharp crinkles */
	float2 w = p * k + float2(t, -t * 0.7);
	float e = 0.01;
	float hh = fbm4(w) + 0.35 * abs(vnoise(w * 3.1 + 7.0) - 0.5);
	float hx = fbm4(w + float2(e, 0.0)) + 0.35 * abs(vnoise((w + float2(e, 0.0)) * 3.1 + 7.0) - 0.5);
	float hy = fbm4(w + float2(0.0, e)) + 0.35 * abs(vnoise((w + float2(0.0, e)) * 3.1 + 7.0) - 0.5);
	float sway = 0.5 + 0.8 * react_mix.x * (0.5 * bass + 0.5 * mid);
	float3 nrm = normalize(float3((hh - hx) / e * 0.06 * sway, (hh - hy) / e * 0.06 * sway, 1.0));
	/* a light sweeping slowly across */
	float3 ldir = normalize(float3(sin(time * 0.3) * 0.6, cos(time * 0.23) * 0.5, 0.8));
	float ndl = dot(nrm, ldir);
	/* thin-film rainbow: colour from the viewing angle */
	float film = dot(nrm.xy, float2(0.8, 0.6)) * 3.5 + uv.x * 0.6 + hh * 1.5;
	float h = cm_hue(hue + film + react_mix.z * 0.3 * mid, uv, frac(film), 0.0);
	float3 irid = prism(h);
	float spec = pow(saturate(ndl), 28.0);
	float glint = pow(saturate(ndl), 300.0) * step(0.6, hash21(floor(p * 160.0)));
	float bright = 0.55 + 0.6 * react_mix.y * level + 0.35 * md_beat();
	float3 light = irid * (0.35 + 0.65 * saturate(ndl)) * bright + md3(1.0) * (spec * 0.6 + glint * 2.0) * bright;
	/* sparkle drifting over it on the beat */
	float sp = step(0.995, hash21(floor(p * 240.0) + floor(time * 8.0))) * md_beat();
	light += md3(1.5) * sp;
	return finish_dither(light, uv, v_in.uv * res);
}

/* ------------------------------------------------------------------ */
/* Neon · Laser Show: club lasers fanning out of the floor through haze */
/* sweeping with the music, the beams fanning wider on the beat.        */
/* Amount = beams per emitter.                                           */
float4 PSModernLasers(VertData v_in) : TARGET
{
	float2 uv = view_uv(v_in.uv);
	float aspect = res.x / res.y;
	float2 p = float2(uv.x * aspect, uv.y);
	float px = 1.0 / res.y;
	float haze = 0.45 + 0.55 * fbm4(p * 2.5 + float2(time * 0.05, -time * 0.08));
	float nb = clamp(floor(4.0 + 3.0 * density), 2.0, 10.0);
	float3 light = float3(0.0, 0.0, 0.0);
	for (int em = 0; em < 3; em++) {
		float fe = float(em);
		float2 org = float2((0.2 + 0.3 * fe) * aspect, 1.02);
		float e = band_level(0.15 + 0.35 * fe);
		float sweep = sin(time * (0.35 + 0.15 * fe) + fe * 2.0) * 0.6 * (0.4 + 0.8 * react_mix.x * (0.5 + e));
		float fan = 0.12 + 0.14 * react_mix.x * mid + 0.25 * md_beat();
		float h = cm_hue(hue + fe * 0.33 + react_mix.z * 0.2 * e, uv, fe / 3.0, fe);
		float3 col = prism(h);
		for (int b = 0; b < 10; b++) {
			float fb = float(b);
			if (fb < nb) {
				float ang = sweep + (fb / max(nb - 1.0, 1.0) - 0.5) * 2.0 * fan;
				float2 dir = float2(sin(ang), -cos(ang));
				float2 rel = p - org;
				float along = dot(rel, dir);
				float across = abs(dot(rel, float2(-dir.y, dir.x)));
				float w = px * (0.8 + along * 2.0);
				float beam = exp(-across * across / (w * w)) + exp(-across / (0.004 + along * 0.01)) * 0.15;
				beam *= step(0.0, along) * exp(-along * 0.6);
				float on = 0.5 + 0.5 * react_mix.y * e + 0.6 * md_beat();
				light += col * beam * (0.4 + 0.9 * haze) * on;
			}
		}
		/* the emitter's hot spot */
		float de = length(p - org);
		light += col * exp(-de * 40.0) * 0.6;
	}
	/* a little haze lit by everything */
	light += prism(hue) * 0.02 * haze;
	return finish_dither(light, uv, v_in.uv * res);
}

/* ================================================================== */
/* techniques                                                           */
/* ================================================================== */

technique ModernSoftOrbs
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSModernSoftOrbs(v_in);
	}
}

technique ModernJellyfish
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSModernJellyfish(v_in);
	}
}

technique ModernGlowTide
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSModernGlowTide(v_in);
	}
}

technique ModernSkyLanterns
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSModernSkyLanterns(v_in);
	}
}

technique ModernSunbeams
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSModernSunbeams(v_in);
	}
}

technique ModernBlossoms
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSModernBlossoms(v_in);
	}
}

technique ModernSnowfall
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSModernSnowfall(v_in);
	}
}

technique ModernCrystalCave
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSModernCrystalCave(v_in);
	}
}

technique ModernCityRain
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSModernCityRain(v_in);
	}
}

technique ModernHexPulse
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSModernHexPulse(v_in);
	}
}

technique ModernTubeEQ
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSModernTubeEQ(v_in);
	}
}

technique ModernLiquidMetal
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSModernLiquidMetal(v_in);
	}
}

technique ModernFerrofluid
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSModernFerrofluid(v_in);
	}
}

technique ModernSoundMap
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSModernSoundMap(v_in);
	}
}

technique ModernHoloFoil
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSModernHoloFoil(v_in);
	}
}

technique ModernLasers
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSModernLasers(v_in);
	}
}
