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
	return step(0.94, h) * exp(-d * d / (px * px * 1.3)) * tw * (0.4 + 0.8 * hash21(id + seed + 1.3));
}

/* palette colour for the style's own warm/cool tint: base when the rainbow
 * palette is on, the chosen palette otherwise */
float3 md_tint(float3 base, float h)
{
	float3 pc = prism(h);
	float lb = dot(base, float3(0.299, 0.587, 0.114));
	float lp = max(dot(pc, float3(0.299, 0.587, 0.114)), 0.05);
	return lerp(base, pc * (lb / lp), 0.12 + 0.75 * pal_mix);
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
/* Calm · Jellyfish: moon jellies, sea nettles and comb jellies         */
/* drifting through deep water. A bell swims by pulsing: a quick        */
/* squeeze, then a long glide while it relaxes, and the tentacles and   */
/* oral arms trail behind its path. On a beat the bell gives a stronger */
/* (eased) pulse and the colours step on; the bass swells the glow.     */
/* Style settings (style_a/b): x count, y size, z drift, w species      */
/* (moon, nettle, comb, mixed); b.x beat pulse, b.y colour on beat,     */
/* b.z tentacle length, b.w glow.                                        */

/* How a jellyfish swims (moon jellies and sea nettles are "rowers"): the
 * bell contracts quickly (about a quarter to a third of each pulse), then
 * relaxes slowly and passively as its jelly springs back. Speed peaks near
 * the end of the squeeze and decays like a coasting body under drag
 * through the long relaxation, so it surges and glides rather than moving
 * steadily. The squeeze runs as a wave from the top of the bell to the
 * rim, so the margin paddles in a little after the dome. Each jelly keeps
 * its own pulse rate, rhythm drift, stroke and path, so no two swim in
 * step. Comb jellies don't pulse: their combs row them along smoothly.
 * Everything is closed-form in time (no state), so it's smooth at any t. */

/* per jelly: x pulses/s, y squeeze fraction of a pulse, z glide decay,
 * w stroke length (screen heights per pulse) */
float4 jf_rates(float fi, float sp)
{
	float a = hash11(fi * 97.37 + 2.0);
	float b = hash11(fi * 12.83 + 5.1);
	float c = hash11(fi * 71.19 + 0.7);
	float f = (0.38 + 0.4 * a) * (0.35 + 0.65 * sp); /* ~0.4..0.8 Hz at drift 1 */
	return float4(f, 0.23 + 0.11 * b, 3.2 + 3.0 * c, (0.035 + 0.03 * b) * (0.3 + 0.7 * sp));
}

/* the pulse phase: its own rate, slowly speeding up and easing off (up to
 * +/-40%, never stopping), so the rhythms drift apart */
float jf_phase(float fi, float t, float f)
{
	float w = 0.11 + 0.12 * hash11(fi * 33.7 + 9.2);
	float ph = hash11(fi * 5.31 + 3.3) * 6.2831853;
	return f * t + hash11(fi * 18.9 + 1.4) + (0.4 * f / w) * sin(w * t + ph);
}

/* distance covered within one pulse (0..1 of a stroke) at phase u in 0..1:
 * speed rises as (1 - cos) through the squeeze to its peak, then decays as
 * exp(-k) while coasting; integrated in closed form and normalised */
float jf_dist(float u, float c, float k)
{
	float tot = c * 0.5 + (1.0 - exp(-k * (1.0 - c))) / k;
	float d = u < c ? (u * 0.5 - c * 0.159155 * sin(3.1415927 * u / c))
			: (c * 0.5 + (1.0 - exp(-k * (u - c))) / k);
	return d / tot;
}

/* speed at phase u, 0..1 of the peak */
float jf_speed(float u, float c, float k)
{
	return u < c ? 0.5 - 0.5 * cos(3.1415927 * u / c) : exp(-k * (u - c));
}

/* the bell's squeeze at phase u (0 relaxed .. 1 fully in): eased in during
 * the squeeze, then an elastic relax that slows as it nears rest */
float jf_squeeze(float u, float c)
{
	float uu = frac(u);
	float rel = saturate((uu - c) / (1.0 - c));
	return uu < c ? smoothstep(0.0, c, uu) : pow(1.0 - rel, 2.2) * (1.0 + 0.6 * rel);
}

/* sideways path: a gentle S-curve tied to the distance swum (so the body
 * leans exactly along its path), plus a slow sideways current in time */
float jf_px(float fi, float s, float t, float aspect)
{
	float x0 = (0.12 + 0.76 * hash11(fi * 56.71 + 1.1)) * aspect;
	float ax = 0.03 + 0.05 * hash11(fi * 8.13 + 2.2);
	float kx = 2.0 + 2.5 * hash11(fi * 3.77 + 6.6);
	float ph = hash11(fi * 41.3 + 0.9) * 6.2831853;
	return x0 + ax * sin(s * kx + ph) + 0.04 * sin(t * (0.02 + 0.02 * hash11(fi * 2.9)) + fi * 2.1);
}

/* the path's slope at distance s (sideways per unit swum) */
float jf_slope(float fi, float s)
{
	float ax = 0.03 + 0.05 * hash11(fi * 8.13 + 2.2);
	float kx = 2.0 + 2.5 * hash11(fi * 3.77 + 6.6);
	float ph = hash11(fi * 41.3 + 0.9) * 6.2831853;
	return ax * kx * cos(s * kx + ph);
}

float4 PSModernJellyfish(VertData v_in) : TARGET
{
	float2 uv = view_uv(v_in.uv);
	float aspect = res.x / res.y;
	float2 p = float2(uv.x * aspect, uv.y);
	float px = 1.0 / res.y;

	float count = clamp(floor(style_a.x + 0.5), 1.0, 9.0);
	float ksize = style_a.y;
	float drift = style_a.z;
	float species = style_a.w;
	float kpulse = style_b.x * react_mix.w;
	float kglow = style_b.w;
	float tlen = style_b.z;
	/* colour steps on with each beat; whole 1/64 steps so the 64-beat wrap
	 * of the glide never jumps */
	float cstep = floor(style_b.y * react_mix.z * 8.0 + 0.5) / 64.0;
	float beatshift = music_ease.x * cstep;
	float swell = (0.85 + 0.5 * react_mix.y * (music_ease.z * 0.6 + calm_in.x * 0.5)) * kglow;

	/* deep water, lighter towards the surface */
	float3 water = md_tint(float3(0.03, 0.15, 0.23), hue + 0.5);
	float3 light = lerp(water * 0.6, water * 0.05, smoothstep(0.0, 1.0, uv.y));
	/* slow shafts of light from above */
	float rx = p.x + uv.y * 0.3;
	float shafts = fbm4(float2(rx * 3.0 + time * 0.015, time * 0.03));
	light += water * 0.8 * pow(saturate(shafts * 1.25 - 0.15), 3.0) * (1.0 - uv.y) * (1.0 + 0.4 * calm_in.w);
	/* marine snow, two depths, sinking slowly */
	for (int k = 0; k < 2; k++) {
		float fk = float(k);
		float s = 18.0 + 22.0 * fk;
		float2 q = p * s + float2(sin(time * 0.08 + fk) * 0.5, -time * (0.12 + 0.1 * fk));
		float2 id = floor(q);
		float2 f = frac(q) - 0.5;
		float h = hash21(id + fk * 13.0);
		float2 o = float2(hash21(id + 3.1), hash21(id + 7.7)) - 0.5;
		float d = length(f - o * 0.7) / s;
		light += md3(0.1 + 0.08 * fk) * step(0.9, h) * exp(-d * d / (px * px * (1.2 + 1.8 * (1.0 - fk))));
	}

	for (int i = 0; i < 9; i++) {
		float fi = float(i);
		if (fi < count) {
			float h1 = hash11(fi * 39.59 + 0.3);
			float h3 = hash11(fi * 24.61 + 7.0);
			/* far ones first, so nearer ones sit in front */
			float depth = saturate((fi + 0.25 + 0.5 * h3) / count);
			float rr = (0.06 + 0.085 * depth) * ksize; /* bell radius */
			/* species: moon, nettle, comb, or a mix */
			float sp_k = species;
			if (species > 2.5) {
				float hs = hash11(fi * 65.27 + 4.4);
				sp_k = hs < 0.45 ? 0.0 : (hs < 0.8 ? 1.0 : 2.0);
			}
			float is_moon = 1.0 - step(0.5, sp_k);
			float is_nettle = step(0.5, sp_k) * (1.0 - step(1.5, sp_k));
			float is_comb = step(1.5, sp_k);

			/* its own slow drift in depth: a little nearer/further over time */
			rr *= 1.0 + 0.08 * sin(time * (0.03 + 0.03 * h3) + fi * 1.7);
			float tl = rr * tlen * (1.2 * is_moon + 4.2 * is_nettle + 2.6 * is_comb);
			float span = 1.0 + rr * 2.0 + tl + 0.1;

			/* ---- swimming ---- */
			float4 jr = jf_rates(fi, drift);
			float u = jf_phase(fi, time, jr.x);
			float uf = frac(u);
			float rowk = 1.0 - is_comb;
			/* rowers: surge and glide; comb jellies: a smooth cilia glide */
			float strokes = is_comb > 0.5 ? time * jr.x * 0.55 : floor(u) + jf_dist(uf, jr.y, jr.z);
			float s = strokes * jr.w * (0.6 + 0.8 * saturate(react_mix.x));
			float vnorm = is_comb > 0.5 ? 0.5 : jf_speed(uf, jr.y, jr.z);
			/* each answers the beat in its own way: some sooner, some later
			 * (a softer or sharper take on the eased pulse), some harder */
			float gam = 0.6 + 1.8 * hash11(fi * 15.7 + 3.9);
			float bstr = 0.6 + 0.7 * hash11(fi * 27.1 + 8.8);
			float bp = pow(saturate(music_ease.y), gam) * kpulse * bstr;
			/* and each beat carries it a little further (whole 1/64 steps of
			 * its wrap, so the 64-beat wrap of the counter never jumps) */
			float kq = floor(saturate(kpulse) * (1.0 + 2.0 * hash11(fi * 6.6 + 1.0)) + 0.5) / 64.0;
			float2 c;
			c.x = jf_px(fi, s, time, aspect);
			c.y = 1.0 + rr - frac(s / span + h1 + music_ease.x * kq) * span + rr * 0.3;
			float2 d0 = p - c;
			float hc = cm_hue(hue + h1 * 0.85 + 0.08 * sp_k + beatshift, uv, saturate(d0.y / max(tl, 0.001)), fi);
			float3 col = prism(hc);
			float bright = (0.3 + 0.7 * depth) * swell;
			/* a soft glow in the water around the bell */
			float hd = length(float2(d0.x, d0.y + rr * 0.3)) / rr;
			light += col * exp(-max(hd - 0.7, 0.0) * 2.2) * 0.05 * bright * (1.0 + 0.6 * bp + 0.25 * vnorm * rowk);
			/* outside its box (bell + trailing tentacles): nothing more to do */
			if (abs(d0.x) < rr * 2.6 + tl * 0.45 && d0.y > -rr * 1.6 && d0.y < tl + rr * 0.6) {
				/* the squeeze, as a wave from the dome to the rim: the rim
				 * paddles in a little after the top. A beat squeezes harder. */
				float bsq = bp * 0.9 * rowk;
				float sq_top = saturate(jf_squeeze(u, jr.y) * 0.85 * rowk + bsq);
				float sq_rim = saturate(jf_squeeze(u - 0.07, jr.y) * 0.85 * rowk + bsq * 0.8);
				float sq = 0.5 * (sq_top + sq_rim);
				/* lean along its path, and rock a little with each stroke */
				float tilt = atan(jf_slope(fi, s)) * 0.9 + 0.06 * rowk * sin(6.2831853 * u + fi) +
					     0.04 * sin(time * 0.4 * (0.5 + h3) + fi * 3.0);
				tilt = clamp(tilt, -0.4, 0.4);
				float upx = px / rr;
				float aa = upx * (1.2 + 5.0 * (1.0 - depth)); /* far ones are soft */

				float3 pale = lerp(col, float3(1.0, 1.0, 1.0), 0.45);

				/* ---- the bell (moon and nettle) / the body (comb) ---- */
				float2 q = rot2(d0, -tilt) / rr; /* origin at the rim, y down */
				/* squeezing narrows the bell (~a fifth) and makes it taller;
				 * how much depends on the height, since the wave reaches the
				 * rim last */
				float bh = (0.55 + 0.3 * is_nettle) * (1.0 + 0.2 * sq_top);
				float lev = saturate((q.y + bh) / bh); /* 0 top .. 1 rim */
				float bw = (1.0 - 0.22 * lerp(sq_top, sq_rim, lev)) * (1.0 - 0.15 * is_nettle);
				/* the margin hangs a little lower at the edges and curls in as it paddles */
				float xn = q.x / bw;
				float rimy = (0.1 - 0.08 * sq_rim) * pow(saturate(abs(xn)), 4.0) - 0.05 * (1.0 - xn * xn) * (0.4 + 1.2 * sq_rim) +
					     (0.012 * is_moon + 0.02 * is_nettle) * abs(sin(xn * (14.0 + 6.0 * is_moon) + fi));
				float e = length(float2(xn, min(q.y, 0.0) / bh));
				float body = (1.0 - smoothstep(1.0 - aa, 1.0 + aa, e)) * (1.0 - smoothstep(rimy - aa, rimy + aa, q.y));
				body *= 1.0 - is_comb;
				float edge = smoothstep(0.55, 1.0, e) * body;
				float inner = exp(-e * e * 2.2) * body;
				/* radial canals (moon) / broad stripes (nettle), from the top of the bell */
				float ang = atan2(q.x, -(q.y + bh * 0.95));
				float canals = pow(abs(cos(ang * 9.0)), 24.0) * is_moon;
				float stripes = (0.5 + 0.5 * cos(ang * 14.0)) * is_nettle;
				/* moon jelly: four horseshoe gonads, seen through the bell */
				float gon = 0.0;
				for (int g = 0; g < 4; g++) {
					float ga = (float(g) + 0.5) * 1.5707963;
					float2 gc = float2(cos(ga) * 0.34 * bw, -bh * 0.5 + sin(ga) * 0.16 * bh);
					float2 gq = float2(q.x - gc.x, (q.y - gc.y) / 0.6);
					float rg = length(gq);
					float2 outw = normalize(float2(gc.x, (gc.y + bh * 0.5) / 0.6) + float2(0.0001, 0.0001));
					float open = smoothstep(-0.6, 0.3, -dot(gq / max(rg, 0.0001), outw));
					float gr = (rg - 0.13) / 0.075;
					gon += exp(-gr * gr) * (0.35 + 0.65 * open);
				}
				gon *= is_moon * body;
				float bell = inner * 0.2 + edge * 0.85 + canals * body * 0.2 + gon * 0.38;
				bell *= 1.0 - 0.35 * stripes;
				bell += 0.25 * smoothstep(-0.25 * bh, 0.0, q.y) * body; /* light inside the opening */
				/* a soft sheen up on the dome */
				bell += 0.35 * exp(-length(float2(q.x + 0.35 * bw, (q.y + 0.72 * bh) * 1.6)) * 7.0) * body;

				/* comb jelly: a clear egg with eight rows of rainbow-shimmering combs */
				float2 cq = float2(q.x / 0.62, (q.y + 0.55) / 0.85);
				float ce = length(cq);
				float cbody = (1.0 - smoothstep(1.0 - aa, 1.0 + aa, ce)) * is_comb;
				float combs = 0.0;
				float spin = time * 0.25 + fi;
				for (int r = 0; r < 4; r++) {
					float th = float(r) * 0.785398 + spin;
					float front = cos(th);
					float wx = sin(th) * sqrt(saturate(1.0 - cq.y * cq.y));
					float dd = abs(cq.x - wx) / (0.05 + upx * 1.5);
					float row = exp(-dd * dd) * smoothstep(0.95, 0.75, abs(cq.y));
					/* the cilia beat in waves that run down each row */
					float wave = 0.5 + 0.5 * sin(cq.y * 18.0 - time * (4.0 + 6.0 * bp) + float(r));
					combs += row * (0.35 + 0.65 * wave) * (0.4 + 0.6 * saturate(front + 0.4));
					/* and the row behind, mirrored */
					float dd2 = abs(cq.x + wx) / (0.05 + upx * 1.5);
					combs += exp(-dd2 * dd2) * smoothstep(0.95, 0.75, abs(cq.y)) * 0.25 * wave;
				}
				combs *= cbody;
				float cedge = smoothstep(0.6, 1.0, ce) * cbody;
				float3 comb_col = prism(hc + cq.y * 0.6 - time * 0.3);
				cedge *= 0.55;

				/* ---- what trails behind: offset along the path it came by ---- */
				/* tentacles follow the path it swam: a point this far down
				 * is where the bell was that much swimming ago (and a
				 * little time ago, for the current) */
				/* in the bell's own frame (it leans), so they hang from its rim */
				float sraw = max(q.y * rr, 0.0);
				float back = sraw * (0.85 + 0.3 * vnorm);
				float lagx = (jf_px(fi, s - back, time - back * 1.5, aspect) - c.x) / rr;
				/* fast on the stroke they stream out straight; coasting,
				 * they relax and bunch up a little */
				float sdown = sraw / (1.0 + 0.16 * (vnorm - 0.4) * rowk);
				float sn = sdown / max(tl, 0.0001);
				float trail = 0.0;
				float arms = 0.0;
				if (sdown > 0.0 && sn < 1.0) {
					float qx = q.x - lagx * (1.0 - 0.6 * is_comb);
					float yq = sdown / rr;
					/* each pulse sends a ripple down the tentacles and arms
					 * (lagging with distance from the bell), over a slow sway
					 * of their own */
					float ph = 6.2831853 * u * rowk + time * (0.5 + 0.4 * h3) + fi;
					/* moon: a fringe of many fine short tentacles along the margin */
					float fr_len = 0.35 * tlen;
					if (is_moon > 0.5 && yq < fr_len + 0.12) {
						float fs = yq / max(fr_len, 0.01);
						float sway = 0.06 * sin(yq * 9.0 - ph + qx * 2.0) * fs;
						float lines = abs(frac((qx + sway) * 14.0 / bw) - 0.5);
						float m = (1.0 - smoothstep(0.08, 0.2 + upx * 14.0, lines)) * (1.0 - smoothstep(0.0, 1.0, fs));
						trail += m * (1.0 - smoothstep(bw * 0.95, bw * 1.05, abs(qx))) * 0.7;
					}
					/* long thin tentacles (nettle, comb) */
					float nt = is_nettle * 8.0 + is_comb * 2.0;
					for (int t = 0; t < 8; t++) {
						float ft = float(t);
						if (ft < nt) {
							float side = is_comb > 0.5 ? (ft - 0.5) * 0.7 : (ft / 7.0 - 0.5) * 1.6 * bw;
							float k = yq * rr / max(tl, 0.0001);
							float amp = (0.25 + 0.3 * hash11(ft + fi * 3.0)) * k;
							float xx = side * (1.0 + 0.3 * k) + amp * sin(yq * (1.1 + 0.5 * hash11(ft + 9.0)) - ph * 0.8 + ft * 1.9);
							float w = 0.011 + upx * 1.1;
							float dd = (qx - xx) / w;
							trail += exp(-dd * dd) * pow(1.0 - k, 1.3) * (0.35 + 0.35 * hash11(ft * 2.0 + fi));
						}
					}
					/* oral arms: ruffled curtains from the middle */
					float na = 4.0 * (1.0 - is_comb);
					float alen = rr * tlen * (0.9 * is_moon + 2.8 * is_nettle);
					for (int a = 0; a < 4; a++) {
						float fa = float(a);
						if (fa < na && sdown < alen) {
							float k = sdown / max(alen, 0.0001);
							float x0 = (fa / 3.0 - 0.5) * 0.3 * bw;
							float xx = x0 * (1.0 + k) + (0.08 + 0.1 * is_nettle) * sin(yq * 2.4 - ph * 0.7 + fa * 1.7) * (0.3 + k);
							float ruffle = 0.65 + 0.35 * sin(yq * 22.0 - time * 1.2 + fa * 2.0);
							float w = ((0.05 + 0.06 * is_nettle) * (1.0 - 0.55 * k) * ruffle) + upx;
							float dd = abs(qx - xx) / w;
							/* bright folded edges, softer middle */
							float m = (1.0 - smoothstep(0.85, 1.0 + upx * 3.0 / w, dd)) * (0.35 + 0.65 * smoothstep(0.3, 0.95, dd));
							arms += m * (1.0 - k * k) * 0.8;
						}
					}
				}

				/* far ones fade into the water */
				float3 fogc = lerp(water * 2.5, col, 0.35 + 0.65 * depth);
				float3 add = lerp(fogc, col, depth) * (bell * (1.0 + 0.5 * bp) + trail * 0.55) * bright;
				add += pale * arms * 0.5 * bright;
				add += (comb_col * combs * 1.3 + col * cedge * 0.5 + col * cbody * 0.06) * bright;
				/* the bell shades what's behind it a little, glows into the water around it */
				light *= 1.0 - 0.25 * (body + cbody) * depth;
				light += add;
			}
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
	float shorey = shore + 0.012 * sin(p.x * 1.7 + 0.5);
	float sea = step(horizon, uv.y) * (1.0 - smoothstep(shorey - 0.02, shorey + 0.01, uv.y));
	float dy = max(uv.y - horizon, 0.0005);
	float z = 0.18 / dy;                        /* distance: large far away */
	float wx = (p.x - aspect * 0.5) * z;
	float swell = 0.6 + 0.9 * react_mix.x * calm_in.y;
	float far_k = saturate((z - 1.0) / 4.0);    /* far water blurs into a sheen */
	float rip = vnoise(float2(wx * 1.5, z * 4.0 - time * 0.35)) * 0.6 + vnoise(float2(wx * 4.0 + 3.0, z * 10.0 - time * 0.6)) * 0.4;
	float3 water = float3(0.006, 0.014, 0.03) * (0.7 + 0.6 * lerp(rip, 0.5, far_k));
	light += water * sea;
	/* glowing crests rolling in: soft lines that thicken and break near us */
	float rows = z * 1.3 + time * 0.18 + 0.35 * sin(wx * 0.45 + z * 0.3) + 0.2 * vnoise(float2(wx * 0.3, z * 0.2));
	float wave = 0.5 + 0.5 * cos(6.2831853 * (rows + 0.15 * vnoise(float2(wx * 0.6, 3.0))));
	float crest = pow(wave, 6.0 + 30.0 * far_k);
	float breakn = smoothstep(0.45, 0.85, fbm4(float2(wx * 1.1 + time * 0.05, z * 0.8 - time * 0.05)));
	float near = 1.0 - far_k;
	float glowwave = crest * breakn * (0.1 + 0.9 * near * near) * swell;
	float bright = 0.8 + 0.8 * react_mix.y * calm_in.x;
	light += glowc * glowwave * sea * bright * 1.6;
	/* the soft glow the waves leave in the water */
	light += glowc * 0.08 * breakn * near * sea * (0.6 + 0.4 * wave);
	/* sparkling plankton, more where the water breaks */
	float2 sq = float2(wx * 9.0, z * 18.0 + time * 0.3);
	float sh = hash21(floor(sq));
	float2 sf = frac(sq) - 0.5;
	float sp = step(1.0 - 0.05 * clamp(density, 0.0, 3.0), sh) * exp(-dot(sf, sf) * 30.0) *
		   (0.5 + 0.5 * sin(time * (2.0 + 4.0 * sh) + sh * 60.0));
	light += glowc * sp * (0.2 + 1.5 * glowwave) * sea * near;

	/* the wash running up the sand: a ragged glowing foam edge */
	float wash = shore + 0.045 + 0.03 * sin(time * 0.3) * swell + 0.025 * sin(p.x * 2.1 + time * 0.25) +
		     0.03 * (fbm4(float2(p.x * 2.5, time * 0.12)) - 0.5);
	float sandm = smoothstep(shorey - 0.02, shorey + 0.01, uv.y);
	float wet = sandm * (1.0 - smoothstep(wash - px, wash + px, uv.y));
	float foam = exp(-pow((uv.y - wash) / (0.006 + px), 2.0)) * (0.4 + 0.6 * vnoise(float2(p.x * 40.0, time * 0.5)));
	float3 sand = float3(0.03, 0.026, 0.035) * (0.75 + 0.25 * vnoise(p * 140.0));
	light = lerp(light, sand, sandm * (1.0 - wet * 0.6));
	light += glowc * (foam * 1.1 * sandm + wet * 0.1 * (1.0 + calm_in.w)) * swell * bright;
	/* wet sand reflects the glow */
	light += glowc * wet * 0.12 * exp(-(wash - uv.y) * 10.0);
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
			float size = 0.022 + 0.05 * depth;
			float speed = 0.006 + 0.012 * depth;
			float y = 0.72 - frac(time * speed + h1 * 3.0) * 0.95;
			float x = (0.05 + 0.9 * hash11(fi * 2.9 + 5.5)) * aspect + 0.03 * sin(time * 0.15 + fi * 1.7);
			float2 q = (p - float2(x, y)) / size;
			/* a slightly tapered paper lantern, wider at the top */
			float w = 0.62 - 0.12 * q.y;
			float d = md_box(float2(q.x * (0.6 / w), q.y), float2(0.45, 0.7)) - 0.15;
			float body = 1.0 - smoothstep(-px / size, px / size * 1.5, d);
			/* the flame glows through the paper, brightest near the bottom */
			float inner = exp(-length(q - float2(0.0, 0.55)) * 1.4);
			float flick = 0.85 + 0.15 * sin(time * (6.0 + 3.0 * h1) + fi);
			float3 col = lerp(warm, warm * float3(1.0, 0.85, 0.7), h1);
			float fade = smoothstep(0.72, 0.6, y) * smoothstep(-0.25, -0.05, y);
			float lum = (0.35 + 0.65 * depth) * fade;
			light += col * body * (0.55 + 1.3 * inner) * flick * lum;
			light += col * exp(-length(q) / 2.2) * 0.22 * lum * flick;
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
		float on = step(1.0 - 0.07 * nm, h);
		light += sun * on * exp(-d * d / (px * px * (1.0 + 2.0 * fk))) * (0.25 + 0.6 * ray) * 0.9;
	}
	return finish_dither(light, uv, v_in.uv * res);
}

/* ------------------------------------------------------------------ */
/* Calm · Blossom Breeze: petals (or leaves, or hearts) blown off a     */
/* blossoming branch in a corner, tumbling on the wind at four depths  */
/* over a soft dusk with out-of-focus lights.                           */
/* Style settings: a.x where from (top-left, top-right, left, right,    */
/* top, everywhere), a.y wind angle (degrees, 0 = away from the corner), */
/* a.z wind strength, a.w gusts; b.x how many, b.y size, b.z kind       */
/* (cherry, plum, rose, maple, hearts), b.w tumble; c.x bokeh, c.y      */
/* colour shift on the beat; style_col rgb the colour, w "use palette". */
/* Music: the bass swirls the gusts, each beat gives a puff of wind.    */

/* turn a colour's hue by a (turns) */
float3 bb_hue_turn(float3 c, float a)
{
	float ang = a * 6.2831853;
	float cs = cos(ang);
	float sn = sin(ang);
	float3 k = float3(0.57735, 0.57735, 0.57735);
	return c * cs + cross(k, c) * sn + k * dot(k, c) * (1.0 - cs);
}

/* the petal shape, signed distance in units of its size; lq local coords */
float bb_petal(float2 lq, float kind)
{
	float d = 0.0;
	if (kind < 0.5) {
		/* cherry: a long rounded petal with a small notch at its tip */
		d = length(float2(lq.x * 1.55, (lq.y - 0.05) * 0.95)) - 0.62;
		d = max(d, -(length(lq - float2(0.0, 0.72)) - 0.17));
		d = max(d, lq.y - 0.66);
	} else if (kind < 1.5) {
		/* plum: round and soft */
		d = length(float2(lq.x * 1.15, lq.y)) - 0.6;
	} else if (kind < 2.5) {
		/* rose: broad and rounded, wider at its outer edge with a soft dip */
		float2 rq = float2(lq.x * (1.15 - 0.3 * saturate(lq.y + 0.3)), lq.y * 1.1);
		d = length(rq) - 0.62;
		d = max(d, -(length(lq - float2(0.0, 0.95)) - 0.32));
	} else if (kind < 3.5) {
		/* maple: five pointed lobes and a little stem */
		float ang = atan2(lq.x, -lq.y);
		float r = length(lq);
		float lobes = 0.38 + 0.3 * pow(abs(cos(ang * 2.5)), 1.5) + 0.05 * pow(abs(cos(ang * 7.5)), 4.0);
		d = r - lobes * smoothstep(-1.0, 0.2, cos(ang * 0.5) * 1.2);
		d = min(d, max(abs(lq.x) - 0.025, abs(lq.y - 0.5) - 0.22));
	} else {
		/* a heart */
		float2 h = float2(abs(lq.x), -lq.y + 0.25) * 1.15;
		d = (length(float2(h.x, h.y - sqrt(h.x) * 0.55)) - 0.55) * 0.9;
	}
	return d;
}

/* a blossoming branch from the top-left corner (mirrored for the right);
 * returns x wood, y blossom, z glow */
float3 bb_branch(float2 b, float sway, float px)
{
	float3 r = float3(0.0, 0.0, 0.0);
	float2 pts[5];
	pts[0] = float2(-0.08, -0.06);
	pts[1] = float2(0.12, 0.06 + sway * 0.2);
	pts[2] = float2(0.3, 0.1 + sway * 0.5);
	pts[3] = float2(0.46, 0.16 + sway * 0.9);
	pts[4] = float2(0.58, 0.17 + sway * 1.3);
	for (int k = 0; k < 4; k++) {
		float fk = float(k);
		float w = 0.022 * (1.0 - fk * 0.21) + px;
		float d = md_seg(b, pts[k], pts[k + 1]);
		r.x = max(r.x, 1.0 - smoothstep(w - px, w + px, d));
		/* a twig off each joint */
		float2 tw = pts[k + 1] + float2(0.05 + 0.03 * fk, 0.09 + sway * (0.6 + fk * 0.3)) * (1.0 - fk * 0.15);
		float dt = md_seg(b, pts[k + 1] * 0.97 + pts[k] * 0.03, tw);
		float wt = 0.008 + px;
		r.x = max(r.x, 1.0 - smoothstep(wt - px, wt + px, dt));
	}
	/* clusters of flowers along it */
	for (int c = 0; c < 9; c++) {
		float fc = float(c);
		float t = fc / 8.0;
		int seg = int(min(floor(t * 4.0), 3.0));
		float2 pa = pts[0];
		float2 pb = pts[1];
		if (seg == 1) {
			pa = pts[1];
			pb = pts[2];
		} else if (seg == 2) {
			pa = pts[2];
			pb = pts[3];
		} else if (seg == 3) {
			pa = pts[3];
			pb = pts[4];
		}
		float2 cc = lerp(pa, pb, frac(t * 4.0)) + (float2(hash11(fc * 13.1), hash11(fc * 7.7 + 2.0)) - 0.5) * 0.07;
		float rad = 0.022 + 0.014 * hash11(fc * 3.3 + 1.0);
		float d = length(b - cc);
		/* five soft petals round a brighter middle */
		float ang = atan2(b.y - cc.y, b.x - cc.x) + fc;
		float shape = rad * (0.75 + 0.25 * abs(cos(ang * 2.5)));
		r.y = max(r.y, 1.0 - smoothstep(shape - px * 1.5, shape + px * 1.5, d));
		r.z += exp(-d / (rad * 2.5)) * 0.25;
	}
	return r;
}

float4 PSModernBlossoms(VertData v_in) : TARGET
{
	float2 uv = view_uv(v_in.uv);
	float aspect = res.x / res.y;
	float2 p = float2(uv.x * aspect, uv.y);
	float px = 1.0 / res.y;

	float from = floor(style_a.x + 0.5);
	float kind = floor(style_b.z + 0.5);
	float count = style_b.x;
	float psize = max(style_b.y, 0.2);
	float tumble = style_b.w;
	float gust_k = style_a.w;
	float strength = style_a.z;

	/* the colour: the picked one, or the palette */
	float cshift = music_ease.x * floor(style_c.y * react_mix.z * 8.0 + 0.5) / 64.0;
	float hp = cm_hue(hue + 0.92 + react_mix.z * 0.06 * calm_in.z + cshift, uv, uv.x, 0.0);
	float3 picked = bb_hue_turn(style_col.rgb, cshift);
	float3 pcol = style_col.w > 0.5 ? md_tint(float3(1.0, 0.62, 0.75), hp) : picked;
	float3 leaf_lit = lerp(pcol, float3(1.0, 0.98, 0.97), 0.35);

	/* dusk sky and soft lights far behind */
	float3 skyA = md_tint(float3(0.15, 0.1, 0.22), hue + 0.75);
	float3 skyB = md_tint(float3(0.34, 0.18, 0.25), hue + 0.95);
	float3 light = lerp(skyA * 0.6, skyB * 0.8, smoothstep(0.0, 1.0, uv.y));
	float bokeh = style_c.x;
	for (int b = 0; b < 10; b++) {
		float fb = float(b);
		if (fb < bokeh * 7.0) {
			float2 c = float2(hash11(fb * 3.3 + 1.0) * aspect, 0.2 + 0.7 * hash11(fb * 7.7 + 2.0));
			c.x += 0.03 * sin(time * 0.05 + fb);
			float r = 0.04 + 0.08 * hash11(fb * 1.9);
			float d = length(p - c);
			float3 bc = lerp(pcol, float3(1.0, 0.85, 0.6), hash11(fb + 4.0) * 0.6);
			light += bc * 0.1 * (1.0 - smoothstep(r * 0.8, r, d)) * (0.6 + 0.4 * calm_e(fb / 10.0, 0.6)) *
				 saturate(bokeh);
		}
	}

	/* the wind: away from the corner, turned by the angle knob */
	float2 wdir = float2(0.94, 0.34);
	float2 origin = float2(0.0, 0.0);
	float mirror = 0.0;
	if (from < 0.5) {
		wdir = float2(0.92, 0.38);
		origin = float2(0.06, 0.08);
	} else if (from < 1.5) {
		wdir = float2(-0.92, 0.38);
		origin = float2(aspect - 0.06, 0.08);
		mirror = 1.0;
	} else if (from < 2.5) {
		wdir = float2(0.97, 0.24);
		origin = float2(0.0, 0.32);
	} else if (from < 3.5) {
		wdir = float2(-0.97, 0.24);
		origin = float2(aspect, 0.32);
		mirror = 1.0;
	} else if (from < 4.5) {
		wdir = float2(0.25, 0.97);
	}
	float wa = style_a.y * 0.0174533 * (mirror > 0.5 ? -1.0 : 1.0);
	wdir = normalize(rot2(wdir, wa));
	float2 wn = float2(-wdir.y, wdir.x);
	bool corner = from < 3.5;

	/* gusts: slow surges every few seconds, and the bass swirling them */
	float gt = time * 0.13;
	float surge = (floor(gt) + smoothstep(0.25, 0.75, frac(gt))) * 0.6 * gust_k;
	float swirl = gust_k * (0.25 + 0.6 * react_mix.x * music_ease.z);
	/* each beat a puff: whole quarter cells, so the 64-beat wrap lands on the same petals */
	float puff = music_ease.x * floor(saturate(react_mix.w) + 0.5) * 0.25;

	for (int l = 0; l < 4; l++) {
		float fl = float(l);
		float depth = (fl + 1.0) / 4.0; /* far .. near */
		float s = (9.0 - 5.0 * depth) / psize; /* cells per unit */
		/* in the wind's frame: along it and across it */
		float2 rel = p - origin;
		float along = dot(rel, wdir);
		float across = dot(rel, wn);
		/* they sink as they go */
		across -= 0.12 * along * along * (wdir.x * wdir.x) * (mirror > 0.5 ? -1.0 : 1.0);
		float travel = time * (0.05 + 0.06 * strength) * (0.6 + 0.6 * depth) * s + surge * (0.7 + 0.6 * depth) + puff;
		float2 q = float2(along * s - travel, across * s);
		/* the gusts bend the whole stream softly */
		float2 wq = float2(along, across) * 1.7 + float2(time * 0.07, fl * 3.1);
		q += (float2(vnoise(wq), vnoise(wq + 5.2)) - 0.5) * swirl * 1.6;
		/* period of 16 cells, so the 64-beat wrap of the puffs is seamless */
		q.y += sin(q.x * 0.3927 + time * 0.4 + fl) * 0.25 * (0.4 + gust_k);
		float2 id = floor(q);
		float2 f = frac(q) - 0.5;
		float2 hid = float2(glmod(id.x, 16.0), id.y);
		float h = hash21(hid + fl * 17.0);
		/* from a corner they spread out in a cone; elsewhere, everywhere */
		float dens = 1.0;
		if (corner) {
			float a_c = (id.x + 0.5 + travel) / s;
			float b_c = (id.y + 0.5) / s;
			float spread = 0.05 + 0.3 * max(a_c, 0.0);
			float bs = b_c / spread;
			dens = exp(-bs * bs) * smoothstep(-0.05, 0.08, a_c) * (1.6 - 0.5 * saturate(a_c));
		}
		if (h < 0.35 * count * dens) {
			float2 o = (float2(hash21(hid + 2.1), hash21(hid + 4.7)) - 0.5) * 0.3;
			float spin = time * (0.3 + 1.0 * hash21(hid + 9.3)) * (0.3 + tumble) + h * 30.0;
			float2 lq = rot2(f - o, spin);
			/* tumbling in 3D: it turns over, so it narrows and shows its back */
			float flipa = time * (0.5 + 0.9 * h) * (0.2 + tumble) + h * 12.0;
			float fc = cos(flipa);
			float squash = max(abs(fc), 0.22);
			lq.y /= squash;
			float sz = 0.36;
			float d = bb_petal(lq / sz, kind) * sz * squash;
			float upx = px * s;
			float blur = upx * (0.8 + 2.5 * pow(depth, 3.0) + 2.0 * (1.0 - depth));
			float m = 1.0 - smoothstep(-blur, blur, d);
			/* shading: lighter to the middle, a faint vein, darker underside */
			float inner = saturate(-d / sz * 2.5);
			float vx = lq.x / (0.02 + upx);
			float vein = exp(-vx * vx) * 0.07 * step(kind, 3.5);
			float under = fc < 0.0 ? 0.72 : 1.0;
			float3 pc = lerp(pcol, leaf_lit, 0.45 * inner) * under * (1.0 - vein);
			/* light through the petal when it faces the sky */
			pc += pcol * 0.25 * pow(saturate(fc), 4.0);
			float glow = (0.6 + 0.55 * depth) * (0.85 + 0.3 * react_mix.y * calm_in.x);
			light = lerp(light, pc * glow, m * (0.75 + 0.23 * depth));
		}
	}

	/* the branch they come from */
	if (from < 4.5) {
		float sway = 0.03 * sin(time * 0.6) * (0.4 + gust_k) + 0.02 * swirl * sin(time * 1.3);
		float3 br = float3(0.0, 0.0, 0.0);
		if (from < 3.5) {
			float2 bp = float2(mirror > 0.5 ? aspect - p.x : p.x, p.y - (from > 1.5 ? 0.25 : 0.0));
			br = bb_branch(bp, sway, px);
		} else {
			br = max(bb_branch(p, sway, px), bb_branch(float2(aspect - p.x, p.y), -sway, px));
		}
		float3 wood = md_tint(float3(0.09, 0.05, 0.06), hue + 0.8);
		light = lerp(light, wood, br.x * 0.95);
		light = lerp(light, lerp(pcol, leaf_lit, 0.35) * 0.95, br.y);
		light += pcol * br.z * 0.12;
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
		float on = step(1.0 - 0.22 * heavy * (0.6 + 0.4 * depth), h);
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
/* bit k of a small integer stored in a float (p2 = 2^k) */
float md_bit(float bits, float p2)
{
	return step(0.5, frac(floor(bits / p2) * 0.5));
}

float md_segon(float2 g, float2 a, float2 b, float on)
{
	return md_seg(g, a, b) + (1.0 - on) * 10.0;
}

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
		float inside = 1.0 - smoothstep(-px, px, md_box(d2, sz));
		/* neon lettering inside: made-up glyphs from seven strokes */
		float horiz = step(sz.y, sz.x);
		float2 lp = lerp(float2(d2.y, -d2.x), d2, horiz);
		float2 lsz = lerp(sz.yx, sz, horiz);
		float ch = lsz.y * 1.25;
		float cw = ch * 0.72;
		float nchar = max(floor((lsz.x * 2.0 - lsz.y * 0.6) / cw), 1.0);
		float cx = (lp.x + nchar * cw * 0.5) / cw;
		float ci = floor(cx);
		float2 g = float2((frac(cx) - 0.5) * cw, lp.y);
		float valid = step(0.0, ci) * step(ci, nchar - 1.0) * inside;
		float bits = floor(hash11(ci * 3.7 + fi * 11.0) * 127.0) + 1.0;
		float sw = cw * 0.3;
		float shh = ch * 0.36;
		float ds = md_segon(g, float2(-sw, -shh), float2(sw, -shh), md_bit(bits, 1.0));
		ds = min(ds, md_segon(g, float2(-sw, 0.0), float2(sw, 0.0), md_bit(bits, 2.0)));
		ds = min(ds, md_segon(g, float2(-sw, shh), float2(sw, shh), md_bit(bits, 4.0)));
		ds = min(ds, md_segon(g, float2(-sw, -shh), float2(-sw, 0.0), md_bit(bits, 8.0)));
		ds = min(ds, md_segon(g, float2(-sw, 0.0), float2(-sw, shh), md_bit(bits, 16.0)));
		ds = min(ds, md_segon(g, float2(sw, -shh), float2(sw, 0.0), md_bit(bits, 32.0)));
		ds = min(ds, md_segon(g, float2(sw, 0.0), float2(sw, shh), md_bit(bits, 64.0)));
		float glyph = (exp(-ds * ds / ((px * 1.3) * (px * 1.3))) + exp(-ds / (0.004 + px)) * 0.25) * valid;
		float tube = exp(-d * d / ((px * 1.5) * (px * 1.5))) + exp(-d / (0.012 + 0.006 * e)) * 0.35 + inside * 0.08 + glyph * 0.9;
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
	float3 haze = md_tint(float3(0.12, 0.06, 0.16), hue + 0.8);
	light += lerp(float3(0.006, 0.006, 0.018), haze, pow(smoothstep(0.0, street, uv.y), 1.5)) * step(uv.y, street);
	float bx = floor(p.x * 9.0);
	float bh = 0.05 + 0.22 * hash11(bx * 1.7 + 3.0);
	float bld = step(bh, uv.y) * step(uv.y, street);
	/* the buildings: dark faces with a faint edge of city glow */
	float edgeb = exp(-abs(uv.y - bh) / (px * 1.5)) * 0.4 + exp(-abs(frac(p.x * 9.0) - 0.0) * 300.0) * 0.1;
	light = lerp(light, float3(0.012, 0.012, 0.022) + haze * 0.1 * (1.0 - uv.y), bld) + haze * edgeb * bld;
	/* lit windows */
	float2 wq = float2(p.x * 90.0, uv.y * 60.0);
	float2 wid = floor(wq);
	float2 wf = frac(wq);
	float win = step(0.72, hash21(wid + bx)) * step(0.25, wf.x) * step(wf.x, 0.75) * step(0.3, wf.y) * step(wf.y, 0.7);
	light += float3(0.25, 0.2, 0.12) * win * bld * 0.7 * (0.5 + 0.5 * hash21(wid));

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
		float drop = step(1.0 - 0.08 * heavy, h) * smoothstep(0.25, 0.0, abs(f.x - 0.5)) * smoothstep(0.0, 0.3, f.y) *
			     smoothstep(1.0, 0.6, f.y);
		light += md3(0.06 + 0.04 * fl) * drop;
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
	float th = 1.0;
	float gl = length(grad) + 0.0001;
	float sd = (field - th) / gl; /* ~ distance inside the surface */
	float m = smoothstep(-px * 1.2, px * 1.2, sd);
	/* treat the field as a height: steep at the rim like a drop of mercury */
	float over_t = max(field - th, 0.0);
	float dhdf = 0.5 / sqrt(over_t + 0.02);
	float2 gh = grad * dhdf * 0.045;
	/* a beat sends a ripple over it */
	float rr = length(p);
	gh += p / max(rr, 0.001) * cos(rr * 50.0 - time * 10.0) * 0.25 * md_beat() * exp(-rr * 3.0);
	float3 nrm = normalize(float3(-gh, 1.0));
	/* the studio it reflects: coloured walls, a bright horizon and two
	 * soft boxes, looked up by the surface's tilt */
	float h = cm_hue(hue + react_mix.z * 0.2 * mid, uv, nrm.x * 0.5 + 0.5, 0.0);
	float3 ca = prism(h);
	float3 cb = prism(h + 0.45);
	float ty = -nrm.y; /* tilted up = sees the ceiling */
	float3 env = lerp(cb * 0.35, ca * 0.8, smoothstep(-0.6, 0.6, ty));
	env += md3(0.9) * exp(-pow((ty - 0.05) / 0.07, 2.0)) * 0.6;     /* horizon line */
	env += md3(1.6) * exp(-dot(nrm.xy - float2(-0.45, -0.45), nrm.xy - float2(-0.45, -0.45)) / 0.02);
	env += ca * 1.2 * exp(-dot(nrm.xy - float2(0.5, 0.2), nrm.xy - float2(0.5, 0.2)) / 0.03);
	float fres = 0.55 + 0.45 * pow(1.0 - nrm.z, 2.0);
	float bright = 0.8 + 0.5 * react_mix.y * level;
	float3 surf = (env * fres + md3(0.03)) * bright;
	float halo = saturate(field) * (1.0 - m);
	float3 light = surf * m + ca * pow(halo, 3.0) * 0.2 * bright;
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
	float sh = saturate(1.0 - abs(fk) * 2.0);
	float spike = (sh * sh * (3.0 - 2.0 * sh)) * sh * (0.03 + 0.2 * e * (0.5 + 0.7 * react_mix.x));
	return 0.17 + 0.01 * sin(a * 3.0 + t) + spike;
}

float4 PSModernFerrofluid(VertData v_in) : TARGET
{
	float2 uv = view_uv(v_in.uv);
	float aspect = res.x / res.y;
	float2 p = float2((uv.x - 0.5) * aspect, uv.y - 0.5);
	float px = 1.0 / res.y;
	float n = clamp(floor(10.0 + 6.0 * density), 6.0, 30.0);
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
	/* height: a dome that rises into each spike */
	float inner = saturate(r / max(R, 0.0001));
	float2 radial = p / max(r, 0.0001);
	float2 tang = float2(-radial.y, radial.x);
	/* surface slope: outwards near the rim, sideways on a spike's flanks */
	float2 g2 = radial * (0.3 + 2.2 * pow(inner, 3.0)) + tang * clamp(-slope / max(R, 0.05), -3.0, 3.0) * 0.5 * inner;
	float3 nrm = normalize(float3(g2, 1.0));
	float h = cm_hue(hue + react_mix.z * 0.15 * mid, uv, a / 6.2831853, 0.0);
	float3 c1 = prism(h);
	float3 c2 = prism(h + 0.33);
	/* glossy black: mostly the reflection of two coloured lights and a window */
	float3 ref = float3(nrm.xy, nrm.z);
	float k1 = pow(saturate(dot(ref, normalize(float3(-0.55, -0.55, 0.6)))), 30.0);
	float k2 = pow(saturate(dot(ref, normalize(float3(0.6, 0.3, 0.5)))), 12.0);
	float k3 = pow(saturate(dot(ref, normalize(float3(0.0, 0.7, 0.7)))), 8.0);
	float fres = pow(1.0 - nrm.z, 2.0);
	float3 fluid = md3(0.008) + md3(1.4) * k1 + c1 * k2 * 0.9 + c2 * k3 * 0.5 + (c1 + c2) * fres * 0.12;
	float bright = 0.8 + 0.5 * react_mix.y * level;
	float3 light = fluid * m * bright;
	/* the dish it sits in: a soft lit gradient, with the fluid's shadow */
	float dish = (1.0 - smoothstep(0.32, 0.5, r)) * 0.11;
	float3 dishc = lerp(md3(0.2), c2, 0.35) * dish * (1.0 - 0.7 * exp(-max(sd, 0.0) * 14.0));
	light += dishc * (1.0 - m);
	light += c1 * exp(-max(sd, 0.0) * 10.0) * 0.12 * (1.0 - m) * bright;
	return finish_dither(light, uv, v_in.uv * res);
}

/* ------------------------------------------------------------------ */
/* Modern · Sound Map: a glowing contour map of hills and valleys that  */
/* rise with the music, lines brightening near the peaks.               */
/* Amount = how many contour lines.                                      */
float md_terrain(float2 q)
{
	float h = 0.75 * fbm4(q * 0.55 + float2(time * 0.03, 0.0));
	/* eight hills, one per stretch of the spectrum, wandering slowly */
	for (int i = 0; i < 8; i++) {
		float fi = float(i);
		float2 c = float2((fi / 7.0 - 0.5) * 7.0 + 0.6 * sin(time * 0.07 + fi), 1.4 * sin(time * 0.05 + fi * 2.3));
		float e = band_level((fi + 0.5) / 8.0);
		h += (0.08 + 0.45 * e * react_mix.x) * exp(-dot(q - c, q - c) * 0.55);
	}
	return h;
}

float4 PSModernSoundMap(VertData v_in) : TARGET
{
	float2 uv = view_uv(v_in.uv);
	float aspect = res.x / res.y;
	float2 q = float2((uv.x - 0.5) * aspect, uv.y - 0.5) * 5.0;
	float px = 5.0 / res.y;
	float lines = 6.0 + 5.0 * clamp(density, 0.2, 3.0);
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
	float k = 1.2 + 1.6 * clamp(density, 0.2, 3.0);
	float t = time * 0.12;
	/* height of the foil: big soft folds plus sharp crinkles */
	float2 w = p * k + float2(t, -t * 0.7);
	float e = 0.01;
	float hh = fbm4(w) + 0.35 * abs(vnoise(w * 3.1 + 7.0) - 0.5);
	float hx = fbm4(w + float2(e, 0.0)) + 0.35 * abs(vnoise((w + float2(e, 0.0)) * 3.1 + 7.0) - 0.5);
	float hy = fbm4(w + float2(0.0, e)) + 0.35 * abs(vnoise((w + float2(0.0, e)) * 3.1 + 7.0) - 0.5);
	float sway = 0.5 + 0.8 * react_mix.x * (0.5 * bass + 0.5 * mid);
	float3 nrm = normalize(float3((hh - hx) / e * 0.32 * sway, (hh - hy) / e * 0.32 * sway, 1.0));
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
	/* foil: a mirror-silver sheet tinted rainbow by the film, light and dark
	 * across its folds */
	float shade = 0.06 + 1.25 * pow(saturate(ndl), 3.0);
	float3 foil = lerp(md3(0.7), irid, 0.45) * shade + irid * 0.05;
	float3 light = foil * bright + md3(1.0) * (spec * 0.7 + glint * 2.0) * bright;
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
