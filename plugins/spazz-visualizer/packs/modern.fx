/* ================================================================== */
/* Modern pack                                                          */
/* (styles listed in pack-modern.h; labels in packs/locale)             */
/* ================================================================== */

/* Soft Orbs: glowing orbs drifting on slow paths, each swelling with its
 * own band. Amount = how many (up to 16). */
float4 PSModernSoftOrbs(VertData v_in) : TARGET
{
	float2 uv = view_uv(v_in.uv);
	float aspect = res.x / res.y;
	float2 p = float2(uv.x * aspect, uv.y);
	float n = clamp(floor(6.0 + 6.0 * density), 1.0, 16.0);

	float3 light = float3(0.0, 0.0, 0.0);
	for (int i = 0; i < 16; i++) {
		float fi = float(i);
		if (fi < n) {
			float h1 = hash11(fi * 7.31 + 1.0);
			float h2 = hash11(fi * 3.17 + 5.0);
			float2 c = float2((0.5 + 0.42 * sin(time * (0.11 + 0.07 * h1) + fi * 2.1)) * aspect,
					  0.5 + 0.38 * cos(time * (0.09 + 0.06 * h2) + fi * 1.3));
			float e = band_level(fi / max(n - 1.0, 1.0));
			float rad = 0.035 + 0.05 * h2 + 0.06 * e;
			float d = length(p - c);
			float core = 1.0 - smoothstep(rad * 0.55, rad, d);
			float glow = exp(-d / (rad * 1.6));
			float h = cm_hue(hue + fi * 0.071, uv, fi / max(n, 1.0), fi);
			light += prism(h) * (core * 0.6 + glow * 0.35) * (0.4 + 1.2 * e);
		}
	}
	return finish_dither(light, uv, v_in.uv * res);
}

technique ModernSoftOrbs
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSModernSoftOrbs(v_in);
	}
}
