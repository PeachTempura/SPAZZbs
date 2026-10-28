// SPONSORCENTER: option lists and presets.
// `node tools/gen-sponsor.js` turns this into spazz-sponsor-spec.h.
// Values are the ones data/sponsor/index.html understands.

const LISTS = {
  style: [
    ['glass', 'Glass card'], ['lower', 'Broadcast lower third'], ['neon', 'Neon sign'], ['ribbon', 'Ribbon banner'],
    ['ticker', 'News ticker (full width)'], ['sticker', 'Sticker'], ['y2k', '2001 window'], ['bug', 'Corner bug (small)'],
    ['spot', 'Spotlight card'], ['minimal', 'Minimal (no background)'],
  ],
  enter: [
    ['slide', 'Slide in'], ['slideup', 'Rise up'], ['fade', 'Fade'], ['pop', 'Pop'], ['zoom', 'Zoom blur'], ['flip', 'Flip'],
    ['drop', 'Drop and bounce'], ['wipe', 'Wipe'], ['glitch', 'Glitch'], ['spin', 'Spin'], ['expand', 'Unfold'],
  ],
  textAnim: [
    ['rise', 'Letters rise in'], ['type', 'Typewriter'], ['pop', 'Words pop'], ['decode', 'Decode (scramble)'],
    ['wave', 'Wave'], ['shimmer', 'Shimmer'], ['glow', 'Glow pulse'], ['none', 'None'],
  ],
  imageAnim: [
    ['crossfade', 'Crossfade'], ['slide', 'Slide'], ['flip', 'Flip'], ['zoom', 'Zoom'], ['blur', 'Blur'], ['wipe', 'Wipe'], ['spin', 'Spin'],
  ],
  idle: [
    ['float', 'Float'], ['tilt', 'Tilt'], ['pulse', 'Pulse'], ['shine', 'Shine sweep'], ['flicker', 'Neon flicker'], ['none', 'Still'],
  ],
  anchor: [
    ['tl', 'Top left'], ['tc', 'Top centre'], ['tr', 'Top right'], ['ml', 'Middle left'], ['mc', 'Centre'],
    ['mr', 'Middle right'], ['bl', 'Bottom left'], ['bc', 'Bottom centre'], ['br', 'Bottom right'],
  ],
  mode: [
    ['loop', 'Loop: show, wait, show again'], ['always', 'Always on screen'],
    ['onshow', 'Once each time the scene goes live'], ['manual', 'Only when I press Play / the hotkey'],
  ],
  font: [
    ['', 'Style default'], ['Segoe UI', 'Segoe UI'], ['Bahnschrift', 'Bahnschrift'], ['Arial Black', 'Arial Black'],
    ['Impact', 'Impact'], ['Trebuchet MS', 'Trebuchet MS'], ['Tahoma', 'Tahoma'], ['Verdana', 'Verdana'], ['Georgia', 'Georgia'],
    ['Comic Sans MS', 'Comic Sans MS'], ['Consolas', 'Consolas'], ['Franklin Gothic Medium', 'Franklin Gothic'],
  ],
  imageShape: [['style', 'Style default'], ['square', 'Square'], ['rounded', 'Rounded'], ['circle', 'Circle']],
  imageFit: [['contain', 'Fit (logos)'], ['cover', 'Fill (photos)']],
};

// Presets set the look and motion; your text, images and timing stay.
const PRESETS = [
  { name: 'Glass Card', p: { style: 'glass', enter: 'slide', exit: 'slide', textAnim: 'rise', imageAnim: 'crossfade', idle: 'float',
    accent: '#8a5cff', accent2: '#ff5ad1', text: '#ffffff', bg: '#140f22', bgOpacity: 0.82, anchor: 'bl' } },
  { name: 'Broadcast Lower Third', p: { style: 'lower', enter: 'wipe', exit: 'wipe', textAnim: 'type', imageAnim: 'slide', idle: 'none',
    accent: '#e8193c', accent2: '#ffd23d', text: '#ffffff', bg: '#101014', bgOpacity: 0.92, anchor: 'bl' } },
  { name: 'Neon Night', p: { style: 'neon', enter: 'glitch', exit: 'fade', textAnim: 'glow', imageAnim: 'blur', idle: 'flicker',
    accent: '#ff2bd6', accent2: '#00e5ff', text: '#ffffff', bg: '#07030f', bgOpacity: 0.75, anchor: 'br' } },
  { name: 'Sunset Ribbon', p: { style: 'ribbon', enter: 'expand', exit: 'slide', textAnim: 'pop', imageAnim: 'flip', idle: 'shine',
    accent: '#b8336a', accent2: '#ffc15e', text: '#ffffff', bg: '#3a1c71', bgOpacity: 1, anchor: 'tl' } },
  { name: 'News Ticker', p: { style: 'ticker', enter: 'slideup', exit: 'slideup', textAnim: 'none', imageAnim: 'flip', idle: 'none',
    accent: '#1e88ff', accent2: '#ffd23d', text: '#ffffff', bg: '#0b1220', bgOpacity: 0.94, anchor: 'bc', marginY: 0 } },
  { name: 'Sticker Pop', p: { style: 'sticker', enter: 'pop', exit: 'pop', textAnim: 'wave', imageAnim: 'zoom', idle: 'float',
    accent: '#ff3f8e', accent2: '#2bb8ff', text: '#ffffff', bg: '#ffffff', bgOpacity: 1, anchor: 'br' } },
  { name: '2001 Window', p: { style: 'y2k', enter: 'drop', exit: 'pop', textAnim: 'type', imageAnim: 'wipe', idle: 'none',
    accent: '#3a1c71', accent2: '#f2545b', text: '#1e1530', bg: '#d8d4de', bgOpacity: 1, anchor: 'tr' } },
  { name: 'Corner Bug', p: { style: 'bug', enter: 'fade', exit: 'fade', textAnim: 'none', imageAnim: 'crossfade', idle: 'pulse',
    accent: '#8a5cff', accent2: '#ff5ad1', text: '#ffffff', bg: '#000000', bgOpacity: 0.6, anchor: 'tr', size: 1 } },
  { name: 'Spotlight', p: { style: 'spot', enter: 'zoom', exit: 'zoom', textAnim: 'decode', imageAnim: 'flip', idle: 'none',
    accent: '#ff5ad1', accent2: '#ffc15e', text: '#ffffff', bg: '#120a1e', bgOpacity: 0.95, anchor: 'mc' } },
  { name: 'Minimal', p: { style: 'minimal', enter: 'fade', exit: 'fade', textAnim: 'rise', imageAnim: 'crossfade', idle: 'none',
    accent: '#8a5cff', accent2: '#ff5ad1', text: '#ffffff', bg: '#000000', bgOpacity: 0, anchor: 'bc' } },
  { name: 'Glitch Promo', p: { style: 'glass', enter: 'glitch', exit: 'glitch', textAnim: 'decode', imageAnim: 'blur', idle: 'tilt',
    accent: '#00e5ff', accent2: '#ff2bd6', text: '#ffffff', bg: '#05060c', bgOpacity: 0.88, anchor: 'bl' } },
  { name: 'Gold Luxe', p: { style: 'spot', enter: 'flip', exit: 'fade', textAnim: 'shimmer', imageAnim: 'crossfade', idle: 'none',
    accent: '#c98a12', accent2: '#ffe39a', text: '#fff6dd', bg: '#14100a', bgOpacity: 0.95, anchor: 'mr', font: 'Georgia' } },
  { name: 'Arcade', p: { style: 'neon', enter: 'spin', exit: 'spin', textAnim: 'pop', imageAnim: 'spin', idle: 'pulse',
    accent: '#ffe14d', accent2: '#5dff7a', text: '#ffffff', bg: '#0b0820', bgOpacity: 0.85, anchor: 'tc', font: 'Impact' } },
];

if (typeof module !== 'undefined') module.exports = { LISTS, PRESETS };
