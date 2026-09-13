// Host self-test for lib/GfxRenderer/GlyphBlit.h: compares the fast blit against
// a per-pixel reference that mirrors GfxRenderer::drawPixel() (rotate, bounds,
// strip-band clip, MSB-first bit set/clear) over random glyphs, positions,
// orientations, text rotations, modes and strip bands.
//   c++ -std=c++17 -O2 -I../../lib/GfxRenderer glyph_blit_selftest.cpp -o /tmp/glyph_blit && /tmp/glyph_blit
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "GlyphBlit.h"

using namespace glyphblit;

static void refDrawPixel(const Target& t, int sx, int sy, bool state) {
  int px, py;
  toPanel(t.orient, sx, sy, t.panelW, t.panelH, px, py);
  if (px < 0 || px >= t.panelW || py < 0 || py >= t.panelH) return;  // drawPixel bounds check
  if (py < t.rowOrigin || py >= t.rowOrigin + t.rows) return;         // strip band clip
  const int row = py - t.rowOrigin;
  const uint32_t byteIndex = row * t.widthBytes + (px / 8);
  const uint8_t bit = 7 - (px % 8);
  if (state)
    t.buf[byteIndex] &= ~(1 << bit);
  else
    t.buf[byteIndex] |= 1 << bit;
}

// Mirrors the original renderCharImpl loops.
static void refBlit(const Target& t, const uint8_t* bitmap, bool is2Bit, int gw, int gh, int screenX0, int screenY0,
                    bool rotated90, uint8_t drawMask, bool state) {
  int pos = 0;
  for (int gy = 0; gy < gh; gy++) {
    for (int gx = 0; gx < gw; gx++, pos++) {
      int sx, sy;
      if (rotated90) {
        sx = screenX0 + gy;
        sy = screenY0 - gx;
      } else {
        sx = screenX0 + gx;
        sy = screenY0 + gy;
      }
      bool ink;
      if (is2Bit) {
        const uint8_t v = 3 - ((bitmap[pos >> 2] >> ((3 - (pos & 3)) * 2)) & 0x3);
        ink = (drawMask >> v) & 1;
      } else {
        ink = (bitmap[pos >> 3] >> (7 - (pos & 7))) & 1;
      }
      if (ink) refDrawPixel(t, sx, sy, state);
    }
  }
}

int main() {
  std::mt19937 rng(12345);
  const uint16_t panelW = 800, panelH = 480, widthBytes = 100;
  std::vector<uint8_t> a(widthBytes * panelH), b(widthBytes * panelH), bitmap(64 * 64);
  int failures = 0;
  const int iterations = 200000;
  for (int it = 0; it < iterations; it++) {
    const bool strip = rng() % 2;
    Target t;
    t.panelW = panelW;
    t.panelH = panelH;
    t.widthBytes = widthBytes;
    t.orient = static_cast<Orient>(rng() % 4);
    if (strip) {
      t.rows = 1 + rng() % 120;
      t.rowOrigin = rng() % (panelH - t.rows + 1);
    } else {
      t.rows = panelH;
      t.rowOrigin = 0;
    }
    const int gw = 1 + rng() % 48, gh = 1 + rng() % 48;
    const bool is2Bit = rng() % 2;
    for (auto& v : bitmap) v = rng();
    // Positions cover fully inside, straddling every edge, and fully outside.
    const int sx = static_cast<int>(rng() % 1000) - 100;
    const int sy = static_cast<int>(rng() % 1000) - 100;
    const bool rotated90 = rng() % 2;
    const uint8_t drawMask = rng() % 16;
    const bool state = rng() % 2;
    // Start from identical random buffers so set/clear both get exercised.
    for (size_t i = 0; i < a.size(); i++) a[i] = b[i] = rng();
    t.buf = a.data();
    blit(t, bitmap.data(), is2Bit, gw, gh, sx, sy, rotated90, drawMask, state);
    t.buf = b.data();
    refBlit(t, bitmap.data(), is2Bit, gw, gh, sx, sy, rotated90, drawMask, state);
    if (memcmp(a.data(), b.data(), a.size()) != 0) {
      failures++;
      if (failures <= 5) {
        printf("MISMATCH it=%d orient=%d strip=%d rowOrigin=%d rows=%d gw=%d gh=%d sx=%d sy=%d rot=%d mask=%u "
               "state=%d 2bit=%d\n",
               it, static_cast<int>(t.orient), strip, t.rowOrigin, t.rows, gw, gh, sx, sy, rotated90, drawMask,
               state, is2Bit);
      }
    }
  }
  printf("%d iterations, %d mismatches\n", iterations, failures);
  return failures == 0 ? 0 : 1;
}
