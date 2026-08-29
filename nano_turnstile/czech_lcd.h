#pragma once
#include <Arduino.h>
#include <LiquidCrystal_I2C.h>

/*
 * Czech text on an HD44780.
 *
 * The controller has no accented glyphs, but it does have eight slots of
 * custom character memory (CGRAM). This module decodes UTF-8, works out
 * which accented letters a message actually uses, loads up to eight of
 * them into CGRAM, and substitutes the slot numbers into the text.
 *
 * Messages needing more than eight distinct accented letters fall back to
 * unaccented ASCII for the extras - rare in practice, since most sentences
 * repeat the same few.
 *
 * d-caron and t-caron are rendered as d' and t', which is how they are
 * normally typeset anyway and saves two CGRAM slots.
 */

struct CzechGlyph {
  uint8_t b1, b2;         // the two UTF-8 bytes
  char    fallback;       // used when CGRAM is full
  uint8_t bitmap[8];      // 5x8 pixels, one byte per row
};

static const CzechGlyph CZECH_GLYPHS[] PROGMEM = {
  // lowercase
  {0xC3,0xA1,'a',{0b00010,0b00100,0b00000,0b01110,0b00001,0b01111,0b10001,0b01111}}, // á
  {0xC4,0x8D,'c',{0b01010,0b00100,0b00000,0b01110,0b10000,0b10000,0b10001,0b01110}}, // č
  {0xC3,0xA9,'e',{0b00010,0b00100,0b00000,0b01110,0b10001,0b11111,0b10000,0b01110}}, // é
  {0xC4,0x9B,'e',{0b01010,0b00100,0b00000,0b01110,0b10001,0b11111,0b10000,0b01110}}, // ě
  {0xC3,0xAD,'i',{0b00010,0b00100,0b00000,0b01100,0b00100,0b00100,0b00100,0b01110}}, // í
  {0xC5,0x88,'n',{0b01010,0b00100,0b00000,0b10110,0b11001,0b10001,0b10001,0b10001}}, // ň
  {0xC3,0xB3,'o',{0b00010,0b00100,0b00000,0b01110,0b10001,0b10001,0b10001,0b01110}}, // ó
  {0xC5,0x99,'r',{0b01010,0b00100,0b00000,0b10110,0b11001,0b10000,0b10000,0b10000}}, // ř
  {0xC5,0xA1,'s',{0b01010,0b00100,0b00000,0b01111,0b10000,0b01110,0b00001,0b11110}}, // š
  {0xC3,0xBA,'u',{0b00010,0b00100,0b00000,0b10001,0b10001,0b10001,0b10011,0b01101}}, // ú
  {0xC5,0xAF,'u',{0b00100,0b01010,0b00100,0b00000,0b10001,0b10001,0b10011,0b01101}}, // ů
  {0xC3,0xBD,'y',{0b00010,0b00100,0b00000,0b10001,0b10001,0b01111,0b00001,0b01110}}, // ý
  {0xC5,0xBE,'z',{0b01010,0b00100,0b00000,0b11111,0b00010,0b00100,0b01000,0b11111}}, // ž
  // uppercase
  {0xC3,0x81,'A',{0b00010,0b00100,0b00000,0b01110,0b10001,0b11111,0b10001,0b10001}}, // Á
  {0xC4,0x8C,'C',{0b01010,0b00100,0b00000,0b01110,0b10001,0b10000,0b10001,0b01110}}, // Č
  {0xC3,0x89,'E',{0b00010,0b00100,0b00000,0b11111,0b10000,0b11110,0b10000,0b11111}}, // É
  {0xC4,0x9A,'E',{0b01010,0b00100,0b00000,0b11111,0b10000,0b11110,0b10000,0b11111}}, // Ě
  {0xC3,0x8D,'I',{0b00010,0b00100,0b00000,0b11111,0b00100,0b00100,0b00100,0b11111}}, // Í
  {0xC5,0x87,'N',{0b01010,0b00100,0b00000,0b10001,0b11001,0b10101,0b10011,0b10001}}, // Ň
  {0xC3,0x93,'O',{0b00010,0b00100,0b00000,0b01110,0b10001,0b10001,0b10001,0b01110}}, // Ó
  {0xC5,0x98,'R',{0b01010,0b00100,0b00000,0b11110,0b10001,0b11110,0b10100,0b10010}}, // Ř
  {0xC5,0xA0,'S',{0b01010,0b00100,0b00000,0b01111,0b10000,0b01110,0b00001,0b11110}}, // Š
  {0xC3,0x9A,'U',{0b00010,0b00100,0b00000,0b10001,0b10001,0b10001,0b10001,0b01110}}, // Ú
  {0xC5,0xAE,'U',{0b00100,0b01010,0b00100,0b10001,0b10001,0b10001,0b10001,0b01110}}, // Ů
  {0xC3,0x9D,'Y',{0b00010,0b00100,0b00000,0b10001,0b01010,0b00100,0b00100,0b00100}}, // Ý
  {0xC5,0xBD,'Z',{0b01010,0b00100,0b00000,0b11111,0b00010,0b00100,0b01000,0b11111}}, // Ž
};

static const uint8_t CZECH_GLYPH_COUNT =
  sizeof(CZECH_GLYPHS) / sizeof(CZECH_GLYPHS[0]);

static const uint8_t CGRAM_SLOTS = 8;

// Find a glyph by its UTF-8 byte pair; returns index or -1.
static int8_t czechFind(uint8_t b1, uint8_t b2) {
  for (uint8_t i = 0; i < CZECH_GLYPH_COUNT; i++) {
    uint8_t g1 = pgm_read_byte(&CZECH_GLYPHS[i].b1);
    uint8_t g2 = pgm_read_byte(&CZECH_GLYPHS[i].b2);
    if (g1 == b1 && g2 == b2) return (int8_t)i;
  }
  return -1;
}

/*
 * Decode UTF-8 into display bytes, loading CGRAM as it goes.
 * Returns the number of bytes written to out.
 */
static uint8_t czechPrepare(LiquidCrystal_I2C &lcd, const char *utf8,
                            uint8_t *out, uint8_t maxOut) {
  int8_t  slotGlyph[CGRAM_SLOTS];
  uint8_t slotsUsed = 0;
  uint8_t outLen = 0;

  for (uint8_t i = 0; i < CGRAM_SLOTS; i++) slotGlyph[i] = -1;

  for (const uint8_t *p = (const uint8_t *)utf8; *p && outLen < maxOut; p++) {
    if (*p < 0x80) {                       // plain ASCII
      if (*p >= 0x20) out[outLen++] = *p;
      continue;
    }
    if ((*p & 0xC0) == 0x80) continue;     // stray continuation byte
    if (!p[1]) break;                      // truncated sequence

    uint8_t b1 = *p, b2 = p[1];
    p++;                                   // consume the second byte

    // d-caron and t-caron become d' and t'
    if (b1 == 0xC4 && (b2 == 0x8F || b2 == 0x8E)) {
      out[outLen++] = (b2 == 0x8F) ? 'd' : 'D';
      if (outLen < maxOut) out[outLen++] = '\'';
      continue;
    }
    if (b1 == 0xC5 && (b2 == 0xA5 || b2 == 0xA4)) {
      out[outLen++] = (b2 == 0xA5) ? 't' : 'T';
      if (outLen < maxOut) out[outLen++] = '\'';
      continue;
    }

    int8_t glyph = czechFind(b1, b2);
    if (glyph < 0) { out[outLen++] = '?'; continue; }

    // Already loaded this one?
    int8_t slot = -1;
    for (uint8_t s = 0; s < slotsUsed; s++) {
      if (slotGlyph[s] == glyph) { slot = (int8_t)s; break; }
    }

    if (slot < 0 && slotsUsed < CGRAM_SLOTS) {
      uint8_t bitmap[8];
      for (uint8_t r = 0; r < 8; r++) {
        bitmap[r] = pgm_read_byte(&CZECH_GLYPHS[glyph].bitmap[r]);
      }
      lcd.createChar(slotsUsed, bitmap);
      slotGlyph[slotsUsed] = glyph;
      slot = (int8_t)slotsUsed;
      slotsUsed++;
    }

    if (slot >= 0) {
      out[outLen++] = (uint8_t)slot;       // CGRAM slots are codes 0-7
    } else {
      out[outLen++] = pgm_read_byte(&CZECH_GLYPHS[glyph].fallback);
    }
  }

  return outLen;
}
