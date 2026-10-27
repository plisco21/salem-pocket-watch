// STRESS TEST OROLOGIO — da caricare su ogni orologio PRIMA di salem.
// Gira da solo per DURATA_MIN minuti e alla fine mostra OK (verde) o ERRORE (rosso).
// Va lanciato A BATTERIA, come all'evento: e' il modo di scoprire i riavvii da batteria debole.
//
// A ogni ciclo prova:
//   1. Display: ridisegni a tutto schermo alla stessa velocita' SPI di salem
//   2. Retroilluminazione: spegni e riaccendi
//   3. Radio: accende, invia 10 pacchetti, spegne (picchi di corrente come negli invii veri).
//      I pacchetti sono per la "cassetta 0": le serrature li ignorano, non si apre niente.
//   4. Sonno leggero di 1 s con risveglio a timer (come salem a schermo spento)
//   5. Memoria: scrittura e rilettura in NVS (spazio "stress", separato da salem) e RAM libera
// Se la scheda si riavvia durante il test, alla riaccensione compare TEST INTERROTTO con il motivo.
// Alla fine: schermata della corona (gira = contatore, click = click) per un controllo a mano.
//
// Arduino IDE > Strumenti: ESP32C3 Dev Module, USB CDC On Boot: Enabled, Partition Scheme: Huge APP
// Display: SCL=4, SDA=3, RES=2, DC=1, CS=0, BLK=5 | Corona: A=20, B=21, SW=10 (come salem)

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <Preferences.h>
#include <esp_task_wdt.h>

// ---- Impostazioni del test ----
const unsigned long DURATA_MIN = 10;      // durata del test
const bool TEST_RADIO  = true;
const bool TEST_SONNO  = true;            // il monitor seriale si stacca durante il sonno: e' normale
const int  PACCHETTI_PER_CICLO = 10;

// ---- Stesse impostazioni di salem/config.h ----
const bool PANEL_INVERT     = true;
const bool PANEL_RGB_ORDER  = true;
const int  SCREEN_ROTATION  = 1;
const uint32_t SPI_FREQ     = 27000000;
const int  PIN_BLK          = 5;
const bool BLK_ACTIVE_HIGH  = true;
const bool INVERT_DIR       = false;
const int  STEPS_PER_DETENT = 4;
const wifi_power_t TX_POWER = WIFI_POWER_8_5dBm;

const int PIN_ENC_A  = 20;
const int PIN_ENC_B  = 21;
const int PIN_ENC_SW = 10;

// Stesso formato dei messaggi di salem, ma per la cassetta 0 (nessuna serratura risponde)
#define MAGIC 0xA5C4
struct __attribute__((packed)) Msg {
  uint16_t magic;
  uint8_t type, box, h, m, s, seq;
};
uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

struct Esito { uint32_t ok, err; };   // qui in alto: serve ai prototipi dell'IDE

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
LGFX_Sprite fb(&display);
Preferences prefs;

// Sprite creato prima di setup, come in salem (dentro setup non c'e' un blocco di RAM abbastanza grande)
struct SpriteEarly {
  bool ok;
  SpriteEarly() { fb.setColorDepth(16); ok = fb.createSprite(240, 240); }
} spriteEarly;

constexpr uint16_t RGB(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
const uint16_t COL_BLACK = RGB(0, 0, 0);
const uint16_t COL_WHITE = RGB(255, 255, 255);
const uint16_t COL_NAVY  = RGB(30, 40, 110);
const uint16_t COL_GOLD  = RGB(190, 150, 60);
const uint16_t COL_OK    = RGB(30, 150, 60);
const uint16_t COL_ERR   = RGB(200, 30, 30);
const uint16_t COL_GREY  = RGB(150, 150, 160);

const int CX = 120, CY = 120;

// ---- Risultati ----
Esito rDisplay = {0, 0}, rLuce = {0, 0}, rRadio = {0, 0}, rSonno = {0, 0}, rNvs = {0, 0};
uint32_t cicli = 0;
uint32_t heapIniziale = 0, heapMin = 0xFFFFFFFF;
float fps = 0;
unsigned long inizio = 0;
bool finito = false;
uint8_t seq = 0;

// ---- Encoder (per il controllo finale) ----
volatile uint8_t encState = 0;
volatile int32_t encDelta = 0;
const int8_t ENC_TABLE[16] = {0,-1,1,0, 1,0,0,-1, -1,0,0,1, 0,1,-1,0};

void IRAM_ATTR encISR() {
  uint8_t cur = (digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B);
  encState = ((encState << 2) | cur) & 0x0F;
  encDelta += ENC_TABLE[encState];
}

void setLight(bool on) { digitalWrite(PIN_BLK, (on == BLK_ACTIVE_HIGH) ? HIGH : LOW); }

void wdt() { esp_task_wdt_reset(); }

// =====================================================================
//  SCHERMATE
// =====================================================================
void riga(const char* nome, const Esito& e, int y) {
  char l[40];
  snprintf(l, sizeof(l), "%s %lu ok", nome, (unsigned long)e.ok);
  fb.setTextColor(e.err ? COL_ERR : COL_WHITE);
  if (e.err) snprintf(l, sizeof(l), "%s %lu ok %lu ERR", nome, (unsigned long)e.ok, (unsigned long)e.err);
  fb.drawString(l, CX, y);
}

void schermataStato(const char* titolo, uint16_t fondo) {
  fb.fillSprite(COL_BLACK);
  fb.fillCircle(CX, CY, 119, fondo);
  fb.setTextDatum(lgfx::middle_center);
  fb.setFont(&fonts::FreeSansBold12pt7b);
  fb.setTextColor(COL_WHITE);
  fb.drawString(titolo, CX, 38);
  fb.setFont(&fonts::Font2);
  char l[40];
  int y = 66;
  if (!finito) {
    unsigned long passati = (millis() - inizio) / 1000;
    unsigned long totale = DURATA_MIN * 60;
    unsigned long resto = passati < totale ? totale - passati : 0;
    snprintf(l, sizeof(l), "CICLO %lu - MANCANO %lu:%02lu", (unsigned long)cicli, resto / 60, resto % 60);
  } else {
    snprintf(l, sizeof(l), "%lu CICLI IN %lu MIN", (unsigned long)cicli, DURATA_MIN);
  }
  fb.setTextColor(COL_GOLD);
  fb.drawString(l, CX, y); y += 18;
  riga("DISPLAY", rDisplay, y); y += 16;
  riga("LUCE", rLuce, y); y += 16;
  if (TEST_RADIO) { riga("RADIO", rRadio, y); y += 16; }
  if (TEST_SONNO) { riga("SONNO", rSonno, y); y += 16; }
  riga("MEMORIA", rNvs, y); y += 16;
  fb.setTextColor(COL_WHITE);
  snprintf(l, sizeof(l), "RAM %lu KB (min %lu KB)", (unsigned long)(ESP.getFreeHeap() / 1024),
           (unsigned long)(heapMin / 1024));
  fb.drawString(l, CX, y); y += 16;
  snprintf(l, sizeof(l), "%.1f FOTOGRAMMI/S", fps);
  fb.drawString(l, CX, y);
  fb.pushSprite(0, 0);
}

// =====================================================================
//  PROVE
// =====================================================================
// 1. Display: 30 fotogrammi a tutto schermo con cerchi colorati che si muovono
void provaDisplay() {
  unsigned long t0 = millis();
  const int N = 30;
  for (int f = 0; f < N; f++) {
    wdt();
    fb.fillSprite(COL_BLACK);
    for (int i = 0; i < 12; i++) {
      float a = (f * 12 + i * 30) * DEG_TO_RAD;
      uint16_t c = RGB(128 + 127 * sinf(a), 128 + 127 * sinf(a + 2.1f), 128 + 127 * sinf(a + 4.2f));
      fb.fillCircle(CX + 80 * cosf(a), CY + 80 * sinf(a), 22, c);
    }
    fb.fillCircle(CX, CY, 40, COL_NAVY);
    fb.setFont(&fonts::FreeSansBold12pt7b);
    fb.setTextDatum(lgfx::middle_center);
    fb.setTextColor(COL_WHITE);
    char n[12];
    snprintf(n, sizeof(n), "%lu", (unsigned long)cicli);
    fb.drawString(n, CX, CY);
    fb.pushSprite(0, 0);
  }
  unsigned long dt = millis() - t0;
  fps = dt ? N * 1000.0f / dt : 0;
  // meno di 5 fotogrammi al secondo = qualcosa non va (SPI o processore bloccato)
  if (fps >= 5) rDisplay.ok++; else rDisplay.err++;
}

// 2. Retroilluminazione
void provaLuce() {
  setLight(false);
  delay(150);
  setLight(true);
  delay(50);
  if (digitalRead(PIN_BLK) == (BLK_ACTIVE_HIGH ? HIGH : LOW)) rLuce.ok++; else rLuce.err++;
}

// 3. Radio: accende, invia, spegne
void provaRadio() {
  wdt();
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_ps(WIFI_PS_NONE);
  WiFi.setTxPower(TX_POWER);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) {
    rRadio.err++;
    WiFi.mode(WIFI_OFF);
    Serial.println("RADIO: esp_now_init fallito");
    return;
  }
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BCAST, 6);
  peer.channel = 0;
  peer.encrypt = false;
  esp_now_add_peer(&peer);

  int falliti = 0;
  for (int i = 0; i < PACCHETTI_PER_CICLO; i++) {
    wdt();
    Msg m = {MAGIC, 3, 0, 0, 0, 0, ++seq};   // cassetta 0: nessuna serratura risponde
    if (esp_now_send(BCAST, (uint8_t*)&m, sizeof(m)) != ESP_OK) falliti++;
    delay(30);
  }
  esp_now_deinit();
  WiFi.mode(WIFI_OFF);
  if (falliti == 0) rRadio.ok++;
  else { rRadio.err++; Serial.printf("RADIO: %d pacchetti non partiti\n", falliti); }
}

// 4. Sonno leggero di 1 s
void provaSonno() {
  wdt();
  Serial.flush();
  unsigned long t0 = millis();
  esp_sleep_enable_timer_wakeup(1000000ULL);
  esp_light_sleep_start();
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  wdt();
  unsigned long dt = millis() - t0;
  // millis() deve continuare a contare durante il sonno (i tempi del gioco si basano su questo)
  if (dt >= 900 && dt <= 1500) rSonno.ok++;
  else { rSonno.err++; Serial.printf("SONNO: durata misurata %lu ms\n", dt); }
}

// 5. Memoria: scrittura e rilettura, e RAM libera
void provaMemoria() {
  uint32_t v = esp_random();
  prefs.putUInt("prova", v);
  bool ok = prefs.getUInt("prova", 0) == v;
  prefs.putUInt("cicli", cicli);
  uint32_t h = ESP.getFreeHeap();
  if (h < heapMin) heapMin = h;
  // Riferimento dopo il primo ciclo completo (la prima accensione della radio tiene ~20 KB per sempre, e' normale).
  // Se poi la RAM libera cala di oltre 10 KB, c'e' una perdita di memoria.
  if (cicli == 1) heapIniziale = h;
  else if (h + 10240 < heapIniziale) ok = false;
  if (ok) rNvs.ok++; else { rNvs.err++; Serial.printf("MEMORIA: errore (RAM %lu)\n", (unsigned long)h); }
}

bool tuttoOk() {
  return !rDisplay.err && !rLuce.err && !rRadio.err && !rSonno.err && !rNvs.err && spriteEarly.ok;
}

// =====================================================================
//  CONTROLLO FINALE DELLA CORONA
// =====================================================================
int32_t corona = 0, click = 0;

void schermataCorona() {
  fb.fillSprite(COL_BLACK);
  fb.fillCircle(CX, CY, 119, tuttoOk() ? COL_OK : COL_ERR);
  fb.setTextDatum(lgfx::middle_center);
  fb.setFont(&fonts::FreeSansBold12pt7b);
  fb.setTextColor(COL_WHITE);
  fb.drawString(tuttoOk() ? "TEST OK" : "ERRORE", CX, 40);
  fb.setFont(&fonts::Font2);
  fb.drawString("GIRA E PREMI LA CORONA", CX, 72);
  fb.setFont(&fonts::FreeSansBold18pt7b);
  char l[24];
  snprintf(l, sizeof(l), "%ld", (long)corona);
  fb.drawString(l, CX, 115);
  fb.setFont(&fonts::Font2);
  snprintf(l, sizeof(l), "CLICK: %ld", (long)click);
  fb.drawString(l, CX, 152);
  fb.drawString("uno scatto = +1 o -1", CX, 176);
  fb.drawString("tieni 3 s = risultati", CX, 194);
  fb.pushSprite(0, 0);
}

// =====================================================================
//  SETUP E LOOP
// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(800);

  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  pinMode(PIN_ENC_SW, INPUT_PULLUP);
  encState = (digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), encISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), encISR, CHANGE);

  display.init();
  display.setRotation(SCREEN_ROTATION);
  pinMode(PIN_BLK, OUTPUT);
  setLight(true);

  prefs.begin("stress", false);
  int motivo = (int)esp_reset_reason();
  Serial.printf("STRESS TEST - motivo dell'ultimo riavvio: %d\n", motivo);

  if (!spriteEarly.ok) {
    display.fillScreen(COL_ERR);
    display.setTextDatum(lgfx::middle_center);
    display.setTextColor(COL_WHITE);
    display.drawString("ERRORE MEMORIA SPRITE", CX, CY);
    Serial.println("ERRORE: sprite non creato");
    while (true) delay(1000);
  }

  // Il test precedente non era finito: la scheda si e' riavviata (o e' stata spenta) durante il test
  if (prefs.getBool("incorso", false)) {
    uint32_t c = prefs.getUInt("cicli", 0);
    prefs.putBool("incorso", false);
    const char* perche = motivo == 9 ? "BROWNOUT (batteria/alimentazione)"
                       : motivo == 4 ? "CRASH"
                       : (motivo >= 5 && motivo <= 7) ? "BLOCCO (watchdog)"
                       : motivo == 1 ? "SPENTO E RIACCESO"
                       : motivo == 11 ? "RICARICATO DAL PC"
                       : "RIAVVIO";
    Serial.printf("TEST INTERROTTO al ciclo %lu: %s (motivo %d)\n", (unsigned long)c, perche, motivo);
    fb.fillSprite(COL_BLACK);
    fb.fillCircle(CX, CY, 119, COL_ERR);
    fb.setTextDatum(lgfx::middle_center);
    fb.setFont(&fonts::FreeSansBold12pt7b);
    fb.setTextColor(COL_WHITE);
    fb.drawString("TEST INTERROTTO", CX, 70);
    fb.setFont(&fonts::Font2);
    char l[48];
    snprintf(l, sizeof(l), "al ciclo %lu", (unsigned long)c);
    fb.drawString(l, CX, 105);
    fb.drawString(perche, CX, 125);
    snprintf(l, sizeof(l), "motivo %d", motivo);
    fb.drawString(l, CX, 145);
    fb.drawString("click = rifai il test", CX, 185);
    fb.pushSprite(0, 0);
    while (digitalRead(PIN_ENC_SW) == HIGH) delay(20);
    while (digitalRead(PIN_ENC_SW) == LOW) delay(20);
  }

  // 15 s di attesa prima di iniziare: il sonno leggero stacca la USB e dopo non si riesce a ricaricare.
  // In questa finestra (subito dopo aver attaccato il cavo) il caricamento di salem funziona sempre.
  for (int s = 15; s > 0; s--) {
    fb.fillSprite(COL_BLACK);
    fb.fillCircle(CX, CY, 119, COL_NAVY);
    fb.setTextDatum(lgfx::middle_center);
    fb.setFont(&fonts::FreeSansBold12pt7b);
    fb.setTextColor(COL_WHITE);
    fb.drawString("STRESS TEST", CX, 80);
    fb.setFont(&fonts::Font2);
    char l[24];
    snprintf(l, sizeof(l), "INIZIO TRA %d s", s);
    fb.drawString(l, CX, 120);
    fb.drawString("(ora si puo' caricare salem)", CX, 145);
    fb.pushSprite(0, 0);
    delay(1000);
  }

  prefs.putBool("incorso", true);
  prefs.putUInt("cicli", 0);
  inizio = millis();
  Serial.printf("Inizio test: %lu minuti, RAM libera %lu\n", DURATA_MIN, (unsigned long)ESP.getFreeHeap());

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t w;
  w.timeout_ms = 5000;
  w.idle_core_mask = 0;
  w.trigger_panic = true;
  esp_task_wdt_reconfigure(&w);
#else
  esp_task_wdt_init(5, true);
#endif
  esp_task_wdt_add(NULL);
}

void loop() {
  wdt();

  if (!finito) {
    cicli++;
    provaDisplay();
    provaLuce();
    if (TEST_RADIO) provaRadio();
    schermataStato("STRESS TEST", COL_NAVY);
    if (TEST_SONNO) provaSonno();
    provaMemoria();
    Serial.printf("Ciclo %lu: display %lu/%lu luce %lu/%lu radio %lu/%lu sonno %lu/%lu memoria %lu/%lu RAM %lu fps %.1f\n",
                  (unsigned long)cicli, rDisplay.ok, rDisplay.err, rLuce.ok, rLuce.err, rRadio.ok, rRadio.err,
                  rSonno.ok, rSonno.err, rNvs.ok, rNvs.err, (unsigned long)ESP.getFreeHeap(), fps);
    schermataStato("STRESS TEST", COL_NAVY);
    delay(300);

    if (millis() - inizio >= DURATA_MIN * 60000UL) {
      finito = true;
      prefs.putBool("incorso", false);
      Serial.printf("FINE TEST: %s\n", tuttoOk() ? "OK" : "ERRORE");
    }
    return;
  }

  // Test finito: controllo a mano della corona. Tenere premuto 3 s alterna risultati / corona.
  static bool mostraRisultati = false;
  static bool giu = false;
  static unsigned long tGiu = 0;
  static bool lungo = false;
  static int32_t acc = 0;

  noInterrupts();
  acc += encDelta;
  encDelta = 0;
  interrupts();
  int passi = acc / STEPS_PER_DETENT;
  if (passi) { acc -= passi * STEPS_PER_DETENT; corona += INVERT_DIR ? -passi : passi; }

  bool sw = digitalRead(PIN_ENC_SW) == LOW;
  if (sw && !giu) { giu = true; tGiu = millis(); lungo = false; }
  if (sw && giu && !lungo && millis() - tGiu >= 3000) { lungo = true; mostraRisultati = !mostraRisultati; }
  if (!sw && giu) {
    giu = false;
    if (!lungo && millis() - tGiu > 40) click++;
  }

  static unsigned long ultimo = 0;
  if (millis() - ultimo >= 50) {
    ultimo = millis();
    if (mostraRisultati) schermataStato(tuttoOk() ? "TEST OK" : "ERRORE", tuttoOk() ? COL_OK : COL_ERR);
    else schermataCorona();
  }
}
