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
#define MODE_UUID  "7e1a0002-6a3d-4c1b-9e57-71e9e0c0de01"  // 1 バイト：0=DVD 1=文字 2=時計
#define TEXT_UUID  "7e1a0003-6a3d-4c1b-9e57-71e9e0c0de01"  // UTF-8 の文字列（最大 96 バイト）
#define SPEED_UUID "7e1a0004-6a3d-4c1b-9e57-71e9e0c0de01"  // 2 バイト：dx, dy（1〜6）
#define TIME_UUID  "7e1a0005-6a3d-4c1b-9e57-71e9e0c0de01"  // 8 バイト：UNIX 秒（int32 LE）＋ UTC からの分（int32 LE）
#define LOGO_UUID  "7e1a0006-6a3d-4c1b-9e57-71e9e0c0de01"  // 跳ねる文字（UTF-8・最大 32 バイト）
#define BOX_UUID   "7e1a0007-6a3d-4c1b-9e57-71e9e0c0de01"  // 書く 1 バイト：当たり判定の幅（8〜120・0＝文字に合わせる）
                                                            // 読む 3 バイト：幅の設定・実際の幅・実際の高さ

enum Mode : uint8_t { MODE_DVD = 0, MODE_TEXT = 1, MODE_CLOCK = 2, MODE_COUNT };

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

void applySpeed() {
  dx = (dx < 0 ? -1 : 1) * speed_x;
  dy = (dy < 0 ? -1 : 1) * speed_y;
}

BLECharacteristic *ch_mode, *ch_text, *ch_speed, *ch_time, *ch_logo, *ch_box;

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
  logo_w = box_w ? box_w : u8g2.getUTF8Width(logo.c_str()) + 1;
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
      u8g2.drawUTF8(x, y, logo.c_str());
      u8g2.setDrawColor(1); // 色を戻す
  } else {
      u8g2.drawUTF8(x, y, logo.c_str());
      u8g2.drawFrame(0, 0, 128, 64); // 通常時は枠を表示
  }
}

void drawText() {
  // 日本語も出せるフォント（JIS 第1水準の一部）。無い字は空白になる
  u8g2.setFont(u8g2_font_unifont_t_japanese1);
  int w = u8g2.getUTF8Width(text.c_str());
  if (w <= 128) {
    u8g2.drawUTF8((128 - w) / 2, 40, text.c_str());  // 収まる時は真ん中に止める
  } else {
    u8g2.drawUTF8(text_x, 40, text.c_str());          // 収まらない時は右から左へ流す
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
    default:         drawDvd();   break;
  }
  u8g2.sendBuffer();
  delay(30);
}
