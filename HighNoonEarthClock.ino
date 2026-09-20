// High Noon Earth Clock: an analog clock with the sunlit Earth as the face: the subsolar point (local
// noon somewhere on Earth) sits under the hands' pivot and the globe turns
// with the day. Waveshare ESP32-S3-Touch-AMOLED-1.75 (466x466, CO5300 QSPI).
// Build: FQBN esp32:esp32:esp32s3 with PSRAM=opi, FlashSize=16M, custom partitions.csv (8MB app).
// Regenerate the texture header with tools/bake_tex.py.
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <math.h>
#include <sys/time.h>
#include <time.h>
#include <WiFi.h>
#include <Preferences.h>
#include <esp_sntp.h>
#include <ESP_I2S.h>
#include "TouchDrvCSTXXX.hpp"
#include "es8311.h"
#include "tex.h"

__asm__(
  ".section .rodata\n"
  ".balign 4\n"
  ".global earth_tex_start\n"
  "earth_tex_start:\n"
  ".incbin \"" TEX_PATH "\"\n"
  ".previous\n");
extern "C" const uint8_t earth_tex_start[];

#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK 38
#define LCD_RESET 2
#define LCD_CS 12
#define IIC_SDA 15
#define IIC_SCL 14
#define TP_INT 11
#define TP_RESET 40
#define TOUCH_ADDR 0x5A
#define PA_PIN 46       // speaker amplifier enable
#define I2S_MCLK 42
#define I2S_BCLK 9
#define I2S_WS 45
#define I2S_DOUT 8      // ESP32 -> codec DAC
#define I2S_DIN 10
#define CHIME_VOLUME 60 // 0..100

#define SCR 466
#define CX 233.0f
#define CY 233.0f
#define GLOBE_R 233
#define GLOBE_N (2 * GLOBE_R)
#define GLOBE_OFF (SCR / 2 - GLOBE_R)
#define LOCAL_TZ "PST8PDT,M3.2.0,M11.1.0"
#define RTC_ADDR 0x51
#define GLOBE_UPDATE_MS 15000

Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *gfx = new Arduino_CO5300(bus, LCD_RESET, 0, SCR, SCR, 6, 0, 0, 0);
Arduino_Canvas *canvas;

uint16_t *fb;          // frame sent to the panel
uint16_t *bases[2];    // bezel + globe, double-buffered
volatile int front = 0;
volatile bool pending = false;
uint16_t *tex;         // equirectangular Earth, RGB565, in PSRAM
uint16_t *zt16;        // sphere depth per globe pixel, 0..65535
uint8_t *ea;           // rim coverage per globe pixel, 0..255
uint16_t shLUT[256], hzLUT[256];
const int hazeR = 13, hazeG = 42, hazeB = 31;
float exv[3], nyv[3], zvv[3];

SemaphoreHandle_t i2cLock;  // Wire's buffer is shared by the RTC and the touch task
struct I2cGuard {
  I2cGuard() { xSemaphoreTake(i2cLock, portMAX_DELAY); }
  ~I2cGuard() { xSemaphoreGive(i2cLock); }
};
TouchDrvCST92xx touch;
I2SClass i2s;
TaskHandle_t chimeTaskHandle;
volatile bool tapFlag = false;
volatile bool chimeEnabled = false;
volatile uint32_t bellUntil = 0;
volatile bool shotRequested = false;

static inline uint16_t blend565(uint16_t d, uint16_t s, int a) {
  int dr = (d >> 11) & 31, dg = (d >> 5) & 63, db = d & 31;
  int sr = (s >> 11) & 31, sg = (s >> 5) & 63, sb = s & 31;
  int r = dr + (((sr - dr) * a) >> 8);
  int g = dg + (((sg - dg) * a) >> 8);
  int b = db + (((sb - db) * a) >> 8);
  return (r << 11) | (g << 5) | b;
}

// Draws in "upright" coordinates; the whole face is turned to suit how the
// panel is mounted, (x, y) -> (SCR - y, x). The driver's own flip mirrors the
// image on this panel, so rotation is done here. renderGlobe uses the same map.
// square=false: round-capped line; square=true: flat-ended bar from p0 to p1.
static void raster(uint16_t *buf, float x0, float y0, float x1, float y1, float hw, uint16_t col, int alpha, bool square) {
  float t0 = SCR - y0, t1 = x0, t2 = SCR - y1, t3 = x1;
  x0 = t0; y0 = t1; x1 = t2; y1 = t3;
  float lim = hw + 0.5f;
  int ix0 = max(0, (int)floorf(fminf(x0, x1) - lim)), ix1 = min(SCR - 1, (int)ceilf(fmaxf(x0, x1) + lim));
  int iy0 = max(0, (int)floorf(fminf(y0, y1) - lim)), iy1 = min(SCR - 1, (int)ceilf(fmaxf(y0, y1) + lim));
  float dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
  float inv = len2 > 0 ? 1.0f / len2 : 0;
  float len = sqrtf(len2), invLen = len > 0 ? 1.0f / len : 0;
  float lim2 = lim * lim;
  for (int y = iy0; y <= iy1; y++) {
    uint16_t *row = buf + y * SCR;
    float py = y + 0.5f - y0;
    int xs = ix0, xe = ix1;
    if (fabsf(dy) > 1e-3f) {  // only scan the columns this row's slice of the capsule can touch
      float ta = (py - lim) / dy, tb = (py + lim) / dy;
      if (ta > tb) { float s = ta; ta = tb; tb = s; }
      ta = ta < 0 ? 0 : ta;
      tb = tb > 1 ? 1 : tb;
      if (ta > tb) continue;
      float xa = x0 + ta * dx, xb = x0 + tb * dx;
      if (xa > xb) { float s = xa; xa = xb; xb = s; }
      xs = max(ix0, (int)floorf(xa - lim));
      xe = min(ix1, (int)ceilf(xb + lim));
    }
    for (int x = xs; x <= xe; x++) {
      float px = x + 0.5f - x0;
      float cov;
      if (square) {
        float u = (px * dx + py * dy) * invLen, v = (px * dy - py * dx) * invLen;
        float cv = hw + 0.5f - fabsf(v), c1 = u + 0.5f, c2 = len - u + 0.5f;
        if (cv <= 0 || c1 <= 0 || c2 <= 0) continue;
        cov = (cv > 1 ? 1 : cv) * (c1 > 1 ? 1 : c1) * (c2 > 1 ? 1 : c2);
      } else {
        float t = (px * dx + py * dy) * inv;
        t = t < 0 ? 0 : (t > 1 ? 1 : t);
        float ex = px - t * dx, ey = py - t * dy;
        float d2 = ex * ex + ey * ey;
        if (d2 >= lim2) continue;
        cov = lim - sqrtf(d2);
        if (cov > 1) cov = 1;
      }
      row[x] = blend565(row[x], col, (int)(cov * alpha));
    }
  }
}

void aaLine(uint16_t *buf, float x0, float y0, float x1, float y1, float hw, uint16_t col, int alpha) {
  raster(buf, x0, y0, x1, y1, hw, col, alpha, false);
}
void aaBar(uint16_t *buf, float x0, float y0, float x1, float y1, float hw, uint16_t col, int alpha) {
  raster(buf, x0, y0, x1, y1, hw, col, alpha, true);
}

// View basis: the subsolar point is at the centre, north is up.
void sunBasis(double unix_s) {
  double jd = unix_s / 86400.0 + 2440587.5, n = jd - 2451545.0;
  double L = 280.460 + 0.9856474 * n;
  double g = radians(357.528 + 0.9856003 * n);
  double lam = radians(L + 1.915 * sin(g) + 0.020 * sin(2 * g));
  double eps = radians(23.439 - 4e-7 * n);
  double dec = asin(sin(eps) * sin(lam));
  double ra = atan2(cos(eps) * sin(lam), cos(lam));
  double gmst = radians(280.46061837 + 360.98564736629 * n);
  double sl = ra - gmst;
  exv[0] = -sin(sl); exv[1] = cos(sl); exv[2] = 0;
  nyv[0] = -sin(dec) * cos(sl); nyv[1] = -sin(dec) * sin(sl); nyv[2] = cos(dec);
  zvv[0] = cos(dec) * cos(sl); zvv[1] = cos(dec) * sin(sl); zvv[2] = sin(dec);
}

void drawTwelveMark(uint16_t *buf) {
  aaLine(buf, CX, CY - 220, CX, CY - 220, 8.0f, 0x0000, 170);
  aaLine(buf, CX, CY - 220, CX, CY - 220, 5.5f, 0xFD20, 256);
}

void renderGlobe(uint16_t *dst, double unix_s) {
  sunBasis(unix_s);
  const float invR = 1.0f / GLOBE_R;
  const float invTwoPi = 1.0f / TWO_PI, invPi = 1.0f / PI;
  for (int j = 0; j < GLOBE_N; j++) {
    float y = (GLOBE_R - (j + 0.5f)) * invR;
    float ax = y * nyv[0], ay = y * nyv[1], az = y * nyv[2];
    uint16_t *dcol = dst + GLOBE_OFF * SCR + (SCR - 1 - (j + GLOBE_OFF));
    for (int i = 0; i < GLOBE_N; i++) {
      int idx = j * GLOBE_N + i;
      uint8_t a = ea[idx];
      if (!a) continue;
      float x = (i + 0.5f - GLOBE_R) * invR;
      uint16_t z16 = zt16[idx];
      float z = z16 * (1.0f / 65535.0f);
      float p0 = x * exv[0] + ax + z * zvv[0];
      float p1 = x * exv[1] + ay + z * zvv[1];
      float p2 = x * exv[2] + az + z * zvv[2];
      if (z16 < 400) {
        float k = 1.0f / sqrtf(p0 * p0 + p1 * p1 + p2 * p2);
        p0 *= k; p1 *= k; p2 *= k;
      }
      p2 = p2 > 1 ? 1 : (p2 < -1 ? -1 : p2);
      float u = (atan2f(p1, p0) * invTwoPi + 0.5f) * TEX_W - 0.5f;
      float v = (0.5f - asinf(p2) * invPi) * TEX_H - 0.5f;
      v = v < 0 ? 0 : (v > TEX_H - 1.001f ? TEX_H - 1.001f : v);
      int x0 = (int)floorf(u);
      float fx = u - x0;
      int y0 = (int)v;
      float fy = v - y0;
      x0 %= TEX_W;
      if (x0 < 0) x0 += TEX_W;
      int x1 = x0 + 1 == TEX_W ? 0 : x0 + 1;
      int fxi = (int)(fx * 256), fyi = (int)(fy * 256);
      int w11 = (fxi * fyi) >> 8, w10 = fyi - w11, w01 = fxi - w11, w00 = 256 - fxi - fyi + w11;
      const uint16_t *r0 = tex + y0 * TEX_W, *r1 = r0 + TEX_W;
      uint16_t c00 = r0[x0], c01 = r0[x1], c10 = r1[x0], c11 = r1[x1];
      int r = (((c00 >> 11) & 31) * w00 + ((c01 >> 11) & 31) * w01 + ((c10 >> 11) & 31) * w10 + ((c11 >> 11) & 31) * w11) >> 8;
      int g = (((c00 >> 5) & 63) * w00 + ((c01 >> 5) & 63) * w01 + ((c10 >> 5) & 63) * w10 + ((c11 >> 5) & 63) * w11) >> 8;
      int b = ((c00 & 31) * w00 + (c01 & 31) * w01 + (c10 & 31) * w10 + (c11 & 31) * w11) >> 8;
      int zi = z16 >> 8;
      int sh = shLUT[zi], hz = hzLUT[zi];
      r = (r * sh + hazeR * hz) >> 8;
      g = (g * sh + hazeG * hz) >> 8;
      b = (b * sh + hazeB * hz) >> 8;
      uint16_t col = (r << 11) | (g << 5) | b;
      dcol[i * SCR] = a == 255 ? col : blend565(0, col, a);
    }
    if ((j & 31) == 31) vTaskDelay(1);
  }
  drawTwelveMark(dst);
}

void globeTask(void *) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(GLOBE_UPDATE_MS));
    if (pending) continue;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint32_t t0 = millis();
    renderGlobe(bases[1 - front], (double)tv.tv_sec + tv.tv_usec * 1e-6);
    Serial.printf("globe render %lu ms\n", (unsigned long)(millis() - t0));
    pending = true;
  }
}

// ---- time ----
static int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = y - era * 400;
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + doe - 719468;
}
static uint8_t bcd2(uint8_t v) { return (v & 0x0F) + 10 * (v >> 4); }
static uint8_t toBcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

bool readRTC(time_t *out) {
  I2cGuard guard;
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(0x04);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(RTC_ADDR, 7) != 7) return false;
  uint8_t s = Wire.read(), mi = Wire.read(), h = Wire.read(), d = Wire.read(), wd = Wire.read(), mo = Wire.read(), y = Wire.read();
  (void)wd;
  if (s & 0x80) return false;  // oscillator-stop flag: time not trustworthy
  int year = 2000 + bcd2(y);
  if (year < 2025) return false;
  int64_t days = daysFromCivil(year, bcd2(mo & 0x1F), bcd2(d & 0x3F));
  *out = (time_t)(days * 86400 + bcd2(h & 0x3F) * 3600 + bcd2(mi & 0x7F) * 60 + bcd2(s & 0x7F));
  return true;
}

void writeRTC(time_t t) {
  I2cGuard guard;
  struct tm u;
  gmtime_r(&t, &u);
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(0x04);
  Wire.write(toBcd(u.tm_sec));
  Wire.write(toBcd(u.tm_min));
  Wire.write(toBcd(u.tm_hour));
  Wire.write(toBcd(u.tm_mday));
  Wire.write(toBcd(u.tm_wday));
  Wire.write(toBcd(u.tm_mon + 1));
  Wire.write(toBcd(u.tm_year - 100));
  Wire.endTransmission();
}

void setSystemTime(time_t t) {
  struct timeval tv = {t, 0};
  settimeofday(&tv, NULL);
}

time_t buildTime() {
  static const char *mons = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char m[4] = {0};
  int d, y, hh, mm, ss;
  sscanf(__DATE__, "%3s %d %d", m, &d, &y);
  sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss);
  struct tm t = {};
  t.tm_year = y - 1900;
  t.tm_mon = (strstr(mons, m) - mons) / 3;
  t.tm_mday = d;
  t.tm_hour = hh;
  t.tm_min = mm;
  t.tm_sec = ss;
  t.tm_isdst = -1;
  return mktime(&t);
}

// ---- Wi-Fi time sync: joins, sets the clock over NTP, writes the RTC, turns Wi-Fi off ----
TaskHandle_t syncTaskHandle;

bool syncOverNtp(const String &ssid, const String &pass) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) vTaskDelay(pdMS_TO_TICKS(500));
  bool ok = false;
  if (WiFi.status() == WL_CONNECTED) {
    configTzTime(LOCAL_TZ, "pool.ntp.org", "time.google.com", "time.nist.gov");
    for (int i = 0; i < 30 && !ok; i++) {
      vTaskDelay(pdMS_TO_TICKS(500));
      ok = sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED;
    }
    if (ok) writeRTC(time(nullptr));
    esp_sntp_stop();
  }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  return ok;
}

void syncTask(void *) {
  vTaskDelay(pdMS_TO_TICKS(3000));
  for (;;) {
    Preferences prefs;
    prefs.begin("wifi", true);
    String ssid = prefs.getString("ssid", ""), pass = prefs.getString("pass", "");
    prefs.end();
    if (ssid.length()) {
      bool ok = syncOverNtp(ssid, pass);
      Serial.println(ok ? "ntp: time synced" : "ntp: sync failed (check Wi-Fi name/password)");
    }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(6UL * 3600UL * 1000UL));
  }
}

// Serial commands (115200): S dump a screenshot; C play the chime; T<unix-epoch> set time;  W<ssid><TAB><password> save Wi-Fi
// and sync now;  X forget Wi-Fi.
void handleSerial() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (line.length() > 1 && line[0] == 'T') {
        time_t t = (time_t)line.substring(1).toInt();
        if (t > 1700000000) {
          setSystemTime(t);
          writeRTC(t);
          Serial.printf("time set to %ld\n", (long)t);
        }
      } else if (line.length() > 1 && line[0] == 'W') {
        int tab = line.indexOf('\t');
        if (tab > 1) {
          Preferences prefs;
          prefs.begin("wifi", false);
          prefs.putString("ssid", line.substring(1, tab));
          prefs.putString("pass", line.substring(tab + 1));
          prefs.end();
          Serial.printf("wifi saved for '%s', syncing\n", line.substring(1, tab).c_str());
          if (syncTaskHandle) xTaskNotifyGive(syncTaskHandle);
        }
      } else if (line == "S") {
        shotRequested = true;
      } else if (line == "C") {
        if (chimeTaskHandle) xTaskNotifyGive(chimeTaskHandle);
      } else if (line == "X") {
        Preferences prefs;
        prefs.begin("wifi", false);
        prefs.clear();
        prefs.end();
        Serial.println("wifi cleared");
      }
      line = "";
    } else if (line.length() < 160) {
      line += c;
    }
  }
}

// ---- hourly chime: two short rising chirps through the ES8311 codec and speaker amp ----
static void playChime() {
  const int rate = 16000;
  const int lead = rate * 12 / 100, chirp = rate / 10, gap = rate * 11 / 100, tail = rate * 6 / 100;
  const int n = lead + chirp + gap + chirp + tail;
  const size_t bytes = (size_t)n * 2 * sizeof(int16_t);
  int16_t *buf = (int16_t *)ps_malloc(bytes);
  if (!buf) return;
  memset(buf, 0, bytes);
  const float d = (float)chirp / rate;
  for (int k = 0; k < 2; k++) {
    int start = lead + k * (chirp + gap);
    for (int i = 0; i < chirp; i++) {
      float t = (float)i / rate;
      float phase = TWO_PI * (1900.0f * t + (3100.0f - 1900.0f) * t * t / (2 * d));
      int16_t v = (int16_t)(sinf(phase) * sinf(PI * t / d) * 0.6f * 32767.0f);
      buf[(start + i) * 2] = v;
      buf[(start + i) * 2 + 1] = v;
    }
  }
  digitalWrite(PA_PIN, HIGH);
  i2s.setPins(I2S_BCLK, I2S_WS, I2S_DOUT, I2S_DIN, I2S_MCLK);
  if (i2s.begin(I2S_MODE_STD, rate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    es8311_handle_t h = es8311_create(0, ES8311_ADDRRES_0);
    if (h) {
      const es8311_clock_config_t clk = {
        .mclk_inverted = false,
        .sclk_inverted = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency = rate * 256,
        .sample_frequency = rate};
      es8311_init(h, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
      es8311_sample_frequency_config(h, clk.mclk_frequency, clk.sample_frequency);
      es8311_microphone_config(h, false);
      es8311_voice_volume_set(h, CHIME_VOLUME, NULL);
      i2s.write((uint8_t *)buf, bytes);
      vTaskDelay(pdMS_TO_TICKS(250));
      es8311_delete(h);
    }
    i2s.end();
  }
  digitalWrite(PA_PIN, LOW);
  free(buf);
}

void chimeTask(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    playChime();
  }
}

// A tap is a short touch that barely moves. Release is declared after 100 ms without a touch report.
void touchTask(void *) {
  int16_t xs[2], ys[2];
  bool down = false;
  uint32_t downAt = 0, lastSeen = 0, lastTap = 0;
  int16_t sx = 0, sy = 0;
  int moved = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(15));
    uint8_t n;
    {
      I2cGuard guard;
      n = touch.getPoint(xs, ys, 1);
    }
    uint32_t now = millis();
    if (n > 0) {
      lastSeen = now;
      if (!down) {
        down = true;
        downAt = now;
        sx = xs[0];
        sy = ys[0];
        moved = 0;
      } else {
        moved = max(moved, abs(xs[0] - sx) + abs(ys[0] - sy));
      }
    } else if (down && now - lastSeen > 100) {
      down = false;
      if (lastSeen - downAt < 700 && moved < 60 && now - lastTap > 350) {
        lastTap = now;
        tapFlag = true;
        Serial.println("tap");
      }
    }
  }
}

void drawBell(bool on, int alpha) {
  const float cx = CX, cy = 330;
  const uint16_t col = on ? 0xFEA0 : 0x8410;
  aaLine(fb, cx, cy, cx, cy, 52, 0x0000, alpha * 170 / 256);
  aaLine(fb, cx, cy - 6, cx, cy - 6, 22, col, alpha);
  aaBar(fb, cx, cy - 6, cx, cy + 12, 22, col, alpha);
  aaBar(fb, cx - 30, cy + 17, cx + 30, cy + 17, 5.5f, col, alpha);
  aaLine(fb, cx, cy + 31, cx, cy + 31, 6, col, alpha);
  aaLine(fb, cx, cy - 28, cx, cy - 28, 4.5f, col, alpha);
  if (!on) {
    aaLine(fb, cx - 34, cy - 34, cx + 34, cy + 34, 6.5f, 0x0000, alpha);
    aaLine(fb, cx - 34, cy - 34, cx + 34, cy + 34, 3.5f, 0xF800, alpha);
  }
}

void setup() {
  Serial.begin(115200);
  i2cLock = xSemaphoreCreateMutex();
  setenv("TZ", LOCAL_TZ, 1);
  tzset();

  if (!gfx->begin()) Serial.println("gfx begin failed");
  gfx->setBrightness(190);
  gfx->fillScreen(0);

  Wire.begin(IIC_SDA, IIC_SCL);
  pinMode(PA_PIN, OUTPUT);
  digitalWrite(PA_PIN, LOW);
  touch.setPins(TP_RESET, TP_INT);
  if (touch.begin(Wire, TOUCH_ADDR, IIC_SDA, IIC_SCL)) {
    touch.setMaxCoordinates(SCR, SCR);
  } else {
    Serial.println("touch init failed");
  }
  {
    Preferences prefs;
    prefs.begin("clock", true);
    chimeEnabled = prefs.getBool("chime", false);
    prefs.end();
  }
  time_t t;
  if (readRTC(&t)) {
    setSystemTime(t);
    Serial.println("time from RTC");
  } else {
    setSystemTime(buildTime());
    Serial.println("RTC invalid, using build time - send T<epoch> over serial");
  }

  canvas = new Arduino_Canvas(SCR, SCR, gfx, 0, 0);
  canvas->begin(GFX_SKIP_OUTPUT_BEGIN);
  fb = canvas->getFramebuffer();
  bases[0] = (uint16_t *)ps_malloc(SCR * SCR * 2);
  bases[1] = (uint16_t *)ps_malloc(SCR * SCR * 2);
  tex = (uint16_t *)ps_malloc(TEX_W * TEX_H * 2);
  zt16 = (uint16_t *)ps_malloc(GLOBE_N * GLOBE_N * 2);
  ea = (uint8_t *)ps_malloc(GLOBE_N * GLOBE_N);
  if (!fb || !bases[0] || !bases[1] || !tex || !zt16 || !ea) {
    Serial.println("out of memory");
    while (1) delay(1000);
  }
  memcpy(tex, earth_tex_start, TEX_W * TEX_H * 2);

  for (int i = 0; i < 256; i++) {
    float z = (i + 0.5f) / 256.0f;
    float shade = 0.28f + 0.72f * powf(z, 0.55f);
    float haze = 0.6f * powf(1.0f - z, 2.8f);
    shLUT[i] = (uint16_t)(shade * (1.0f - haze) * 256.0f);
    hzLUT[i] = (uint16_t)(haze * 256.0f);
  }
  for (int j = 0; j < GLOBE_N; j++)
    for (int i = 0; i < GLOBE_N; i++) {
      float x = (i + 0.5f - GLOBE_R) / GLOBE_R, y = (GLOBE_R - (j + 0.5f)) / GLOBE_R;
      float rho = sqrtf(x * x + y * y);
      float cov = (1.0f - rho) * GLOBE_R + 0.5f;
      cov = cov < 0 ? 0 : (cov > 1 ? 1 : cov);
      ea[j * GLOBE_N + i] = (uint8_t)(cov * 255.0f + 0.5f);
      zt16[j * GLOBE_N + i] = rho < 1.0f ? (uint16_t)(sqrtf(1.0f - rho * rho) * 65535.0f) : 0;
    }

  memset(bases[0], 0, SCR * SCR * 2);
  memset(bases[1], 0, SCR * SCR * 2);
  struct timeval tv;
  gettimeofday(&tv, NULL);
  renderGlobe(bases[0], (double)tv.tv_sec + tv.tv_usec * 1e-6);
  front = 0;
  xTaskCreatePinnedToCore(globeTask, "globe", 8192, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(syncTask, "sync", 8192, NULL, 1, &syncTaskHandle, 0);
  xTaskCreatePinnedToCore(chimeTask, "chime", 8192, NULL, 1, &chimeTaskHandle, 0);
  xTaskCreatePinnedToCore(touchTask, "touch", 4096, NULL, 2, NULL, 0);
}

void loop() {
  static uint32_t lastFps = 0, frames = 0;
  handleSerial();
  if (pending) {
    front = 1 - front;
    pending = false;
  }

  struct timeval tv;
  gettimeofday(&tv, NULL);
  uint32_t nowMs = millis();
  time_t sec = tv.tv_sec;
  struct tm lt;
  localtime_r(&sec, &lt);
  float s = lt.tm_sec + tv.tv_usec * 1e-6f;
  float m = lt.tm_min + s / 60.0f;
  float h = (lt.tm_hour % 12) + m / 60.0f;

  memcpy(fb, bases[front], SCR * SCR * 2);

  float ha = h * (TWO_PI / 12), ma = m * (TWO_PI / 60), sa = s * (TWO_PI / 60);
  float hx = sinf(ha), hy = -cosf(ha), mx = sinf(ma), my = -cosf(ma), sx = sinf(sa), sy = -cosf(sa);

  // Swiss railway style: flat white bars with a thin dark edge, red second hand with a disc.
  auto bar = [&](float ux, float uy, float a, float b, float hw, uint16_t col, int al, float off) {
    aaBar(fb, CX + ux * a + off, CY + uy * a + off, CX + ux * b + off, CY + uy * b + off, hw, col, al);
  };
  bar(hx, hy, -30, 150, 11.5f, 0x0000, 110, 3);
  bar(mx, my, -38, 212, 8.5f, 0x0000, 110, 3);
  bar(hx, hy, -31.5f, 151.5f, 11.5f, 0x0000, 256, 0);
  bar(hx, hy, -30, 150, 10, 0xFFFF, 256, 0);
  bar(mx, my, -39.5f, 213.5f, 8.5f, 0x0000, 256, 0);
  bar(mx, my, -38, 212, 7, 0xFFFF, 256, 0);
  aaLine(fb, CX + sx * -45 + 4, CY + sy * -45 + 4, CX + sx * 165 + 4, CY + sy * 165 + 4, 3.0f, 0x0000, 100);
  aaLine(fb, CX + sx * 176 + 4, CY + sy * 176 + 4, CX + sx * 176 + 4, CY + sy * 176 + 4, 16.5f, 0x0000, 100);
  aaLine(fb, CX + sx * -45, CY + sy * -45, CX + sx * 165, CY + sy * 165, 1.7f, 0xF800, 256);
  aaLine(fb, CX + sx * 176, CY + sy * 176, CX + sx * 176, CY + sy * 176, 16, 0xF800, 256);
  aaLine(fb, CX, CY, CX, CY, 7, 0xF800, 256);

  if (tapFlag) {
    tapFlag = false;
    chimeEnabled = !chimeEnabled;
    Preferences prefs;
    prefs.begin("clock", false);
    prefs.putBool("chime", chimeEnabled);
    prefs.end();
    bellUntil = nowMs + 1800;
    if (chimeEnabled && chimeTaskHandle) xTaskNotifyGive(chimeTaskHandle);
    Serial.printf("hourly chime %s\n", chimeEnabled ? "on" : "off");
  }
  if ((int32_t)(bellUntil - nowMs) > 0) {
    int32_t left = (int32_t)(bellUntil - nowMs);
    drawBell(chimeEnabled, left > 400 ? 256 : left * 256 / 400);
  }

  static int lastChimeStamp = -1;
  int stamp = lt.tm_yday * 24 + lt.tm_hour;
  if (lastChimeStamp < 0) {
    lastChimeStamp = stamp;
  } else if (stamp != lastChimeStamp) {
    lastChimeStamp = stamp;
    if (chimeEnabled && lt.tm_min == 0 && lt.tm_sec < 5 && chimeTaskHandle) xTaskNotifyGive(chimeTaskHandle);
  }

  if (shotRequested) {  // raw RGB565 frame: "SHOT", byte count, pixels (see tools/screenshot.py)
    shotRequested = false;
    const size_t total = (size_t)SCR * SCR * 2;
    uint32_t hdr[2] = {0x544F4853u, (uint32_t)total};
    Serial.write((const uint8_t *)hdr, sizeof(hdr));
    for (size_t off = 0; off < total;) {
      size_t n = Serial.write((const uint8_t *)fb + off, min((size_t)4096, total - off));
      off += n;
      if (!n) delay(1);
    }
  }

  canvas->flush();

  frames++;
  if (nowMs - lastFps >= 5000) {
    Serial.printf("%.1f fps\n", frames * 1000.0f / (nowMs - lastFps));
    frames = 0;
    lastFps = nowMs;
  }
}
