// BitLogic.h - Logica de texto pura (sin Arduino) para NH-03. Nanohumano Labs.
// Se puede probar en un PC con g++.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum Answer : uint8_t { ANS_NONE = 0, ANS_SI = 1, ANS_NO = 2, ANS_NOSE = 3 };

// UTF-8 -> ASCII para la fuente integrada de la pantalla.
// Quita acentos y la enie, elimina simbolos de apertura y marcas de markdown.
inline void foldAscii(const char* in, char* out, size_t cap) {
  size_t o = 0;
  if (!cap) return;
  for (size_t i = 0; in[i] && o + 1 < cap;) {
    uint8_t c = (uint8_t)in[i];
    if (c < 0x80) {
      if (c == '*' || c == '`' || c == '#' || c == '\r') { i++; continue; }
      if (c == '\n' || c == '\t') c = ' ';
      out[o++] = (char)c; i++;
    } else if (c == 0xC3 && in[i + 1]) {
      uint8_t d = (uint8_t)in[i + 1]; char r = 0;
      switch (d) {
        case 0xA1: case 0xA0: case 0xA4: r = 'a'; break;
        case 0xA9: case 0xA8: case 0xAB: r = 'e'; break;
        case 0xAD: case 0xAC: case 0xAF: r = 'i'; break;
        case 0xB3: case 0xB2: case 0xB6: r = 'o'; break;
        case 0xBA: case 0xB9: case 0xBC: r = 'u'; break;
        case 0xB1: r = 'n'; break;
        case 0x81: r = 'A'; break; case 0x89: r = 'E'; break;
        case 0x8D: r = 'I'; break; case 0x93: r = 'O'; break;
        case 0x9A: case 0x9C: r = 'U'; break; case 0x91: r = 'N'; break;
        default: r = '?';
      }
      out[o++] = r; i += 2;
    } else if (c == 0xC2 && in[i + 1]) {
      uint8_t d = (uint8_t)in[i + 1];
      if (d == 0xBF || d == 0xA1) { i += 2; continue; }   // inicio de pregunta o exclamacion
      i += 2;
    } else if (c == 0xE2 && in[i + 1] && in[i + 2]) {
      uint8_t d = (uint8_t)in[i + 2];
      if (d == 0x98 || d == 0x99) out[o++] = '\'';
      else if (d == 0x9C || d == 0x9D) out[o++] = '"';
      else if (d == 0x93 || d == 0x94) out[o++] = '-';
      else if (d == 0xA6) { if (o + 3 < cap) { out[o++] = '.'; out[o++] = '.'; out[o++] = '.'; } }
      i += 3;
    } else {
      // Otros caracteres multibyte (emojis...): se omiten.
      i++;
      while (((uint8_t)in[i] & 0xC0) == 0x80) i++;
    }
  }
  out[o] = 0;
}

// Lee la respuesta de Gemini de forma tolerante: JSON {"respuesta":"SI"}, o texto suelto.
// Orden de prioridad: NO_SE, SI, NO. Mira solo palabras completas.
inline Answer parseAnswer(const char* text) {
  char buf[512];
  foldAscii(text ? text : "", buf, sizeof(buf));
  for (char* p = buf; *p; ++p) if (*p >= 'a' && *p <= 'z') *p -= 32;
  // Trocear en palabras de letras y guion bajo.
  char tok[16][24]; int nt = 0;
  for (size_t i = 0; buf[i] && nt < 16;) {
    while (buf[i] && !((buf[i] >= 'A' && buf[i] <= 'Z') || buf[i] == '_')) i++;
    if (!buf[i]) break;
    int l = 0;
    while (buf[i] && ((buf[i] >= 'A' && buf[i] <= 'Z') || buf[i] == '_')) {
      if (l < 23) tok[nt][l++] = buf[i];
      i++;
    }
    tok[nt][l] = 0; nt++;
  }
  for (int i = 0; i < nt; i++) {
    if (!strcmp(tok[i], "NO_SE") || !strcmp(tok[i], "NOSE")) return ANS_NOSE;
    if (!strcmp(tok[i], "SI")) return ANS_SI;
    if (!strcmp(tok[i], "NO")) {
      if (i + 1 < nt && (!strcmp(tok[i + 1], "SE") ||
          (!strcmp(tok[i + 1], "LO") && i + 2 < nt && !strcmp(tok[i + 2], "SE"))))
        return ANS_NOSE;
      return ANS_NO;
    }
  }
  return ANS_NONE;
}
