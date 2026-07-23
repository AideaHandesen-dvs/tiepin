#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>

U8G2_SH1107_64X128_F_HW_I2C u8g2(U8G2_R1, /* reset=*/ U8X8_PIN_NONE);

int x = 10, y = 31;
int dx = 2, dy = 1; // 速度を変えると軌道が変わって「角待ち」が楽しめます
const int logo_w = 32; // フォントに合わせた幅
const int logo_h = 14; // フォントに合わせた高さ
bool is_inverse = false;

void setup() {
  Wire.begin(8, 9);
  u8g2.begin();
  u8g2.setContrast(255);
}

void loop() {
  u8g2.clearBuffer();

  // 反転モードの適用
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

  // フォントを確実に存在する「u8g2_font_helvB10_tr」に変更
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

  u8g2.sendBuffer();
  delay(30);
}
