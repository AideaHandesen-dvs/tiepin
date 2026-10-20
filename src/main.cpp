#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <Preferences.h>
#include <BLEDevice.h>  // core 付属の BLE（外付けの NimBLE-Arduino 1.4 は今の core と噛み合わず、起動時に落ちる）
#include <sys/time.h>

// スマホ（Android の Chrome＝Web Bluetooth）から表示を選ぶ。BLE の口は下の UUID・ページは docs/index.html。
// 選んだ物は NVS に覚える＝電源を入れ直しても残る。時計の時刻だけは覚えない（RTC が無い）＝つなぐたびにページが送る。

U8G2_SH1107_64X128_F_HW_I2C u8g2(U8G2_R1, /* reset=*/ U8X8_PIN_NONE);

#define SVC_UUID   "7e1a0001-6a3d-4c1b-9e57-71e9e0c0de01"
#define MODE_UUID  "7e1a0002-6a3d-4c1b-9e57-71e9e0c0de01"  // 1 バイト：0=DVD 1=文字 2=時計 3=ライフ（ライフ中にもう一度 3 でまき直す）
#define TEXT_UUID  "7e1a0003-6a3d-4c1b-9e57-71e9e0c0de01"  // UTF-8 の文字列（最大 96 バイト）
#define SPEED_UUID "7e1a0004-6a3d-4c1b-9e57-71e9e0c0de01"  // 2 バイト：dx, dy（1〜6）
#define TIME_UUID  "7e1a0005-6a3d-4c1b-9e57-71e9e0c0de01"  // 8 バイト：UNIX 秒（int32 LE）＋ UTC からの分（int32 LE）
#define LOGO_UUID  "7e1a0006-6a3d-4c1b-9e57-71e9e0c0de01"  // 跳ねる文字（UTF-8・最大 32 バイト）
#define BOX_UUID   "7e1a0007-6a3d-4c1b-9e57-71e9e0c0de01"  // 書く 1 バイト：当たり判定の幅（8〜120・0＝文字に合わせる）
                                                            // 読む 3 バイト：幅の設定・実際の幅・実際の高さ

enum Mode : uint8_t { MODE_DVD = 0, MODE_TEXT = 1, MODE_CLOCK = 2, MODE_LIFE = 3, MODE_COUNT };

Preferences prefs;
volatile uint8_t mode = MODE_DVD;
String text = "tiepin";
int speed_x = 2, speed_y = 1;
volatile bool dirty = false;      // BLE から書かれた＝次の loop で NVS に保存する
// 文字は BLE の糸で受けて、描く糸（loop）で受け取る＝描いている最中に String を差し替えない
portMUX_TYPE text_mux = portMUX_INITIALIZER_UNLOCKED;
char pending_text[97];
volatile bool text_pending = false;
char pending_logo[33];
volatile bool logo_pending = false;
bool time_set = false;
int32_t tz_min = 9 * 60;

// DVD
int x = 10, y = 31;
int dx = 2, dy = 1; // 速度を変えると軌道が変わって「角待ち」が楽しめます
String logo = "DVD";
uint8_t box_w = 32;    // 当たり判定の幅の設定（0＝文字に合わせる）。32 は "DVD" に合わせて目で決めた幅
int logo_w = 32;       // 実際に使う幅
int logo_h = 14;       // 実際に使う高さ（フォントで決まる）
bool is_inverse = false;

// 文字
int text_x = 128;

// ライフゲーム（盤＝画面の 128×64・端は反対側とつながる）
const int LW = 128, LH = 64;
uint8_t life[LH][LW], life_next[LH][LW];
volatile bool life_reseed = true;
uint32_t life_hist[8];      // 直近の盤の指紋＝短い周期の繰り返しを見つける
int life_hist_i = 0;
// 300 世代ごとに生きているマスの数の幅を見る＝ほとんど増減しないなら落ち着いた（グライダーと点滅が残るだけ、など）
int life_gen = 0, life_min = 0, life_max = 0;
bool life_calm = false;
int life_stale = 0;         // 止まった・繰り返しと見てからの世代＝少し見せてからまき直す

void applySpeed() {
  dx = (dx < 0 ? -1 : 1) * speed_x;
  dy = (dy < 0 ? -1 : 1) * speed_y;
}

BLECharacteristic *ch_mode, *ch_text, *ch_speed, *ch_time, *ch_logo, *ch_box;

// ---- 文字に混ぜるアイコン
// 記号（♥ など）を受けたら Open Iconic（16px）の絵で描く。絵文字キーボードの別の書き方（❤ など）も同じ絵に寄せる。
// ページの ICONS と揃える。
struct Icon { uint32_t cp; uint16_t glyph; };
enum : uint16_t { DRAW_BOX = 1, DRAW_CIRCLE, DRAW_RADIO };  // Open Iconic に無いもの＝線で描く
const Icon ICONS[] = {
  { 0x2665, 183 }, { 0x2764, 183 }, { 0x2661, 183 },  // ♥ ❤ ♡
  { 0x2605, 258 }, { 0x2B50, 258 }, { 0x2606, 258 },  // ★ ⭐ ☆
  { 0x1F44D, 268 }, { 0x1F44E, 267 },                 // 👍 👎
  { 0x2713, 115 }, { 0x2714, 115 }, { 0x2705, 120 },  // ✓ ✔ ✅
  { 0x274C, 121 }, { 0x2716, 283 }, { 0x2611, 264 },  // ❌ ✖ ☑
  { 0x2610, DRAW_BOX }, { 0x25CB, DRAW_CIRCLE }, { 0x25C9, DRAW_RADIO }, { 0x25CF, DRAW_RADIO },  // ☐ ○ ◉ ●
  { 0x26A0, 280 }, { 0x2753, 240 }, { 0x2139, 188 },  // ⚠ ❓ ℹ
  { 0x266A, 225 }, { 0x266B, 225 }, { 0x1F3B5, 225 }, // ♪ ♫ 🎵
  { 0x1F3A7, 182 }, { 0x1F3A4, 220 }, { 0x1F514, 93 }, { 0x1F4E3, 105 }, { 0x1F4AC, 114 },  // 🎧 🎤 🔔 📣 💬
  { 0x1F4F1, 230 }, { 0x260E, 230 }, { 0x1F4DE, 230 },  // 📱 ☎ 📞
  { 0x2709, 156 }, { 0x1F4E7, 156 }, { 0x1F4F7, 108 }, { 0x1F4A1, 196 }, { 0x1F525, 168 },  // ✉ 📧 📷 💡 🔥
  { 0x26A1, 96 }, { 0x2600, 259 }, { 0x2601, 124 }, { 0x2614, 241 }, { 0x2602, 241 }, { 0x1F319, 223 },  // ⚡ ☀ ☁ ☔ ☂ 🌙
  { 0x1F3E0, 184 }, { 0x1F4CD, 209 }, { 0x1F4CC, 232 }, { 0x1F6A9, 169 },  // 🏠 📍 📌 🚩
  { 0x1F511, 193 }, { 0x1F512, 202 }, { 0x1F50D, 207 },  // 🔑 🔒 🔍
  { 0x1F552, 123 }, { 0x23F1, 269 }, { 0x1F4C5, 107 }, { 0x1F4BC, 100 }, { 0x1F4D6, 97 }, { 0x270F, 227 },  // 🕒 ⏱ 📅 💼 📖 ✏
  { 0x1F464, 229 }, { 0x1F465, 228 }, { 0x1F3AF, 263 }, { 0x1F50B, 91 }, { 0x1F41B, 104 }, { 0x1F6D2, 113 },  // 👤 👥 🎯 🔋 🐛 🛒
  { 0x221E, 187 }, { 0x1F310, 175 }, { 0x1F527, 282 }, { 0x1F5D1, 271 },  // ∞ 🌐 🔧 🗑
};
const int ICON_W = 17;  // 16px の絵＋1px の隙間

uint16_t iconFor(uint32_t cp) {
  for (const Icon &i : ICONS) if (i.cp == cp) return i.glyph;
  return 0;
}

// UTF-8 を 1 字ずつ読む（壊れた並びは 1 バイト飛ばす）
uint32_t nextCp(const char *&p) {
  uint8_t c = *p++;
  int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
  uint32_t cp = n == 3 ? c & 7 : n == 2 ? c & 15 : n == 1 ? c & 31 : c;
  while (n-- && (*p & 0xC0) == 0x80) cp = (cp << 6) | (*p++ & 0x3F);
  return cp;
}
bool isInvisible(uint32_t cp) { return cp == 0xFE0F || cp == 0xFE0E || cp == 0x200D; }  // 絵文字の後ろに付く見えない字

// 文字とアイコンを混ぜて描く。draw=false なら幅だけ返す。文字のフォントは呼ぶ側が選んでおく。
// y は文字のベースライン。アイコンは下端をベースラインの 2px 下に揃える（unifont の字の箱に合わせる）
int drawRich(int x, int y, const String &s, bool draw = true) {
  const uint8_t *font = u8g2.getU8g2()->font;
  int x0 = x;
  for (const char *p = s.c_str(); *p;) {
    uint32_t cp = nextCp(p);
    if (isInvisible(cp)) continue;
    uint16_t g = iconFor(cp);
    if (g == DRAW_BOX) {
      if (draw) u8g2.drawFrame(x + 2, y - 11, 12, 12);
      x += ICON_W;
    } else if (g == DRAW_CIRCLE || g == DRAW_RADIO) {
      if (draw) {
        u8g2.drawCircle(x + 8, y - 5, 6);
        if (g == DRAW_RADIO) u8g2.drawDisc(x + 8, y - 5, 3);
      }
      x += ICON_W;
    } else if (g) {
      if (draw) {
        u8g2.setFont(u8g2_font_open_iconic_all_2x_t);
        u8g2.drawGlyph(x, y + 2, g);
        u8g2.setFont(font);
      }
      x += ICON_W;
    } else if (cp <= 0xFFFF) {
      x += draw ? u8g2.drawGlyph(x, y, cp) : u8g2_GetGlyphWidth(u8g2.getU8g2(), cp);
    }
  }
  return x - x0;
}

bool isAscii(const String &s) {
  for (size_t i = 0; i < s.length(); i++) if ((uint8_t)s[i] >= 0x80) return false;
  return true;
}

// 英数字だけなら太字、日本語が混じったら日本語の出るフォント
void setLogoFont() {
  u8g2.setFont(isAscii(logo) ? u8g2_font_helvB10_tr : u8g2_font_unifont_t_japanese1);
}

// 跳ねる文字か幅が変わったら、当たり判定の箱を決め直す（loop の中で呼ぶ＝u8g2 を触るので）
void applyBox() {
  setLogoFont();
  logo_h = isAscii(logo) ? 14 : 16;
  logo_w = box_w ? box_w : drawRich(0, 0, logo, false) + 1;
  logo_w = constrain(logo_w, 8, 120);
  x = constrain(x, 1, 127 - logo_w);  // 箱が広がって壁にめり込んだら内へ戻す（めり込んだままだと毎コマ跳ね返る）
  uint8_t b[3] = { box_w, (uint8_t)logo_w, (uint8_t)logo_h };
  if (ch_box) ch_box->setValue(b, 3);
}

class Writes : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    const uint8_t *v = c->getData();
    size_t len = c->getLength();
    if (c == ch_mode && len >= 1 && v[0] < MODE_COUNT) {
      if (v[0] == MODE_LIFE) life_reseed = true;  // ライフを選ぶたびにまき直す
      mode = v[0];
    } else if (c == ch_text) {
      portENTER_CRITICAL(&text_mux);
      size_t n = min((size_t)96, len);
      memcpy(pending_text, v, n);
      pending_text[n] = 0;
      text_pending = true;
      portEXIT_CRITICAL(&text_mux);
    } else if (c == ch_logo) {
      portENTER_CRITICAL(&text_mux);
      size_t n = min((size_t)32, len);
      memcpy(pending_logo, v, n);
      pending_logo[n] = 0;
      logo_pending = true;
      portEXIT_CRITICAL(&text_mux);
    } else if (c == ch_box && len >= 1) {
      box_w = v[0] ? constrain(v[0], 8, 120) : 0;
      logo_pending = true;  // 箱は loop で決め直す
    } else if (c == ch_speed && len >= 2) {
      speed_x = constrain(v[0], 1, 6);
      speed_y = constrain(v[1], 1, 6);
      applySpeed();
    } else if (c == ch_time && len >= 8) {
      int32_t t, tz;
      memcpy(&t, v, 4);
      memcpy(&tz, v + 4, 4);
      struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
      settimeofday(&tv, nullptr);
      tz_min = tz;
      time_set = true;
      return;  // 時刻は覚えない
    } else {
      return;
    }
    dirty = true;
  }
};

// つながっている間も名乗り続ける＝前のつながりが裏で残っていても、つなぎ直せる
class Conn : public BLEServerCallbacks {
  void onConnect(BLEServer *s) override { BLEDevice::startAdvertising(); }
  void onDisconnect(BLEServer *s) override { BLEDevice::startAdvertising(); }
};

void setupBle() {
  BLEDevice::init("tiepin");
  BLEServer *srv = BLEDevice::createServer();
  srv->setCallbacks(new Conn());
  BLEService *svc = srv->createService(SVC_UUID);
  static Writes cb;
  const uint32_t RW = BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE;
  ch_mode  = svc->createCharacteristic(MODE_UUID, RW);
  ch_text  = svc->createCharacteristic(TEXT_UUID, RW);
  ch_speed = svc->createCharacteristic(SPEED_UUID, RW);
  ch_time  = svc->createCharacteristic(TIME_UUID, BLECharacteristic::PROPERTY_WRITE);
  ch_logo  = svc->createCharacteristic(LOGO_UUID, RW);
  ch_box   = svc->createCharacteristic(BOX_UUID, RW);
  for (BLECharacteristic *c : { ch_mode, ch_text, ch_speed, ch_time, ch_logo, ch_box }) c->setCallbacks(&cb);
  // 今の値を読めるようにしておく（ページが開いた時に今の設定を表示する）
  uint8_t m = mode;
  ch_mode->setValue(&m, 1);
  ch_text->setValue(text);
  uint8_t sp[2] = { (uint8_t)speed_x, (uint8_t)speed_y };
  ch_speed->setValue(sp, 2);
  ch_logo->setValue(logo);
  applyBox();
  svc->start();
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SVC_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
}

void drawDvd() {
  u8g2.setBitmapMode(is_inverse);

  bool hit_x = false;
  bool hit_y = false;

  // 壁判定
  if (x <= 0 || x + logo_w >= 128) {
    dx = -dx;
    hit_x = true;
  }
  if (y - logo_h <= 0 || y >= 64) {
    dy = -dy;
    hit_y = true;
  }

  // 「角」ヒット判定（XとYが同時に当たったら反転）
  if (hit_x && hit_y) {
    is_inverse = !is_inverse;
  }

  x += dx;
  y += dy;

  setLogoFont();

  if (is_inverse) {
      u8g2.drawBox(0, 0, 128, 64);
      u8g2.setDrawColor(0); // 反転時は黒で文字を書く
      drawRich(x, y, logo);
      u8g2.setDrawColor(1); // 色を戻す
  } else {
      drawRich(x, y, logo);
      u8g2.drawFrame(0, 0, 128, 64); // 通常時は枠を表示
  }
}

void drawText() {
  // 日本語も出せるフォント（JIS 第1水準の一部）。無い字は空白になる
  u8g2.setFont(u8g2_font_unifont_t_japanese1);
  int w = drawRich(0, 0, text, false);
  if (w <= 128) {
    drawRich((128 - w) / 2, 40, text);  // 収まる時は真ん中に止める
  } else {
    drawRich(text_x, 40, text);         // 収まらない時は右から左へ流す
    text_x -= 2;
    if (text_x < -w) text_x = 128;
  }
}

void drawClock() {
  u8g2.setFont(u8g2_font_logisoso28_tn);
  char buf[8];
  if (!time_set) {
    strcpy(buf, "--:--");
  } else {
    time_t t = time(nullptr) + tz_min * 60;
    struct tm tm;
    gmtime_r(&t, &tm);
    snprintf(buf, sizeof(buf), (tm.tm_sec % 2) ? "%02d:%02d" : "%02d %02d", tm.tm_hour, tm.tm_min);
  }
  int w = u8g2.getStrWidth(buf);
  u8g2.drawStr((128 - w) / 2, 46, buf);
}

void seedLife() {
  for (int y = 0; y < LH; y++)
    for (int x = 0; x < LW; x++) life[y][x] = (esp_random() % 100) < 30;  // 3 割くらいまく
  memset(life_hist, 0, sizeof(life_hist));
  life_gen = 0; life_min = INT_MAX; life_max = 0; life_calm = false; life_stale = 0;
}

void drawLife() {
  if (life_reseed) { life_reseed = false; seedLife(); }
  int pop = 0;
  uint32_t h = 2166136261u;  // FNV-1a で盤の指紋を取る
  for (int y = 0; y < LH; y++) {
    const uint8_t *up = life[(y + LH - 1) % LH], *row = life[y], *dn = life[(y + 1) % LH];
    for (int x = 0; x < LW; x++) {
      int l = (x + LW - 1) % LW, r = (x + 1) % LW;
      int n = up[l] + up[x] + up[r] + row[l] + row[r] + dn[l] + dn[x] + dn[r];
      uint8_t a = n == 3 || (n == 2 && row[x]);
      life_next[y][x] = a;
      if (a) { pop++; u8g2.drawPixel(x, y); h = (h ^ (y * LW + x)) * 16777619u; }
    }
  }
  memcpy(life, life_next, sizeof(life));

  // 全滅・同じ盤の繰り返し・数がほとんど動かない＝見どころは終わり
  bool repeat = false;
  for (uint32_t old : life_hist) if (old == h) repeat = true;
  life_hist[life_hist_i++ % 8] = h;
  life_min = min(life_min, pop); life_max = max(life_max, pop);
  if (++life_gen % 300 == 0) {
    life_calm = life_max - life_min <= 12;
    life_min = INT_MAX; life_max = 0;
  }
  if (pop == 0 || repeat || life_calm) {
    if (++life_stale > 90) life_reseed = true;  // 3 秒くらい見せてからまき直す
  } else {
    life_stale = 0;
  }
}

void setup() {
  Wire.begin(8, 9);
  u8g2.begin();
  u8g2.setContrast(255);
  u8g2.enableUTF8Print();

  prefs.begin("tiepin", false);
  mode = prefs.getUChar("mode", MODE_DVD);
  if (mode >= MODE_COUNT) mode = MODE_DVD;
  text = prefs.getString("text", "tiepin");
  speed_x = prefs.getUChar("sx", 2);
  speed_y = prefs.getUChar("sy", 1);
  applySpeed();
  logo = prefs.getString("logo", "DVD");
  box_w = prefs.getUChar("bw", 32);

  setupBle();
}

void loop() {
  if (text_pending) {
    char buf[97];
    portENTER_CRITICAL(&text_mux);
    memcpy(buf, pending_text, sizeof(buf));
    text_pending = false;
    portEXIT_CRITICAL(&text_mux);
    text = String(buf);
    text_x = 128;
    dirty = true;  // 受け取った文字で保存し直す（BLE の dirty が先に消費されていても取りこぼさない）
  }
  if (logo_pending) {
    char buf[33];
    portENTER_CRITICAL(&text_mux);
    memcpy(buf, pending_logo, sizeof(buf));
    pending_logo[0] = 0;  // 幅だけ変えた時は空のまま＝文字はそのまま
    logo_pending = false;
    portEXIT_CRITICAL(&text_mux);
    if (buf[0]) logo = String(buf);
    applyBox();
    dirty = true;
  }
  if (dirty) {  // NVS への書き込みは BLE の割り込みの外で
    dirty = false;
    prefs.putUChar("mode", mode);
    prefs.putString("text", text);
    prefs.putUChar("sx", speed_x);
    prefs.putUChar("sy", speed_y);
    prefs.putString("logo", logo);
    prefs.putUChar("bw", box_w);
  }

  u8g2.clearBuffer();
  switch (mode) {
    case MODE_TEXT:  drawText();  break;
    case MODE_CLOCK: drawClock(); break;
    case MODE_LIFE:  drawLife();  break;
    default:         drawDvd();   break;
  }
  u8g2.sendBuffer();
  delay(30);
}
