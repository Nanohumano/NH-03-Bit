// Bit3D.h - Motor 3D minimo para "Bit" (NH-03). Nanohumano Labs.
// Poliedros con sombreado plano, cara oculta y orden de pintor.
// Funciona con cualquier lienzo que tenga fillTriangle() y drawLine()
// (LovyanGFX LGFX_Sprite en la placa, un lienzo simulado en el PC).
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

namespace bit3d {

struct V3 { float x, y, z; };

// Malla: hasta 40 vertices y 64 triangulos (la estrella usa 32 y 60).
// m[] = mascara de aristas por triangulo:
//   bits 0..2 -> arista de contorno (v0v1, v1v2, v2v0)
//   bits 4..6 -> arista interior   (idem)
struct Mesh {
  V3 v[40];
  uint8_t nv = 0;
  uint8_t t[64][3];
  uint8_t m[64];
  uint8_t nt = 0;
};

inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// ---------- Icosaedro base ----------
static const float PHI = 1.6180339887f;
static const float ICO_V[12][3] = {
  {-1, PHI, 0}, {1, PHI, 0}, {-1, -PHI, 0}, {1, -PHI, 0},
  {0, -1, PHI}, {0, 1, PHI}, {0, -1, -PHI}, {0, 1, -PHI},
  {PHI, 0, -1}, {PHI, 0, 1}, {-PHI, 0, -1}, {-PHI, 0, 1}
};
static const uint8_t ICO_F[20][3] = {
  {0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11},
  {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
  {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9},
  {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}
};

// Estrella: icosaedro (spike=0) con una piramide en cada cara.
// spike ~0.0 -> icosaedro liso (reposo, azul). spike ~1.0 -> estrella afilada (NO).
inline void buildStar(Mesh& M, float spike) {
  M.nv = 0; M.nt = 0;
  for (int i = 0; i < 12; i++) {
    float x = ICO_V[i][0], y = ICO_V[i][1], z = ICO_V[i][2];
    float l = sqrtf(x * x + y * y + z * z);
    M.v[M.nv++] = {x / l, y / l, z / l};
  }
  for (int f = 0; f < 20; f++) {
    const V3& a = M.v[ICO_F[f][0]];
    const V3& b = M.v[ICO_F[f][1]];
    const V3& c = M.v[ICO_F[f][2]];
    V3 g = {(a.x + b.x + c.x) / 3, (a.y + b.y + c.y) / 3, (a.z + b.z + c.z) / 3};
    float len = sqrtf(g.x * g.x + g.y * g.y + g.z * g.z);
    float r = len + spike;
    uint8_t ap = M.nv;
    M.v[M.nv++] = {g.x / len * r, g.y / len * r, g.z / len * r};
    uint8_t A = ICO_F[f][0], B = ICO_F[f][1], C = ICO_F[f][2];
    const uint8_t tri[3][3] = {{A, B, ap}, {B, C, ap}, {C, A, ap}};
    for (int k = 0; k < 3; k++) {
      M.t[M.nt][0] = tri[k][0]; M.t[M.nt][1] = tri[k][1]; M.t[M.nt][2] = tri[k][2];
      M.m[M.nt] = 0x01 | 0x20 | 0x40;   // base = contorno; lados a la punta = interiores
      M.nt++;
    }
  }
}

// Octaedro (SI).
inline void buildOcta(Mesh& M, float r) {
  M.nv = 6; M.nt = 0;
  M.v[0] = { r, 0, 0}; M.v[1] = {-r, 0, 0};
  M.v[2] = {0,  r, 0}; M.v[3] = {0, -r, 0};
  M.v[4] = {0, 0,  r}; M.v[5] = {0, 0, -r};
  for (int sx = 0; sx < 2; sx++) for (int sy = 2; sy < 4; sy++) for (int sz = 4; sz < 6; sz++) {
    M.t[M.nt][0] = sx; M.t[M.nt][1] = sy; M.t[M.nt][2] = sz;
    M.m[M.nt] = 0x07;
    M.nt++;
  }
}

// Cubo (NO LO SE).
inline void buildCube(Mesh& M, float s) {
  M.nv = 8; M.nt = 0;
  for (int i = 0; i < 8; i++)
    M.v[i] = {(i & 1) ? s : -s, (i & 2) ? s : -s, (i & 4) ? s : -s};
  static const uint8_t Q[6][4] = {
    {0, 1, 3, 2}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 3, 7, 5}
  };
  for (int q = 0; q < 6; q++) {
    M.t[M.nt][0] = Q[q][0]; M.t[M.nt][1] = Q[q][1]; M.t[M.nt][2] = Q[q][2];
    M.m[M.nt++] = 0x03 | 0x40;
    M.t[M.nt][0] = Q[q][0]; M.t[M.nt][1] = Q[q][2]; M.t[M.nt][2] = Q[q][3];
    M.m[M.nt++] = 0x16;
  }
}

struct Pose {
  float yaw = 0, pitch = 0, roll = 0;
  float scale = 60;          // pixeles por unidad (radio ~1)
  float cx = 160, cy = 120;  // centro en pantalla
};

// edgeMode: 0 = solo contorno, 1 = solo interiores (puntas), 2 = ambos, 3 = ninguno, 4 = todas las aristas
template <class G>
void draw(G& g, const Mesh& M, const Pose& P,
          uint8_t cr, uint8_t cg, uint8_t cb, uint16_t edgeCol, int edgeMode) {
  const float CAM = 4.0f;
  const float F = P.scale * CAM;
  float sy_ = sinf(P.yaw), cy_ = cosf(P.yaw);
  float sp = sinf(P.pitch), cp = cosf(P.pitch);
  float sr = sinf(P.roll), cr_ = cosf(P.roll);

  V3 R[40];
  float sx[40], sy[40];
  for (int i = 0; i < M.nv; i++) {
    float x = M.v[i].x, y = M.v[i].y, z = M.v[i].z;
    float x1 = x * cy_ + z * sy_;  float z1 = -x * sy_ + z * cy_;  float y1 = y;
    float y2 = y1 * cp - z1 * sp;  float z2 = y1 * sp + z1 * cp;   float x2 = x1;
    float x3 = x2 * cr_ - y2 * sr; float y3 = x2 * sr + y2 * cr_;  float z3 = z2;
    R[i] = {x3, y3, z3};
    float zc = z3 + CAM;
    sx[i] = P.cx + x3 * F / zc;
    sy[i] = P.cy - y3 * F / zc;
  }

  // Luz fija (espacio camara, la camara mira hacia +z).
  const V3 L = {0.30f, 0.50f, -0.80f};

  uint8_t order[64];
  float key[64];
  float inten[64];
  int n = 0;
  for (int i = 0; i < M.nt; i++) {
    const V3& a = R[M.t[i][0]]; const V3& b = R[M.t[i][1]]; const V3& c = R[M.t[i][2]];
    V3 u = {b.x - a.x, b.y - a.y, b.z - a.z};
    V3 w = {c.x - a.x, c.y - a.y, c.z - a.z};
    V3 nn = {u.y * w.z - u.z * w.y, u.z * w.x - u.x * w.z, u.x * w.y - u.y * w.x};
    V3 cen = {(a.x + b.x + c.x) / 3, (a.y + b.y + c.y) / 3, (a.z + b.z + c.z) / 3};
    if (nn.x * cen.x + nn.y * cen.y + nn.z * cen.z < 0) { nn.x = -nn.x; nn.y = -nn.y; nn.z = -nn.z; }
    float l = sqrtf(nn.x * nn.x + nn.y * nn.y + nn.z * nn.z);
    if (l < 1e-6f) continue;
    nn.x /= l; nn.y /= l; nn.z /= l;
    // visible si la normal mira hacia la camara (en (0,0,-CAM))
    float vis = nn.x * (0 - cen.x) + nn.y * (0 - cen.y) + nn.z * (-CAM - cen.z);
    if (vis <= 0) continue;
    float d = nn.x * L.x + nn.y * L.y + nn.z * L.z;
    if (d < 0) d = 0;
    inten[i] = 0.40f + 0.60f * d;
    key[n] = cen.z;
    order[n++] = (uint8_t)i;
  }
  // Orden de pintor: lo mas lejano (z mayor) primero.
  for (int i = 1; i < n; i++) {
    uint8_t o = order[i]; float k = key[i]; int j = i - 1;
    while (j >= 0 && key[j] < k) { key[j + 1] = key[j]; order[j + 1] = order[j]; j--; }
    key[j + 1] = k; order[j + 1] = o;
  }

  for (int q = 0; q < n; q++) {
    int i = order[q];
    int a = M.t[i][0], b = M.t[i][1], c = M.t[i][2];
    float s = inten[i];
    uint16_t col = rgb565((uint8_t)(cr * s), (uint8_t)(cg * s), (uint8_t)(cb * s));
    g.fillTriangle((int)sx[a], (int)sy[a], (int)sx[b], (int)sy[b], (int)sx[c], (int)sy[c], col);
    if (edgeMode == 3) continue;
    uint8_t mk = M.m[i];
    const int e[3][2] = {{a, b}, {b, c}, {c, a}};
    for (int k = 0; k < 3; k++) {
      bool outline = mk & (1 << k);
      bool inner = mk & (1 << (k + 4));
      bool draw_ = (edgeMode == 0 && outline) || (edgeMode == 1 && inner) ||
                   (edgeMode == 2 && (outline || inner)) || (edgeMode == 4);
      if (draw_) g.drawLine((int)sx[e[k][0]], (int)sy[e[k][0]], (int)sx[e[k][1]], (int)sy[e[k][1]], edgeCol);
    }
  }
}

}  // namespace bit3d
