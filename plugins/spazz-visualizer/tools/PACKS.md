# Visualizer style packs

The built-in styles live in `data/spazz-visualizer.effect` (techniques listed
in `style_techniques[]` in `spazz-visualizer.c`). More styles come in three
packs, each its own effect file, so packs can be written in parallel without
touching the same files:

| pack     | you edit                                                  | generated (commit it)       |
|----------|-----------------------------------------------------------|-----------------------------|
| retro    | `packs/retro.fx`, `pack-retro.h`, `packs/locale/pack-retro.ini.part`          | `data/pack-retro.effect`    |
| feedback | `packs/feedback.fx`, `pack-feedback.h`, `packs/locale/pack-feedback.ini.part` | `data/pack-feedback.effect` |
| modern   | `packs/modern.fx`, `pack-modern.h`, `packs/locale/pack-modern.ini.part`       | `data/pack-modern.effect`   |

Don't edit `packs/common.fxh`, `spazz-visualizer.c`, `data/spazz-visualizer.effect`
or `data/locale/en-US.ini` from a pack: the `.ini.part` lines get merged into
`en-US.ini` at the end.

## Adding a style

1. **Shader** — in `packs/<pack>.fx`, write `float4 PS<Name>(VertData v_in) : TARGET`
   and its technique:

   ```
   technique <Name>
   {
   	pass
   	{
   		vertex_shader = VSDefault(v_in);
   		pixel_shader  = PS<Name>(v_in);
   	}
   }
   ```

   Technique names must be unique across ALL effects (built-in + every pack);
   prefix them with the pack (`Retro…`, `Milk…`, `Modern…`).

2. **Row** — add to `pack-<pack>.h`, in the order the Style list should show it:

   ```
   PACK_STYLE("<Key>", "<Name>", VZ_AMOUNT | VZ_SPEED)
   ```

   - `<Key>`: the setting value saved in scenes and the locale key
     `Style.<Key>`. Letters/digits/`_`, unique across all styles. Never rename
     a shipped key (saved scenes point at it). Usually the same as the technique.
   - flags (or `0`):
     - `VZ_AMOUNT` the shader reads `density` (shows the Amount slider)
     - `VZ_SPEED` it moves with `time`/`flow` (shows Speed)
     - `VZ_CMOTION` its colours go through `cm_hue()` (shows Colour motion)
     - `VZ_FEEDBACK` it reads last frame (see below) — required if it does
     - `VZ_CALM` a calm style (slow, no flashing; use `calm_in` / `calm_e()`)

3. **Label** — add `Style.<Key>="Pack · Nice Name"` to `packs/locale/pack-<pack>.ini.part`.

4. **Generate** — `/opt/node22/bin/node tools/gen-packs.js` (from the plugin
   folder). It writes `data/pack-<pack>.effect` and checks that rows,
   techniques and labels agree, keys/techniques are unique, flags are known,
   and that only `VZ_FEEDBACK` styles read `prev`. CI runs it with `--check`.

## What a shader gets (`packs/common.fxh`)

Uniforms (set by the plugin on every draw): `bands` (64×1: r level, g peak,
b onset flash; read with `band_level(x)` / `band_full(x)`), `res`, `time`,
`flow`, `bass`, `mid`, `treble`, `level`, `beat`, `hue`, `density`, `bg`,
`pal_a/b/c`, `pal_mix`, `grade`, `bg_col`, `bg_flat`, `view`, `flip_x`,
`cmotion`, `calm_in`, `prev`, `prev_on`.

Helpers: `hash21`, `hash11`, `prism(h)` (palette colour), `view_uv(uv)`
(zoom/mirror/flip — call it on `v_in.uv` first), `graded`, `midnight`,
`finish(light, uv)` (tone-map light + backdrop + fade: what most styles
return), `finish_dither`, `finish_paint(rgba, uv)` (solid painted styles),
`over`, `glmod`, `rot2`, `vnoise`, `fbm4`, `cm_hue(h, uv, along, layer)`,
`calm_e(x, k)`.

Coordinates: `uv` 0..1 from the top left; for round shapes use
`float2(uv.x * res.x / res.y, uv.y)`; one pixel is `1.0 / res.y` in those units.

## Feedback styles (MilkDrop / Geiss / Alchemy look)

Flag them `VZ_FEEDBACK`. The plugin keeps two buffers the size of the source
and, once per video frame, draws the style into one with **last frame** as
`prev`, blending off, then swaps; the result is shown through the pack's
`FeedbackShow` technique (grading, backdrop and crossfade happen there).

In the shader:

- `prev_at(uv)` — last frame at `uv` (0..1, buffer space: use `v_in.uv`, not
  `view_uv`), straight colour + alpha, transparent outside 0..1 and on the
  first frame (`prev_on.x` is 0 then).
- `prev_on`: x 1 when last frame is valid, y seconds since last frame, z/w
  size in pixels. Scale per-frame warps by `prev_on.y * 60.0` and fades with
  `fb_decay(keep_per_second)`, so it looks the same at any fps.
- Draw new content with `view_uv(v_in.uv)` coordinates, composite it over the
  warped/decayed `prev`, and **return `fb_out(rgb, alpha)`** — ungraded,
  without backdrop, never `finish()` (it would grade and tone-map again every
  frame). Keep alpha meaningful so a transparent background still works.
- Only the style on screen (or the one fading out, if the new one isn't a
  feedback style) uses the buffers; the buffers are cleared when a feedback
  style comes on, and freed 5 s after no feedback style is showing.

See `PSMilkZoomTunnel` in `packs/feedback.fx`.

## Rules (both D3D11/HLSL and OpenGL/GLSL)

- No C-style casts (`float(i)` is fine, `(float)i` isn't).
- Avoid names reserved in HLSL or GLSL: `lit`, `active`, `half`, `line`,
  `far`, `near`, `sample`, `input`, `output`, `filter`, `texture`, `common`,
  `smooth`, `flat`, `noise`, `fixed`, `precision`, `in`/`out` as names, etc.
- `glmod(x, y)` instead of `%`/`fmod` for negative numbers.
- Sample textures with `SampleLevel(..., 0.0)` inside loops and branches.
- `[branch]` only via the `BRANCH` macro (D3D only).
- Bounded loops (constant upper limit, `if` inside to stop early).
- Don't declare new uniforms: the plugin only sets the ones in `common.fxh`
  (an unset uniform makes D3D11 skip the draw).

## Style settings (per-style knobs)

A style can have its own controls, shown in a "Style settings" group only
while that style is picked. Add rows to the `knobs[]` table in
`spazz-visualizer.c`: style key, setting name, locale label, type (slider,
list, bool or colour), range, default, and a slot. The shader reads the slot:

| slots | uniform |
|-------|---------|
| 0-3   | `style_a.xyzw` |
| 4-7   | `style_b.xyzw` |
| 8-11  | `style_c.xyzw` |
| 12    | `style_col.rgb` (a colour knob) |
| 15    | `style_col.w` |

Sliders and bools give their value, lists the item's index. List items are
labelled `<label>.<item>` in the locale, and `<label>.Tip` (if present) is the
tooltip. Unused slots are 0.

`music_ease` carries smoothed music made for calm, flowing styles:
`x` counts beats with a 0.3 s glide (wraps at 64, so use it in whole steps
that repeat every 64 beats), `y` a soft pulse on each beat (eases up and
back down over about half a second), `z` the bass, smoothed over a second.
See `PSModernJellyfish` and `PSModernBlossoms` in `packs/modern.fx`.

## Checking a pack

From the repo root:

```
node plugins/spazz-visualizer/tools/gen-packs.js --check
S=/tmp/claude-0/-home-claude/8884d516-1966-5e41-abde-ceee8ce9f4ad/scratchpad
$S/fxparse plugins/spazz-visualizer/data/pack-retro.effect $S/out-retro     # HLSL
/home/claude/deps/gltest/fxgl plugins/spazz-visualizer/data/pack-retro.effect $S/out-retro  # GLSL
```

Render in the WebGL harness (`/home/claude/deps/vizgl`):

```
python3 packs.py retro:RetroBattery,feedback:MilkZoomTunnel out.png 60,150,151 '{}' 1.0 640 360
```

`packs.py` regenerates GLSL from the pack effects itself; feedback styles are
run frame by frame from 0 through ping-pong framebuffers, so ask for
consecutive frames (e.g. `150,151`) to judge motion. Arguments: styles as
`pack:Technique`, output png, frames, extra uniforms as JSON (e.g.
`{"density":1.6,"cmotion":[1,0.3,0,0]}`), background 0..1, width, height.
