#pragma once

#include <cstdint>

// Row-walking glyph blit into a 1-bpp, MSB-first, row-major panel buffer.
//
// GfxRenderer::drawPixel() rotates, bounds-checks and bit-twiddles every pixel
// through a non-inlined call. A page of anti-aliased text walks its glyphs up
// to 15 times (BW pass + two gray planes in strips), so that per-pixel cost is
// the largest software share of a page turn. This blit resolves the
// glyph-to-panel mapping once per glyph: the mapping is axis-aligned with
// unit steps, so each glyph row becomes a run along either the physical row
// (bit shifts) or the physical column (byte strides), clipped to the target
// once up front. Kept free of GfxRenderer types so it can be verified on the
// host against a per-pixel reference (test/glyph_blit).
namespace glyphblit {

// Same order and meaning as GfxRenderer::Orientation.
enum class Orient : uint8_t { Portrait = 0, LandscapeCW = 1, PortraitInverted = 2, LandscapeCCW = 3 };

struct Target {
  uint8_t* buf;         // framebuffer, or the strip scratch in tiled-grayscale mode
  uint16_t panelW;      // physical panel width in pixels
  uint16_t panelH;      // physical panel height in pixels
  uint16_t widthBytes;  // bytes per physical row
  int rowOrigin;        // physical y of buf row 0 (0 for the framebuffer, band start for a strip)
  int rows;             // rows held by buf (panelH, or the band height)
  Orient orient;
};

// Logical screen (sx, sy) -> physical panel (px, py). Mirrors GfxRenderer's rotateCoordinates().
inline void toPanel(const Orient o, const int sx, const int sy, const int panelW, const int panelH, int& px,
                    int& py) {
  switch (o) {
    case Orient::Portrait:
      px = sy;
      py = panelH - 1 - sx;
      break;
    case Orient::LandscapeCW:
      px = panelW - 1 - sx;
      py = panelH - 1 - sy;
      break;
    case Orient::PortraitInverted:
      px = panelW - 1 - sy;
      py = sx;
      break;
    case Orient::LandscapeCCW:
    default:
      px = sx;
      py = sy;
      break;
  }
}

// Range of g in [gMin, gMax] with lo <= c + s*g <= hi (s = +1 or -1). Shrinks [gMin, gMax] in place.
inline void clipAxis(const int c, const int s, const int lo, const int hi, int& gMin, int& gMax) {
  int a, b;
  if (s > 0) {
    a = lo - c;
    b = hi - c;
  } else {
    a = c - hi;
    b = c - lo;
  }
  if (a > gMin) gMin = a;
  if (b < gMax) gMax = b;
}

// Draw glyph pixels. Glyph pixel (gx, gy) sits at logical screen
//   rotated90 == false: (screenX0 + gx, screenY0 + gy)
//   rotated90 == true:  (screenX0 + gy, screenY0 - gx)     (drawTextRotated90CW)
// is2Bit: 2 bits per pixel, MSB-first, value v = 3 - raw (0 black .. 3 white); a
// pixel is drawn when bit v of drawMask is set. 1-bit: drawn when the bit is 1.
// state true clears the buffer bit (black), false sets it.
inline void blit(const Target& t, const uint8_t* bitmap, const bool is2Bit, const int gw, const int gh,
                 const int screenX0, const int screenY0, const bool rotated90, const uint8_t drawMask,
                 const bool state) {
  if (gw <= 0 || gh <= 0 || t.rows <= 0) return;

  // Affine mapping (unit coefficients) from glyph space to panel space, derived
  // by evaluating the logical->panel transform at three points.
  int px0, py0, pxX, pyX, pxY, pyY;
  if (!rotated90) {
    toPanel(t.orient, screenX0, screenY0, t.panelW, t.panelH, px0, py0);
    toPanel(t.orient, screenX0 + 1, screenY0, t.panelW, t.panelH, pxX, pyX);
    toPanel(t.orient, screenX0, screenY0 + 1, t.panelW, t.panelH, pxY, pyY);
  } else {
    toPanel(t.orient, screenX0, screenY0, t.panelW, t.panelH, px0, py0);
    toPanel(t.orient, screenX0, screenY0 - 1, t.panelW, t.panelH, pxX, pyX);  // gx += 1 -> sy -= 1
    toPanel(t.orient, screenX0 + 1, screenY0, t.panelW, t.panelH, pxY, pyY);  // gy += 1 -> sx += 1
  }
  const int ax = pxX - px0, ay = pyX - py0;  // per-gx step in (px, py)
  const int bx = pxY - px0, by = pyY - py0;  // per-gy step in (px, py)

  // Clip the glyph box to the target: x in [0, panelW), y in [rowOrigin, rowOrigin + rows).
  int gxMin = 0, gxMax = gw - 1, gyMin = 0, gyMax = gh - 1;
  const int yLo = t.rowOrigin;
  const int yHi = t.rowOrigin + t.rows - 1;
  if (ax != 0) clipAxis(px0, ax, 0, t.panelW - 1, gxMin, gxMax);
  if (ay != 0) clipAxis(py0, ay, yLo, yHi, gxMin, gxMax);
  if (bx != 0) clipAxis(px0, bx, 0, t.panelW - 1, gyMin, gyMax);
  if (by != 0) clipAxis(py0, by, yLo, yHi, gyMin, gyMax);
  if (gxMin > gxMax || gyMin > gyMax) return;

  const int rowStride = ay * static_cast<int>(t.widthBytes);  // byte step per gx when moving along columns
  for (int gy = gyMin; gy <= gyMax; gy++) {
    const int px = px0 + ax * gxMin + bx * gy;
    const int py = py0 + ay * gxMin + by * gy;
    int byteIndex = (py - t.rowOrigin) * static_cast<int>(t.widthBytes) + (px >> 3);
    int bit = 7 - (px & 7);
    int pos = gy * gw + gxMin;  // bitmap pixel index
    for (int gx = gxMin; gx <= gxMax; gx++, pos++) {
      bool ink;
      if (is2Bit) {
        const uint8_t v = 3 - ((bitmap[pos >> 2] >> ((3 - (pos & 3)) * 2)) & 0x3);
        ink = (drawMask >> v) & 1;
      } else {
        ink = (bitmap[pos >> 3] >> (7 - (pos & 7))) & 1;
      }
      if (ink) {
        if (state) {
          t.buf[byteIndex] &= static_cast<uint8_t>(~(1u << bit));
        } else {
          t.buf[byteIndex] |= static_cast<uint8_t>(1u << bit);
        }
      }
      // Advance one glyph pixel: along the physical row (ax) or down/up columns (ay).
      if (ax > 0) {
        if (--bit < 0) {
          bit = 7;
          byteIndex++;
        }
      } else if (ax < 0) {
        if (++bit > 7) {
          bit = 0;
          byteIndex--;
        }
      } else {
        byteIndex += rowStride;
      }
    }
  }
}

}  // namespace glyphblit
