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
  layout: [
    ['left', 'Logo left of the text'], ['right', 'Logo right of the text'], ['above', 'Logo above the text'],
    ['behind', 'Logo behind the text'], ['only', 'Logo only'], ['text', 'Text only'],
  ],
  logoAnim: [
    ['none', 'Still'], ['float', 'Float'], ['pulse', 'Pulse'], ['spin', 'Coin flip'], ['wobble', 'Wobble'],
    ['bounce', 'Bounce'], ['shine', 'Shine'],
  ],
  mask: [
    ['frame', "Style's frame"], ['none', 'No frame (just the picture)'], ['square', 'Square'], ['rounded', 'Rounded'],
    ['circle', 'Circle'], ['squircle', 'Squircle'], ['hexagon', 'Hexagon'], ['diamond', 'Diamond'], ['star', 'Star'],
    ['heart', 'Heart'], ['shield', 'Shield'], ['blob', 'Blob'],
  ],
  aspect: [
    ['square', "Style's box"], ['original', "Picture's own shape"], ['wide', 'Wide 16:9'], ['landscape', '4:3'],
    ['portrait', 'Tall 3:4'],
  ],
  fit: [['contain', 'Show the whole picture'], ['cover', 'Fill the shape (crop)']],
  runWhen: [
    ['always', 'Whenever the Sponsors switch is on'], ['streaming', 'Only while streaming'],
    ['live', 'While streaming or recording'],
  ],
  idle: [
    ['float', 'Float'], ['tilt', 'Tilt'], ['pulse', 'Pulse'], ['shine', 'Shine sweep'], ['flicker', 'Neon flicker'], ['none', 'Still'],
  ],
  anchor: [
    ['tl', 'Top left'], ['tc', 'Top centre'], ['tr', 'Top right'], ['ml', 'Middle left'], ['mc', 'Centre'],
    ['mr', 'Middle right'], ['bl', 'Bottom left'], ['bc', 'Bottom centre'], ['br', 'Bottom right'],
  ],
  mode: [
    ['loop', 'Every few minutes'], ['always', 'Always on screen'],
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

// Presets set the look and motion; enter / exit / textAnim / logoAnim go onto every sponsor.
// Your logos, text and timing stay.
// Mask shapes in a 100 x 100 box (SVG path data). The page masks the logo
// with these and SPONSORCENTER draws the same shapes on the card thumbnails.
const MASKS = {
  square: 'M0 0H100V100H0Z',
  rounded: 'M18 0H82Q100 0 100 18V82Q100 100 82 100H18Q0 100 0 82V18Q0 0 18 0Z',
  circle: 'M50 0A50 50 0 1 1 50 100A50 50 0 1 1 50 0Z',
  squircle: 'M50 0C90 0 100 10 100 50S90 100 50 100S0 90 0 50S10 0 50 0Z',
  hexagon: 'M25 3L75 3L100 50L75 97L25 97L0 50Z',
  diamond: 'M50 0L100 50L50 100L0 50Z',
  star: 'M50 4L62.3 37L97.6 38.5L70 60.5L79.4 94.5L50 75L20.6 94.5L30 60.5L2.4 38.5L37.7 37Z',
  heart: 'M50 92C20 72 0 54 0 30C0 13 13 2 28 2C38 2 46 8 50 16C54 8 62 2 72 2C87 2 100 13 100 30C100 54 80 72 50 92Z',
  shield: 'M50 0L95 14V48C95 74 75 92 50 100C25 92 5 74 5 48V14Z',
  blob: 'M52 2C74 1 93 14 97 37C100 60 92 84 69 95C46 100 18 95 7 72C0 50 5 23 24 11C32 5 42 3 52 2Z',
};

const PRESETS = [
  { name: 'Glass Card', p: { style: 'glass', enter: 'slide', exit: 'slide', textAnim: 'rise', logoAnim: 'float', idle: 'float',
    accent: '#8a5cff', accent2: '#ff5ad1', text: '#ffffff', bg: '#140f22', bgOpacity: 0.82, anchor: 'bl' } },
  { name: 'Broadcast Lower Third', p: { style: 'lower', enter: 'wipe', exit: 'wipe', textAnim: 'type', logoAnim: 'none', idle: 'none',
    accent: '#e8193c', accent2: '#ffd23d', text: '#ffffff', bg: '#101014', bgOpacity: 0.92, anchor: 'bl' } },
  { name: 'Neon Night', p: { style: 'neon', enter: 'glitch', exit: 'fade', textAnim: 'glow', logoAnim: 'pulse', idle: 'flicker',
    accent: '#ff2bd6', accent2: '#00e5ff', text: '#ffffff', bg: '#07030f', bgOpacity: 0.75, anchor: 'br' } },
  { name: 'Sunset Ribbon', p: { style: 'ribbon', enter: 'expand', exit: 'slide', textAnim: 'pop', logoAnim: 'wobble', idle: 'shine',
    accent: '#b8336a', accent2: '#ffc15e', text: '#ffffff', bg: '#3a1c71', bgOpacity: 1, anchor: 'tl' } },
  { name: 'News Ticker', p: { style: 'ticker', enter: 'slideup', exit: 'slideup', textAnim: 'none', logoAnim: 'none', idle: 'none',
    accent: '#1e88ff', accent2: '#ffd23d', text: '#ffffff', bg: '#0b1220', bgOpacity: 0.94, anchor: 'bc', marginY: 0 } },
  { name: 'Sticker Pop', p: { style: 'sticker', enter: 'pop', exit: 'pop', textAnim: 'wave', logoAnim: 'bounce', idle: 'float',
    accent: '#ff3f8e', accent2: '#2bb8ff', text: '#ffffff', bg: '#ffffff', bgOpacity: 1, anchor: 'br' } },
  { name: '2001 Window', p: { style: 'y2k', enter: 'drop', exit: 'pop', textAnim: 'type', logoAnim: 'none', idle: 'none',
    accent: '#3a1c71', accent2: '#f2545b', text: '#1e1530', bg: '#d8d4de', bgOpacity: 1, anchor: 'tr' } },
  { name: 'Corner Bug', p: { style: 'bug', enter: 'fade', exit: 'fade', textAnim: 'none', logoAnim: 'none', idle: 'pulse',
    accent: '#8a5cff', accent2: '#ff5ad1', text: '#ffffff', bg: '#000000', bgOpacity: 0.6, anchor: 'tr', size: 1 } },
  { name: 'Spotlight', p: { style: 'spot', enter: 'zoom', exit: 'zoom', textAnim: 'decode', logoAnim: 'spin', idle: 'none',
    accent: '#ff5ad1', accent2: '#ffc15e', text: '#ffffff', bg: '#120a1e', bgOpacity: 0.95, anchor: 'mc' } },
  { name: 'Minimal', p: { style: 'minimal', enter: 'fade', exit: 'fade', textAnim: 'rise', logoAnim: 'none', idle: 'none',
    accent: '#8a5cff', accent2: '#ff5ad1', text: '#ffffff', bg: '#000000', bgOpacity: 0, anchor: 'bc' } },
  { name: 'Glitch Promo', p: { style: 'glass', enter: 'glitch', exit: 'glitch', textAnim: 'decode', logoAnim: 'pulse', idle: 'tilt',
    accent: '#00e5ff', accent2: '#ff2bd6', text: '#ffffff', bg: '#05060c', bgOpacity: 0.88, anchor: 'bl' } },
  { name: 'Gold Luxe', p: { style: 'spot', enter: 'flip', exit: 'fade', textAnim: 'shimmer', logoAnim: 'shine', idle: 'none',
    accent: '#c98a12', accent2: '#ffe39a', text: '#fff6dd', bg: '#14100a', bgOpacity: 0.95, anchor: 'mr', font: 'Georgia' } },
  { name: 'Arcade', p: { style: 'neon', enter: 'spin', exit: 'spin', textAnim: 'pop', logoAnim: 'spin', idle: 'pulse',
    accent: '#ffe14d', accent2: '#5dff7a', text: '#ffffff', bg: '#0b0820', bgOpacity: 0.85, anchor: 'tc', font: 'Impact' } },
];

if (typeof module !== 'undefined') module.exports = { LISTS, PRESETS, MASKS };
