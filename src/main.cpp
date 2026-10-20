#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <sys/time.h>

// スマホ（Android の Chrome＝Web Bluetooth）から表示を選ぶ。BLE の口は下の UUID・ページは docs/index.html。
// 選んだ物は NVS に覚える＝電源を入れ直しても残る。時計の時刻だけは覚えない（RTC が無い）＝つなぐたびにページが送る。

U8G2_SH1107_64X128_F_HW_I2C u8g2(U8G2_R1, /* reset=*/ U8X8_PIN_NONE);

#define SVC_UUID   "7e1a0001-6a3d-4c1b-9e57-71e9e0c0de01"
#define MODE_UUID  "7e1a0002-6a3d-4c1b-9e57-71e9e0c0de01"  // 1 バイト：0=DVD 1=文字 2=時計
#define TEXT_UUID  "7e1a0003-6a3d-4c1b-9e57-71e9e0c0de01"  // UTF-8 の文字列（最大 96 バイト）
#define SPEED_UUID "7e1a0004-6a3d-4c1b-9e57-71e9e0c0de01"  // 2 バイト：dx, dy（1〜6）
#define TIME_UUID  "7e1a0005-6a3d-4c1b-9e57-71e9e0c0de01"  // 8 バイト：UNIX 秒（int32 LE）＋ UTC からの分（int32 LE）

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
bool time_set = false;
int32_t tz_min = 9 * 60;

// DVD
int x = 10, y = 31;
int dx = 2, dy = 1; // 速度を変えると軌道が変わって「角待ち」が楽しめます
const int logo_w = 32; // フォントに合わせた幅
const int logo_h = 14; // フォントに合わせた高さ
bool is_inverse = false;

// 文字
int text_x = 128;

void applySpeed() {
  dx = (dx < 0 ? -1 : 1) * speed_x;
  dy = (dy < 0 ? -1 : 1) * speed_y;
}

class Writes : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c) override {
    std::string v = c->getValue();
    String uuid = c->getUUID().toString().c_str();
    if (uuid == MODE_UUID && v.size() >= 1 && (uint8_t)v[0] < MODE_COUNT) {
      mode = (uint8_t)v[0];
    } else if (uuid == TEXT_UUID) {
      portENTER_CRITICAL(&text_mux);
      size_t n = min((size_t)96, v.size());
      memcpy(pending_text, v.data(), n);
      pending_text[n] = 0;
      text_pending = true;
      portEXIT_CRITICAL(&text_mux);
    } else if (uuid == SPEED_UUID && v.size() >= 2) {
      speed_x = constrain((uint8_t)v[0], 1, 6);
      speed_y = constrain((uint8_t)v[1], 1, 6);
      applySpeed();
    } else if (uuid == TIME_UUID && v.size() >= 8) {
      int32_t t, tz;
      memcpy(&t, v.data(), 4);
      memcpy(&tz, v.data() + 4, 4);
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

class Conn : public NimBLEServerCallbacks {
  void onDisconnect(NimBLEServer *s) override { NimBLEDevice::startAdvertising(); }  // 切れたらまた見つけてもらう
};

void setupBle() {
  NimBLEDevice::init("tiepin");
  NimBLEServer *srv = NimBLEDevice::createServer();
  srv->setCallbacks(new Conn());
  NimBLEService *svc = srv->createService(SVC_UUID);
  static Writes cb;
  const uint32_t RW = NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE;
  svc->createCharacteristic(MODE_UUID, RW)->setCallbacks(&cb);
  svc->createCharacteristic(TEXT_UUID, RW)->setCallbacks(&cb);
  svc->createCharacteristic(SPEED_UUID, RW)->setCallbacks(&cb);
  svc->createCharacteristic(TIME_UUID, NIMBLE_PROPERTY::WRITE)->setCallbacks(&cb);
  // 今の値を読めるようにしておく（ページが開いた時に今の設定を表示する）
  uint8_t m = mode;
  svc->getCharacteristic(MODE_UUID)->setValue(&m, 1);
  svc->getCharacteristic(TEXT_UUID)->setValue(text.c_str());
  uint8_t sp[2] = { (uint8_t)speed_x, (uint8_t)speed_y };
  svc->getCharacteristic(SPEED_UUID)->setValue(sp, 2);
  svc->start();
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(SVC_UUID);
  adv->setScanResponse(true);
  adv->start();
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

  u8g2.setFont(u8g2_font_helvB10_tr);

  if (is_inverse) {
      u8g2.drawBox(0, 0, 128, 64);
      u8g2.setDrawColor(0); // 反転時は黒で文字を書く
      u8g2.drawStr(x, y, "DVD");
      u8g2.setDrawColor(1); // 色を戻す
  } else {
      u8g2.drawStr(x, y, "DVD");
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
  if (dirty) {  // NVS への書き込みは BLE の割り込みの外で
    dirty = false;
    prefs.putUChar("mode", mode);
    prefs.putString("text", text);
    prefs.putUChar("sx", speed_x);
    prefs.putUChar("sy", speed_y);
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
