/* SPAZZbs visualizer pack: feedback (MilkDrop / Geiss style).
 * One row per style, in the order the Style list shows them:
 *   PACK_STYLE(key, technique, flags)
 * key       setting value and locale key "Style.<key>" (unique across ALL styles)
 * technique a technique in packs/feedback.fx
 * flags     VZ_AMOUNT | VZ_SPEED | VZ_CMOTION | VZ_FEEDBACK | VZ_CALM, or 0
 *           (a style that reads last frame through prev/prev_at needs VZ_FEEDBACK)
 * See tools/PACKS.md. */
PACK_STYLE("MilkZoomTunnel", "MilkZoomTunnel", VZ_SPEED | VZ_FEEDBACK)
