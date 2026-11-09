/* ================================================================== */
/* Feedback pack: MilkDrop / Geiss / WMP Alchemy style. Every frame     */
/* last frame is warped (zoom, turn, swirl, fold...), faded and colour- */
/* shifted, and new content is drawn over it.                          */
/* (styles listed in pack-feedback.h with VZ_FEEDBACK; see tools/PACKS.md) */
/*                                                                      */
/* The buffers are 8 bits per channel, so fades also take a small fixed */
/* step off the alpha (fbk_take): a pure multiply would stall at a dim  */
/* value and leave ghosts that never clear.                             */
/*                                                                      */
/* react_mix: x movement (zoom/warp/twist following the music),         */
/* y brightness/glow, z colour shifts, w beat events.                   */
/* ================================================================== */

/* frames at 60 fps this step stands for, so motion doesn't follow fps */
float fbk_steps()
{
	return clamp(prev_on.y * 60.0, 0.0, 4.0);
}

/* buffer uv (0..1) -> centred, aspect-correct coordinates, and back */
float2 fbk_asp(float2 uv)
{
	return float2((uv.x - 0.5) * res.x / max(res.y, 1.0), uv.y - 0.5);
}

float2 fbk_unasp(float2 p)
{
	return float2(p.x * res.y / max(res.x, 1.0) + 0.5, p.y + 0.5);
}

/* mirrored repeat, so warps that reach past the edge fold back in */
float2 fbk_mirror(float2 uv)
{
	return float2(1.0 - abs(1.0 - glmod(uv.x, 2.0)), 1.0 - abs(1.0 - glmod(uv.y, 2.0)));
}

/* last frame at uv, faded by keep_ps per second plus a small fixed step
 * per frame so it always clears fully in 8-bit buffers */
float4 fbk_take(float2 uv, float keep_ps)
{
	float4 o = prev_at(uv);
	/* soften the last few percent at the edges, so warps that pull from
	 * past the edge don't leave hard seams or teeth */
	float2 e = abs(uv - 0.5);
	o.a *= smoothstep(0.5, 0.47, max(e.x, e.y));
	o.a = max(o.a * fb_decay(keep_ps) - 0.9 / 255.0 * max(fbk_steps(), 0.5), 0.0);
	return o;
}

/* turn a colour around the grey axis (a hue shift that keeps brightness) */
float3 fbk_huerot(float3 c, float ang)
{
	float3 k = float3(0.57735, 0.57735, 0.57735);
	float cs = cos(ang);
	float sn = sin(ang);
	return saturate(c * cs + cross(k, c) * sn + k * dot(k, c) * (1.0 - cs));
}

/* half-a-level dither for the stored result */
float fbk_dith(float2 pix)
{
	return (hash21(pix + frac(time * 7.31) * 91.0) - 0.5) / 255.0;
}

/* the beat, as an event: beat is an envelope that jumps up on a beat and
 * falls away, so only its top part counts as a hit (0..1, scaled by
 * react_mix.w) */
float fbk_hit()
{
	return saturate((beat - 0.55) * 2.5) * react_mix.w;
}

/* a waveform made from the spectrum, roughly -1..1 across x 0..1 */
float fbk_wave(float x, float t)
{
	float s = 0.0;
	for (int k = 0; k < 8; k++) {
		float fk = float(k);
		float amp = band_level(fk / 8.0 + 0.03);
		s += amp * sin(6.2831853 * (x * (1.0 + fk * 1.7) + t * (0.3 + fk * 0.21)) + fk * 1.3) / (1.0 + fk * 0.35);
	}
	return s * 0.6;
}

/* coverage of the waveform line y = base + amp * wave(x) at p (uv units),
 * w is the half width in uv units */
float fbk_wline(float2 p, float base, float amp, float t, float w)
{
	float aspect = res.x / max(res.y, 1.0);
	float y0 = base + amp * fbk_wave(p.x, t);
	float y1 = base + amp * fbk_wave(p.x + 0.002, t);
	float slope = (y1 - y0) / (0.002 * aspect);
	float d = abs(p.y - y0) / sqrt(1.0 + slope * slope);
	return saturate(1.0 - d / w);
}

/* fold the plane into n mirrored wedges, turned by twist */
float2 fbk_kfold(float2 p, float n, float twist)
{
	float r = length(p);
	float a = atan2(p.y, p.x) + twist;
	float sector = 6.2831853 / max(n, 1.0);
	a = glmod(a, sector);
	a = abs(a - sector * 0.5);
	return float2(cos(a), sin(a)) * r;
}

/* soft round blob */
float fbk_blob(float2 p, float2 c, float size)
{
	float2 d = p - c;
	return exp(-dot(d, d) / max(size * size, 0.000001));
}

/* ------------------------------------------------------------------ */

/* Zoom Tunnel: a ring shaped by the spectrum is drawn every frame, and
 * last frame is zoomed in and turned a little, so the rings stream into a
 * tunnel. Bass zooms harder, mids turn it, beats throw an outer ring. */
float4 PSMilkZoomTunnel(VertData v_in) : TARGET
{
	float steps = fbk_steps();
	float rx = react_mix.x;
	float hit = fbk_hit();

	/* last frame, zoomed towards the centre and turned */
	float2 c = fbk_asp(v_in.uv);
	float z = pow(0.985 - 0.02 * saturate(bass) * rx, steps);
	c = rot2(c * z, (0.004 + 0.012 * mid * rx) * steps);
	float4 old = fbk_take(fbk_unasp(c), 0.3);
	old.rgb = fbk_huerot(old.rgb, 0.006 * steps * react_mix.z);

	/* this frame's ring */
	float2 uv = view_uv(v_in.uv);
	float2 p = fbk_asp(uv);
	float r = length(p);
	float s = abs(atan2(p.y, p.x)) / 3.14159265;
	float rad = 0.12 + 0.17 * band_level(s) + 0.05 * hit;
	float w = (1.6 + 1.2 * level * react_mix.y) / res.y;
	float ring = saturate(1.0 - abs(r - rad) / (w * 2.0));
	float3 col = prism(cm_hue(hue + s * 0.5 + time * 0.05, uv, s, 0.0));
	float4 o = over(old, col, ring);

	/* beats: a thin bright ring further out */
	float outer = saturate(1.0 - abs(r - (0.36 + 0.06 * hit)) / (w * 1.5)) * hit;
	o = over(o, lerp(col, float3(1.0, 1.0, 1.0), 0.5), outer);
	return fb_out(o.rgb + fbk_dith(v_in.uv * res), o.a);
}

/* Kaleido Trip: last frame folded through a kaleidoscope whose wedge count,
 * zoom and twist keep morphing with the music, so the pattern keeps turning
 * into new ones. Big hits shatter it: a hard twist, a jump of the fold
 * centre and a colour jump, and it folds itself back together. */
float4 PSMilkKaleidoTrip(VertData v_in) : TARGET
{
	float steps = fbk_steps();
	float rx = react_mix.x;
	float hit = fbk_hit();

	float2 p = fbk_asp(v_in.uv);
	/* the fold centre wobbles, and jumps on hits */
	p += float2(sin(time * 0.37), cos(time * 0.29)) * 0.015 * rx;
	p += float2(sin(time * 3.1), cos(time * 2.3)) * 0.05 * hit;

	float seg = 6.0 + 3.0 * sin(time * 0.11) + 3.0 * saturate(mid) * rx;
	float n0 = floor(seg);
	float twist = time * 0.05 + (0.01 + 0.03 * mid * rx) * steps + 0.5 * hit;
	float z = pow(0.978 - 0.025 * saturate(bass) * rx - 0.04 * hit, steps);

	float2 q0 = rot2(fbk_kfold(p, n0, twist), time * 0.07) * z;
	float2 q1 = rot2(fbk_kfold(p, n0 + 1.0, twist), time * 0.07) * z;
	float4 a0 = fbk_take(fbk_unasp(q0), 0.42);
	float4 a1 = fbk_take(fbk_unasp(q1), 0.42);
	float4 old = lerp(a0, a1, smoothstep(0.0, 1.0, frac(seg)));
	old.rgb = fbk_huerot(old.rgb, (0.012 * steps + 0.25 * hit) * react_mix.z);

	float2 uv = view_uv(v_in.uv);
	float2 pv = fbk_asp(uv);
	float r = length(pv);
	float th = atan2(pv.y, pv.x);
	float s = abs(th) / 3.14159265;
	float w = (1.3 + 1.5 * level * react_mix.y) / res.y;

	/* a spectrum ring */
	float rad = 0.06 + 0.12 * band_level(s) * rx + 0.03 * hit;
	float ring = saturate(1.0 - abs(r - rad) / (w * 2.0));
	float3 col = prism(cm_hue(hue + s * 0.6 + time * 0.08, uv, s, 0.0));
	float4 o = over(old, col, ring);

	/* a rose of petals that changes its count */
	float k = 3.0 + floor(4.0 * frac(time * 0.031));
	float rose = (0.12 + 0.08 * level * react_mix.y) * abs(cos(k * 0.5 * th + time * 0.4));
	float petal = saturate(1.0 - abs(r - rose) / (w * 2.0)) * step(0.02, rose);
	o = over(o, prism(cm_hue(hue + 0.45 + time * 0.05, uv, s, 1.0)), petal * 0.85);

	/* small bright dots orbiting */
	float count = clamp(3.0 + 3.0 * density, 2.0, 9.0);
	for (int i = 0; i < 9; i++) {
		float fi = float(i);
		if (fi < count) {
			float t = fi / count;
			float ang = time * (0.5 + 0.2 * fi) + t * 6.2831853;
			float e = band_level(t);
			float rr = 0.1 + 0.2 * e * rx + 0.04 * sin(time * 0.9 + fi);
			float b = fbk_blob(pv, float2(cos(ang), sin(ang)) * rr, 0.004 + 0.008 * e);
			o = over(o, prism(cm_hue(hue + t + time * 0.06, uv, t, fi)), saturate(b * 1.5));
		}
	}
	/* treble sparks */
	float spark = step(0.9975 - 0.006 * treble * rx, hash21(floor(v_in.uv * res * 0.5) + floor(time * 60.0))) * step(r, 0.25);
	o = over(o, float3(1.0, 1.0, 1.0), spark);
	return fb_out(o.rgb + fbk_dith(v_in.uv * res), o.a);
}

/* Kaleido Dream: the kaleidoscope slowed right down: soft lights fold into
 * a breathing mandala; the music only swells it. */
float4 PSMilkKaleidoDream(VertData v_in) : TARGET
{
	float steps = fbk_steps();
	float rx = react_mix.x;
	float swell = calm_e(0.2, 0.7);

	/* the mandala slowly blooms outwards: each ring of petals drawn in the
	 * middle echoes out through the kaleidoscope */
	float2 p = fbk_asp(v_in.uv);
	/* one fixed fold (8 wedges) keeps the echoes crisp */
	float twist = 0.002 * steps + time * 0.012;
	float z = pow(0.986 - 0.004 * calm_in.y * rx, steps);
	float4 old = fbk_take(fbk_unasp(fbk_kfold(p, 8.0, twist) * z), 0.45);
	old.rgb = fbk_huerot(old.rgb, 0.004 * steps * react_mix.z);

	float2 uv = view_uv(v_in.uv);
	float2 pv = fbk_asp(uv);
	float r = length(pv);
	float th = atan2(pv.y, pv.x);
	float w = (2.0 + 1.0 * swell * react_mix.y) / res.y;
	/* petals in the middle, opening and closing slowly */
	float k = 5.0 + floor(3.0 * frac(time * 0.013));
	float rose = (0.09 + 0.03 * swell * react_mix.y) * (0.6 + 0.4 * abs(cos(k * 0.5 * th + time * 0.15)));
	float petal = saturate(1.0 - abs(r - rose) / (w * 2.0));
	float3 col = prism(cm_hue(hue + th / 6.2831853 * 0.3 + time * 0.01, uv, r, 0.0));
	float4 o = over(old, col, petal * 0.7);

	/* soft dots circling the petals */
	float count = clamp(2.0 + 2.0 * density, 2.0, 6.0);
	for (int i = 0; i < 6; i++) {
		float fi = float(i);
		if (fi < count) {
			float t = fi / count;
			float ang = time * 0.08 + t * 6.2831853;
			float b = fbk_blob(pv, float2(cos(ang), sin(ang)) * 0.13, 0.009 + 0.005 * swell);
			o = over(o, prism(cm_hue(hue + t * 0.5 + 0.3, uv, t, fi)), saturate(b * 0.8));
		}
	}
	return fb_out(o.rgb + fbk_dith(v_in.uv * res), o.a);
}

/* Kaleido Chaos: two kaleidoscopes folded into each other with a moving
 * offset, hard twists on the bass and colour flips on hits. */
float4 PSMilkKaleidoChaos(VertData v_in) : TARGET
{
	float steps = fbk_steps();
	float rx = react_mix.x;
	float hit = fbk_hit();

	float2 p = fbk_asp(v_in.uv);
	float n1 = floor(5.0 + 4.0 * frac(time * 0.071));
	float n2 = floor(3.0 + 3.0 * frac(time * 0.053 + 0.5));
	float2 q = fbk_kfold(p, n1, time * 0.21 + 0.9 * hit);
	q -= float2(0.12 + 0.08 * sin(time * 0.6), 0.05 * cos(time * 0.4)) * (0.6 + 0.6 * mid * rx);
	q = fbk_kfold(q, n2, -time * 0.17 + (0.02 + 0.06 * bass * rx) * steps);
	q *= pow(0.965 - 0.04 * saturate(bass) * rx, steps);
	float4 old = fbk_take(fbk_mirror(fbk_unasp(q)), 0.35);
	old.rgb = fbk_huerot(old.rgb, (0.03 * steps + 0.9 * hit) * react_mix.z);
	old.rgb = lerp(old.rgb, 1.0 - old.rgb, 0.35 * hit * react_mix.z);

	float2 uv = view_uv(v_in.uv);
	float2 pv = fbk_asp(uv);
	float r = length(pv);
	float s = abs(atan2(pv.y, pv.x)) / 3.14159265;
	float rad = 0.07 + 0.16 * band_level(s) * rx + 0.04 * hit;
	float w = (2.0 + 2.0 * level * react_mix.y) / res.y;
	float ring = saturate(1.0 - abs(r - rad) / (w * 2.0));
	float3 col = prism(cm_hue(hue + s + time * 0.13, uv, s, 0.0));
	float4 o = over(old, col, ring);
	float spark = step(0.996 - 0.01 * treble * rx, hash21(floor(v_in.uv * res * 0.5) + floor(time * 60.0))) * step(r, 0.3);
	o = over(o, float3(1.0, 1.0, 1.0), spark);
	return fb_out(o.rgb + fbk_dith(v_in.uv * res), o.a);
}

/* Geiss Plasma: last frame pushed around by a slowly turning wave field and
 * zoomed, while a waveform is drawn across the middle: the classic Geiss
 * look of liquid colour pouring out of a line. */
float4 PSMilkGeissPlasma(VertData v_in) : TARGET
{
	float steps = fbk_steps();
	float rx = react_mix.x;

	float2 cc = fbk_asp(v_in.uv);
	float2 disp = float2(sin(cc.y * 6.0 + time * 0.9) + sin(cc.x * 3.0 - time * 0.6),
			     cos(cc.x * 5.0 - time * 0.7) + sin(cc.y * 4.0 + time * 0.5));
	float2 q = cc * pow(0.99 - 0.012 * saturate(bass) * rx, steps) + disp * (0.0015 + 0.004 * mid * rx) * steps;
	float4 old = fbk_take(fbk_mirror(fbk_unasp(q)), 0.45);
	old.rgb = fbk_huerot(old.rgb, 0.015 * steps * react_mix.z);

	float2 uv = view_uv(v_in.uv);
	float w = (1.5 + 2.5 * level * react_mix.y) / res.y;
	float ln = fbk_wline(uv, 0.5, 0.16 * (0.5 + rx * 0.7), time * 0.6, w * 2.0);
	float3 col = prism(cm_hue(hue + uv.x * 0.6 + time * 0.04, uv, uv.x, 0.0));
	float4 o = over(old, col, ln);
	return fb_out(o.rgb + fbk_dith(v_in.uv * res), o.a);
}

/* Alchemy Smoke: like Windows Media Player's Alchemy, coloured smoke is
 * carried along a swirling noise current; glowing emitters wander around
 * and puff out more of it as their part of the spectrum gets louder. */
float4 PSMilkAlchemySmoke(VertData v_in) : TARGET
{
	float steps = fbk_steps();
	float rx = react_mix.x;

	float2 cc = fbk_asp(v_in.uv);
	float2 cp = cc * 2.5 + float2(time * 0.05, -time * 0.03);
	float e = 0.06;
	float nx1 = fbm4(cp + float2(e, 0.0));
	float nx2 = fbm4(cp - float2(e, 0.0));
	float ny1 = fbm4(cp + float2(0.0, e));
	float ny2 = fbm4(cp - float2(0.0, e));
	float2 curl = float2(ny1 - ny2, -(nx1 - nx2)) / (2.0 * e);
	float2 q = cc - curl * (0.0016 + 0.004 * mid * rx) * steps + float2(0.0, 0.0006 * steps);
	q *= pow(0.998, steps);
	float4 old = fbk_take(fbk_unasp(q), 0.72);
	old.rgb = fbk_huerot(old.rgb, 0.004 * steps * react_mix.z);

	float2 uv = view_uv(v_in.uv);
	float2 pv = fbk_asp(uv);
	float4 o = old;
	float count = clamp(4.0 + 3.0 * density, 2.0, 10.0);
	for (int i = 0; i < 10; i++) {
		float fi = float(i);
		if (fi < count) {
			float t = (fi + 0.5) / count;
			float2 cpos = float2(sin(time * (0.21 + 0.05 * fi) + fi * 2.1) * 0.6, sin(time * (0.17 + 0.04 * fi) + fi * 1.3) * 0.34);
			float en = band_level(t);
			float b = fbk_blob(pv, cpos, 0.025 + 0.06 * en * react_mix.y);
			float3 col = prism(cm_hue(hue + t * 0.8 + time * 0.03, uv, t, fi));
			o = over(o, col, saturate(b * (0.3 + 0.6 * en)));
		}
	}
	return fb_out(o.rgb + fbk_dith(v_in.uv * res), o.a);
}

/* Warp Tunnel: everything flows inwards into a tunnel; each frame a ring
 * of spectrum dots and a waveform polygon is drawn at the mouth. */
float4 PSMilkWarpTunnel(VertData v_in) : TARGET
{
	float steps = fbk_steps();
	float rx = react_mix.x;
	float hit = fbk_hit();

	float2 cc = fbk_asp(v_in.uv);
	float r = length(cc);
	float k = pow(1.012 + 0.025 * saturate(bass) * rx + 0.03 * hit, steps);
	float tw = 0.0015 * steps * (1.0 + 2.0 * mid * rx) / (r + 0.2);
	float4 old = fbk_take(fbk_unasp(rot2(cc * k, tw)), 0.5);
	old.rgb = fbk_huerot(old.rgb, 0.008 * steps * react_mix.z);

	float2 uv = view_uv(v_in.uv);
	float2 pv = fbk_asp(uv);
	float rv = length(pv);
	float av = atan2(pv.y, pv.x);
	float t = av / 6.2831853 + 0.5;
	/* polygon whose radius follows the spectrum */
	float rad = 0.42 + 0.06 * band_level(abs(t * 2.0 - 1.0)) * rx;
	float w = (1.5 + 2.0 * level * react_mix.y) / res.y;
	float ring = saturate(1.0 - abs(rv - rad) / (w * 2.0));
	float3 col = prism(cm_hue(hue + t + time * 0.05, uv, t, 0.0));
	float4 o = over(old, col, ring);
	/* spectrum dots around the mouth */
	float dots = clamp(16.0 + 16.0 * density, 8.0, 64.0);
	float cell = floor(t * dots);
	float ct = (cell + 0.5) / dots;
	float ca = (ct - 0.5) * 6.2831853;
	float en = band_level(abs(ct * 2.0 - 1.0));
	float2 dp = float2(cos(ca), sin(ca)) * (0.47 + 0.03 * en);
	float d = fbk_blob(pv, dp, 0.004 + 0.008 * en);
	o = over(o, lerp(col, float3(1.0, 1.0, 1.0), 0.4), saturate(d * en * 1.5));
	return fb_out(o.rgb + fbk_dith(v_in.uv * res), o.a);
}

/* Spiral Galaxy: the middle turns faster than the edge, so new sparks set
 * along two arms wind up into a spinning galaxy with a breathing core. */
float4 PSMilkSpiralGalaxy(VertData v_in) : TARGET
{
	float steps = fbk_steps();
	float rx = react_mix.x;

	/* the middle turns faster than the edge, so the arms wind up */
	float2 cc = fbk_asp(v_in.uv);
	float r0 = length(cc);
	float ang = 0.045 / (r0 * 5.0 + 0.35) * steps * (0.6 + mid * rx);
	float4 old = fbk_take(fbk_unasp(rot2(cc, -ang) * pow(0.999, steps)), 0.75);
	old.rgb = fbk_huerot(old.rgb, 0.003 * steps * react_mix.z);

	float2 uv = view_uv(v_in.uv);
	float2 pv = fbk_asp(uv);
	float r = length(pv) + 0.0001;
	float th = atan2(pv.y, pv.x);
	/* two faint arms drawn every frame, brighter where their part of the
	 * spectrum is loud */
	float ph = th * 2.0 - log(r) * 4.0 - time * 0.4;
	float arm = pow(0.5 + 0.5 * cos(ph), 10.0) * smoothstep(0.46, 0.08, r) * smoothstep(0.02, 0.06, r);
	float en = band_level(saturate(r * 2.2));
	float3 col = prism(cm_hue(hue + r * 1.2 + time * 0.02, uv, r, 0.0));
	float4 o = over(old, col, saturate(arm * (0.1 + 0.35 * en) * clamp(density, 0.3, 2.0)));

	/* star dust sparkling on the treble */
	float dust = step(0.9993 - 0.003 * treble * rx, hash21(floor(v_in.uv * res * 0.5) + floor(time * 60.0))) * smoothstep(0.45, 0.1, r);
	o = over(o, lerp(col, float3(1.0, 1.0, 1.0), 0.6), dust);

	float core = fbk_blob(pv, float2(0.0, 0.0), 0.02 + 0.03 * level * react_mix.y);
	o = over(o, lerp(prism(hue), float3(1.0, 1.0, 1.0), 0.6), saturate(core * (0.35 + 0.6 * level * react_mix.y)));
	return fb_out(o.rgb + fbk_dith(v_in.uv * res), o.a);
}

/* Mirror Echoes: last frame folded into four mirrored, shrinking copies of
 * itself, so a star drawn in the middle echoes away as a fractal. */
float4 PSMilkMirrorEcho(VertData v_in) : TARGET
{
	float steps = fbk_steps();
	float rx = react_mix.x;
	float hit = fbk_hit();

	float2 cc = fbk_asp(v_in.uv);
	float sc = 1.75 + 0.35 * saturate(bass) * rx + 0.25 * hit;
	float2 q = rot2(cc, time * 0.09) * sc;
	q = abs(q) - float2(0.42, 0.24) * (1.0 + 0.2 * sin(time * 0.3));
	q = rot2(q, 0.3 * sin(time * 0.13) + 0.2 * mid * rx);
	float4 fold = fbk_take(fbk_unasp(q), 0.35);
	float4 zoom = fbk_take(fbk_unasp(cc * pow(0.99, steps)), 0.35);
	float4 old = lerp(zoom, fold, 0.5);
	old.rgb = fbk_huerot(old.rgb, 0.01 * steps * react_mix.z);

	float2 uv = view_uv(v_in.uv);
	float2 pv = fbk_asp(uv);
	float r = length(pv);
	float a = atan2(pv.y, pv.x);
	float pts = 5.0 + floor(3.0 * frac(time * 0.05));
	float shape = 0.08 + 0.05 * level * react_mix.y + 0.03 * cos(a * pts + time) + 0.04 * band_level(frac(a / 6.2831853 + 0.5)) * rx;
	float w = 2.0 / res.y;
	float edge = saturate(1.0 - abs(r - shape) / (w * 2.0));
	float3 col = prism(cm_hue(hue + a / 6.2831853 + time * 0.05, uv, frac(a / 6.2831853), 0.0));
	float4 o = over(old, col, edge);
	return fb_out(o.rgb + fbk_dith(v_in.uv * res), o.a);
}

/* Ripple Pond: still water; drops land now and then and their rings ripple
 * across what's already there. Calm: the music only makes drops a little
 * more likely and the colours gently brighter. */
float4 PSMilkRipplePond(VertData v_in) : TARGET
{
	float steps = fbk_steps();
	float rx = react_mix.x;

	float2 cc = fbk_asp(v_in.uv);
	float2 disp = float2(0.0, 0.0);
	for (int i = 0; i < 3; i++) {
		float fi = float(i);
		float2 c = float2(sin(time * 0.05 + fi * 2.1) * 0.5, cos(time * 0.04 + fi * 1.7) * 0.25);
		float2 d = cc - c;
		float r = length(d) + 0.0001;
		disp += d / r * sin(r * 38.0 - time * 2.2 - fi) * exp(-r * 2.5);
	}
	float2 q = cc + disp * (0.0008 + 0.0012 * calm_in.x * rx) * steps;
	float4 old = fbk_take(fbk_unasp(q), 0.3);
	old.rgb = fbk_huerot(old.rgb, 0.002 * steps * react_mix.z);

	float2 uv = view_uv(v_in.uv);
	float2 pv = fbk_asp(uv);
	float4 o = old;
	/* drops: a few slots, each lands, rings out, and lands again elsewhere;
	 * each drop sends out three rings */
	float count = clamp(2.0 + 2.0 * density, 1.0, 6.0);
	for (int j = 0; j < 6; j++) {
		float fj = float(j);
		if (fj < count) {
			float period = 3.2 + fj * 0.7 - 1.0 * calm_in.x;
			float ph = time / period + fj * 0.37;
			float cyc = floor(ph);
			float age = frac(ph) * period;
			float2 dp = float2(hash11(cyc * 7.1 + fj) - 0.5, (hash11(cyc * 3.3 + fj * 1.9) - 0.5) * 0.6) * float2(1.6, 0.9);
			float dd = length(pv - dp);
			float rr = age * 0.09;
			float rings = 0.0;
			for (int m = 0; m < 3; m++) {
				float rm = rr - float(m) * 0.022;
				rings += step(0.0, rm) * saturate(1.0 - abs(dd - rm) * res.y / 1.5) * (1.0 - float(m) * 0.3);
			}
			float3 col = prism(cm_hue(hue + fj * 0.17 + cyc * 0.11, uv, fj / 6.0, fj));
			o = over(o, col, saturate(rings * exp(-age * 0.7) * (0.6 + 0.3 * calm_in.x * react_mix.y)));
		}
	}
	return fb_out(o.rgb + fbk_dith(v_in.uv * res), o.a);
}

/* Infinite Stars: fresh stars appear near the middle every frame and the
 * whole field zooms outwards, so they stretch into streaks: endless
 * hyperspace. Bass is the throttle, treble adds stars. */
float4 PSMilkInfiniteStars(VertData v_in) : TARGET
{
	float steps = fbk_steps();
	float rx = react_mix.x;

	/* last frame zooms out a little for a soft afterglow */
	float2 cc = fbk_asp(v_in.uv);
	float4 old = fbk_take(fbk_unasp(cc * pow(0.988, steps)), 0.3);

	/* stars fly out of the middle along their own direction, each drawn
	 * as a streak from where it was a moment ago to where it is now;
	 * three depths, the bass stretches the streaks */
	float2 uv = view_uv(v_in.uv);
	float2 pv = fbk_asp(uv);
	float r = length(pv);
	float a = atan2(pv.y, pv.x) / 6.2831853 + 0.5;
	float4 o = old;
	for (int l = 0; l < 3; l++) {
		float fl = float(l);
		float k = 70.0 + 45.0 * fl;
		float cell = floor(a * k);
		float ca = (cell + 0.5) / k;
		float h = hash11(cell * 1.37 + fl * 17.3);
		float there = step(h, 0.45 * clamp(density, 0.3, 2.0));
		float spd = 0.18 + 0.3 * hash11(cell * 3.1 + fl * 5.7);
		float ph = frac(time * spd + h * 7.0);
		float rs = 0.012 * exp(ph * 4.2);
		float len = rs * (0.1 + 0.3 * saturate(bass) * rx);
		float along = smoothstep(rs - len, rs, r) * step(r, rs + 0.002);
		float side = abs(a - ca) * 6.2831853 * r * res.y;
		float across = saturate(1.0 - side / (0.8 + 0.6 * ph));
		float fade = smoothstep(0.0, 0.3, ph) * (0.6 + 0.4 * level * react_mix.y);
		float3 col = lerp(float3(1.0, 1.0, 1.0), prism(cm_hue(hue + h * 3.0, uv, r, fl)), 0.45);
		o = over(o, col, saturate(along * across * fade * there));
	}
	return fb_out(o.rgb + fbk_dith(v_in.uv * res), o.a);
}

/* Fire Rising: heat from spectrum flames at the bottom rises with a
 * flicker, cooling from hot colours down through red to nothing. */
float4 PSMilkFireRising(VertData v_in) : TARGET
{
	float steps = fbk_steps();
	float rx = react_mix.x;

	float2 q = v_in.uv;
	float turb = vnoise(float2(q.x * 9.0, q.y * 6.0 + time * 1.3)) - 0.5;
	float turb2 = vnoise(float2(q.x * 23.0 + 3.0, q.y * 14.0 + time * 2.1)) - 0.5;
	q += float2((turb * 0.006 + turb2 * 0.0025), 0.005 + 0.005 * saturate(bass) * rx) * steps;
	float4 old = fbk_take(q, 0.55);
	/* cooling: lose blue and green first */
	float3 cool = pow(float3(0.997, 0.975, 0.93), float3(steps, steps, steps));
	old.rgb *= lerp(float3(1.0, 1.0, 1.0), cool, saturate(react_mix.z * 0.7 + 0.3));

	float2 uv = view_uv(v_in.uv);
	float en = band_level(abs(uv.x * 2.0 - 1.0) * 0.9);
	float hgt = 0.06 + 0.24 * en * (0.5 + rx * 0.7);
	float flick = vnoise(float2(uv.x * 30.0, time * 8.0));
	float heat = smoothstep(1.0 - hgt, 1.0, uv.y + 0.03 * flick);
	/* fire colours by default; the palette tints them */
	float3 fire = lerp(float3(1.0, 0.35, 0.05), float3(1.0, 0.85, 0.35), heat);
	float3 base = lerp(fire, prism(cm_hue(hue + uv.x * 0.25, uv, uv.x, 0.0)), 0.25 + 0.6 * pal_mix);
	float3 col = lerp(base, float3(1.0, 0.97, 0.85), 0.35 * heat * heat);
	float4 o = over(old, col, saturate(heat * (0.7 + 0.5 * level * react_mix.y)));

	/* a few embers thrown up by the treble */
	float ember = step(0.9993 - 0.003 * treble * rx, hash21(floor(v_in.uv * res * 0.5) + floor(time * 60.0))) * smoothstep(0.75, 0.92, uv.y);
	o = over(o, float3(1.0, 0.8, 0.45), ember);
	return fb_out(o.rgb + fbk_dith(v_in.uv * res), o.a);
}

/* Liquid Marble: three slow whirlpools stir thin lines of colour into
 * marbled swirls. Calm: barely any decay, gentle motion. */
float4 PSMilkLiquidMarble(VertData v_in) : TARGET
{
	float steps = fbk_steps();
	float rx = react_mix.x;

	float2 cc = fbk_asp(v_in.uv);
	float2 vel = float2(0.0, 0.0);
	for (int i = 0; i < 3; i++) {
		float fi = float(i);
		float2 c = float2(sin(time * 0.031 + fi * 2.3) * 0.5, cos(time * 0.027 + fi * 1.9) * 0.28);
		float2 d = cc - c;
		float s = 1.0 - 2.0 * step(0.5, fi) * step(fi, 1.5);
		vel += s * float2(-d.y, d.x) / (dot(d, d) + 0.04);
	}
	float2 q = cc - vel * (0.0011 + 0.0008 * calm_in.x * rx) * steps;
	float4 old = fbk_take(fbk_unasp(q), 0.9);
	old.rgb = fbk_huerot(old.rgb, 0.0015 * steps * react_mix.z);

	float2 uv = view_uv(v_in.uv);
	float w = (1.2 + 1.0 * calm_in.x * react_mix.y) / res.y;
	float ln1 = fbk_wline(uv, 0.36 + 0.06 * sin(time * 0.07), 0.07, time * 0.15, w * 2.0);
	float ln2 = fbk_wline(uv, 0.64 + 0.06 * sin(time * 0.05 + 2.0), 0.07, time * 0.12 + 3.0, w * 2.0);
	float4 o = over(old, prism(cm_hue(hue + uv.x * 0.4 + time * 0.01, uv, uv.x, 0.0)), ln1 * 0.6);
	o = over(o, prism(cm_hue(hue + 0.5 + uv.x * 0.4 + time * 0.01, uv, uv.x, 1.0)), ln2 * 0.6);
	return fb_out(o.rgb + fbk_dith(v_in.uv * res), o.a);
}

/* Rainbow Smear: the Winamp classic: a waveform across the middle, last
 * frame drifting outwards and turning through the rainbow, so every
 * wiggle leaves a colourful smear. */
float4 PSMilkRainbowSmear(VertData v_in) : TARGET
{
	float steps = fbk_steps();
	float rx = react_mix.x;

	float2 cc = fbk_asp(v_in.uv);
	float2 q = cc * float2(pow(0.993, steps), pow(0.985 - 0.012 * saturate(bass) * rx, steps));
	float4 old = fbk_take(fbk_unasp(q), 0.4);
	old.rgb = fbk_huerot(old.rgb, (0.04 + 0.04 * mid) * steps * react_mix.z);

	float2 uv = view_uv(v_in.uv);
	float w = (1.6 + 2.0 * level * react_mix.y) / res.y;
	float ln = fbk_wline(uv, 0.5, 0.2 * (0.5 + rx * 0.7), time * 0.8, w * 2.0);
	float3 col = prism(cm_hue(hue + time * 0.1, uv, uv.x, 0.0));
	float4 o = over(old, col, ln);
	return fb_out(o.rgb + fbk_dith(v_in.uv * res), o.a);
}

/* Vortex Drain: a ring of spectrum colour at the edge spirals tighter and
 * faster into a drain in the middle. */
float4 PSMilkVortexDrain(VertData v_in) : TARGET
{
	float steps = fbk_steps();
	float rx = react_mix.x;

	float2 cc = fbk_asp(v_in.uv);
	float r = length(cc);
	float k = pow(1.008 + 0.015 * saturate(bass) * rx, steps);
	float tw = 0.006 * steps * (1.0 + 1.5 * mid * rx) / (r + 0.08);
	float4 old = fbk_take(fbk_unasp(rot2(cc * k, tw)), 0.55);
	old.rgb = fbk_huerot(old.rgb, 0.006 * steps * react_mix.z);

	float2 uv = view_uv(v_in.uv);
	float2 pv = fbk_asp(uv);
	float rv = length(pv);
	float t = atan2(pv.y, pv.x) / 6.2831853 + 0.5;
	float segs = clamp(24.0 + 24.0 * density, 12.0, 96.0);
	float cell = floor(t * segs);
	float en = band_level(abs((cell + 0.5) / segs * 2.0 - 1.0));
	float rad = 0.44;
	float band = saturate(1.0 - abs(rv - rad) / (0.004 + 0.02 * en * react_mix.y));
	float gap = smoothstep(0.0, 0.15, frac(t * segs)) * smoothstep(1.0, 0.85, frac(t * segs));
	float3 col = prism(cm_hue(hue + (cell + 0.5) / segs + time * 0.04, uv, t, 0.0));
	float4 o = over(old, col, saturate(band * gap * (0.3 + en)));
	return fb_out(o.rgb + fbk_dith(v_in.uv * res), o.a);
}

/* Pulse Echoes: a glowing shape in the middle, and every bass pulse throws
 * the picture outwards so the shapes ripple out as echoes. */
float4 PSMilkPulseEcho(VertData v_in) : TARGET
{
	float steps = fbk_steps();
	float rx = react_mix.x;
	float hit = fbk_hit();

	/* the picture is thrown outwards mostly on the beat, so each hit
	 * leaves its own echo of the shape */
	float2 cc = fbk_asp(v_in.uv);
	float z = pow(0.995 - 0.008 * saturate(bass) * rx - 0.1 * hit, steps);
	float4 old = fbk_take(fbk_unasp(rot2(cc * z, 0.12 * hit)), 0.25);
	old.rgb = fbk_huerot(old.rgb, (0.004 * steps + 0.25 * hit) * react_mix.z);

	float2 uv = view_uv(v_in.uv);
	float2 pv = fbk_asp(uv);
	float r = length(pv);
	float a = atan2(pv.y, pv.x) + time * 0.3;
	float sides = 3.0 + floor(5.0 * frac(time * 0.03));
	float sector = 6.2831853 / sides;
	float poly = cos(sector * 0.5) / cos(glmod(a, sector) - sector * 0.5);
	float rad = (0.06 + 0.05 * level * react_mix.y) * poly;
	float w = 2.2 / res.y;
	float edge = saturate(1.0 - abs(r - rad) / (w * 2.0));
	float3 col = prism(cm_hue(hue + time * 0.07, uv, 0.0, 0.0));
	float4 o = over(old, col, edge * saturate(0.15 + 1.4 * hit));
	return fb_out(o.rgb + fbk_dith(v_in.uv * res), o.a);
}

/* Scope Trails: an oscilloscope Lissajous figure (bass across, treble up
 * and down) drawn every frame and left to drift and change colour, like a
 * phosphor screen that never quite forgets. */
float4 PSMilkScopeTrails(VertData v_in) : TARGET
{
	float steps = fbk_steps();
	float rx = react_mix.x;

	float2 cc = fbk_asp(v_in.uv);
	float4 old = fbk_take(fbk_unasp(rot2(cc * pow(0.997, steps), 0.002 * steps)), 0.12);
	old.rgb = fbk_huerot(old.rgb, 0.02 * steps * react_mix.z);

	float2 uv = view_uv(v_in.uv);
	float2 pv = fbk_asp(uv);
	float ax = 0.3 * (0.6 + 0.8 * saturate(bass) * rx);
	float ay = 0.3 * (0.6 + 0.8 * saturate(treble) * rx);
	float fa = 3.0;
	float fbq = 2.0 + floor(3.0 * frac(time * 0.02));
	fbq += 2.0 * step(2.5, fbq) * step(fbq, 3.5); /* 3:3 would collapse to a line */
	float best = 1.0;
	float2 prevp = float2(sin(time * 0.7) * ax, sin(time * 0.5 + 1.0) * ay);
	for (int i = 1; i <= 48; i++) {
		float u = float(i) / 48.0 * 6.2831853;
		float2 pt = float2(sin(fa * u + time * 0.7) * ax, sin(fbq * u + time * 0.5 + 1.0) * ay);
		float2 ab = pt - prevp;
		float hh = saturate(dot(pv - prevp, ab) / max(dot(ab, ab), 0.0000001));
		best = min(best, length(pv - prevp - ab * hh));
		prevp = pt;
	}
	float w = (1.0 + 1.0 * level * react_mix.y) / res.y;
	float ln = saturate(1.0 - best / (w * 2.0));
	float glow = exp(-best / (w * 5.0)) * 0.25;
	float3 col = prism(cm_hue(hue + time * 0.06, uv, 0.0, 0.0));
	float4 o = over(old, col, glow);
	o = over(o, lerp(col, float3(1.0, 1.0, 1.0), 0.35), ln);
	return fb_out(o.rgb + fbk_dith(v_in.uv * res), o.a);
}

/* ------------------------------------------------------------------ */

technique MilkZoomTunnel
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSMilkZoomTunnel(v_in);
	}
}

technique MilkKaleidoTrip
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSMilkKaleidoTrip(v_in);
	}
}

technique MilkKaleidoDream
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSMilkKaleidoDream(v_in);
	}
}

technique MilkKaleidoChaos
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSMilkKaleidoChaos(v_in);
	}
}

technique MilkGeissPlasma
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSMilkGeissPlasma(v_in);
	}
}

technique MilkAlchemySmoke
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSMilkAlchemySmoke(v_in);
	}
}

technique MilkWarpTunnel
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSMilkWarpTunnel(v_in);
	}
}

technique MilkSpiralGalaxy
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSMilkSpiralGalaxy(v_in);
	}
}

technique MilkMirrorEcho
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSMilkMirrorEcho(v_in);
	}
}

technique MilkRipplePond
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSMilkRipplePond(v_in);
	}
}

technique MilkInfiniteStars
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSMilkInfiniteStars(v_in);
	}
}

technique MilkFireRising
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSMilkFireRising(v_in);
	}
}

technique MilkLiquidMarble
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSMilkLiquidMarble(v_in);
	}
}

technique MilkRainbowSmear
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSMilkRainbowSmear(v_in);
	}
}

technique MilkVortexDrain
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSMilkVortexDrain(v_in);
	}
}

technique MilkPulseEcho
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSMilkPulseEcho(v_in);
	}
}

technique MilkScopeTrails
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSMilkScopeTrails(v_in);
	}
}
