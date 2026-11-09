/* ================================================================== */
/* Feedback pack: MilkDrop / Geiss style, each frame warps the last     */
/* (styles listed in pack-feedback.h with VZ_FEEDBACK; see tools/PACKS.md) */
/* ================================================================== */

/* Zoom Tunnel: a ring shaped by the spectrum is drawn every frame, and
 * last frame is zoomed in and turned a little, so the rings stream into a
 * tunnel. Bass zooms harder, mids turn it. */
float4 PSMilkZoomTunnel(VertData v_in) : TARGET
{
	float aspect = res.x / res.y;
	/* frames at 60 fps this step stands for, so speed doesn't follow fps */
	float steps = clamp(prev_on.y * 60.0, 0.0, 4.0);

	/* last frame, zoomed towards the centre and turned */
	float2 c = v_in.uv - 0.5;
	c.x *= aspect;
	float z = pow(0.985 - 0.02 * saturate(bass), steps);
	c = rot2(c * z, (0.004 + 0.012 * mid) * steps);
	c.x /= aspect;
	float4 old = prev_at(c + 0.5);
	float keep = fb_decay(0.3);
	old *= keep;

	/* this frame's ring */
	float2 uv = view_uv(v_in.uv);
	float2 p = float2((uv.x - 0.5) * aspect, uv.y - 0.5);
	float r = length(p);
	float s = abs(atan2(p.y, p.x)) / 3.14159265;
	float rad = 0.12 + 0.17 * band_level(s) + 0.05 * beat;
	float w = 1.6 / res.y;
	float ring = saturate(1.0 - abs(r - rad) / (w * 2.0));
	float3 col = prism(hue + s * 0.5 + time * 0.05);

	float out_a = ring + old.a * (1.0 - ring);
	float3 rgb = (col * ring + old.rgb * old.a * (1.0 - ring)) / max(out_a, 0.0001);
	return fb_out(rgb, out_a);
}

technique MilkZoomTunnel
{
	pass
	{
		vertex_shader = VSDefault(v_in);
		pixel_shader  = PSMilkZoomTunnel(v_in);
	}
}
