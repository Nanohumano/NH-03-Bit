// NH-03 BIT V0.8 - Asistente de escritorio estilo Tron (Bit: SI / NO / NO LO SE)
// Nanohumano Labs. Placa: LCDWIKI ES3C28P (ESP32-S3 2.8" tactil FT6336G, ES8311).
// Gemini (nivel gratuito, sin facturacion) + voz de entrada por el micro + sonidos propios.
//
// Librerias: LovyanGFX, WiFiManager (tzapu), ArduinoJson v7. Core ESP32 3.x (ESP_I2S).
// Placa en Arduino IDE: ESP32S3 Dev Module, PSRAM = OPI PSRAM, Flash 16 MB.
//
// Uso:
//   - Tocar la pantalla (modo BIT o TEXTO): habla tu pregunta.
//   - Deslizar a izquierda/derecha: BIT <-> TEXTO <-> TIEMPO.
//   - Mantener pulsado 1,5 s el icono WiFi (arriba a la derecha): portal de configuracion
//     (WiFi, clave de Gemini, modelo).
//   - Monitor serie (115200): escribe una frase y se envia como pregunta de texto.
//     Comandos: KEY <clave>, MODEL <modelo>, STATUS, ANS SI|NO|NOSE, TXT <texto>, SND <n>, W, VOL+, VOL-
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <mbedtls/base64.h>
#include <esp_heap_caps.h>
#include "ESP_I2S.h"
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <time.h>
#include <math.h>
#include "Bit3D.h"
#include "BitLogic.h"

// Tipos propios: deben ir antes de cualquier funcion (el IDE genera prototipos arriba).
struct Note { uint16_t f0, f1, ms; uint8_t wave; uint8_t amp; };   // wave 0 cuadrada, 1 triangular, 2 seno
struct Reply { Answer a; char text[1100]; char heard[96]; char err[56]; int http; };
struct BitAnim { float sc = 1.0f, ay = 0.0f, h = 1.3f; int shape = 0; uint32_t last = 0; };

#ifdef SET_LOOP_TASK_STACK_SIZE
SET_LOOP_TASK_STACK_SIZE(16 * 1024);
#endif

// ---------------------------------------------------------------- Configuracion
static const char* LAT_S = "39.5755";            // Andratx
static const char* LON_S = "2.4202";
static const char* DEFAULT_MODEL = "gemini-3.1-flash-lite-preview";  // el que ya te funciono; editable en el portal
static const int   DAILY_LIMIT = 150;            // llamadas/dia propias (tope de seguridad)
static const uint32_t ANSWER_HOLD_MS = 12000;
#define FW_VER "V0.8"
static const int   MIC_GAIN = 12;                // como en NanoDesk V8.2
static const int   VAD_MIN = 90;                 // umbral minimo de voz (RMS tras ganancia)
static const uint32_t VAD_END_MS = 900;          // silencio que cierra la grabacion
static const uint32_t NOSPEECH_MS = 4000;
static const uint32_t MIN_REC_MS = 700;
static const size_t REC_MAX_S = 6;
static const int   BEEP_AMP = 4500;              // amplitud PCM de los sonidos

#define I2C_SDA 16
#define I2C_SCL 15
#define I2S_MCLK 4
#define I2S_BCLK 5
#define I2S_DOUT 8
#define I2S_LRCK 7
#define I2S_DIN 6
#define PIN_PA 1          // amplificador activo en LOW (verificado en NanoDesk)

// ---------------------------------------------------------------- Pantalla
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 panel;
  lgfx::Bus_SPI bus;
public:
  LGFX() {
    auto b = bus.config();
    b.spi_host = SPI2_HOST; b.spi_mode = 0;
    b.freq_write = 40000000; b.freq_read = 16000000;
    b.spi_3wire = false; b.use_lock = true; b.dma_channel = SPI_DMA_CH_AUTO;
    b.pin_sclk = 12; b.pin_mosi = 11; b.pin_miso = 13; b.pin_dc = 46;
    bus.config(b); panel.setBus(&bus);
    auto p = panel.config();
    p.pin_cs = 10; p.pin_rst = -1; p.pin_busy = -1;
    p.panel_width = 240; p.panel_height = 320;
    p.offset_x = 0; p.offset_y = 0; p.readable = false;
    p.invert = true; p.rgb_order = false; p.dlen_16bit = false; p.bus_shared = false;
    panel.config(p); setPanel(&panel);
  }
};
LGFX lcd;
LGFX_Sprite canvas(&lcd);
static const int W = 320, H = 240, BAR_H = 24;

static inline uint16_t C(uint8_t r, uint8_t g, uint8_t b) { return bit3d::rgb565(r, g, b); }
uint16_t cBG, cPANEL, cLINE, cGRID, cCYAN, cWHITE, cMUTED, cAMBER, cRED, cVIOLET, cGREEN;
void initColors() {
  cBG = C(2, 8, 18); cPANEL = C(6, 18, 32); cLINE = C(22, 78, 105); cGRID = C(8, 34, 52);
  cCYAN = C(80, 225, 255); cWHITE = 0xFFFF; cMUTED = C(120, 160, 180);
  cAMBER = C(255, 190, 40); cRED = C(255, 60, 50); cVIOLET = C(175, 100, 255); cGREEN = C(80, 255, 140);
}
uint16_t mixBG(uint8_t r, uint8_t g, uint8_t b, float k) {   // color atenuado hacia el fondo
  return C((uint8_t)(2 + (r - 2) * k), (uint8_t)(8 + (g - 8) * k), (uint8_t)(18 + (b - 18) * k));
}

// ---------------------------------------------------------------- Estado compartido
enum Mode : uint8_t { M_BIT = 0, M_TEXT, M_WEATHER, M_LIGHT, M_SETTINGS, M_COUNT };
enum AState : uint8_t { S_IDLE = 0, S_LISTEN, S_THINK, S_ANSWER, S_ERROR };
enum Job : uint8_t { J_NONE = 0, J_VOICE, J_TEXT, J_WEATHER, J_SOUND };

Mode mode = M_BIT;
volatile AState st = S_IDLE;
volatile uint32_t stAt = 0;
volatile uint8_t ansCode = ANS_NONE;
volatile float micLevel = 0;
volatile Job job = J_NONE;
volatile bool jobTextMode = false;
volatile bool stopRec = false;
volatile int pendingSnd = 0;
volatile uint32_t blockedUntil = 0;
String pendingQ;
String ansText, heardText, errText;
SemaphoreHandle_t mtx;
Preferences prefs;
String gKey, gModel;
bool portalRequested = false;
String rrName = "?", lastAbn = "-";
int abnCount = 0;

// Meteo
struct Day { char name[4]; int code; float tmax, tmin; int pop; int sr, ss; };
Day fc[6]; volatile int nfc = 0;
float tNow = NAN, hNow = NAN, wNow = NAN; volatile int cNow = -1; volatile bool wxOk = false;
uint32_t nextWeather = 0;
const char* dayShort[] = {"DOM", "LUN", "MAR", "MIE", "JUE", "VIE", "SAB"};
const char* dayLong[] = {"domingo", "lunes", "martes", "miercoles", "jueves", "viernes", "sabado"};
const char* monthLong[] = {"enero", "febrero", "marzo", "abril", "mayo", "junio", "julio", "agosto",
                           "septiembre", "octubre", "noviembre", "diciembre"};

// ---------------------------------------------------------------- Audio (ES8311)
I2SClass i2s;
bool audioReady = false;
uint8_t ES_ADDR = 0x18;
uint8_t* recBuf = nullptr;
uint8_t volumeStep = 0;
const uint8_t volumeSteps[] = {0x80, 0x90, 0xA0, 0xAF, 0xBF};

SemaphoreHandle_t i2cMtx = nullptr;                       // el bus I2C lo usan el tactil y el codec
int ledPct = 90; bool ledDirty = false;                    // intensidad del LED trasero (0..100)
int volIdx = 1, micIdx = 2;                               // 0..3 = 25, 50, 75, 100 %
static const uint8_t VOL_REG[4] = {0x88, 0xA0, 0xB0, 0xBF};   // ES8311 reg 0x32 (DAC): -27,5 / -15,5 / -7,5 / 0 dB
static const uint8_t MIC_PGA[4] = {0x03, 0x04, 0x05, 0x06};   // ES8311 reg 0x16 (ganancia analogica): 18 / 24 / 30 / 36 dB
bool esWrite(uint8_t reg, uint8_t val) {
  if (i2cMtx) xSemaphoreTake(i2cMtx, portMAX_DELAY);
  Wire.beginTransmission(ES_ADDR); Wire.write(reg); Wire.write(val);
  bool ok = Wire.endTransmission() == 0;
  if (i2cMtx) xSemaphoreGive(i2cMtx);
  return ok;
}
void applyVolume() { esWrite(0x32, VOL_REG[constrain(volIdx, 0, 3)]); }
bool i2sStart(uint32_t rate) {
  i2s.setPins(I2S_BCLK, I2S_LRCK, I2S_DOUT, I2S_DIN, I2S_MCLK);
  i2s.setTimeout(200);
  return i2s.begin(I2S_MODE_STD, rate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
}
// Secuencia de registros que ya te funcionaba en NanoDesk V8.2.
bool codecInit() {
  Wire.beginTransmission(0x18);
  if (Wire.endTransmission() == 0) ES_ADDR = 0x18;
  else {
    Wire.beginTransmission(0x19);
    if (Wire.endTransmission() == 0) ES_ADDR = 0x19;
    else { Serial.println("Codec ES8311 no encontrado (0x18/0x19)"); return false; }
  }
  if (!i2sStart(16000)) { Serial.println("Fallo al iniciar I2S"); return false; }
  delay(50);
  esWrite(0x00, 0x1F); delay(20);
  esWrite(0x00, 0x00);
  esWrite(0x00, 0x80);
  esWrite(0x01, 0x3F);
  esWrite(0x02, 0x00);
  esWrite(0x03, 0x10);
  esWrite(0x04, 0x10);
  esWrite(0x05, 0x00);
  esWrite(0x06, 0x03);
  esWrite(0x07, 0x00);
  esWrite(0x08, 0xFF);
  esWrite(0x09, 0x0C);
  esWrite(0x0A, 0x0C);
  esWrite(0x0D, 0x01);
  esWrite(0x0E, 0x02);
  esWrite(0x12, 0x00);
  esWrite(0x13, 0x10);
  esWrite(0x1B, 0x0A);
  esWrite(0x1C, 0x6A);
  esWrite(0x37, 0x08);
  esWrite(0x44, 0x08);
  esWrite(0x14, 0x1A);
  esWrite(0x16, 0x03);
  esWrite(0x17, 0xBF);
  esWrite(0x31, 0x00);
  esWrite(0x32, 0x80);
  return true;
}
void changeVolume(int dir) {
  volumeStep = (uint8_t)constrain((int)volumeStep + dir, 0, 4);
  esWrite(0x32, volumeSteps[volumeStep]);
  Serial.printf("Volumen ES8311: nivel %u/5\n", volumeStep + 1);
}

// Sonidos propios (sintetizados, sin audio con derechos).
static const Note SND_BOOT[] = {{500, 1000, 90, 1, 80}, {1000, 1000, 40, 1, 0}, {1000, 1600, 130, 1, 80}};
static const Note SND_START[] = {{900, 1500, 80, 1, 80}};
static const Note SND_YES[] = {{660, 660, 80, 0, 70}, {880, 880, 80, 0, 70}, {1320, 1320, 190, 0, 70}};
static const Note SND_NO[] = {{300, 300, 110, 0, 80}, {0, 0, 35, 0, 0}, {240, 130, 330, 0, 80}};
static const Note SND_NOSE[] = {{500, 800, 120, 2, 85}, {800, 500, 120, 2, 85}, {500, 800, 120, 2, 85}, {800, 520, 170, 2, 85}};
static const Note SND_ERR[] = {{400, 300, 130, 1, 80}, {300, 190, 240, 1, 80}};
static const Note SND_TEXT[] = {{1000, 1000, 35, 0, 50}, {0, 0, 25, 0, 0}, {1400, 1400, 45, 0, 50}};

void playSeq(const Note* seq, int cnt) {
  if (!audioReady) return;
  digitalWrite(PIN_PA, LOW);
  delay(40);
  int16_t buf[128 * 2];
  float phase = 0;
  for (int k = 0; k < cnt; k++) {
    const Note& n = seq[k];
    int total = n.ms * 16;
    for (int i = 0; i < total;) {
      int blk = min(128, total - i);
      for (int j = 0; j < blk; j++) {
        float t = (float)(i + j) / total;
        float f = n.f0 + (n.f1 - (float)n.f0) * t;
        float env = min(1.0f, min((i + j) / 48.0f, (total - i - j) / 48.0f));
        float v = 0;
        if (f > 1 && n.amp) {
          phase += f / 16000.0f; if (phase >= 1) phase -= 1;
          if (n.wave == 0) v = (phase < 0.5f ? 0.6f : -0.6f);
          else if (n.wave == 1) v = 4 * fabsf(phase - 0.5f) - 1;
          else v = sinf(6.2831853f * phase);
        }
        int16_t s = (int16_t)(v * BEEP_AMP * (n.amp / 100.0f) * env);
        buf[2 * j] = s; buf[2 * j + 1] = s;
      }
      i2s.write((uint8_t*)buf, blk * 4);
      i += blk;
    }
  }
  delay(60);
  digitalWrite(PIN_PA, HIGH);
}
#define PLAY(s) playSeq(s, sizeof(s) / sizeof(Note))
void playSound(int id) {
  switch (id) {
    case 1: PLAY(SND_YES); break;
    case 2: PLAY(SND_NO); break;
    case 3: PLAY(SND_NOSE); break;
    case 4: PLAY(SND_ERR); break;
    case 5: PLAY(SND_START); break;
    case 6: PLAY(SND_TEXT); break;
    case 7: PLAY(SND_BOOT); break;
  }
}

// Voz de Bit: un "blip" digital por letra (28 ms por caracter, igual que el texto al aparecer).
void talkBegin() { if (!audioReady) return; digitalWrite(PIN_PA, LOW); delay(40); }
void talkEnd()   { if (!audioReady) return; delay(60); digitalWrite(PIN_PA, HIGH); }
void talkChar(char ch) {
  if (!audioReady) return;
  static int16_t buf[448 * 2];               // 28 ms a 16 kHz
  static float phase = 0;
  bool voiced = isalnum((unsigned char)ch);
  float f = 300.0f + (((uint8_t)ch | 0x20) * 37 % 12) * 38.0f;   // tono distinto por letra
  const int act = voiced ? 20 * 16 : 0;
  for (int i = 0; i < 448; i++) {
    int16_t v = 0;
    if (i < act) {
      phase += f / 16000.0f; if (phase >= 1) phase -= 1;
      float w = 4 * fabsf(phase - 0.5f) - 1;
      float env = min(1.0f, min(i / 40.0f, (act - i) / 60.0f));
      v = (int16_t)(w * BEEP_AMP * 0.55f * env);
    }
    buf[2 * i] = v; buf[2 * i + 1] = v;
  }
  i2s.write((uint8_t*)buf, sizeof(buf));
}
// Habla el texto mientras aparece en pantalla. Se corta si cambia el estado (toque del usuario).
void playTalk(const char* text, uint32_t myStAt) {
  talkBegin();
  for (int c = 0; text[c] && c < 320; c++) {                     // tope: ~9 s de voz aunque la respuesta sea larga
    if (st != S_ANSWER || stAt != myStAt) break;
    talkChar(text[c]);
  }
  talkEnd();
}

void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (v >> (8 * i)) & 255; }
void put16(uint8_t* p, uint16_t v) { p[0] = v & 255; p[1] = v >> 8; }

// Graba con cierre automatico por silencio. Devuelve false si no se oyo voz.
float gLastPeak = 0, gLastNoise = 0;
bool recordWav(size_t& wavBytes) {
  if (!audioReady || !recBuf) return false;
  digitalWrite(PIN_PA, HIGH);
  esWrite(0x14, 0x1A); esWrite(0x15, 0x40); esWrite(0x16, MIC_PGA[constrain(micIdx, 0, 3)]); esWrite(0x17, 0xBF);
  i2s.end();
  if (!i2sStart(16000)) { audioReady = false; return false; }
  delay(80);
  int16_t* pcm = (int16_t*)(recBuf + 44);
  const size_t maxFrames = 16000 * REC_MAX_S;
  size_t frames = 0;
  int16_t in[512];
  uint32_t t0 = millis(), lastSpeech = 0;
  bool spoken = false; float noise = 0; int chunks = 0; float dc = 0; bool dcInit = false; float peak = 0;
  while (frames < maxFrames && !stopRec) {
    size_t bytes = i2s.readBytes((char*)in, sizeof(in));
    if (bytes < 4) break;
    size_t n = bytes / 4; double sum = 0; size_t cnt = 0;
    for (size_t j = 0; j < n && frames < maxFrames; j++) {
      int32_t a = in[2 * j], b = in[2 * j + 1];
      float x = (float)(abs(a) > abs(b) ? a : b);
      if (!dcInit) { dc = x; dcInit = true; }
      dc += (x - dc) * 0.002f;                 // quita el nivel continuo del microfono
      int32_t v = (int32_t)((x - dc) * MIC_GAIN);
      v = constrain(v, -32768, 32767);
      pcm[frames++] = (int16_t)v; sum += (double)v * v; cnt++;
    }
    float rms = cnt ? sqrt(sum / cnt) : 0;
    micLevel = constrain(rms / 6000.0f, 0.0f, 1.0f);
    uint32_t now = millis();
    if (rms > peak) peak = rms;
    // Suelo de ruido = minimo reciente (sube muy despacio); asi la propia voz o un golpe inicial no lo inflan.
    if (chunks >= 3) { if (noise <= 0 || rms < noise) noise = rms; else noise += (rms - noise) * 0.003f; }
    if (chunks >= 12) {                       // ~100 ms de margen para los transitorios del codec
      float thr = constrain(noise * 2.5f, (float)VAD_MIN, 2500.0f);
      if (rms > thr) { spoken = true; lastSpeech = now; }
    }
    if (chunks % 8 == 0) Serial.printf("MIC rms=%.0f ruido=%.0f voz=%d\n", rms, noise, spoken);
    chunks++;
    if (spoken && now - lastSpeech > VAD_END_MS && now - t0 > MIN_REC_MS) break;
    if (!spoken && now - t0 > NOSPEECH_MS) break;
  }
  micLevel = 0;
  gLastPeak = peak; gLastNoise = noise;
  Serial.printf("MIC fin: pico=%.0f ruido=%.0f voz=%d\n", peak, noise, spoken);
  if (!spoken && peak > 2500) spoken = true;   // hubo sonido claro aunque el umbral no lo viera
  if (!spoken) return false;
  size_t audioBytes = frames * 2;
  memcpy(recBuf, "RIFF", 4); put32(recBuf + 4, 36 + audioBytes);
  memcpy(recBuf + 8, "WAVEfmt ", 8); put32(recBuf + 16, 16); put16(recBuf + 20, 1);
  put16(recBuf + 22, 1); put32(recBuf + 24, 16000); put32(recBuf + 28, 32000);
  put16(recBuf + 32, 2); put16(recBuf + 34, 16);
  memcpy(recBuf + 36, "data", 4); put32(recBuf + 40, audioBytes);
  wavBytes = 44 + audioBytes;
  return true;
}

// ---------------------------------------------------------------- Cuota diaria propia
uint32_t dayId() {
  time_t t = time(nullptr);
  if (t < 1700000000) return 0;
  struct tm l; localtime_r(&t, &l);
  return (uint32_t)(l.tm_year * 400 + l.tm_yday);
}
int quotaToday() {
  uint32_t d = dayId();
  if (d && prefs.getUInt("qday", 0) != d) { prefs.putUInt("qday", d); prefs.putInt("qcnt", 0); }
  return prefs.getInt("qcnt", 0);
}
void quotaAdd() { quotaToday(); prefs.putInt("qcnt", prefs.getInt("qcnt", 0) + 1); }

// ---------------------------------------------------------------- Meteo
const char* wxDesc(int c) {
  if (c == 0) return "Despejado";
  if (c <= 2) return "Poco nuboso";
  if (c == 3) return "Nublado";
  if (c == 45 || c == 48) return "Niebla";
  if (c >= 51 && c <= 57) return "Llovizna";
  if (c >= 61 && c <= 67) return "Lluvia";
  if (c >= 71 && c <= 77) return "Nieve";
  if (c >= 80 && c <= 82) return "Chubascos";
  if (c >= 95) return "Tormenta";
  return "Variable";
}
void fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return;
  String url = String("https://api.open-meteo.com/v1/forecast?latitude=") + LAT_S + "&longitude=" + LON_S +
    "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m"
    "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset"
    "&forecast_days=6&timezone=Europe%2FMadrid";
  HTTPClient http;
  http.setTimeout(8000);
  http.useHTTP10(true);
  if (!http.begin(url)) return;
  int status = http.GET();
  if (status == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getStream())) {
      tNow = doc["current"]["temperature_2m"] | NAN;
      hNow = doc["current"]["relative_humidity_2m"] | NAN;
      wNow = doc["current"]["wind_speed_10m"] | NAN;
      cNow = doc["current"]["weather_code"] | -1;
      int n = 0;
      JsonArray times = doc["daily"]["time"].as<JsonArray>();
      for (int i = 0; i < 6 && i < (int)times.size(); i++) {
        const char* ds = times[i] | "";
        int y = 0, m = 0, d = 0;
        if (sscanf(ds, "%d-%d-%d", &y, &m, &d) == 3) {
          struct tm t = {}; t.tm_year = y - 1900; t.tm_mon = m - 1; t.tm_mday = d; t.tm_hour = 12;
          mktime(&t);
          strncpy(fc[n].name, dayShort[t.tm_wday], 3); fc[n].name[3] = 0;
        } else strcpy(fc[n].name, "---");
        fc[n].code = doc["daily"]["weather_code"][i] | 0;
        fc[n].tmax = doc["daily"]["temperature_2m_max"][i] | NAN;
        fc[n].tmin = doc["daily"]["temperature_2m_min"][i] | NAN;
        fc[n].pop = doc["daily"]["precipitation_probability_max"][i] | 0;
        { int h1 = 0, m1 = 0, h2 = 0, m2 = 0;
          const char* a1 = doc["daily"]["sunrise"][i] | ""; const char* a2 = doc["daily"]["sunset"][i] | "";
          const char* t1 = strchr(a1, 'T'); const char* t2 = strchr(a2, 'T');
          fc[n].sr = (t1 && sscanf(t1 + 1, "%d:%d", &h1, &m1) == 2) ? h1 * 60 + m1 : 0;
          fc[n].ss = (t2 && sscanf(t2 + 1, "%d:%d", &h2, &m2) == 2) ? h2 * 60 + m2 : 0; }
        n++;
      }
      nfc = n; wxOk = true;
      Serial.printf("METEO: %.1f C, codigo %d, %d dias\n", tNow, (int)cNow, n);
    }
  } else Serial.printf("METEO: HTTP %d\n", status);
  http.end();
}

String buildContext() {
  String s;
  struct tm t;
  if (getLocalTime(&t, 50)) {
    char b[120];
    snprintf(b, sizeof(b), "Fecha y hora locales: %s %d de %s de %d, %02d:%02d (Mallorca, Espana). ",
             dayLong[t.tm_wday], t.tm_mday, monthLong[t.tm_mon], t.tm_year + 1900, t.tm_hour, t.tm_min);
    s += b;
  }
  if (wxOk && !isnan(tNow)) {
    char b[200];
    snprintf(b, sizeof(b), "Tiempo ahora en Andratx: %.0f grados, %s, viento %.0f km/h. ", tNow, wxDesc(cNow), wNow);
    s += b;
    if (nfc > 1) {
      snprintf(b, sizeof(b), "Prevision de manana: %s, maxima %.0f, minima %.0f, probabilidad de lluvia %d%%. ",
               wxDesc(fc[1].code), fc[1].tmax, fc[1].tmin, fc[1].pop);
      s += b;
    }
  }
  return s;
}

// ---------------------------------------------------------------- Gemini

static const char* SYS_YN =
  "Eres Bit, la criatura binaria de la pelicula Tron. Solo puedes responder con una de tres palabras: "
  "SI, NO o NO_SE. Responde SI o NO solo cuando la respuesta sea clara y cierta. Usa NO_SE si la pregunta "
  "es ambigua, subjetiva, depende de datos que no tienes, o si el audio no contiene una pregunta clara. ";
static const char* SYS_TXT =
  "Eres Bit, un companero de escritorio simpatico y algo seco, inspirado en la pelicula Tron. Responde en espanol "
  "de Espana, sin markdown, listas ni emojis. Se breve: normalmente una o dos frases (unos 120 caracteres). "
  "Solo si te piden explicar, comparar o dar varios datos, usa como mucho cuatro frases (unos 300 caracteres, "
  "nunca mas de 350) y termina siempre con la frase completa. Usa los datos de contexto (hora, fecha, tiempo) solo si la pregunta "
  "trata de ellos; si no, no los menciones. Responde directo a lo que te preguntan, sin frases de relleno ni despedidas. "
  "En el campo pregunta escribe brevemente lo que has entendido. ";

bool hasKey() { return gKey.length() > 15; }

bool askGemini(size_t wavBytes, const char* question, bool yesNo, Reply& R) {
  memset(&R, 0, sizeof(R));
  bool voice = wavBytes > 0;
  String sys = String(yesNo ? SYS_YN : SYS_TXT) + "Contexto actual: " + buildContext();
  JsonDocument req;
  req["systemInstruction"]["parts"][0]["text"] = sys;
  JsonArray parts = req["contents"][0]["parts"].to<JsonArray>();
  JsonObject p0 = parts.add<JsonObject>();
  p0["text"] = voice ? "Escucha el audio adjunto y responde segun tus instrucciones." : question;
  if (voice) {
    JsonObject p1 = parts.add<JsonObject>();
    p1["inline_data"]["mime_type"] = "audio/wav";
    p1["inline_data"]["data"] = "@@AUDIO@@";
  }
  JsonObject gc = req["generationConfig"].to<JsonObject>();
  gc["temperature"] = yesNo ? 0.2 : 0.7;
  gc["maxOutputTokens"] = yesNo ? 256 : 900;
  gc["responseMimeType"] = "application/json";
  JsonObject sc = gc["responseSchema"].to<JsonObject>();
  sc["type"] = "OBJECT";
  if (yesNo) {
    JsonObject pr = sc["properties"]["respuesta"].to<JsonObject>();
    pr["type"] = "STRING";
    JsonArray en = pr["enum"].to<JsonArray>();
    en.add("SI"); en.add("NO"); en.add("NO_SE");
    sc["required"][0] = "respuesta";
  } else {
    sc["properties"]["pregunta"]["type"] = "STRING";
    sc["properties"]["respuesta"]["type"] = "STRING";
    sc["required"][0] = "pregunta"; sc["required"][1] = "respuesta";
  }
  String body;
  serializeJson(req, body);

  // Montar el cuerpo en PSRAM insertando el audio en base64 en su sitio.
  int pos = body.indexOf("@@AUDIO@@");
  uint8_t* buf = nullptr; size_t len = 0;
  if (voice && pos >= 0) {
    size_t b64cap = 4 * ((wavBytes + 2) / 3) + 4, b64len = 0;
    size_t total = (size_t)pos + b64cap + (body.length() - pos - 9) + 8;
    buf = (uint8_t*)heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { strcpy(R.err, "Sin memoria"); return false; }
    memcpy(buf, body.c_str(), pos);
    int enc = mbedtls_base64_encode(buf + pos, b64cap, &b64len, recBuf, wavBytes);
    if (enc) { free(buf); strcpy(R.err, "Error de audio"); return false; }
    size_t tail = body.length() - pos - 9;
    memcpy(buf + pos + b64len, body.c_str() + pos + 9, tail);
    len = pos + b64len + tail;
  }

  quotaAdd();
  WiFiClientSecure client;
  client.setInsecure();   // TLS cifrado sin validar certificado (igual que en NanoDesk). Mejorable con CA.
  HTTPClient http;
  String url = String("https://generativelanguage.googleapis.com/v1beta/models/") + gModel + ":generateContent";
  if (!http.begin(client, url)) { if (buf) free(buf); strcpy(R.err, "Error de red"); return false; }
  http.setTimeout(30000);
  http.useHTTP10(true);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-goog-api-key", gKey);
  int status = buf ? http.POST(buf, len) : http.POST((uint8_t*)body.c_str(), body.length());
  if (buf) free(buf);
  R.http = status;
  String resp = status > 0 ? http.getString() : String("");
  http.end();
  Serial.printf("GEMINI: HTTP %d (%u bytes)\n", status, (unsigned)resp.length());
  if (status != 200) {
    Serial.println(resp.substring(0, 300));
    if (status == 429) { blockedUntil = millis() + 10UL * 60UL * 1000UL; strcpy(R.err, "Estoy cansado. Prueba en un rato."); }
    else if (status == 400 || status == 401 || status == 403) strcpy(R.err, "Revisa la clave de Gemini.");
    else if (status == 404) strcpy(R.err, "Modelo no valido.");
    else if (status < 0) strcpy(R.err, "Sin conexion con Gemini.");
    else strcpy(R.err, "Gemini no responde.");
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, resp)) { strcpy(R.err, "Respuesta ilegible."); return false; }
  const char* text = doc["candidates"][0]["content"]["parts"][0]["text"] | "";
  Serial.printf("GEMINI: texto='%s'\n", text);
  if (!text[0]) { R.a = ANS_NOSE; strcpy(R.text, "Sin respuesta."); return true; }
  if (yesNo) {
    R.a = parseAnswer(text);
    if (R.a == ANS_NONE) R.a = ANS_NOSE;
  } else {
    JsonDocument d2;
    if (!deserializeJson(d2, text)) {
      foldAscii(d2["pregunta"] | "", R.heard, sizeof(R.heard));
      foldAscii(d2["respuesta"] | "", R.text, sizeof(R.text));
    } else foldAscii(text, R.text, sizeof(R.text));
    if (!R.text[0]) strcpy(R.text, "No he entendido.");
  }
  return true;
}

// ---------------------------------------------------------------- Tarea de trabajo (nucleo 0)
void publish(Answer a, const char* text, const char* heard, bool isErr, const char* err) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  ansText = text; heardText = heard; errText = err;
  xSemaphoreGive(mtx);
  ansCode = a; stAt = millis();
  st = isErr ? S_ERROR : S_ANSWER;
  if (isErr) playSound(4);
  else if (a == ANS_SI) playSound(1);
  else if (a == ANS_NO) playSound(2);
  else if (a == ANS_NOSE) playSound(3);
  else playTalk(text, stAt);          // respuesta de texto: Bit "habla" mientras aparece
}

void doAsk(bool voice) {
  bool yesNo = !jobTextMode;
  if (!hasKey()) { publish(ANS_NOSE, "", "", true, "Falta la clave de Gemini. Manten pulsado el icono WiFi."); return; }
  if (WiFi.status() != WL_CONNECTED) { publish(ANS_NOSE, "", "", true, "Sin WiFi."); return; }
  if ((int32_t)(blockedUntil - millis()) > 0 || quotaToday() >= DAILY_LIMIT) {
    publish(ANS_NOSE, "", "", true, "Estoy cansado. Vuelvo manana."); return;
  }
  size_t wavBytes = 0;
  if (voice) {
    st = S_LISTEN; stAt = millis(); stopRec = false;
    playSound(5);
    if (!recordWav(wavBytes)) { char eb[56]; snprintf(eb, sizeof(eb), "No te he oido (nivel %.0f, ruido %.0f)", gLastPeak, gLastNoise); publish(ANS_NOSE, "", "", true, eb); return; }
  }
  st = S_THINK; stAt = millis();
  Reply R;
  bool ok = askGemini(wavBytes, pendingQ.c_str(), yesNo, R);
  if (!ok) publish(ANS_NOSE, "", "", true, R.err[0] ? R.err : "Algo ha fallado.");
  else if (yesNo) publish(R.a, "", "", false, "");
  else publish(ANS_NONE, R.text, R.heard, false, "");
}

void workerTask(void*) {
  for (;;) {
    Job j = job;
    if (j == J_NONE) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
    switch (j) {
      case J_VOICE: doAsk(true); break;
      case J_TEXT: doAsk(false); break;
      case J_WEATHER: fetchWeather(); break;
      case J_SOUND: playSound(pendingSnd); break;
      default: break;
    }
    job = J_NONE;
  }
}

// ---------------------------------------------------------------- Dibujo: utilidades
void txt(const char* s, int x, int y, const lgfx::IFont* f, uint16_t col, textdatum_t d = textdatum_t::top_left, float size = 1.0f) {
  canvas.setFont(f); canvas.setTextSize(size); canvas.setTextDatum(d); canvas.setTextColor(col);
  canvas.drawString(s, x, y);
  canvas.setTextSize(1);
}
int wrapLines(const char* s, int maxW, char out[][80], int maxLines) {
  canvas.setFont(&fonts::Font2); canvas.setTextSize(1);
  int n = 0; char cur[80] = "";
  const char* p = s;
  while (*p && n < maxLines) {
    char word[40]; int wl = 0;
    while (*p == ' ') p++;
    while (*p && *p != ' ' && wl < 39) word[wl++] = *p++;
    word[wl] = 0;
    if (!wl) break;
    char test[130];
    snprintf(test, sizeof(test), "%s%s%s", cur, cur[0] ? " " : "", word);
    if (!cur[0] || canvas.textWidth(test) <= maxW) { strncpy(cur, test, 79); cur[79] = 0; }
    else { strcpy(out[n++], cur); strncpy(cur, word, 79); cur[79] = 0; }
  }
  if (cur[0] && n < maxLines) strcpy(out[n++], cur);
  return n;
}

// Rejilla Tron en perspectiva (suelo). hz = y del horizonte; anim = las lineas avanzan hacia ti.
void drawGrid(uint32_t now, int hz, uint16_t col, bool anim) {
  float ph = anim ? fmodf(now / 2600.0f, 1.0f) : 0.0f;
  int span = 240 - hz;
  for (int i = -8; i <= 8; i++) canvas.drawLine(160 + i * 9, hz, 160 + i * 66, 239, col);
  for (int j = 0; j < 7; j++) {
    float u = (j + ph) / 7.0f;
    int y = hz + (int)(span * u * u);
    if (y > hz && y < 240) canvas.drawFastHLine(0, y, 320, col);
  }
  canvas.drawFastHLine(0, hz, 320, anim ? C(22, 78, 105) : col);
}

// ---------------------------------------------------------------- Dibujo: iconos del tiempo
void cloudShape(int cx, int cy, int r, uint16_t col) {
  canvas.fillCircle(cx - r * 45 / 100, cy + r * 10 / 100, r * 38 / 100, col);
  canvas.fillCircle(cx + r * 5 / 100, cy - r * 15 / 100, r * 50 / 100, col);
  canvas.fillCircle(cx + r * 50 / 100, cy + r * 15 / 100, r * 34 / 100, col);
  canvas.fillRect(cx - r * 45 / 100, cy + r * 10 / 100, r * 95 / 100, r * 40 / 100, col);
}
void drawWxIcon(int cx, int cy, int r, int code) {
  uint16_t sun = C(255, 200, 50), cl = C(150, 180, 200), rain = C(70, 150, 255);
  if (code == 0 || code == 1) {
    canvas.fillCircle(cx, cy, r * 45 / 100, sun);
    for (int i = 0; i < 8; i++) {
      float a = i * 0.7853982f;
      canvas.drawLine(cx + cosf(a) * r * 62 / 100, cy + sinf(a) * r * 62 / 100,
                      cx + cosf(a) * r * 92 / 100, cy + sinf(a) * r * 92 / 100, sun);
    }
  } else if (code == 2) {
    canvas.fillCircle(cx - r * 25 / 100, cy - r * 25 / 100, r * 35 / 100, sun);
    cloudShape(cx + r * 8 / 100, cy + r * 12 / 100, r * 85 / 100, cl);
  } else if (code == 3) cloudShape(cx, cy, r, cl);
  else if (code == 45 || code == 48) {
    for (int i = 0; i < 4; i++) canvas.drawFastHLine(cx - r * 70 / 100, cy - r * 45 / 100 + i * r * 30 / 100, r * 140 / 100, cl);
  } else if (code >= 95) {
    cloudShape(cx, cy - r * 15 / 100, r * 90 / 100, C(110, 130, 160));
    canvas.fillTriangle(cx + r * 10 / 100, cy + r * 10 / 100, cx - r * 15 / 100, cy + r * 55 / 100, cx + r * 5 / 100, cy + r * 45 / 100, sun);
    canvas.fillTriangle(cx + r * 5 / 100, cy + r * 45 / 100, cx + r * 28 / 100, cy + r * 20 / 100, cx - r * 5 / 100, cy + r * 85 / 100, sun);
  } else if ((code >= 71 && code <= 77) || code == 85 || code == 86) {
    cloudShape(cx, cy - r * 20 / 100, r * 90 / 100, cl);
    for (int i = -1; i <= 1; i++) canvas.fillCircle(cx + i * r * 35 / 100, cy + r * 55 / 100 + ((i & 1) ? 0 : r * 12 / 100), max(1, r / 11), 0xFFFF);
  } else {   // lluvia, llovizna, chubascos
    cloudShape(cx, cy - r * 20 / 100, r * 90 / 100, cl);
    for (int i = -1; i <= 1; i++)
      canvas.drawLine(cx + i * r * 35 / 100, cy + r * 40 / 100, cx + i * r * 35 / 100 - r * 12 / 100, cy + r * 80 / 100, rain);
  }
}

// ---------------------------------------------------------------- Dibujo: barra superior
bool gNight = false;                         // true entre la puesta y la salida del sol
void moonShape(int cx, int cy, int r) {
  int R = r * 62 / 100;
  uint16_t m = C(235, 238, 205);
  canvas.fillCircle(cx, cy, R, m);
  canvas.fillCircle(cx + R * 48 / 100, cy - R * 32 / 100, R * 86 / 100, cBG);   // recorta la media luna
  int sr_ = max(1, r / 12);
  canvas.fillCircle(cx + r * 55 / 100, cy - r * 55 / 100, sr_, m);
  canvas.fillCircle(cx + r * 80 / 100, cy - r * 10 / 100, sr_, m);
}
// Icono del cielo: de noche el sol pasa a luna (las nubes y la lluvia no cambian).
void drawSkyIcon(int cx, int cy, int r, int code) {
  if (gNight && code <= 2) {
    if (code == 2) {
      moonShape(cx - r * 22 / 100, cy - r * 22 / 100, r * 85 / 100);
      cloudShape(cx + r * 8 / 100, cy + r * 12 / 100, r * 85 / 100, C(150, 180, 200));
    } else moonShape(cx, cy, r);
  } else drawWxIcon(cx, cy, r, code);
}
// Salida/puesta de sol: medio sol sobre el horizonte con flecha arriba o abajo.
void drawHorizon(int cx, int cy, bool up, uint16_t col) {
  canvas.fillCircle(cx, cy + 2, 6, col);
  canvas.fillRect(cx - 8, cy + 3, 17, 9, cBG);
  canvas.drawFastHLine(cx - 9, cy + 3, 19, col);
  if (up) canvas.fillTriangle(cx, cy - 11, cx - 4, cy - 6, cx + 4, cy - 6, col);
  else    canvas.fillTriangle(cx, cy + 12, cx - 4, cy + 7, cx + 4, cy + 7, col);
}
int wifiBars() {
  if (WiFi.status() != WL_CONNECTED) return 0;
  int rssi = WiFi.RSSI();
  return rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -77 ? 2 : 1;
}
void updateNight() {
  struct tm t;
  if (getLocalTime(&t, 0) && wxOk && nfc > 0 && fc[0].sr > 0 && fc[0].ss > 0) {
    int mn = t.tm_hour * 60 + t.tm_min;
    gNight = (mn < fc[0].sr || mn >= fc[0].ss);
  } else gNight = false;
}
void drawTopBar(uint32_t now, bool showWx = true) {
  char b[40];
  {   // cada digito en su sitio fijo: no se mueve al cambiar ni al parpadear los dos puntos
    struct tm t; char d[4];
    if (getLocalTime(&t, 0)) { d[0] = '0' + t.tm_hour / 10; d[1] = '0' + t.tm_hour % 10; d[2] = '0' + t.tm_min / 10; d[3] = '0' + t.tm_min % 10; }
    else { d[0] = d[1] = d[2] = d[3] = '-'; }
    char c[2] = {0, 0};
    static const int px[4] = {15, 33, 61, 79};
    for (int i = 0; i < 4; i++) { c[0] = d[i]; txt(c, px[i], 17, &fonts::Orbitron_Light_24, C(225, 245, 255), textdatum_t::middle_center); }
    if ((now / 500) % 2) txt(":", 47, 15, &fonts::Orbitron_Light_24, cCYAN, textdatum_t::middle_center);
  }
  if (!showWx) {
  } else if (wxOk && !isnan(tNow) && nfc > 0) {
    drawWxIcon(124, 17, 11, cNow);                       // solo icono + temperatura actual
    snprintf(b, sizeof(b), "%.0fC", tNow);
    txt(b, 142, 17, &fonts::Orbitron_Light_24, C(225, 245, 255), textdatum_t::middle_left);
  } else txt("--", 128, 17, &fonts::Orbitron_Light_24, cMUTED, textdatum_t::middle_left);
  int bars = wifiBars();
  for (int i = 0; i < 4; i++) {
    int h = 5 + i * 4;
    canvas.fillRect(274 + i * 8, 24 - h, 5, h, i < bars ? C(93, 202, 165) : C(68, 68, 65));
  }
  if (bars == 0) for (int o = 0; o < 3; o++) canvas.drawLine(272, 8 + o, 304, 26 + o, cRED);
}
void drawDots() {
  for (int i = 0; i < M_COUNT; i++)
    canvas.fillCircle((int)(160 + (i - (M_COUNT - 1) / 2.0f) * 14), 235, 3, i == mode ? C(93, 202, 165) : C(68, 68, 65));
}

// ---------------------------------------------------------------- Dibujo: Bit
// Port fiel de la maqueta que te gusto: mismos numeros, mismas animaciones.
bit3d::Mesh gMesh;
BitAnim animBig, animMini;
bool gTyping = false;
static float S_(double x) { return sinf((float)fmod(x, 6.283185307179586)); }

void drawBit(BitAnim& A, int cx, int cy, float base, uint32_t now, bool mini) {
  float dt = A.last ? min(0.05f, (now - A.last) / 1000.0f) : 0.033f;
  A.last = now;
  AState s = st;
  float e = (now - stAt) / 1000.0f;                 // segundos desde el cambio de estado
  Answer a = (Answer)ansCode;
  bool up = (WiFi.status() == WL_CONNECTED);
  uint8_t r = up ? 140 : 120, g = up ? 190 : 125, b = up ? 255 : 135;
  float spin = mini ? 0.6f : 0.5f;
  float hT = 1.3f + 0.18f * S_(now / 900.0);
  float ox = 0, oy = S_(now / 700.0) * (mini ? 1.5f : 4.0f), roll = 0, pul = 1;
  int want = 0; bool mark = false;
  bool strobe = (now / 110) % 2;

  if (s == S_LISTEN) {
    r = 120; g = 210; b = 255; spin = mini ? 2.0f : 1.2f;
    float lv = constrain(0.25f + micLevel * 1.2f + 0.25f * fabsf(S_(now / 120.0) * S_(now / 310.0 + 1)), 0.0f, 1.0f);
    pul = mini ? 1.0f : 1 + 0.1f * lv;
    hT = 1.3f + (mini ? 0.3f : 0.2f) * lv;
    if (!mini) for (int i = 0; i < 3; i++) {
      float p = fmodf(now / 800.0f + i / 3.0f, 1.0f);
      canvas.drawCircle(cx, cy, (int)(45 + p * 55), mixBG(133, 183, 235, (1 - p) * 0.6f));
    }
  } else if (s == S_THINK) {
    if (strobe) { r = 255; g = 255; b = 255; } else { r = 120; g = 170; b = 255; }
    spin = mini ? 6.0f : 5.0f;
    hT = 1.3f + (mini ? 0.3f : 0.25f) * S_(now / 60.0);
  } else if (s == S_ANSWER && mini) {
    if (gTyping) { spin = 1.6f; hT = 1.3f + 0.15f * fabsf(S_(now / 110.0)); oy = -fabsf(S_(now / 220.0)) * 2; }
  } else if (s == S_ANSWER && a == ANS_SI) {
    want = 1; r = 255; g = 190; b = 30;
    spin = 1 + 3 * expf(-e * 2.0f);
    oy = -fabsf(S_(e * 1000.0 / 160.0)) * 16 * expf(-e / 1.8f);
    pul = 1 + 0.05f * S_(e * 1000.0 / 70.0);
  } else if (s == S_ANSWER && a == ANS_NO) {
    r = 235; g = 60; b = 50; spin = 0.9f;
    hT = 2.0f + 0.08f * S_(e * 1000.0 / 80.0);
    ox = S_(e * 1000.0 / 65.0) * 12 * expf(-e);
    oy = 0;
  } else if (s == S_ANSWER && a == ANS_NOSE) {
    r = 175; g = 169; b = 236; spin = 0.35f;
    hT = 1.3f + 0.3f * S_(e * 1000.0 / 130.0) * S_(e * 1000.0 / 90.0 + 1);
    roll = S_(e * 1000.0 / 280.0) * 18 * (0.4f + 0.6f * expf(-e / 2.5f)) * 0.0174533f;
    mark = true;
  } else if (s == S_ERROR) {
    r = 120; g = 115; b = 150; spin = 0.25f; hT = 1.0f + 0.05f * S_(now / 700.0);
  }

  // transicion entre formas: se encoge, cambia y vuelve a crecer
  if (A.shape != want) { A.sc -= dt * 7; if (A.sc <= 0.12f) { A.sc = 0.12f; A.shape = want; } }
  else A.sc = min(1.0f, A.sc + dt * 7);
  A.h += (hT - A.h) * (1 - expf(-dt * 9));
  A.ay += spin * dt;

  bit3d::Pose P;
  P.cx = cx + ox; P.cy = cy + oy; P.yaw = A.ay; P.pitch = 0.5f + 0.15f * S_(now / 1800.0); P.roll = roll;
  if (A.shape == 0) {
    bit3d::buildStar(gMesh, A.h - 0.795f);
    P.scale = base * 1.3f / max(1.3f, A.h) * A.sc * pul;
  } else {
    bit3d::buildOcta(gMesh, 1.0f);
    P.scale = base * 1.19f * A.sc * pul;
  }
  uint16_t edge = C((uint8_t)(r * 0.3f), (uint8_t)(g * 0.3f), (uint8_t)(b * 0.3f));
  bit3d::draw(canvas, gMesh, P, r, g, b, edge, 4);

  if (!mini && s == S_ANSWER && a == ANS_SI) {            // rayos al decir SI
    float al = 1 - e / 1.8f;
    if (al > 0) for (int k = 0; k < 8; k++) {
      float an = k * 0.7853982f + e * 2.0f;
      float r1 = 80 + S_(e * 1000.0 / 90.0 + k) * 4, r2 = r1 + 12;
      uint16_t c = mixBG(250, 199, 117, al);
      for (int o = -1; o <= 1; o++)
        canvas.drawLine((int)(cx + cosf(an) * r1) + o, (int)(cy + sinf(an) * r1), (int)(cx + cosf(an) * r2) + o, (int)(cy + sinf(an) * r2), c);
    }
  }
  if (!mini && mark) {                                    // "?" flotando
    float k = min(1.0f, e / 0.3f);
    txt("?", 160, (int)(66 + S_(now / 300.0) * 3), &fonts::Font4, mixBG(206, 203, 246, k), textdatum_t::middle_center, 1.3f);
  }
}

void sceneBit(uint32_t now) {
  canvas.fillScreen(cBG);
  drawGrid(now, 150, C(14, 52, 74), true);
  drawBit(animBig, 160, 122, 52, now, false);
  AState s = st;
  Answer a = (Answer)ansCode;
  const char* hint = "";
  const char* label = "";
  uint16_t lc = cWHITE;
  if (s == S_IDLE) hint = "toca para preguntar";
  else if (s == S_LISTEN) hint = "te escucho...";
  else if (s == S_THINK) hint = "pensando...";
  else if (s == S_ANSWER) {
    if (a == ANS_SI) { label = "SI"; lc = C(250, 199, 117); }
    else if (a == ANS_NO) { label = "NO"; lc = C(240, 149, 149); }
    else { label = "NO LO SE"; lc = C(206, 203, 246); }
  }
  if (s == S_ERROR) {
    char lines[3][80];
    xSemaphoreTake(mtx, portMAX_DELAY);
    String e = errText;
    xSemaphoreGive(mtx);
    int n = wrapLines(e.c_str(), 290, lines, 2);
    for (int i = 0; i < n; i++) txt(lines[i], 160, 196 + i * 18, &fonts::Font2, cAMBER, textdatum_t::middle_center);
  } else {
    if (hint[0]) txt(hint, 160, 191, &fonts::Font2, C(136, 135, 128), textdatum_t::middle_center);
    if (label[0]) txt(label, 160, 214, &fonts::Orbitron_Light_24, lc, textdatum_t::middle_center);
  }
  drawTopBar(now);
  drawDots();
}

// ---------------------------------------------------------------- Modo TEXTO
float txtScroll = 0; int txtMax = 0; bool txtUser = false; uint32_t txtAnsAt = 0;   // desplazamiento vertical (modo TEXTO)
void sceneText(uint32_t now) {
  canvas.fillScreen(cBG);
  drawGrid(now, 175, C(7, 26, 40), false);
  drawTopBar(now);
  AState s = st;
  String heard, ans, err;
  xSemaphoreTake(mtx, portMAX_DELAY);
  heard = heardText; ans = ansText; err = errText;
  xSemaphoreGive(mtx);
  float e = (now - stAt) / 1000.0f;
  gTyping = false;
  char lines[40][80]; int n = 0;
  String q = "";
  char shownLine[80];

  if (s == S_LISTEN) {
    q = "te escucho...";
    float k = constrain(0.4f + micLevel * 1.5f, 0.4f, 1.6f);
    for (int i = 0; i < 12; i++) {
      float v = 8 + fabsf(S_(now / 110.0 + i * 0.8) * S_(now / 330.0 + i)) * 44 * k;
      canvas.fillRoundRect(70 + i * 15, (int)(140 - v / 2), 8, (int)v, 4, mixBG(93, 202, 165, 0.8f));
    }
  } else if (s == S_THINK) {
    q = heard;
    for (int j = 0; j < 3; j++)
      canvas.fillCircle(145 + j * 15, (int)(140 - fabsf(S_(now / 260.0 - j * 0.7)) * 10), 5, C(93, 202, 165));
  } else if (s == S_ERROR) {
    n = wrapLines(err.c_str(), 296, lines, 4);
    for (int i = 0; i < n; i++) txt(lines[i], 12, 70 + i * 20, &fonts::Font2, cAMBER);
  } else if (ans.length()) {
    q = heard;
    int total = wrapLines(ans.c_str(), 290, lines, 40);
    if (stAt != txtAnsAt) { txtAnsAt = stAt; txtScroll = 0; txtUser = false; }      // respuesta nueva: arriba del todo
    int alen = ans.length();
    int perChar = alen > 300 ? max(8, 9000 / alen) : 28;                            // las largas se escriben mas rapido
    int rem = (int)((now - stAt) / perChar);
    if (s != S_ANSWER) rem = 100000;
    gTyping = (s == S_ANSWER) && rem < alen;
    txtMax = max(0, (total - 1) * 20 - 138);
    int lastShown = 0;
    canvas.setClipRect(0, 62, 320, 168);
    for (int i = 0; i < total; i++) {
      int len = strlen(lines[i]);
      int yy = 70 + i * 20 - (int)txtScroll;
      bool vis = (yy > 44 && yy < 232);
      if (rem >= len) { if (vis) txt(lines[i], 12, yy, &fonts::Font2, cWHITE); rem -= len + 1; lastShown = i; }
      else {
        int m = max(0, rem);
        strncpy(shownLine, lines[i], m); shownLine[m] = 0;
        if ((now / 400) % 2) strcat(shownLine, "_");
        if (vis) txt(shownLine, 12, yy, &fonts::Font2, cWHITE);
        lastShown = i;
        break;
      }
    }
    canvas.clearClipRect();
    if (gTyping && !txtUser) txtScroll = constrain((float)(lastShown * 20 - 138), 0.0f, (float)txtMax);   // sigue al texto mientras se escribe
    txtScroll = constrain(txtScroll, 0.0f, (float)txtMax);
    if (txtMax > 0) {                                                                 // barra de desplazamiento
      int th = max(16, 160 * 160 / (160 + txtMax));
      int ty = 66 + (int)((160 - th) * (txtScroll / txtMax));
      canvas.fillRoundRect(316, 66, 2, 160, 1, cLINE);
      canvas.fillRoundRect(315, ty, 4, th, 2, cCYAN);
    }
    (void)e;
  } else {
    n = wrapLines("Toca la pantalla y haz tu pregunta.", 296, lines, 2);
    for (int i = 0; i < n; i++) txt(lines[i], 12, 70 + i * 20, &fonts::Font2, cWHITE);
  }
  if (q.length()) {
    if (q.length() > 40) q = q.substring(0, 39) + "...";
    txt(q.c_str(), 12, 46, &fonts::Font2, C(136, 135, 128));
  }
  drawBit(animMini, 288, 210, 15, now, true);
  drawDots();
}

// ---------------------------------------------------------------- Modo TIEMPO
void sceneWeather(uint32_t now) {
  canvas.fillScreen(cBG);
  drawTopBar(now, false);
  if (!wxOk) {
    txt(WiFi.status() == WL_CONNECTED ? "Cargando el tiempo..." : "Sin WiFi: no hay datos del tiempo", 160, 110, &fonts::Font2, cMUTED, textdatum_t::middle_center);
    drawDots(); return;
  }
  char b[48];
  updateNight();
  auto frame = [&](int x, int y, int w, int h) {      // marco Tron con esquinas brillantes
    canvas.drawRoundRect(x, y, w, h, 6, cLINE);
    canvas.drawFastHLine(x + 6, y, 18, cCYAN);
    canvas.drawFastHLine(x + w - 24, y + h - 1, 18, cCYAN);
  };
  // fila A: icono | temperatura | sol
  frame(8, 42, 74, 64);
  drawSkyIcon(45, 74, 26, cNow);
  snprintf(b, sizeof(b), "%.0f", tNow);
  canvas.setFont(&fonts::Orbitron_Light_32); canvas.setTextSize(1.5f);
  int tw = canvas.textWidth(b); canvas.setTextSize(1);
  txt(b, 92, 44, &fonts::Orbitron_Light_32, cWHITE, textdatum_t::top_left, 1.5f);
  canvas.drawCircle(92 + tw + 6, 52, 3, cCYAN);
  txt("C", 92 + tw + 12, 50, &fonts::Orbitron_Light_24, cCYAN);
  txt(wxDesc(cNow), 94, 92, &fonts::Font2, cCYAN);
  frame(206, 42, 106, 64);
  if (nfc > 0 && fc[0].sr > 0) {
    drawHorizon(226, 62, true, C(255, 200, 80));
    snprintf(b, sizeof(b), "%02d:%02d", fc[0].sr / 60, fc[0].sr % 60);
    txt(b, 246, 62, &fonts::Font2, cWHITE, textdatum_t::middle_left);
    drawHorizon(226, 88, false, C(255, 130, 80));
    snprintf(b, sizeof(b), "%02d:%02d", fc[0].ss / 60, fc[0].ss % 60);
    txt(b, 246, 88, &fonts::Font2, cWHITE, textdatum_t::middle_left);
    canvas.drawFastHLine(214, 75, 90, cGRID);
  }
  // fila B: humedad, viento, lluvia
  canvas.drawFastHLine(12, 112, 296, cGRID);
  txt("HUMEDAD", 12, 116, &fonts::Font0, cMUTED);
  snprintf(b, sizeof(b), "%.0f%%", hNow);
  txt(b, 12, 126, &fonts::Font2, cWHITE);
  txt("VIENTO", 112, 116, &fonts::Font0, cMUTED);
  snprintf(b, sizeof(b), "%.0f km/h", wNow);
  txt(b, 112, 126, &fonts::Font2, cWHITE);
  txt("LLUVIA", 222, 116, &fonts::Font0, cMUTED);
  snprintf(b, sizeof(b), "%d%%", nfc > 0 ? fc[0].pop : 0);
  txt(b, 222, 126, &fonts::Font2, C(70, 150, 255));
  txt("PROXIMOS DIAS", 12, 142, &fonts::Font0, cCYAN);
  canvas.drawFastHLine(12, 152, 296, cLINE);
  int n = min((int)nfc, 6);
  for (int i = 1; i < n; i++) {
    int x = 8 + (i - 1) * 61;
    canvas.drawRoundRect(x, 156, 57, 70, 6, cLINE);
    txt(fc[i].name, x + 5, 160, &fonts::Font2, cCYAN, textdatum_t::top_left);
    drawWxIcon(x + 28, 192, 12, fc[i].code);
    snprintf(b, sizeof(b), "%.0f/%.0f", fc[i].tmax, fc[i].tmin);
    txt(b, x + 28, 208, &fonts::Font2, cWHITE, textdatum_t::top_center);
    snprintf(b, sizeof(b), "%d%%", fc[i].pop);
    txt(b, x + 53, 164, &fonts::Font0, C(70, 150, 255), textdatum_t::top_right);
  }
  drawDots();
}

// ---------------------------------------------------------------- Modo AJUSTES
void drawBtnRow(int y, const char* label, int sel) {
  txt(label, 16, y - 18, &fonts::Font2, cCYAN);
  const char* names[4] = {"25%", "50%", "75%", "100%"};
  for (int i = 0; i < 4; i++) {
    int x = 16 + i * 74; bool on = (i == sel);
    if (on) canvas.fillRoundRect(x, y, 66, 36, 8, C(93, 202, 165));
    else canvas.drawRoundRect(x, y, 66, 36, 8, cLINE);
    txt(names[i], x + 33, y + 18, &fonts::Font2, on ? cBG : cWHITE, textdatum_t::middle_center);
  }
}
void sceneSettings(uint32_t now) {
  canvas.fillScreen(cBG);
  drawTopBar(now);
  drawBtnRow(78, "VOLUMEN DEL ALTAVOZ", volIdx);
  drawBtnRow(142, "GANANCIA DEL MICROFONO", micIdx);
  String ssid = (WiFi.status() == WL_CONNECTED) ? WiFi.SSID() : String("sin WiFi");
  static int qc = 0; static uint32_t qt = 0;
  if (now - qt > 5000 || qt == 0) { qt = now; qc = quotaToday(); }
  char b[120];
  snprintf(b, sizeof(b), "IA hoy: %d/%d   %s", qc, DAILY_LIMIT, ssid.c_str());
  txt(b, 160, 196, &fonts::Font2, cMUTED, textdatum_t::middle_center);
  snprintf(b, sizeof(b), "Arranque: %s   Reinicios raros: %d (%s)", rrName.c_str(), abnCount, lastAbn.c_str());
  txt(b, 160, 212, &fonts::Font0, C(100, 130, 150), textdatum_t::middle_center);
  snprintf(b, sizeof(b), "RAM libre min: %uK   PSRAM: %uK", (unsigned)(ESP.getMinFreeHeap() / 1024), (unsigned)(ESP.getFreePsram() / 1024));
  txt(b, 160, 223, &fonts::Font0, C(100, 130, 150), textdatum_t::middle_center);
  drawDots();
}

// ---------------------------------------------------------------- Modo LUZ (regulador del LED)
static const int SL_X0 = 24, SL_X1 = 296, SL_Y = 112;
void sceneLight(uint32_t now) {
  canvas.fillScreen(cBG);
  drawGrid(now, 150, C(14, 52, 74), true);
  drawTopBar(now);
  txt("LUZ DEL LED", 160, 52, &fonts::Orbitron_Light_24, cCYAN, textdatum_t::middle_center);
  char b[16];
  snprintf(b, sizeof(b), "%d%%", ledPct);
  txt(b, 160, 84, &fonts::Orbitron_Light_32, cWHITE, textdatum_t::middle_center, 1.2f);
  canvas.fillRoundRect(SL_X0, SL_Y, SL_X1 - SL_X0, 12, 6, cGRID);
  canvas.drawRoundRect(SL_X0, SL_Y, SL_X1 - SL_X0, 12, 6, cLINE);
  int kx = SL_X0 + (SL_X1 - SL_X0) * ledPct / 100;
  if (kx > SL_X0 + 2) canvas.fillRoundRect(SL_X0, SL_Y, kx - SL_X0, 12, 6, C(60, 140, 255));
  canvas.fillCircle(kx, SL_Y + 6, 13, cWHITE);
  canvas.drawCircle(kx, SL_Y + 6, 15, cCYAN);
  for (int i = 0; i <= 4; i++) {
    int x = SL_X0 + (SL_X1 - SL_X0) * i / 4;
    canvas.drawFastVLine(x, SL_Y + 24, 5, cLINE);
    snprintf(b, sizeof(b), "%d", i * 25);
    txt(b, x, SL_Y + 38, &fonts::Font0, cMUTED, textdatum_t::middle_center);
  }
  txt(ledPct == 0 ? "LED apagado" : "toca o arrastra", 160, 190, &fonts::Font2, cMUTED, textdatum_t::middle_center);
  drawDots();
}

// ---------------------------------------------------------------- Tactil
bool touchReadRaw(int& x, int& y) {
  Wire.beginTransmission(0x38); Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)0x38, (uint8_t)5) != 5) return false;
  int count = Wire.read() & 15;
  int xh = Wire.read(), xl = Wire.read(), yh = Wire.read(), yl = Wire.read();
  if (count < 1 || count > 2) return false;
  int rawX = ((xh & 15) << 8) | xl, rawY = ((yh & 15) << 8) | yl;
  x = constrain(rawY, 0, W - 1);          // misma transformacion que NH-02 (rotacion 3)
  y = constrain(239 - rawX, 0, H - 1);
  return true;
}
bool touchRead(int& x, int& y) {
  xSemaphoreTake(i2cMtx, portMAX_DELAY);
  bool r = touchReadRaw(x, y);
  xSemaphoreGive(i2cMtx);
  return r;
}
struct TouchState { bool down = false; int sx = 0, sy = 0, lx = 0, ly = 0, py = 0; uint32_t t0 = 0; bool longFired = false; bool scrolling = false; int miss = 0; } tp;

void onTap(int tx, int ty) {
  if (mode == M_SETTINGS) {
    int col = -1;
    for (int i = 0; i < 4; i++) if (tx >= 16 + i * 74 - 3 && tx < 16 + i * 74 + 69) col = i;
    if (col < 0) return;
    if (ty >= 68 && ty < 124) { volIdx = col; prefs.putInt("vol", volIdx); applyVolume(); pendingSnd = 6; }
    else if (ty >= 132 && ty < 188) { micIdx = col; prefs.putInt("mic", micIdx); pendingSnd = 5; }
    else return;
    if (job == J_NONE) job = J_SOUND;
    return;
  }
  if (mode == M_WEATHER) {
    if (job == J_NONE) { job = J_WEATHER; nextWeather = millis() + 15UL * 60UL * 1000UL; }
    return;
  }
  AState s = st;
  if (s == S_IDLE && job == J_NONE) { pendingQ = ""; jobTextMode = (mode == M_TEXT); job = J_VOICE; }
  else if (s == S_LISTEN) stopRec = true;
  else if (s == S_ANSWER || s == S_ERROR) st = S_IDLE;
}
void onSwipe(int dir) { mode = (Mode)((mode + dir + M_COUNT) % M_COUNT); }

void handleTouch(uint32_t now) {
  static uint32_t lastPoll = 0;
  if (now - lastPoll < 20) return;
  lastPoll = now;
  int x, y;
  if (touchRead(x, y)) {
    tp.miss = 0;
    if (!tp.down) { tp.down = true; tp.sx = x; tp.sy = y; tp.py = y; tp.t0 = now; tp.longFired = false; tp.scrolling = false; }
    tp.lx = x; tp.ly = y;
    if (mode == M_TEXT && txtMax > 0 && tp.sy > 55 && tp.sy < 232) {            // arrastrar arriba/abajo para leer
      if (!tp.scrolling && abs(y - tp.sy) > 14 && abs(y - tp.sy) > abs(x - tp.sx)) { tp.scrolling = true; tp.longFired = true; }
      if (tp.scrolling) { txtScroll = constrain(txtScroll - (y - tp.py), 0.0f, (float)txtMax); txtUser = true; }
    }
    tp.py = y;
    if (mode == M_LIGHT && tp.sy >= 85 && tp.sy <= 165) {     // regulador: arrastrar o tocar
      ledPct = constrain((x - SL_X0) * 100 / (SL_X1 - SL_X0), 0, 100);
      ledPct = (ledPct + 2) / 5 * 5; if (ledPct > 100) ledPct = 100;   // pasos de 5
      ledDirty = true; tp.longFired = true;                   // evita que cuente como deslizar o tocar
    }
    if (!tp.longFired && now - tp.t0 > 1500 && tp.sx > 250 && tp.sy < 40 && abs(tp.lx - tp.sx) < 25 && abs(tp.ly - tp.sy) < 25) {
      tp.longFired = true; portalRequested = true;
    }
  } else if (tp.down && ++tp.miss >= 2) {
    tp.down = false;
    if (ledDirty) { prefs.putInt("led", ledPct); ledDirty = false; }
    if (!tp.longFired) {
      int dx = tp.lx - tp.sx, dy = tp.ly - tp.sy;
      uint32_t dur = now - tp.t0;
      if (abs(dx) > 55 && abs(dy) < 80 && dur < 1000) onSwipe(dx < 0 ? 1 : -1);
      else if (abs(dx) < 30 && abs(dy) < 30 && dur < 800) onTap(tp.sx, tp.sy);
    }
  }
}

// ---------------------------------------------------------------- WiFi / portal
WiFiManager wm;
WiFiManagerParameter* pKey = nullptr;
WiFiManagerParameter* pModel = nullptr;

void drawMsg(const char* l1, const char* l2 = "", const char* l3 = "", const char* l4 = "", bool btn = false) {
  canvas.fillScreen(cBG);
  txt("NH-03  BIT", 160, 24, &fonts::Orbitron_Light_24, cCYAN, textdatum_t::middle_center);
  canvas.drawFastHLine(20, 44, 280, cLINE);
  txt(l1, 160, 80, &fonts::Font2, cWHITE, textdatum_t::middle_center);
  txt(l2, 160, 112, &fonts::Font4, cCYAN, textdatum_t::middle_center);
  txt(l3, 160, 148, &fonts::Font2, cWHITE, textdatum_t::middle_center);
  txt(l4, 160, 176, &fonts::Font4, cCYAN, textdatum_t::middle_center);
  if (btn) {
    canvas.fillRoundRect(100, 198, 120, 34, 8, C(93, 202, 165));
    txt("SALIR", 160, 215, &fonts::Font2, cBG, textdatum_t::middle_center);
  } else txt("NANOHUMANO LABS", 160, 222, &fonts::Font0, cMUTED, textdatum_t::middle_center);
  canvas.pushSprite(0, 0);
}
void apCallback(WiFiManager*) {
  drawMsg("1. Conecta el movil a la red:", "Bit-Setup", "2. Abre en el navegador:", "192.168.4.1", true);
}
void saveParamsCb() {
  String k = pKey->getValue(); k.trim();
  if (k.length() > 15) { prefs.putString("key", k); gKey = k; }
  String m = pModel->getValue(); m.trim();
  if (m.length() > 3) { prefs.putString("model", m); gModel = m; }
  Serial.println("Configuracion guardada.");
}
// Portal no bloqueante: se atiende aqui y se puede salir tocando SALIR.
void portalLoop() {
  uint32_t t0 = millis();
  bool wasDown = false;
  while (millis() - t0 < 300000UL) {
    wm.process();
    if (!wm.getConfigPortalActive()) break;            // guardado, conectado o caducado
    int x, y;
    bool d = touchRead(x, y);
    if (d && !wasDown && x >= 90 && x <= 230 && y >= 190) { wm.stopConfigPortal(); break; }
    wasDown = d;
    delay(15);
  }
  if (wm.getConfigPortalActive()) wm.stopConfigPortal();
  WiFi.mode(WIFI_STA);
  if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
  stAt = millis();
}
void openPortal() {
  portalRequested = false;
  apCallback(nullptr);
  wm.setConfigPortalTimeout(180);
  wm.startConfigPortal("Bit-Setup");
  portalLoop();
}

// ---------------------------------------------------------------- Serie
void onLine(String s) {
  s.trim();
  if (!s.length()) return;
  String u = s; u.toUpperCase();
  if (u.startsWith("KEY ")) { gKey = s.substring(4); gKey.trim(); prefs.putString("key", gKey); Serial.println("Clave guardada."); }
  else if (u.startsWith("MODEL ")) { gModel = s.substring(6); gModel.trim(); prefs.putString("model", gModel); Serial.println("Modelo: " + gModel); }
  else if (u == "STATUS") {
    Serial.printf("WiFi=%d  clave=%s  modelo=%s  llamadas hoy=%d/%d  PSRAM libre=%u\n", WiFi.status() == WL_CONNECTED,
                  hasKey() ? "SI" : "NO", gModel.c_str(), quotaToday(), DAILY_LIMIT, (unsigned)ESP.getFreePsram());
  } else if (u.startsWith("ANS ") && job == J_NONE) {
    ansCode = u.endsWith("NOSE") ? ANS_NOSE : (u.endsWith("NO") ? ANS_NO : ANS_SI);
    stAt = millis(); st = S_ANSWER; pendingSnd = ansCode; job = J_SOUND;
  } else if (u.startsWith("TXT ") && job == J_NONE) {
    char f[320]; foldAscii(s.substring(4).c_str(), f, sizeof(f));
    xSemaphoreTake(mtx, portMAX_DELAY); ansText = f; heardText = "demo"; xSemaphoreGive(mtx);
    ansCode = ANS_NONE; stAt = millis(); st = S_ANSWER; pendingSnd = 6; job = J_SOUND;
  } else if (u.startsWith("SND ") && job == J_NONE) { pendingSnd = s.substring(4).toInt(); job = J_SOUND; }
  else if (u == "W") portalRequested = true;
  else if (u == "VOL+") changeVolume(+1);
  else if (u == "VOL-") changeVolume(-1);
  else if (job == J_NONE && st == S_IDLE) { pendingQ = s; jobTextMode = (mode == M_TEXT); job = J_TEXT; }
}
void handleSerial() {
  static char buf[240]; static int n = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') { buf[n] = 0; if (n) onLine(String(buf)); n = 0; }
    else if (n < (int)sizeof(buf) - 1) buf[n++] = c;
  }
}

// ---------------------------------------------------------------- setup / loop
// ---------------------------------------------------------------- LED RGB trasero (WS2812B en GPIO42)
#define PIN_LED 42
void ledSet(float r, float g, float b, float k) {
  k = constrain(k, 0.0f, 1.0f) * (ledPct / 100.0f);
  neopixelWrite(PIN_LED, (uint8_t)(r * k), (uint8_t)(g * k), (uint8_t)(b * k));
}
void ledUpdate(uint32_t now) {
  static uint32_t last = 0;
  if (now - last < 33) return;
  last = now;
  AState s = st; Answer a = (Answer)ansCode;
  float e = (now - stAt) / 1000.0f;
  if (mode == M_LIGHT && s == S_IDLE) {
    ledSet(60, 140, 255, 1.0f);                                        // vista previa fija en la pantalla de luz
  } else if (s == S_LISTEN) {
    ledSet(0, 220, 255, 0.30f + micLevel * 1.2f);                      // cian: late con tu voz
  } else if (s == S_THINK) {
    if ((now / 110) % 2) ledSet(255, 255, 255, 0.9f); else ledSet(60, 110, 255, 0.7f);
  } else if (s == S_ERROR) {
    ledSet(255, 120, 0, ((now / 400) % 2) ? 0.7f : 0.1f);              // ambar intermitente
  } else if (s == S_ANSWER && a == ANS_SI) {
    ledSet(255, 150, 0, 0.5f + 0.5f * expf(-e * 1.5f) * (0.6f + 0.4f * S_(e * 1000.0 / 70.0)));
  } else if (s == S_ANSWER && a == ANS_NO) {
    ledSet(255, 0, 0, e < 0.6f ? (((int)(e * 1000 / 80)) % 2 ? 1.0f : 0.15f) : 0.7f);
  } else if (s == S_ANSWER && a == ANS_NOSE) {
    ledSet(165, 90, 255, 0.55f + 0.35f * S_(e * 1000.0 / 260.0));
  } else if (s == S_ANSWER) {                                          // texto: parpadea mientras "habla"
    float v = gTyping ? 0.45f + 0.5f * fabsf(S_(now / 55.0) * S_(now / 130.0 + 1)) : 0.45f;
    ledSet(0, 210, 255, v);
  } else {                                                             // reposo: respira
    float br = 0.22f + 0.28f * (0.5f + 0.5f * S_(now / 640.0));
    if (WiFi.status() == WL_CONNECTED) ledSet(30, 90, 255, br);
    else ledSet(255, 70, 0, ((now / 1000) % 2) ? 0.5f : 0.08f);       // sin WiFi: naranja lento
  }
}

// Presentacion de Bit: aparece, se presenta hablando y dice la fecha de hoy.
void introAnim() {
  struct tm t; bool timeOk = false;
  uint32_t w0 = millis();
  while (millis() - w0 < 4000) { if (getLocalTime(&t, 0) && t.tm_year > 120) { timeOk = true; break; } delay(100); }
  char msg[200];
  if (timeOk) snprintf(msg, sizeof(msg), "Hola, soy Bit, tu asistente de escritorio. Fui creado por Nanohumano Labs el 8 de noviembre de 2026.");
  else strcpy(msg, "Hola, soy Bit, tu asistente de escritorio creado por Nanohumano Labs.");
  char lines[6][80];
  int n = wrapLines(msg, 290, lines, 6);
  int total = strlen(msg);
  animBig = BitAnim(); animBig.sc = 0.12f;          // empieza pequeno y crece
  st = S_ANSWER; ansCode = ANS_NONE;
  uint32_t t0 = millis();
  talkBegin();
  int shown = -14;                                  // pequena pausa antes de hablar
  uint32_t endAt = 0;
  while (true) {
    uint32_t now = millis();
    if (shown < total) shown++;
    else if (!endAt) endAt = now;
    if (endAt && now - endAt > 2200) break;
    stAt = now;
    gTyping = shown < total;
    ledUpdate(now);
    canvas.fillScreen(cBG);
    drawGrid(now, 200, C(14, 52, 74), true);
    float k = min(1.0f, (now - t0) / 700.0f);       // lineas Tron que se abren desde el centro
    int hw = (int)(120 * k);
    canvas.drawFastHLine(160 - hw, 106, hw * 2, cLINE);
    canvas.drawFastHLine(160 - hw / 2, 107, hw, cCYAN);
    drawBit(animBig, 160, 58, 34, now, true);
    int rem = shown;
    for (int i = 0; i < n && rem > 0; i++) {
      int len = strlen(lines[i]);
      char part[80];
      int m = min(rem, len);
      memcpy(part, lines[i], m); part[m] = 0;
      txt(part, 160, 126 + i * 20, &fonts::Font2, i == 0 ? cCYAN : cWHITE, textdatum_t::top_center);
      rem -= len + 1;
    }
    txt("NANOHUMANO LABS   " FW_VER, 160, 228, &fonts::Font0, cMUTED, textdatum_t::middle_center);
    canvas.pushSprite(0, 0);
    if (shown >= 0 && shown < total) talkChar(msg[shown - 1]);
    else delay(28);
  }
  talkEnd();
  st = S_IDLE; gTyping = false;
  animBig = BitAnim();
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\nNH-03 BIT " FW_VER);
  mtx = xSemaphoreCreateMutex();
  i2cMtx = xSemaphoreCreateMutex();
  initColors();
  pinMode(PIN_PA, OUTPUT); digitalWrite(PIN_PA, HIGH);
  pinMode(45, OUTPUT); digitalWrite(45, HIGH);          // retroiluminacion
  neopixelWrite(42, 0, 0, 0);                           // LED trasero apagado hasta tener estado
  lcd.init();
  lcd.setRotation(3);
  canvas.setColorDepth(16);
  canvas.setPsram(true);
  if (!canvas.createSprite(W, H)) {
    Serial.println("Sprite 16 bit sin memoria; probando 8 bit");
    canvas.setPsram(false); canvas.setColorDepth(8);
    if (!canvas.createSprite(W, H)) { Serial.println("ERROR: sin memoria para la pantalla"); while (true) delay(1000); }
  }
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);
  prefs.begin("bit", false);
  gKey = prefs.getString("key", "");
  gModel = prefs.getString("model", DEFAULT_MODEL);
  {
    esp_reset_reason_t rr = esp_reset_reason();
    const char* rn = "desconocido";
    switch (rr) {
      case ESP_RST_POWERON: rn = "encendido"; break;
      case ESP_RST_EXT: rn = "boton RESET"; break;
      case ESP_RST_SW: rn = "software"; break;
      case ESP_RST_PANIC: rn = "FALLO (panic)"; break;
      case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: rn = "WATCHDOG"; break;
      case ESP_RST_BROWNOUT: rn = "FALTA DE TENSION"; break;
      case ESP_RST_DEEPSLEEP: rn = "sleep"; break;
      default: break;
    }
    rrName = rn;
    bool abn = (rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT || rr == ESP_RST_BROWNOUT);
    if (abn) { abnCount = prefs.getInt("abn", 0) + 1; prefs.putInt("abn", abnCount); prefs.putString("lastrr", rn); }
    else abnCount = prefs.getInt("abn", 0);
    lastAbn = prefs.getString("lastrr", "-");
    Serial.printf("Motivo del arranque: %s   reinicios raros acumulados: %d (ultimo: %s)\n", rn, abnCount, lastAbn.c_str());
  }
  volIdx = constrain(prefs.getInt("vol", 1), 0, 3);
  micIdx = constrain(prefs.getInt("mic", 2), 0, 3);
  ledPct = constrain(prefs.getInt("led", 90), 0, 100);

  audioReady = codecInit();
  if (audioReady) applyVolume();
  Serial.printf("Audio: %s   PSRAM: %s\n", audioReady ? "OK" : "ERROR", psramFound() ? "SI" : "NO");
  recBuf = (uint8_t*)heap_caps_malloc(44 + 16000 * REC_MAX_S * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!recBuf) Serial.println("AVISO: sin memoria para grabar");

  // Pantalla de arranque: Bit girando.
  uint32_t t0 = millis(); bool beeped = false;
  while (millis() - t0 < 1600) {
    uint32_t now = millis();
    canvas.fillScreen(cBG);
    st = S_THINK; stAt = now;
    drawBit(animBig, 160, 96, 46, now, false);
    txt("NH-03  BIT", 160, 190, &fonts::Orbitron_Light_24, cCYAN, textdatum_t::middle_center);
    txt("NANOHUMANO LABS   " FW_VER, 160, 218, &fonts::Font0, cMUTED, textdatum_t::middle_center);
    canvas.pushSprite(0, 0);
    if (!beeped && now - t0 > 200) { beeped = true; playSound(7); }
  }
  st = S_IDLE;
  animBig = BitAnim();

  pKey = new WiFiManagerParameter("gkey", "Clave API de Gemini (vacio = conservar la actual)", "", 80);
  pModel = new WiFiManagerParameter("gmodel", "Modelo de Gemini", gModel.c_str(), 60);
  wm.addParameter(pKey); wm.addParameter(pModel);
  wm.setSaveParamsCallback(saveParamsCb);
  wm.setAPCallback(apCallback);
  wm.setConfigPortalTimeout(180);
  wm.setConfigPortalBlocking(false);
  drawMsg("Conectando al WiFi...");
  bool ok = wm.autoConnect("Bit-Setup");
  if (!ok && wm.getConfigPortalActive()) portalLoop();
  ok = (WiFi.status() == WL_CONNECTED);
  Serial.printf("WiFi: %s\n", ok ? "CONECTADO" : "SIN CONEXION");
  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.google.com");
  if (ok) introAnim();
  if (!hasKey()) Serial.println("Falta la clave de Gemini: usa el portal (mantener pulsado el icono WiFi) o el comando KEY.");

  xTaskCreatePinnedToCore(workerTask, "worker", 24576, nullptr, 1, nullptr, 0);
  job = J_WEATHER;
  nextWeather = millis() + 15UL * 60UL * 1000UL;
  stAt = millis();
  Serial.println("Listo. Toca la pantalla para hablar.");
}

void loop() {
  uint32_t now = millis();
  handleTouch(now);
  handleSerial();

  AState s = st;
  if (s == S_ANSWER && now - stAt > (mode == M_TEXT ? 40000UL : ANSWER_HOLD_MS)) st = S_IDLE;
  if (s == S_ERROR && now - stAt > 6000) st = S_IDLE;

  if (portalRequested && job == J_NONE && st == S_IDLE) openPortal();

  if (job == J_NONE && st == S_IDLE && (int32_t)(now - nextWeather) >= 0) {
    nextWeather = now + 15UL * 60UL * 1000UL;
    job = J_WEATHER;
  }
  static uint32_t nextCheck = 0;
  if (now > nextCheck) {
    nextCheck = now + 30000;
    if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
  }
  static uint32_t nextHeap = 0;
  if (now > nextHeap) {
    nextHeap = now + 60000;
    Serial.printf("HEAP libre=%u min=%u PSRAM=%u WiFi=%d\n", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(), (unsigned)ESP.getFreePsram(), WiFi.status() == WL_CONNECTED);
  }
  ledUpdate(now);
  static uint32_t lastFrame = 0;
  uint32_t interval = (mode == M_WEATHER || mode == M_SETTINGS) ? 100 : 33;
  if (now - lastFrame >= interval) {
    lastFrame = now;
    if (mode == M_BIT) sceneBit(now);
    else if (mode == M_TEXT) sceneText(now);
    else if (mode == M_WEATHER) sceneWeather(now);
    else if (mode == M_LIGHT) sceneLight(now);
    else sceneSettings(now);
    canvas.pushSprite(0, 0);
  }
  delay(1);
}
