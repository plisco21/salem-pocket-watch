// TEST SCHERMO + CORONA — GC9A01, retroilluminazione su GPIO5, encoder EC11. Senza radio.
// Display: SCL=4, SDA=3, RES=2, DC=1, CS=0, BLK=5
// Corona (come salem): A=GPIO20, B=GPIO21, SW=GPIO10 | centrale (lato da 3 pin) e secondo pin switch -> GND
// Arduino IDE > Strumenti: Scheda ESP32C3 Dev Module, USB CDC On Boot: Enabled
//
// 1) All'accensione: colori pieni (ROSSO, VERDE, BLU, BIANCO, NERO), schermata di orientamento,
//    retroilluminazione spenta 1 s e riaccesa.
// 2) Poi schermata della corona: gira = il numero cambia, click = contatore click,
//    pressione lunga (oltre 1 s) = ripete il test dei colori.
// Tutto viene stampato anche sul Monitor seriale (115200): la corona si prova anche a schermo nero.
//
// Cosa controllare:
//  - ROSSO e' rosso e BLU e' blu          -> altrimenti cambia PANEL_RGB_ORDER
//  - NERO e' nero e BIANCO e' bianco      -> altrimenti cambia PANEL_INVERT
//  - "ALTO" e' in alto                    -> altrimenti cambia SCREEN_ROTATION (0..3)
//  - la luce si spegne e si riaccende     -> altrimenti BLK non e' su GPIO5 (o BLK_ACTIVE_HIGH al contrario)
//  - uno scatto della corona = +1 o -1    -> altrimenti cambia STEPS_PER_DETENT (2 o 4)
//  - girando in senso orario il numero sale -> altrimenti INVERT_DIR = true

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// ---- Stesse impostazioni di salem ----
const bool PANEL_INVERT     = true;
const bool PANEL_RGB_ORDER  = true;
const int  SCREEN_ROTATION  = 1;       // 0 = normale, 1 = 90 gradi orario, 2 = 180, 3 = 270
const int  PIN_BLK          = 5;
const bool BLK_ACTIVE_HIGH  = true;
const uint32_t SPI_FREQ     = 10000000; // 10 MHz per la prova (salem usa 27 MHz): piu' tollerante con fili lunghi

const int  PIN_ENC_A        = 20;
const int  PIN_ENC_B        = 21;
const int  PIN_ENC_SW       = 10;
const int  STEPS_PER_DETENT = 4;
const bool INVERT_DIR       = false;

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_GC9A01 _panel_instance;
  lgfx::Bus_SPI _bus_instance;

public:
  LGFX(void) {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = SPI_FREQ;
      cfg.freq_read = 16000000;
      cfg.pin_sclk = 4;
      cfg.pin_mosi = 3;
      cfg.pin_miso = -1;
      cfg.pin_dc   = 1;
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }
    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs   = 0;
      cfg.pin_rst  = 2;
      cfg.panel_width  = 240;
      cfg.panel_height = 240;
      cfg.readable  = false;
      cfg.invert    = PANEL_INVERT;
      cfg.rgb_order = PANEL_RGB_ORDER;
      _panel_instance.config(cfg);
    }
    setPanel(&_panel_instance);
  }
};

LGFX display;

// ---- Encoder (quadratura via interrupt, come salem) ----
volatile uint8_t encState = 0;
volatile int32_t encDelta = 0;
const int8_t ENC_TABLE[16] = {0,-1,1,0, 1,0,0,-1, -1,0,0,1, 0,1,-1,0};

void IRAM_ATTR encISR() {
  uint8_t cur = (digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B);
  encState = ((encState << 2) | cur) & 0x0F;
  encDelta += ENC_TABLE[encState];
}

int value = 0;        // sale/scende con la corona
int clicks = 0;       // click contati
bool pressed = false;
bool dirty = true;

void light(bool on) {
  digitalWrite(PIN_BLK, (on == BLK_ACTIVE_HIGH) ? HIGH : LOW);
}

// ---- Test dei colori ----
void colorScreen(uint16_t bg, uint16_t fg, const char* name) {
  display.fillScreen(bg);
  display.setTextColor(fg);
  display.setTextDatum(lgfx::middle_center);
  display.setFont(&fonts::FreeSansBold18pt7b);
  display.drawString(name, 120, 120);
  Serial.printf("Schermata: %s\n", name);
  delay(1500);
}

void orientationScreen() {
  display.fillScreen(TFT_BLACK);
  display.drawCircle(120, 120, 118, TFT_WHITE);
  display.drawCircle(120, 120, 117, TFT_WHITE);
  display.fillTriangle(120, 14, 104, 44, 136, 44, TFT_YELLOW);   // freccia verso l'alto
  display.setTextColor(TFT_YELLOW);
  display.setTextDatum(lgfx::middle_center);
  display.setFont(&fonts::FreeSansBold12pt7b);
  display.drawString("ALTO", 120, 64);
  display.setTextColor(TFT_WHITE);
  display.setFont(&fonts::Font2);
  display.drawString("SINISTRA", 50, 120);
  display.drawString("DESTRA", 192, 120);
  display.drawString("BASSO", 120, 205);
  char r[24];
  snprintf(r, sizeof(r), "rotazione %d", SCREEN_ROTATION);
  display.drawString(r, 120, 150);
  Serial.println("Schermata: orientamento");
  delay(3000);
}

void colorTest() {
  colorScreen(TFT_RED,   TFT_WHITE, "ROSSO");
  colorScreen(TFT_GREEN, TFT_BLACK, "VERDE");
  colorScreen(TFT_BLUE,  TFT_WHITE, "BLU");
  colorScreen(TFT_WHITE, TFT_BLACK, "BIANCO");
  colorScreen(TFT_BLACK, TFT_WHITE, "NERO");
  orientationScreen();
  Serial.println("Retroilluminazione SPENTA");
  light(false);
  delay(1000);
  Serial.println("Retroilluminazione ACCESA");
  light(true);
  Serial.println("Ora prova la corona: gira, click, pressione lunga = ripete i colori");
  dirty = true;
}

// ---- Schermata della corona ----
void crownScreen() {
  display.fillScreen(TFT_BLACK);
  display.drawCircle(120, 120, 118, TFT_DARKGREY);
  display.setTextDatum(lgfx::middle_center);

  display.setFont(&fonts::Font2);
  display.setTextColor(TFT_LIGHTGREY);
  display.drawString("TEST CORONA", 120, 40);
  display.drawString("gira / click / tieni premuto", 120, 200);

  display.setFont(&fonts::FreeSansBold24pt7b);
  display.setTextColor(TFT_WHITE);
  char v[12];
  snprintf(v, sizeof(v), "%d", value);
  display.drawString(v, 120, 105);

  display.setFont(&fonts::FreeSansBold12pt7b);
  display.setTextColor(pressed ? TFT_GREEN : TFT_DARKGREY);
  display.drawString(pressed ? "PREMUTO" : "rilasciato", 120, 150);

  display.setFont(&fonts::Font2);
  display.setTextColor(TFT_YELLOW);
  char c[20];
  snprintf(c, sizeof(c), "click: %d", clicks);
  display.drawString(c, 120, 175);
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BLK, OUTPUT);
  light(true);

  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  pinMode(PIN_ENC_SW, INPUT_PULLUP);
  encState = (digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), encISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), encISR, CHANGE);

  delay(800);
  Serial.println("TEST SCHERMO + CORONA avviato");
  // A riposo A e B possono essere 0 o 1; il pulsante deve essere 1 (non premuto)
  Serial.printf("Pin a riposo: A(20)=%d  B(21)=%d  pulsante(10)=%d  (pulsante deve essere 1)\n",
                digitalRead(PIN_ENC_A), digitalRead(PIN_ENC_B), digitalRead(PIN_ENC_SW));

  display.init();
  display.setRotation(SCREEN_ROTATION);
  colorTest();
}

void loop() {
  // Rotazione
  static int32_t acc = 0;
  noInterrupts();
  acc += encDelta;
  encDelta = 0;
  interrupts();
  int steps = acc / STEPS_PER_DETENT;
  if (steps != 0) {
    acc -= steps * STEPS_PER_DETENT;
    if (INVERT_DIR) steps = -steps;
    value += steps;
    Serial.printf("Giro %+d  -> valore %d\n", steps, value);
    dirty = true;
  }

  // Pulsante: click breve contato, pressione lunga = ripete il test dei colori
  static bool swDown = false;
  static unsigned long swT = 0;
  bool sw = (digitalRead(PIN_ENC_SW) == LOW);
  if (sw && !swDown) {
    swDown = true;
    swT = millis();
    pressed = true;
    dirty = true;
    Serial.println("Pulsante PREMUTO");
  }
  if (!sw && swDown) {
    swDown = false;
    pressed = false;
    unsigned long held = millis() - swT;
    dirty = true;
    if (held > 1000) {
      Serial.printf("Pressione lunga (%lu ms): ripeto il test dei colori\n", held);
      colorTest();
    } else if (held > 40) {
      clicks++;
      Serial.printf("Click (%lu ms) -> click totali %d\n", held, clicks);
    }
  }

  if (dirty) {
    dirty = false;
    crownScreen();
  }
}
