/* SPAZZbs visualizer pack: feedback (MilkDrop / Geiss / Alchemy style).
 * One row per style, in the order the Style list shows them:
 *   PACK_STYLE(key, technique, flags)
 * key       setting value and locale key "Style.<key>" (unique across ALL styles)
 * technique a technique in packs/feedback.fx
 * flags     VZ_AMOUNT | VZ_SPEED | VZ_CMOTION | VZ_FEEDBACK | VZ_CALM, or 0
 *           (a style that reads last frame through prev/prev_at needs VZ_FEEDBACK)
 * See tools/PACKS.md. */
PACK_STYLE("MilkZoomTunnel", "MilkZoomTunnel", VZ_SPEED | VZ_CMOTION | VZ_FEEDBACK)
PACK_STYLE("MilkKaleidoTrip", "MilkKaleidoTrip", VZ_AMOUNT | VZ_SPEED | VZ_CMOTION | VZ_FEEDBACK)
PACK_STYLE("MilkKaleidoDream", "MilkKaleidoDream", VZ_AMOUNT | VZ_SPEED | VZ_CMOTION | VZ_FEEDBACK | VZ_CALM)
PACK_STYLE("MilkKaleidoChaos", "MilkKaleidoChaos", VZ_SPEED | VZ_CMOTION | VZ_FEEDBACK)
PACK_STYLE("MilkGeissPlasma", "MilkGeissPlasma", VZ_SPEED | VZ_CMOTION | VZ_FEEDBACK)
PACK_STYLE("MilkAlchemySmoke", "MilkAlchemySmoke", VZ_AMOUNT | VZ_SPEED | VZ_CMOTION | VZ_FEEDBACK)
PACK_STYLE("MilkWarpTunnel", "MilkWarpTunnel", VZ_AMOUNT | VZ_SPEED | VZ_CMOTION | VZ_FEEDBACK)
PACK_STYLE("MilkSpiralGalaxy", "MilkSpiralGalaxy", VZ_AMOUNT | VZ_SPEED | VZ_CMOTION | VZ_FEEDBACK)
PACK_STYLE("MilkMirrorEcho", "MilkMirrorEcho", VZ_SPEED | VZ_CMOTION | VZ_FEEDBACK)
PACK_STYLE("MilkRipplePond", "MilkRipplePond", VZ_AMOUNT | VZ_SPEED | VZ_CMOTION | VZ_FEEDBACK | VZ_CALM)
PACK_STYLE("MilkInfiniteStars", "MilkInfiniteStars", VZ_AMOUNT | VZ_SPEED | VZ_CMOTION | VZ_FEEDBACK)
PACK_STYLE("MilkFireRising", "MilkFireRising", VZ_SPEED | VZ_CMOTION | VZ_FEEDBACK)
PACK_STYLE("MilkLiquidMarble", "MilkLiquidMarble", VZ_SPEED | VZ_CMOTION | VZ_FEEDBACK | VZ_CALM)
PACK_STYLE("MilkRainbowSmear", "MilkRainbowSmear", VZ_SPEED | VZ_CMOTION | VZ_FEEDBACK)
PACK_STYLE("MilkVortexDrain", "MilkVortexDrain", VZ_AMOUNT | VZ_SPEED | VZ_CMOTION | VZ_FEEDBACK)
PACK_STYLE("MilkPulseEcho", "MilkPulseEcho", VZ_SPEED | VZ_CMOTION | VZ_FEEDBACK)
PACK_STYLE("MilkScopeTrails", "MilkScopeTrails", VZ_SPEED | VZ_CMOTION | VZ_FEEDBACK)
