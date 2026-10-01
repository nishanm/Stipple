// SPDX-License-Identifier: GPL-3.0-or-later
//
// Draws the panels.
//
// Everything here works on a 52 x 16 buffer and nothing scales in JavaScript:
// the canvas is its real pixel size and CSS blows it up with
// image-rendering: pixelated, so a pixel on the page is a pixel on the device.
// Anything else would soften the one detail worth showing.

import { PANEL, FRAMES, FONT } from './frames.js';

const W = PANEL.width;
const H = PANEL.height;

// The firmware's own colours, so the clock on this page is the colour it is
// on the device.
const WHITE = [230, 230, 230];
const CYAN = [0, 200, 255];

// An unlit LED is not absolutely black.
//
// Drawn as #000 on a black page, the panel vanished and the clock read as
// floating text - which hid the one thing worth showing, that this is 52x16
// and every glyph is built from countable pixels. Looking at a real matrix
// you can see the dark LEDs, so showing them is the more truthful choice as
// well as the one that makes the panel legible.
const UNLIT = [12, 18, 22];

/* --- a 52x16 buffer ------------------------------------------------------- */

function blank() {
  const buf = new Uint8ClampedArray(W * H * 4);
  for (let p = 0; p < W * H; p++) {
    buf[p * 4] = UNLIT[0];
    buf[p * 4 + 1] = UNLIT[1];
    buf[p * 4 + 2] = UNLIT[2];
    buf[p * 4 + 3] = 255;
  }
  return buf;
}

function plot(buf, x, y, rgb) {
  if (x < 0 || y < 0 || x >= W || y >= H) return;
  const i = (y * W + x) * 4;
  buf[i] = rgb[0];
  buf[i + 1] = rgb[1];
  buf[i + 2] = rgb[2];
  buf[i + 3] = 255;
}

function present(canvas, buf) {
  const ctx = canvas.getContext('2d');
  ctx.putImageData(new ImageData(buf, W, H), 0, 0);
}

/* --- real frames ---------------------------------------------------------- */

// The fixtures are raw RGB, three bytes a pixel, which is what the device
// writes to the panel.
function decode(base64) {
  const binary = atob(base64);
  const buf = blank();
  for (let p = 0; p < W * H; p++) {
    const r = binary.charCodeAt(p * 3);
    const g = binary.charCodeAt(p * 3 + 1);
    const b = binary.charCodeAt(p * 3 + 2);
    // A pixel the firmware left black is an LED that is off, and off is what
    // UNLIT already holds. Everything else is drawn exactly as produced.
    if (r === 0 && g === 0 && b === 0) {
      continue;
    }
    buf[p * 4] = r;
    buf[p * 4 + 1] = g;
    buf[p * 4 + 2] = b;
  }
  return buf;
}

const byId = Object.fromEntries(FRAMES.map((f) => [f.id, f]));

/* --- the device font ------------------------------------------------------ */

// One pixel of space between glyphs, the same as the firmware's renderer.
const TRACKING = 1;

function glyph(ch) {
  return FONT[ch.codePointAt(0)] || null;
}

function measure(text) {
  let width = 0;
  for (const ch of text) {
    const g = glyph(ch);
    if (g) width += g[0] + TRACKING;
  }
  return Math.max(0, width - TRACKING);
}

function write(buf, text, x, y, rgb) {
  let cursor = x;
  for (const ch of text) {
    const g = glyph(ch);
    if (!g) continue;
    const advance = g[0];
    for (let row = 0; row < 7; row++) {
      const bits = g[row + 1];
      for (let col = 0; col < 5; col++) {
        // Five bits, most significant on the left; glyphs narrower than the
        // cell are left-aligned, which is why advance and width differ.
        if (bits & (1 << (4 - col))) plot(buf, cursor + col, y + row, rgb);
      }
    }
    cursor += advance + TRACKING;
  }
}

/* --- the live clock ------------------------------------------------------- */

// Built to match clock-secondsbar: the time across the top, its colon in the
// accent colour, and a bar along the bottom row filling through the minute.
function clockFrame(now) {
  const buf = blank();
  const hh = String(now.getHours()).padStart(2, '0');
  const mm = String(now.getMinutes()).padStart(2, '0');

  const text = hh + ':' + mm;
  const x = Math.round((W - measure(text)) / 2);
  write(buf, text, x, 1, WHITE);

  // The colon alone in cyan, which is what makes it read as a clock rather
  // than four digits.
  const colonX = x + measure(hh) + TRACKING;
  write(buf, ':', colonX, 1, CYAN);

  const filled = Math.round((W * now.getSeconds()) / 60);
  for (let bx = 0; bx < filled; bx++) {
    plot(buf, bx, 14, CYAN);
    plot(buf, bx, 15, CYAN);
  }
  return buf;
}

/* --- hero ----------------------------------------------------------------- */

const hero = document.getElementById('hero-panel');
const caption = document.getElementById('hero-caption');

function runHero() {
  if (!hero) return;
  const quiet = window.matchMedia('(prefers-reduced-motion: reduce)').matches;

  let ticking = null;
  const startClock = () => {
    const tick = () => {
      present(hero, clockFrame(new Date()));
      if (caption) caption.textContent = '52 × 16 · 832 pixels · your time, in the device font';
    };
    tick();
    ticking = setInterval(tick, 1000);
  };

  if (quiet) {
    startClock();
    return;
  }

  // One orchestrated moment on load: the boot sequence the device actually
  // runs, then it settles into the clock and nothing else on the page moves.
  const script = [
    { id: 'splash-page-one', text: 'It starts by saying what it is', hold: 1800 },
    { id: 'splash-page-two', text: 'Then where to find it on your network', hold: 1800 },
  ];

  let step = 0;
  const advance = () => {
    if (step >= script.length) {
      startClock();
      return;
    }
    const { id, text, hold } = script[step++];
    const frame = byId[id];
    if (frame) {
      present(hero, decode(frame.rgb));
      if (caption) caption.textContent = text;
    }
    setTimeout(advance, hold);
  };
  advance();

  window.addEventListener('pagehide', () => clearInterval(ticking));
}

/* --- specimens ------------------------------------------------------------ */

// The built-in apps, as animations of the real renderer. Generated by
// `dev.ps1 previews` into site/defaults/.
const DEFAULT_APPS = [
  { file: 'clock', caption: 'The clock, the app it boots into' },
  { file: 'stopwatch', caption: 'A stopwatch: press to start, stop and reset' },
  { file: 'battery', caption: 'The battery, charging' },
];

function buildSpecimens() {
  const host = document.querySelector('.specimens');
  if (!host) return;

  for (const app of DEFAULT_APPS) {
    const figure = document.createElement('figure');
    figure.className = 'specimen';

    const img = document.createElement('img');
    img.src = 'defaults/' + app.file + '.gif';
    img.width = 312;
    img.height = 96;
    img.loading = 'lazy';
    img.alt = app.caption;

    const text = document.createElement('p');
    text.textContent = app.caption;

    figure.append(img, text);
    host.append(figure);
  }
}

buildSpecimens();
runHero();
