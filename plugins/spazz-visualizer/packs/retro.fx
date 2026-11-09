/* ================================================================== */
/* Retro pack: old media-player visualizations                         */
/* (styles listed in pack-retro.h; labels in packs/locale)             */
/* ================================================================== */

/* Battery: a kaleidoscope of glowing wavy rings round a hot core, each
 * ring one part of the spectrum. Amount = number of mirror arms. */
float4 PSRetroBattery(VertData v_in) : TARGET
{
	float2 uv = view_uv(v_in.uv);
	float aspect = res.x / res.y;
	float2 p = float2((uv.x - 0.5) * aspect, uv.y - 0.5);
	float r = length(p);
	float a = atan2(p.y, p.x);
	float arms = floor(4.0 + 4.0 * clamp(density, 0.3, 2.0));
	float seg = 6.2831853 / arms;
	float am = abs(glmod(a + time * 0.15, seg) - seg * 0.5);
	float2 q = float2(cos(am), sin(am)) * r;

	float3 light = float3(0.0, 0.0, 0.0);
	for (int i = 0; i < 5; i++) {
		float fi = float(i);
		float e = band_level(0.04 + fi * 0.19);
		float ring = 0.07 + fi * 0.07 +
			     0.025 * sin(q.y * 22.0 - time * (1.2 + fi * 0.45) + fi * 1.7) * (0.35 + e);
		float d = abs(length(q) - ring);
		float w = 0.003 + 0.008 * e;
		float g = w / (d + w);
		light += prism(hue + fi * 0.13 + r * 0.6) * g * g * (0.3 + 1.3 * e);
	}
	light += prism(hue + 0.5) * exp(-r * 10.0) * (0.25 + 1.6 * bass);
	return finish(light, uv);
}

technique RetroBattery
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSRetroBattery(v_in);
	}
}
