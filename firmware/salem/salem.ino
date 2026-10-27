// SALEM — Tamagotchi, caccia al tesoro e orologio (ESP32-C3 SuperMini + GC9A01 + EC11)
// Versione 2 (ottobre 2026).
//
// Display GC9A01: SCL=4, SDA=3, RES=2, DC=1, CS=0
// EC11: A=GPIO20, B=GPIO21, SW=GPIO10 | centrale (lato da 3 pin) e secondo pin switch -> GND
// Retroilluminazione BLK: GPIO5 (si spegne dopo 1 min senza toccare, si riaccende alla corona)
//
// Arduino IDE > Strumenti:
//   Scheda: ESP32C3 Dev Module
//   USB CDC On Boot: Enabled          (GPIO20/21 sono i pin UART di default)
//   Partition Scheme: Huge APP (3MB No OTA)   (quadrante e mappa occupano ~350 KB)
//
// Tutte le impostazioni del gioco (frasi, percorsi, password, combinazioni, tempi) sono in config.h.
// Nella cartella dello sketch servono: salem.ino, config.h, quadrante.h, mappa.h
// (autotest.h serve solo alla versione di test, compilata con -DSALEM_AUTOTEST)
//
// Collegato al PC l'orologio non va in sonno: il monitor seriale e il caricamento funzionano sempre.

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <math.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <driver/usb_serial_jtag.h>
#include <Preferences.h>
#include <esp_task_wdt.h>

// quadrante.h dichiara "byte" ma contiene valori RGB888 a 24 bit: li leggiamo come uint32_t
#define byte uint32_t
#include "quadrante.h"
#undef byte
#include "mappa.h"   // immagine della mappa, RGB565 a 16 bit

// ---- Tipi usati da config.h (non modificare) ----
enum { CIBO = 0, CAREZZA = 1, PULIZIA = 2, SPASSO = 3 };
enum { DOLCETTO = 0, SCHERZETTO = 1 };
struct Passo { uint8_t scenario; uint8_t mossa; };
const int N_SQUADRE = 8;
const int N_BUSTE   = 8;

#include "config.h"

const unsigned long MIN_MS = 60000UL;   // un minuto in ms

#ifdef SALEM_TEST_SCARICA   // prova della batteria scarica: quante volte e' scattata la protezione
RTC_DATA_ATTR uint8_t brownoutSleeps = 0;
#endif

#ifdef SALEM_AUTOTEST   // funzioni della versione di test, definite in autotest.h
bool atSimSend(int kind);
void atSimLoop();
#endif

#define LINE_LEN 40   // caratteri massimi per riga di testo (qui in alto: serve ai prototipi dell'IDE)

#ifdef SALEM_PIN_SCAMBIATI   // orologio cablato con A e pulsante invertiti (tools/upload.ps1 -pinScambiati)
const int PIN_ENC_A  = 10;
const int PIN_ENC_SW = 20;
#else
const int PIN_ENC_A  = 20;
const int PIN_ENC_SW = 10;
#endif
const int PIN_ENC_B  = 21;

// ---- Protocollo ESP-NOW (uguale a serratura.ino) ----
#define MAGIC 0xA5C4
enum { MSG_OPEN = 1, MSG_REPLY = 2, MSG_TEST = 3 };
enum { RES_WRONG = 0, RES_OPEN = 1, RES_FAR = 2 };
struct __attribute__((packed)) Msg {
  uint16_t magic;
  uint8_t type;   // MSG_OPEN / MSG_REPLY / MSG_TEST
  uint8_t box;    // 1 = cassetta bonus, 2 = cassetta orologio
  uint8_t h, m, s;
  uint8_t seq;    // numero dell'invio: i tentativi ripetuti hanno lo stesso seq
};
uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

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

// Lo sprite 240x240 vuole 115 KB in un blocco solo. Si crea qui, durante la costruzione degli
// oggetti globali (prima di setup e dei task di sistema), quando la RAM e' ancora tutta libera:
// dentro setup il blocco piu' grande e' di poco sotto i 115 KB e la creazione fallisce (schermo nero).
unsigned spriteLargestBefore = 0;
struct SpriteEarly {
  bool ok;
  SpriteEarly() {
    spriteLargestBefore = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    fb.setColorDepth(16);
    ok = fb.createSprite(240, 240);
  }
} spriteEarly;

// ---- Colori ----
constexpr uint16_t RGB(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
const uint16_t COL_OUT    = RGB(0, 0, 0);
const uint16_t COL_BG     = RGB(250, 236, 205);
const uint16_t COL_RIM    = RGB(184, 134, 60);
const uint16_t COL_TRACK  = RGB(225, 208, 175);
const uint16_t COL_ICONBG = RGB(236, 220, 188);
const uint16_t COL_CAT    = RGB(22, 22, 30);
const uint16_t COL_EYE    = RGB(190, 230, 50);
const uint16_t COL_EYE_RED = RGB(240, 60, 50);
const uint16_t COL_PINK   = RGB(245, 140, 170);
const uint16_t COL_MOUTH  = RGB(150, 40, 70);
const uint16_t COL_WHISK  = RGB(170, 170, 180);
const uint16_t COL_COLLAR = RGB(200, 40, 40);
const uint16_t COL_BOW    = RGB(255, 110, 180);   // fiocco rosa (Salem e' femmina)
const uint16_t COL_BOW_DK = RGB(200, 50, 120);
const uint16_t COL_BELL   = RGB(240, 200, 60);
const uint16_t COL_BOWL   = RGB(60, 110, 200);
const uint16_t COL_KIBBLE = RGB(150, 90, 40);
const uint16_t COL_FOOD   = RGB(240, 140, 40);
const uint16_t COL_LOVE   = RGB(235, 70, 120);
const uint16_t COL_CLEAN  = RGB(40, 170, 220);
const uint16_t COL_FUN    = RGB(70, 170, 70);
const uint16_t COL_WHITE  = RGB(255, 255, 255);
const uint16_t COL_GREY   = RGB(140, 140, 150);
// Scenari
const uint16_t COL_CANDY  = RGB(240, 110, 170);
const uint16_t COL_GHOST  = RGB(140, 90, 200);
const uint16_t COL_MAPICO = RGB(170, 120, 60);
const uint16_t COL_LETTER = RGB(200, 160, 60);
const uint16_t COL_SUNSET_TOP = RGB(110, 60, 140);
const uint16_t COL_SUNSET_MID = RGB(235, 120, 120);
const uint16_t COL_SUNSET_LOW = RGB(250, 185, 100);
const uint16_t COL_SUN    = RGB(250, 210, 60);
const uint16_t COL_DUSK_GROUND = RGB(120, 75, 85);
const uint16_t COL_NIGHT  = RGB(28, 28, 70);
const uint16_t COL_NIGHT_GROUND = RGB(45, 30, 70);
const uint16_t COL_MOON   = RGB(245, 240, 200);
const uint16_t COL_GLOW   = RGB(120, 70, 170);
const uint16_t COL_HAT    = RGB(70, 30, 100);
const uint16_t COL_HATBAND = RGB(240, 140, 40);
// Passeggiata
const uint16_t COL_SKY    = RGB(195, 228, 248);
const uint16_t COL_GRASS  = RGB(110, 185, 80);
const uint16_t COL_PATH   = RGB(215, 190, 140);
const uint16_t COL_PATHD  = RGB(185, 160, 115);
const uint16_t COL_TREE   = RGB(60, 140, 60);
const uint16_t COL_TRUNK  = RGB(120, 80, 45);
const uint16_t COL_LEASH  = RGB(170, 30, 30);
// Orologio, menu, mappa, lucchetto
const uint16_t COL_HAND   = RGB(30, 28, 35);
const uint16_t COL_SEL    = RGB(200, 30, 30);
const uint16_t COL_GOLD   = RGB(190, 150, 60);
const uint16_t COL_NAVY   = RGB(30, 40, 110);
const uint16_t COL_OK     = RGB(30, 130, 50);
const uint16_t COL_ERR    = RGB(200, 30, 30);
const uint16_t COL_NORADIO = RGB(120, 40, 140);
const uint16_t COL_PAPER  = RGB(235, 215, 170);
const uint16_t COL_INK    = RGB(90, 50, 20);

const int CX = 120, CY = 120;

// =====================================================================
//  STATO DEL GIOCO
// =====================================================================
enum { PH_FREE = 0, PH_BONUS = 1, PH_HUNT = 2, PH_CLOCK = 3, PH_WAIT = 4 };
const char* PHASE_NAMES[5] = {"GIOCO LIBERO", "FASE 1", "CACCIA", "OROLOGIO", "ATTESA"};

int phase = PH_FREE;
int waitFor = PH_BONUS;          // fase che parte alla fine dell'attesa
unsigned long waitElapsed = 0;   // ms di attesa gia' passati
int team = 0;                    // 0..7
int scenario = DOLCETTO;
int barVal[4];                   // cibo, affetto, pulizia, svago

// Fase 1
unsigned long bonusElapsed = 0;
int questIdx = 0;                // richieste gia' fatte (0..N_RICHIESTE)
bool requestActive = false;      // c'e' una richiesta in corso
unsigned long questWait = 0;     // ms dalla mossa fatta (verso la richiesta successiva)
bool bonusUnlocked = false, box1Open = false, bonusExpired = false;

// Fase 2
enum { HS_COUNT = 0, HS_LOCK = 1, HS_WAIT = 2 };
int huntState = HS_COUNT;
unsigned long huntWait = 0;      // ms di attesa dopo un ordine sbagliato
int huntCount = 0;
Passo hunt[N_BUSTE];
bool mapUnlocked = false;

// Orologio
int hours = CLOCK_START_H % 12, minutes = CLOCK_START_M, seconds = CLOCK_START_S;
bool box2Open = false;

// Salvataggio in memoria (NVS)
struct __attribute__((packed)) SaveData {
  uint8_t ver;
  uint8_t phase, waitFor, team, scenario;
  uint8_t barVal[4];
  uint32_t waitElapsed;
  uint32_t bonusElapsed;
  uint8_t questIdx, requestActive;
  uint32_t questWait;
  uint8_t bonusUnlocked, box1Open, bonusExpired;
  uint8_t huntCount, huntState;
  uint32_t huntWait;
  Passo hunt[N_BUSTE];
  uint8_t mapUnlocked;
  uint8_t h, m, s, box2Open;
};
const uint8_t SAVE_VER = 2;
bool saveDirty = false;
unsigned long lastSave = 0;

// ---- Interfaccia ----
enum { UI_MAIN, UI_MODAL, UI_MAP, UI_MENU, UI_STATUS, UI_PIN, UI_INTRO };
int ui = UI_MAIN;

enum { IT_FOOD, IT_LOVE, IT_CLEAN, IT_WALK, IT_SWEET, IT_TRICK, IT_MAP, IT_LETTER };
int items[8];
int nItems = 0;
int sel = 0;

enum { A_NONE, A_EAT, A_PET, A_CLEAN, A_WALK };
const unsigned long ANIM_MS[5] = {0, 2400, 2000, 2200, 6000};
int anim = A_NONE;
unsigned long animStart = 0, animDur = 0;

char bubbleText[32] = "";
unsigned long bubbleUntil = 0;
unsigned long blinkAt = 0, blinkUntil = 0;
unsigned long nextComplaint = 0;
unsigned long nextRequestSay = 0;
unsigned long barTimer[4];      // prossimo calo di ogni barra
unsigned long barHoldUntil[4];  // barra ferma a 100 fino a qui

// Cose da fare quando finisce l'animazione
bool pendingBonusMsg = false;
bool pendingHuntCheck = false;

// Combinazione bonus: ultimi click sulla mossa giusta
unsigned long bonusClicks[8];
int bonusClickN = 0;

// Scheda (messaggio lungo, a pagine). LINE_LEN e' definito in cima allo sketch.
const int MODAL_MAX_LINES = 24;
const int MODAL_PAGE_LINES = 6;
char modalLines[MODAL_MAX_LINES][LINE_LEN];
int modalNLines = 0, modalPage = 0;
unsigned long modalAt = 0;

// Mappa
bool mapAfterUnlock = false;

// Lucchetto della caccia
int lockDigits[3] = {0, 0, 0};
int lockPos = 0;
unsigned long lockErrUntil = 0;

// PIN del menu staff
int pinDigits[4] = {0, 0, 0, 0};
int pinPos = 0;
unsigned long pinLast = 0;

// Menu staff
int menuSel = 0;
unsigned long menuLast = 0;
unsigned long resetArmedUntil = 0;
char menuInfo[24] = "";

// Orologio
int hand = 0;
const char* statusText = "";
uint16_t statusColor = COL_NAVY;
unsigned long statusUntil = 0;
bool clockDirty = true;
unsigned long introAt = 0;
unsigned long finalAt = 0;   // cassetta 2 aperta: momento in cui e' arrivata la risposta

// Ascolto combinazioni segrete
bool listening = false;
unsigned long listenStart = 0, lastSym = 0;
char codeBuf[16];
int codeLen = 0;

// Pulsante
bool swDown = false;

// Radio
enum { REQ_NONE, REQ_BOX1, REQ_BOX2, REQ_TEST1, REQ_TEST2 };
int reqKind = REQ_NONE;
bool awaiting = false;
bool radioOn = false;
unsigned long sentAt = 0, lastTry = 0;
int tries = 0;
volatile uint8_t txSeq = 0;
volatile int replyState = -1;   // -1 nessuna, altrimenti RES_*
Msg lastSent;

// =====================================================================
//  UTILITA'
// =====================================================================
void say(const char* t, unsigned long ms) {
  strncpy(bubbleText, t, sizeof(bubbleText) - 1);
  bubbleText[sizeof(bubbleText) - 1] = 0;
  bubbleUntil = millis() + ms;
}

// ---- Retroilluminazione ----
bool lightOn = true;
unsigned long lastInput = 0;

void setLight(bool on) {
  if (on == lightOn) return;
  lightOn = on;
  if (on) {
    // Display reinizializzato (non solo svegliato): se un calo di tensione lo ha azzerato, torna comunque
    if (SLEEP_ENABLED) { display.init(); display.setRotation(SCREEN_ROTATION); }
    digitalWrite(PIN_BLK, BLK_ACTIVE_HIGH ? HIGH : LOW);
    clockDirty = true;   // ridisegna subito l'orologio alla riaccensione
  } else {
    digitalWrite(PIN_BLK, BLK_ACTIVE_HIGH ? LOW : HIGH);
    if (SLEEP_ENABLED) display.sleep();
  }
}

// Riaccende lo schermo (quando arriva un messaggio importante)
void lightWake() {
  lastInput = millis();
  setLight(true);
}

void setStatus(const char* t, uint16_t c, unsigned long ms) {
  statusText = t;
  statusColor = c;
  statusUntil = millis() + ms;
  clockDirty = true;
  lightWake();
}

int clampBar(int v) { return v < 0 ? 0 : (v > 100 ? 100 : v); }
int wrap(int v, int m) { return ((v % m) + m) % m; }

// Tempo passato da t. Usa sempre l'ora attuale: se t e' stato registrato dopo l'inizio del giro
// di loop, "now - t" sarebbe negativo e, senza segno, diventerebbe un numero enorme.
unsigned long since(unsigned long t) { return millis() - t; }
void markSave() { saveDirty = true; }

// Posizione che scorre verso sinistra e ricomincia da destra
float scrollX(float base, float speed, float t, float period) {
  float x = fmodf(base - speed * t, period);
  if (x < 0) x += period;
  return x - 40;
}

// Divide un testo in righe larghe al massimo maxW pixel (col font attuale di fb)
int wrapText(const char* txt, int maxW, char out[][LINE_LEN], int maxLines) {
  int n = 0;
  char line[LINE_LEN] = "";
  const char* p = txt;
  while (*p && n < maxLines) {
    while (*p == ' ') p++;
    char word[LINE_LEN];
    int k = 0;
    while (*p && *p != ' ' && k < LINE_LEN - 1) word[k++] = *p++;
    word[k] = 0;
    if (!k) break;
    char test[2 * LINE_LEN + 2];
    snprintf(test, sizeof(test), "%s%s%s", line, line[0] ? " " : "", word);
    if (line[0] && (fb.textWidth(test) > maxW || strlen(test) >= (size_t)LINE_LEN)) {
      strcpy(out[n++], line);
      strcpy(line, word);
    } else {
      strncpy(line, test, LINE_LEN - 1);
      line[LINE_LEN - 1] = 0;
    }
  }
  if (line[0] && n < maxLines) strcpy(out[n++], line);
  return n;
}

// ---- Scheda a pagine ----
void showModal(const char* t) {
  if (!t || !t[0]) return;
  lightWake();
  fb.setFont(&fonts::Font2);
  modalNLines = wrapText(t, 168, modalLines, MODAL_MAX_LINES);
  modalPage = 0;
  ui = UI_MODAL;
  modalAt = millis();
  Serial.printf("Scheda: %s\n", t);
}

int modalPages() { return (modalNLines + MODAL_PAGE_LINES - 1) / MODAL_PAGE_LINES; }

void modalNext() {
  if (modalPage + 1 < modalPages()) {
    modalPage++;
    modalAt = millis();
  } else {
    ui = UI_MAIN;
    Serial.println("Scheda chiusa");
  }
}

// =====================================================================
//  SALVATAGGIO
// =====================================================================
void saveNow() {
  SaveData d;
  d.ver = SAVE_VER;
  d.phase = phase; d.waitFor = waitFor; d.team = team; d.scenario = scenario;
  for (int i = 0; i < 4; i++) d.barVal[i] = barVal[i];
  d.waitElapsed = waitElapsed;
  d.bonusElapsed = bonusElapsed;
  d.questIdx = questIdx; d.requestActive = requestActive;
  d.questWait = questWait;
  d.bonusUnlocked = bonusUnlocked; d.box1Open = box1Open; d.bonusExpired = bonusExpired;
  d.huntCount = huntCount; d.huntState = huntState;
  d.huntWait = huntWait;
  memcpy(d.hunt, hunt, sizeof(hunt));
  d.mapUnlocked = mapUnlocked;
  d.h = hours; d.m = minutes; d.s = seconds; d.box2Open = box2Open;
  prefs.putBytes("s", &d, sizeof(d));
  saveDirty = false;
  lastSave = millis();
}

void resetBarTimers() {
  unsigned long now = millis();
  for (int i = 0; i < 4; i++) {
    barTimer[i] = now + BAR_DECAY_SEC[i] * 1000UL;
    barHoldUntil[i] = barVal[i] >= 100 ? now + FULL_HOLD_SEC * 1000UL : 0;
  }
}

void resetAll() {
  phase = PH_FREE;
  waitFor = PH_BONUS; waitElapsed = 0;
  scenario = START_SCENARIO;
  for (int i = 0; i < 4; i++) barVal[i] = BAR_START[i];
  resetBarTimers();
  bonusElapsed = 0;
  questIdx = 0; requestActive = false; questWait = 0;
  bonusUnlocked = box1Open = bonusExpired = false;
  huntCount = 0; huntState = HS_COUNT; huntWait = 0;
  mapUnlocked = false;
  hours = CLOCK_START_H % 12; minutes = CLOCK_START_M; seconds = CLOCK_START_S;
  box2Open = false;
  anim = A_NONE;
  pendingBonusMsg = pendingHuntCheck = false;
}

void loadState() {
  SaveData d;
  resetAll();
  if (prefs.getBytesLength("s") != sizeof(d)) return;
  prefs.getBytes("s", &d, sizeof(d));
  if (d.ver != SAVE_VER) return;
  phase = d.phase <= PH_WAIT ? d.phase : PH_FREE;
  waitFor = (d.waitFor == PH_HUNT) ? PH_HUNT : PH_BONUS;
  team = d.team < N_SQUADRE ? d.team : 0;
  scenario = d.scenario ? SCHERZETTO : DOLCETTO;
  for (int i = 0; i < 4; i++) barVal[i] = clampBar(d.barVal[i]);
  waitElapsed = d.waitElapsed;
  bonusElapsed = d.bonusElapsed;
  questIdx = d.questIdx <= N_RICHIESTE ? d.questIdx : 0;
  requestActive = d.requestActive && questIdx < N_RICHIESTE;
  questWait = d.questWait;
  bonusUnlocked = d.bonusUnlocked; box1Open = d.box1Open; bonusExpired = d.bonusExpired;
  huntCount = d.huntCount <= N_BUSTE ? d.huntCount : 0;
  huntState = d.huntState <= HS_WAIT ? d.huntState : HS_COUNT;
  huntWait = d.huntWait;
  memcpy(hunt, d.hunt, sizeof(hunt));
  mapUnlocked = d.mapUnlocked;
  hours = d.h % 12; minutes = d.m % 60; seconds = d.s % 60;
  box2Open = d.box2Open;
  resetBarTimers();
}

// =====================================================================
//  FASI
// =====================================================================
void buildItems() {
  nItems = 0;
  if (phase == PH_HUNT) {
    items[nItems++] = IT_SWEET;
    items[nItems++] = IT_TRICK;
  }
  items[nItems++] = IT_FOOD;
  items[nItems++] = IT_LOVE;
  items[nItems++] = IT_CLEAN;
  items[nItems++] = IT_WALK;
  if (phase == PH_FREE && mapUnlocked) items[nItems++] = IT_MAP;
  if (phase == PH_BONUS && bonusUnlocked && !box1Open) items[nItems++] = IT_LETTER;
  if (sel >= nItems) sel = 0;
}

void setPhase(int p) {
  phase = p;
  anim = A_NONE;
  pendingBonusMsg = pendingHuntCheck = false;
  if (p == PH_CLOCK) { hand = 0; clockDirty = true; }
  if (p == PH_FREE || p == PH_HUNT) resetBarTimers();   // da qui le barre ricominciano a scendere
  buildItems();
  markSave();
  Serial.printf("-> fase %s\n", PHASE_NAMES[p]);
}

// Richiesta della fase 1: schermo acceso, fumetto, icona che lampeggia
void sayRequest() {
  say(TXT_REQUEST[QUEST_ORDER[questIdx]], 4000);
  nextRequestSay = millis() + REQUEST_REPEAT_SEC * 1000UL;
}

void activateRequest() {
  requestActive = true;
  lightWake();
  sayRequest();
  markSave();
  Serial.printf("Richiesta %d/%d: mossa %d\n", questIdx + 1, N_RICHIESTE, QUEST_ORDER[questIdx]);
}

void startBonus() {
  for (int i = 0; i < 4; i++) barVal[i] = 0;   // barre vuote all'avvio
  bonusElapsed = 0;
  questIdx = 0; questWait = 0; requestActive = false;
  bonusUnlocked = box1Open = bonusExpired = false;
  bonusClickN = 0;
  setPhase(PH_BONUS);
  activateRequest();   // la prima richiesta arriva subito
}

void startHunt() {
  huntCount = 0;
  huntState = HS_COUNT;
  huntWait = 0;
  scenario = START_SCENARIO;
  setPhase(PH_HUNT);
  lightWake();
}

// Dal menu staff: la fase parte dopo START_DELAY_MIN (Salem dorme)
void schedulePhase(int p) {
  waitFor = p;
  waitElapsed = 0;
  setPhase(PH_WAIT);
}

void startPhaseNow(int p) {
  if (p == PH_HUNT) startHunt();
  else startBonus();
}

// =====================================================================
//  RADIO
// =====================================================================
#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
#else
void onRecv(const uint8_t *mac, const uint8_t *data, int len) {
#endif
  if (len != sizeof(Msg)) return;
  Msg m;
  memcpy(&m, data, sizeof(Msg));
  if (m.magic != MAGIC || m.type != MSG_REPLY || m.seq != txSeq) return;   // solo la risposta al nostro invio
  replyState = m.h;
}

bool radioStart() {
  if (radioOn) return true;
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_ps(WIFI_PS_NONE);                    // radio sempre sveglia: col risparmio perde pacchetti
  WiFi.setTxPower(TX_POWER);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);   // stesso canale delle serrature
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW: errore di avvio");
    return false;
  }
  esp_now_register_recv_cb(onRecv);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BCAST, 6);
  peer.channel = 0;
  peer.encrypt = false;
  esp_now_add_peer(&peer);
  radioOn = true;
  return true;
}

void radioStop() {
  if (!radioOn) return;
  esp_now_deinit();
  WiFi.mode(WIFI_OFF);
  radioOn = false;
}

void sendTry() {
  esp_err_t e = esp_now_send(BCAST, (uint8_t*)&lastSent, sizeof(lastSent));
  tries++;
  lastTry = millis();
  Serial.printf("Invio cassetta %d tipo %d orario %d:%02d:%02d (seq %d, tentativo %d) -> %s\n",
                lastSent.box, lastSent.type, lastSent.h, lastSent.m, lastSent.s,
                lastSent.seq, tries, e == ESP_OK ? "ok" : "ERRORE");
}

void onNoReply();


void sendRequest(int kind) {
  reqKind = kind;
  if (kind == REQ_BOX2) setStatus("INVIO...", COL_NAVY, 5000);
#ifdef SALEM_AUTOTEST
  if (atSimSend(kind)) return;   // radio simulata durante l'autotest
#endif
  if (!radioStart()) { onNoReply(); return; }
  txSeq++;
  replyState = -1;
  lastSent.magic = MAGIC;
  lastSent.type = (kind == REQ_TEST1 || kind == REQ_TEST2) ? MSG_TEST : MSG_OPEN;
  lastSent.box = (kind == REQ_BOX1 || kind == REQ_TEST1) ? 1 : 2;
  lastSent.h = hours;
  lastSent.m = minutes;
  lastSent.s = seconds;
  lastSent.seq = txSeq;
  tries = 0;
  sendTry();
  awaiting = true;
  sentAt = millis();
}

void onReply(int res) {
  Serial.printf("Risposta cassetta: %d\n", res);
  if (reqKind == REQ_BOX1) {
    if (res == RES_OPEN) {
      box1Open = true;
      setPhase(PH_FREE);
      saveNow();   // subito: se il colpo del solenoide disturba l'orologio, al riavvio la cassetta risulta aperta
      showModal(TXT_QUEST_OK);
    } else if (res == RES_FAR) {
      say(TXT_GO_CLOSER, 2000);
    }
  } else if (reqKind == REQ_BOX2) {
    if (res == RES_OPEN) {
      box2Open = true;
      saveNow();
      setStatus("APERTO", COL_OK, FINAL_DELAY_MS + 500);
      finalAt = millis();   // tra poco: ritorno al gioco libero
    } else {
      setStatus("ERRATO", COL_ERR, 2500);
    }
  } else {
    snprintf(menuInfo, sizeof(menuInfo), "%s: %s", reqKind == REQ_TEST1 ? "SERR.1" : "SERR.2",
             res == RES_OPEN ? "APERTA" : "RISPOSTA");
  }
}

void onNoReply() {
  if (reqKind == REQ_BOX1) say(TXT_NO_RADIO, 2000);
  else if (reqKind == REQ_BOX2) setStatus("NO RADIO", COL_NORADIO, 2500);
  else snprintf(menuInfo, sizeof(menuInfo), "%s: NO RADIO", reqKind == REQ_TEST1 ? "SERR.1" : "SERR.2");
}

void radioLoop() {
  if (!awaiting) return;
#ifdef SALEM_AUTOTEST
  atSimLoop();
#endif
  if (replyState >= 0) {
    awaiting = false;
    int r = replyState;
    replyState = -1;
    if (RADIO_ON_DEMAND) radioStop();
    onReply(r);
    return;
  }
  if (tries < SEND_TRIES && since(lastTry) >= RETRY_MS) sendTry();
  if (since(sentAt) > REPLY_TIMEOUT_MS) {
    awaiting = false;
    if (RADIO_ON_DEMAND) radioStop();
    onNoReply();
  }
}

// =====================================================================
//  ENCODER (quadratura via interrupt)
// =====================================================================
volatile uint8_t encState = 0;
volatile int32_t encDelta = 0;
const int8_t ENC_TABLE[16] = {0,-1,1,0, 1,0,0,-1, -1,0,0,1, 0,1,-1,0};

void IRAM_ATTR encISR() {
  uint8_t cur = (digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B);
  encState = ((encState << 2) | cur) & 0x0F;
  encDelta += ENC_TABLE[encState];
}

void attachEncoder() {
  encState = (digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B);
  encDelta = 0;
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), encISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), encISR, CHANGE);
}

// =====================================================================
//  AZIONI DEL GATTO
// =====================================================================
const int ANIM_OF[4] = {A_EAT, A_PET, A_CLEAN, A_WALK};

void startAnim(int a) {
  anim = a;
  animStart = millis();
  animDur = ANIM_MS[a];
}

void raiseBar(int i, int amount) {
  barVal[i] = clampBar(barVal[i] + amount);
  if (barVal[i] >= 100) barHoldUntil[i] = millis() + FULL_HOLD_SEC * 1000UL;
  barTimer[i] = millis() + BAR_DECAY_SEC[i] * 1000UL;
}

void sayAction(int act) {
  say(TXT_ACTION[act], act == SPASSO ? 1500 : ANIM_MS[ANIM_OF[act]]);
}

// Mossa normale (gioco libero, caccia, fase 1 dopo le 16 richieste)
void doMove(int act) {
  raiseBar(act, BAR_INC[act]);
  if (act == SPASSO && phase != PH_BONUS) {
    barVal[PULIZIA] = clampBar(barVal[PULIZIA] - WALK_CLEAN_COST);
    barVal[CIBO]    = clampBar(barVal[CIBO] - WALK_FOOD_COST);
  }
  startAnim(ANIM_OF[act]);
  sayAction(act);

  // Fase 2: ogni mossa conta, con lo scenario attivo
  if (phase == PH_HUNT && huntState == HS_COUNT && huntCount < N_BUSTE) {
    hunt[huntCount].scenario = scenario;
    hunt[huntCount].mossa = act;
    huntCount++;
    Serial.printf("Caccia: mossa %d/%d (scenario %d, mossa %d)\n", huntCount, N_BUSTE, scenario, act);
    if (huntCount >= N_BUSTE) pendingHuntCheck = true;
  }
  markSave();
}

// Fase 1: la mossa richiesta riempie 1/4 di barra
void questMove(int act) {
  barVal[act] = clampBar(barVal[act] + QUEST_STEP);
  startAnim(ANIM_OF[act]);
  sayAction(act);
  questIdx++;
  requestActive = false;
  questWait = 0;
  Serial.printf("Richiesta fatta: %d/%d\n", questIdx, N_RICHIESTE);
  if (questIdx >= N_RICHIESTE && !bonusExpired) {
    for (int i = 0; i < 4; i++) barVal[i] = 100;
    bonusUnlocked = true;
    pendingBonusMsg = true;
    buildItems();
    Serial.println("BONUS SBLOCCATO");
  }
  markSave();
}

// Combinazione della cassetta 1: N click sulla mossa giusta entro la finestra
void bonusComboClick(unsigned long now) {
  if (bonusClickN < BONUS_CLICKS) bonusClicks[bonusClickN++] = now;
  else {
    for (int i = 1; i < BONUS_CLICKS; i++) bonusClicks[i - 1] = bonusClicks[i];
    bonusClicks[BONUS_CLICKS - 1] = now;
  }
  if (bonusClickN >= BONUS_CLICKS && now - bonusClicks[0] <= BONUS_WINDOW_MS) {
    bonusClickN = 0;
    if (!awaiting) sendRequest(REQ_BOX1);
  }
}

void checkHunt() {
  const Passo* p = PERCORSI[team];
  bool ok = true;
  for (int i = 0; i < N_BUSTE; i++) {
    if (hunt[i].scenario != p[i].scenario || hunt[i].mossa != p[i].mossa) { ok = false; break; }
  }
  Serial.printf("Caccia completata: %s\n", ok ? "ORDINE GIUSTO -> lucchetto" : "sbagliato, attesa e si riparte");
  huntCount = 0;
  if (ok) {
    huntState = HS_LOCK;
    lockPos = 0;
    lockDigits[0] = lockDigits[1] = lockDigits[2] = 0;
    lockErrUntil = 0;
    lightWake();
  } else {
    huntState = HS_WAIT;   // nessun messaggio: si riparte da 0/8 dopo l'attesa
    huntWait = 0;
  }
  markSave();
}

void openMap(bool afterUnlock) {
  mapAfterUnlock = afterUnlock;
  ui = UI_MAP;
  lightWake();
}

void closeMap() {
  ui = UI_MAIN;
  if (mapAfterUnlock) showModal(TXT_MAP_OK);   // non fa niente se il testo e' vuoto
  mapAfterUnlock = false;
}

// Lucchetto: click = cifra successiva, all'ultima controlla la password
void lockClick(unsigned long now) {
  if (now < lockErrUntil) return;
  if (++lockPos < 3) return;
  char typed[4];
  snprintf(typed, sizeof(typed), "%d%d%d", lockDigits[0], lockDigits[1], lockDigits[2]);
  Serial.printf("Password: %s\n", typed);
  if (strcmp(typed, PASSWORD[team]) == 0) {
    mapUnlocked = true;
    huntState = HS_COUNT;
    setPhase(PH_FREE);
    saveNow();
    openMap(true);
  } else {
    lockErrUntil = now + 1200;   // cifre rosse, poi si riprova (le 8 mosse restano valide)
    lockPos = 2;
  }
}

void onCatClick(unsigned long now) {
  int it = items[sel];
  if (it == IT_MAP) { openMap(false); return; }
  if (it == IT_LETTER) { showModal(TXT_BONUS_MSG); return; }
  if (anim != A_NONE) {
    // durante l'animazione i click non contano, tranne quelli della combinazione bonus
    if (phase == PH_BONUS && bonusUnlocked && !box1Open && it == (int)BONUS_MOVE) bonusComboClick(now);
    return;
  }
  if (it == IT_SWEET || it == IT_TRICK) {
    scenario = (it == IT_SWEET) ? DOLCETTO : SCHERZETTO;
    say(scenario == DOLCETTO ? TXT_SWEET : TXT_TRICK, 1200);
    markSave();
    return;
  }
  int act = it;   // IT_FOOD..IT_WALK coincidono con CIBO..SPASSO

  if (phase == PH_BONUS && !bonusUnlocked) {
    if (requestActive && act == QUEST_ORDER[questIdx]) questMove(act);
    else say(TXT_NOT_NOW, 1800);   // mossa non richiesta: non riempie
    return;
  }
  if (phase == PH_HUNT && huntState == HS_WAIT) {
    say(TXT_NOT_NOW, 1800);        // attesa dopo un ordine sbagliato: le mosse non contano
    return;
  }
  if (phase == PH_BONUS && bonusUnlocked && !box1Open && act == (int)BONUS_MOVE) bonusComboClick(now);
  doMove(act);
}

// Fine animazione: messaggi e controlli rimandati
void onAnimEnd() {
  if (anim == A_WALK) say(TXT_WALK_BACK, 1800);
  anim = A_NONE;
  if (pendingBonusMsg) {
    pendingBonusMsg = false;
    showModal(TXT_BONUS_MSG);
  }
  if (pendingHuntCheck) {
    pendingHuntCheck = false;
    checkHunt();
  }
}

// =====================================================================
//  TEMPI DEL GIOCO (contano solo da acceso; il sonno leggero conta come acceso)
// =====================================================================
void updateBars(unsigned long now) {
  if (phase != PH_FREE && phase != PH_HUNT) return;   // fase 1: barre ferme
  for (int i = 0; i < 4; i++) {
    if ((long)(now - barTimer[i]) < 0) continue;
    barTimer[i] = now + BAR_DECAY_SEC[i] * 1000UL;
    if (barVal[i] >= 100 && (long)(now - barHoldUntil[i]) < 0) continue;   // ferma a 100 per un po'
    if (barVal[i] > 0) { barVal[i]--; markSave(); }
  }
}

void updateTimers(unsigned long dt) {
  if (phase == PH_WAIT) {
    waitElapsed += dt;
    if (waitElapsed >= START_DELAY_MIN * MIN_MS) {
      Serial.println("Fine attesa: la fase parte");
      startPhaseNow(waitFor);
    }
    return;
  }
  if (phase == PH_BONUS && !bonusExpired) {
    bonusElapsed += dt;
    if (bonusElapsed >= BONUS_DURATION_MIN * MIN_MS) {
      bonusExpired = true;
      requestActive = false;
      Serial.println("Bonus scaduto");
      setPhase(PH_FREE);   // in silenzio: da qui e' solo un gattino
      return;
    }
    if (!requestActive && questIdx < N_RICHIESTE) {
      questWait += dt;
      if (questWait >= QUEST_GAP_MIN * MIN_MS) activateRequest();
    }
    return;
  }
  if (phase == PH_HUNT && huntState == HS_WAIT) {
    huntWait += dt;
    if (huntWait >= HUNT_ERROR_WAIT_MIN * MIN_MS) {
      huntState = HS_COUNT;
      huntWait = 0;
      markSave();
      Serial.println("Caccia: fine attesa, si riparte da 0/8");
    }
  }
}

void complaints(unsigned long now) {
  if (ui != UI_MAIN || anim != A_NONE) return;
  // Fase 1: il fumetto della richiesta si ripete finche' non la fanno
  if (phase == PH_BONUS) {
    if (requestActive && lightOn && now >= nextRequestSay && now >= bubbleUntil) sayRequest();
    return;
  }
  if (phase != PH_FREE) return;
  if (now < nextComplaint) return;
  nextComplaint = now + COMPLAINT_SEC * 1000UL;
  if (now < bubbleUntil) return;
  for (int i = 0; i < 4; i++) {
    if (barVal[i] < LOW_THRESHOLD) { say(TXT_LOW[i], 2500); return; }
  }
  if (random(3) == 0) say(TXT_HELLO, 1500);
}

// =====================================================================
//  DISEGNO: ELEMENTI COMUNI
// =====================================================================
void drawHeart(int x, int y, int s, uint16_t c) {
  fb.fillCircle(x - s / 2, y, s / 2 + 1, c);
  fb.fillCircle(x + s / 2, y, s / 2 + 1, c);
  fb.fillTriangle(x - s - 1, y + 1, x + s + 1, y + 1, x, y + s + 3, c);
}

// Fiocco: due ali e il nodo al centro (s = mezza larghezza)
void drawBow(int x, int y, int s) {
  int h = s * 3 / 4;
  fb.fillTriangle(x, y, x - s, y - h, x - s, y + h, COL_BOW);
  fb.fillTriangle(x, y, x + s, y - h, x + s, y + h, COL_BOW);
  fb.drawLine(x - s, y - h, x - s, y + h, COL_BOW_DK);
  fb.drawLine(x + s, y - h, x + s, y + h, COL_BOW_DK);
  fb.fillCircle(x, y, s / 3 + 1, COL_BOW_DK);
}

void drawPaw(int x, int y, uint16_t c) {
  fb.fillEllipse(x, y + 3, 5, 4, c);
  fb.fillCircle(x - 5, y - 3, 2, c);
  fb.fillCircle(x,     y - 6, 2, c);
  fb.fillCircle(x + 5, y - 3, 2, c);
}

void drawBars(unsigned long now) {
  const float A0[4] = {200, 236, 272, 308};
  const float SPAN = 32;
  const uint16_t COLS[4] = {COL_FOOD, COL_LOVE, COL_CLEAN, COL_FUN};
  if (phase == PH_HUNT) return;   // nella caccia le barre non servono
  bool blinkOff = (now / 400) % 2 && phase != PH_BONUS;   // in fase 1 le barre basse non lampeggiano
  for (int i = 0; i < 4; i++) {
    fb.fillArc(CX, CY, 116, 108, A0[i], A0[i] + SPAN, COL_TRACK);
    if (barVal[i] <= 0 || (barVal[i] < 25 && blinkOff)) continue;
    fb.fillArc(CX, CY, 116, 108, A0[i], A0[i] + SPAN * barVal[i] / 100.0f, COLS[i]);
  }
}

uint16_t itemColor(int it) {
  switch (it) {
    case IT_FOOD: return COL_FOOD;
    case IT_LOVE: return COL_LOVE;
    case IT_CLEAN: return COL_CLEAN;
    case IT_WALK: return COL_FUN;
    case IT_SWEET: return COL_CANDY;
    case IT_TRICK: return COL_GHOST;
    case IT_MAP: return COL_MAPICO;
    default: return COL_LETTER;
  }
}

void itemPos(int i, int &x, int &y, int &r) {
  int spacing = nItems <= 4 ? 32 : (nItems == 5 ? 29 : 26);
  x = CX - (nItems - 1) * spacing / 2 + i * spacing;
  y = nItems <= 4 ? 210 : 207;
  r = nItems <= 4 ? 12 : 11;
}

void drawIconGlyph(int it, int x, int y) {
  switch (it) {
    case IT_FOOD:
      fb.fillEllipse(x, y + 1, 6, 4, COL_KIBBLE);
      fb.fillRoundRect(x - 8, y + 1, 16, 6, 3, COL_BOWL);
      break;
    case IT_LOVE:
      drawHeart(x, y - 2, 5, COL_LOVE);
      break;
    case IT_CLEAN:
      fb.fillCircle(x - 2, y + 2, 5, COL_CLEAN);
      fb.fillCircle(x - 4, y, 1, COL_WHITE);
      fb.drawCircle(x + 5, y - 5, 3, COL_CLEAN);
      break;
    case IT_WALK:
      drawPaw(x, y, COL_FUN);
      break;
    case IT_SWEET:   // caramella incartata
      fb.fillTriangle(x - 9, y - 4, x - 9, y + 4, x - 4, y, COL_CANDY);
      fb.fillTriangle(x + 9, y - 4, x + 9, y + 4, x + 4, y, COL_CANDY);
      fb.fillEllipse(x, y, 5, 4, COL_CANDY);
      fb.drawLine(x - 2, y - 3, x + 2, y + 3, COL_WHITE);
      break;
    case IT_TRICK:   // fantasmino
      fb.fillCircle(x, y - 2, 6, COL_GHOST);
      fb.fillRect(x - 6, y - 2, 13, 8, COL_GHOST);
      fb.fillCircle(x - 2, y - 3, 1, COL_WHITE);
      fb.fillCircle(x + 2, y - 3, 1, COL_WHITE);
      break;
    case IT_MAP:     // foglio con percorso
      fb.fillRect(x - 7, y - 6, 14, 12, COL_PAPER);
      fb.drawLine(x - 5, y + 3, x - 1, y - 2, COL_INK);
      fb.drawLine(x - 1, y - 2, x + 4, y + 2, COL_INK);
      fb.drawLine(x + 2, y - 4, x + 5, y - 1, COL_ERR);
      fb.drawLine(x + 5, y - 4, x + 2, y - 1, COL_ERR);
      break;
    case IT_LETTER:  // busta
      fb.fillRect(x - 7, y - 5, 14, 10, COL_WHITE);
      fb.drawRect(x - 7, y - 5, 14, 10, COL_LETTER);
      fb.drawLine(x - 7, y - 5, x, y + 1, COL_LETTER);
      fb.drawLine(x + 7, y - 5, x, y + 1, COL_LETTER);
      break;
  }
}

void drawIcons(unsigned long now) {
  // Fase 1: l'icona della mossa richiesta lampeggia
  int wanted = (phase == PH_BONUS && requestActive) ? QUEST_ORDER[questIdx] : -1;
  bool flash = (now / 300) % 2;
  for (int i = 0; i < nItems; i++) {
    int x, y, r;
    itemPos(i, x, y, r);
    int it = items[i];
    bool active = (it == IT_SWEET && scenario == DOLCETTO) || (it == IT_TRICK && scenario == SCHERZETTO);
    uint16_t bg = active ? COL_WHITE : COL_ICONBG;
    if (it == wanted && flash) {
      fb.fillCircle(x, y, r + 3, itemColor(it));
      fb.fillCircle(x, y, r - 1, COL_WHITE);
    } else if (i == sel) {
      fb.fillCircle(x, y, r + 3, itemColor(it));
      fb.fillCircle(x, y, r, active ? COL_WHITE : COL_BG);
    } else {
      fb.fillCircle(x, y, r, bg);
      if (active) fb.drawCircle(x, y, r, itemColor(it));
    }
    drawIconGlyph(it, x, y);
  }
}

// Fumetto: una riga, oppure due se il testo e' lungo
void drawSpeech() {
  fb.setFont(&fonts::Font2);
  fb.setTextColor(COL_CAT);
  fb.setTextDatum(lgfx::middle_center);
  if (fb.textWidth(bubbleText) <= 104) {
    fb.fillRoundRect(64, 26, 112, 26, 10, COL_WHITE);
    fb.drawRoundRect(64, 26, 112, 26, 10, COL_CAT);
    fb.fillTriangle(114, 51, 126, 51, 120, 58, COL_WHITE);
    fb.drawLine(114, 51, 120, 58, COL_CAT);
    fb.drawLine(126, 51, 120, 58, COL_CAT);
    fb.drawString(bubbleText, 120, 39);
    return;
  }
  char l[2][LINE_LEN];
  int n = wrapText(bubbleText, 132, l, 2);
  fb.fillRoundRect(48, 22, 144, 42, 12, COL_WHITE);
  fb.drawRoundRect(48, 22, 144, 42, 12, COL_CAT);
  fb.fillTriangle(114, 63, 126, 63, 120, 70, COL_WHITE);
  fb.drawLine(114, 63, 120, 70, COL_CAT);
  fb.drawLine(126, 63, 120, 70, COL_CAT);
  if (n >= 1) fb.drawString(l[0], 120, n == 1 ? 43 : 34);
  if (n >= 2) fb.drawString(l[1], 120, 52);
}

// Riquadro piccolo a sinistra: contatore caccia o tempo del bonus
void drawSideBox(const char* t) {
  fb.fillRoundRect(10, 108, 44, 24, 8, COL_WHITE);
  fb.drawRoundRect(10, 108, 44, 24, 8, COL_CAT);
  fb.setFont(&fonts::Font2);
  fb.setTextColor(COL_CAT);
  fb.setTextDatum(lgfx::middle_center);
  fb.drawString(t, 32, 120);
}

// Zampina: l'orologio sta ascoltando una combinazione
void drawListenPaw(int x, int y) {
  fb.fillCircle(x, y, 13, COL_WHITE);
  fb.drawCircle(x, y, 13, COL_CAT);
  drawPaw(x, y, COL_CAT);
  for (int i = 0; i < codeLen && i < 10; i++) {
    int dx = x - 18 + i * 4;
    if (codeBuf[i] == '.') fb.fillCircle(dx, y + 18, 1, COL_CAT);
    else fb.drawFastHLine(dx - 1, y + 18, 3, COL_CAT);
  }
}

void drawModal() {
  fb.fillRoundRect(26, 52, 188, 136, 14, COL_WHITE);
  fb.drawRoundRect(26, 52, 188, 136, 14, COL_CAT);
  fb.setFont(&fonts::Font2);
  fb.setTextColor(COL_CAT);
  fb.setTextDatum(lgfx::middle_center);
  // Righe della pagina centrate in verticale nello spazio sopra "click" (y 58..166)
  int first = modalPage * MODAL_PAGE_LINES;
  int n = modalNLines - first;
  if (n > MODAL_PAGE_LINES) n = MODAL_PAGE_LINES;
  int y0 = 112 - (n - 1) * 17 / 2;
  for (int i = 0; i < n; i++) fb.drawString(modalLines[first + i], CX, y0 + i * 17);
  fb.setTextColor(COL_GREY);
  fb.setTextDatum(lgfx::middle_center);
  int pages = modalPages();
  if (pages > 1) {
    char f[16];
    snprintf(f, sizeof(f), "%d/%d  click", modalPage + 1, pages);
    fb.drawString(f, CX, 176);
  } else {
    fb.drawString("click", CX, 176);
  }
}

// =====================================================================
//  DISEGNO: SFONDI DEGLI SCENARI
// =====================================================================
enum { TH_NORMAL, TH_SWEET, TH_TRICK };

int currentTheme() {
  if (phase != PH_HUNT) return TH_NORMAL;
  return scenario == DOLCETTO ? TH_SWEET : TH_TRICK;
}

uint16_t lerpCol(uint16_t a, uint16_t b, float t) {
  int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
  int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
  int r = ar + (br - ar) * t, g = ag + (bg - ag) * t, bl = ab + (bb - ab) * t;
  return (r << 11) | (g << 5) | bl;
}

void drawCupcake(int x, int y) {
  fb.fillTriangle(x - 8, y, x + 8, y, x + 5, y + 10, COL_HATBAND);
  fb.fillTriangle(x - 8, y, x - 5, y + 10, x + 5, y + 10, COL_HATBAND);
  fb.fillCircle(x - 4, y - 2, 5, COL_PINK);
  fb.fillCircle(x + 4, y - 2, 5, COL_PINK);
  fb.fillCircle(x, y - 6, 5, COL_PINK);
  fb.fillCircle(x, y - 12, 2, COL_ERR);
}

void drawCandy(int x, int y, uint16_t c) {
  fb.fillTriangle(x - 9, y - 4, x - 9, y + 4, x - 4, y, c);
  fb.fillTriangle(x + 9, y - 4, x + 9, y + 4, x + 4, y, c);
  fb.fillEllipse(x, y, 6, 5, c);
  fb.drawLine(x - 3, y - 4, x + 3, y + 4, COL_WHITE);
}

void drawBat(int x, int y, float t) {
  int f = (int)(4 * sinf(t * 12));
  fb.fillCircle(x, y, 3, COL_CAT);
  fb.fillTriangle(x, y, x - 12, y - 4 + f, x - 6, y + 3, COL_CAT);
  fb.fillTriangle(x, y, x + 12, y - 4 + f, x + 6, y + 3, COL_CAT);
}

void drawBackground(int th, float t) {
  fb.fillSprite(COL_OUT);
  if (th == TH_NORMAL) {
    fb.fillCircle(CX, CY, 119, COL_BG);
  } else if (th == TH_SWEET) {
    // tramonto a bande
    for (int y = 0; y < 180; y += 6) {
      float k = y / 180.0f;
      uint16_t c = k < 0.5f ? lerpCol(COL_SUNSET_TOP, COL_SUNSET_MID, k * 2)
                            : lerpCol(COL_SUNSET_MID, COL_SUNSET_LOW, (k - 0.5f) * 2);
      fb.fillRect(0, y, 240, 6, c);
    }
    fb.fillCircle(58, 180, 20, COL_SUN);
    fb.fillRect(0, 180, 240, 60, COL_DUSK_GROUND);
    drawCupcake(36, 160);
    drawCandy(44, 92, COL_CANDY);
    drawCandy(196, 86, COL_BELL);
  } else {
    // notte con luna, stelle e pipistrelli
    fb.fillRect(0, 0, 240, 180, COL_NIGHT);
    fb.fillRect(0, 180, 240, 60, COL_NIGHT_GROUND);
    const int SX[10] = {40, 70, 95, 150, 178, 205, 60, 30, 200, 110};
    const int SY[10] = {80, 52, 34, 30, 48, 100, 140, 120, 140, 18};
    for (int i = 0; i < 10; i++) {
      if (((int)(t * 2) + i) % 5) fb.drawPixel(SX[i], SY[i], COL_WHITE);
    }
    fb.fillCircle(188, 76, 15, COL_MOON);
    fb.fillCircle(196, 70, 13, COL_NIGHT);
    drawBat(56 + (int)(6 * sinf(t)), 70, t);
    drawBat(84 + (int)(5 * sinf(t + 1)), 56, t + 0.5f);
  }
  fb.fillArc(CX, CY, 170, 120, 0, 360, COL_OUT);   // tutto fuori dal cerchio resta nero
  fb.drawCircle(CX, CY, 119, COL_RIM);
}

// =====================================================================
//  DISEGNO: GATTO
// =====================================================================
// Coda bianca con bordo scuro
void drawTail(const float* px, const float* py, float r) {
  for (int i = 0; i < 3; i++) fb.drawWideLine(px[i], py[i], px[i + 1], py[i + 1], r + 1.5f, COL_CAT);
  fb.fillCircle(px[3], py[3], (int)(r + 1.5f), COL_CAT);
  for (int i = 0; i < 3; i++) fb.drawWideLine(px[i], py[i], px[i + 1], py[i + 1], r, COL_WHITE);
  fb.fillCircle(px[3], py[3], (int)r, COL_WHITE);
}

enum { EYES_OPEN, EYES_BLINK, EYES_HAPPY, EYES_SAD };

void drawEye(int ex, int ey, int mode, int look, uint16_t eyeCol) {
  if (mode == EYES_BLINK) {
    fb.drawWideLine(ex - 8, ey, ex + 8, ey, 1.5, eyeCol);
    return;
  }
  if (mode == EYES_HAPPY) {
    fb.fillArc(ex, ey + 5, 10, 7, 200, 340, eyeCol);
    return;
  }
  fb.fillEllipse(ex, ey, 9, 10, eyeCol);
  fb.fillEllipse(ex + look, ey + 1, 3, 8, COL_CAT);
  fb.fillCircle(ex + look + 3, ey - 4, 2, COL_WHITE);
  if (mode == EYES_SAD) fb.fillRect(ex - 10, ey - 11, 21, 9, COL_CAT);   // palpebre basse
}

void drawCat(unsigned long now, int th, int dx, int dy, int eyes, bool mouthOpen, bool sad) {
  float t = now / 1000.0f;
  bool trick = (th == TH_TRICK);
  int hx = 120 + dx, hy = 108 + dy;
  int breath = (int)roundf(1.5f * sinf(t * 2.0f));

  float sw = 8 * sinf(t * 2.2f);
  float px[4] = {160, 188, 200 + sw * 0.5f, 192 + sw};
  float py[4] = {176, 166, 140, 116 + fabsf(sw) * 0.3f};

  // Alone viola di notte, cosi' il gatto nero si vede sullo sfondo scuro
  if (trick) {
    fb.fillEllipse(120, 158, 53, 37 + breath, COL_GLOW);
    fb.fillCircle(hx, hy, 41, COL_GLOW);
    fb.fillTriangle(hx - 40, hy - 10, hx - 32, hy - 55, hx - 3, hy - 33, COL_GLOW);
    fb.fillTriangle(hx + 40, hy - 10, hx + 32, hy - 55, hx + 3, hy - 33, COL_GLOW);
  }

  drawTail(px, py, 5);

  // Corpo che respira e zampe
  fb.fillEllipse(120, 158, 50, 34 + breath, COL_CAT);
  fb.fillEllipse(100, 190, 12, 7, COL_CAT);
  fb.fillEllipse(140, 190, 12, 7, COL_CAT);

  // Orecchie
  fb.fillTriangle(hx - 36, hy - 12, hx - 30, hy - 50, hx - 6, hy - 32, COL_CAT);
  fb.fillTriangle(hx + 36, hy - 12, hx + 30, hy - 50, hx + 6, hy - 32, COL_CAT);
  fb.fillTriangle(hx - 28, hy - 20, hx - 27, hy - 42, hx - 13, hy - 31, COL_PINK);
  fb.fillTriangle(hx + 28, hy - 20, hx + 27, hy - 42, hx + 13, hy - 31, COL_PINK);

  // Testa
  fb.fillCircle(hx, hy, 38, COL_CAT);

  // Cappello da strega nello scenario scherzetto
  if (trick) {
    fb.fillTriangle(hx - 15, hy - 31, hx + 15, hy - 31, hx + 8, hy - 66, COL_HAT);
    fb.fillRect(hx - 14, hy - 38, 28, 5, COL_HATBAND);
    fb.fillEllipse(hx, hy - 31, 28, 5, COL_HAT);
  }

  // Fiocco rosa sopra l'orecchio sinistro (di notte sta sulla falda del cappello)
  drawBow(hx - 22, hy - 30, 9);

  // Occhi: le pupille guardano l'icona selezionata
  int ix, iy, ir;
  itemPos(sel, ix, iy, ir);
  int look = (ix - CX) / 16;
  uint16_t eyeCol = trick ? COL_EYE_RED : COL_EYE;
  drawEye(hx - 14, hy - 2, eyes, look, eyeCol);
  drawEye(hx + 14, hy - 2, eyes, look, eyeCol);

  // Naso e bocca
  fb.fillTriangle(hx - 4, hy + 10, hx + 4, hy + 10, hx, hy + 15, COL_PINK);
  if (mouthOpen) {
    fb.fillEllipse(hx, hy + 21, 5, 4, COL_MOUTH);
  } else if (sad) {
    fb.drawArc(hx, hy + 24, 6, 5, 200, 340, COL_WHISK);
  } else {
    fb.drawLine(hx, hy + 15, hx - 5, hy + 20, COL_WHISK);
    fb.drawLine(hx, hy + 15, hx + 5, hy + 20, COL_WHISK);
  }

  // Baffi
  fb.drawLine(hx - 20, hy + 12, hx - 48, hy + 7,  COL_WHISK);
  fb.drawLine(hx - 20, hy + 16, hx - 48, hy + 18, COL_WHISK);
  fb.drawLine(hx + 20, hy + 12, hx + 48, hy + 7,  COL_WHISK);
  fb.drawLine(hx + 20, hy + 16, hx + 48, hy + 18, COL_WHISK);

  // Collare con campanella
  fb.fillEllipse(hx, hy + 35, 22, 4, COL_COLLAR);
  fb.fillCircle(hx, hy + 41, 4, COL_BELL);
}

void drawBowl(float level) {
  if (level > 0.02f) fb.fillEllipse(120, 188, 24, (int)(2 + 5 * level), COL_KIBBLE);
  fb.fillRoundRect(90, 188, 60, 14, 6, COL_BOWL);
  fb.drawFastHLine(94, 192, 52, COL_WHITE);
}

void drawHearts(float p, float t) {
  if (p > 0.9f) return;
  for (int i = 0; i < 3; i++) {
    float ph = fmodf(p * 1.5f + i * 0.33f, 1.0f);
    int x = 120 + (i - 1) * 50 + (int)(6 * sinf(t * 4 + i));
    int y = 120 - (int)(ph * 70);
    drawHeart(x, y, 5 + (int)(3 * ph), COL_LOVE);
  }
}

void drawBubbles(float p, float t) {
  for (int i = 0; i < 8; i++) {
    float a = radians(i * 45 + t * 60);
    float r = 62 + 8 * sinf(t * 3 + i);
    int x = 120 + (int)(r * cosf(a));
    int y = 140 + (int)(r * 0.75f * sinf(a));
    int s = 4 + (i % 3) * 2;
    fb.drawCircle(x, y, s, COL_CLEAN);
    fb.fillCircle(x - s / 3, y - s / 3, 1, COL_WHITE);
  }
  if (p > 0.6f) {   // scintille finali
    const int SX[3] = {78, 165, 150}, SY[3] = {95, 88, 175};
    for (int i = 0; i < 3; i++) {
      fb.drawLine(SX[i] - 5, SY[i], SX[i] + 5, SY[i], COL_BELL);
      fb.drawLine(SX[i], SY[i] - 5, SX[i], SY[i] + 5, COL_BELL);
    }
  }
}

// ---- Passeggiata: gatto di lato al guinzaglio, paesaggio che scorre ----
const int GROUND_Y = 184;

void drawScenery(int th, float t) {
  uint16_t sky   = th == TH_SWEET ? COL_SUNSET_MID : (th == TH_TRICK ? RGB(45, 50, 100) : COL_SKY);
  uint16_t grass = th == TH_SWEET ? RGB(140, 150, 70) : (th == TH_TRICK ? RGB(70, 60, 95) : COL_GRASS);
  uint16_t tree  = th == TH_TRICK ? RGB(35, 30, 55) : COL_TREE;
  fb.fillSprite(sky);
  if (th == TH_TRICK) {
    fb.fillCircle(178, 74, 12, COL_MOON);
    fb.fillCircle(184, 69, 10, sky);
  } else if (th == TH_SWEET) {
    // Tramonto come lo sfondo della scena ferma: cielo a bande e sole basso sull'orizzonte
    for (int y = 0; y < GROUND_Y; y += 6) {
      float k = y / (float)GROUND_Y;
      uint16_t c = k < 0.5f ? lerpCol(COL_SUNSET_TOP, COL_SUNSET_MID, k * 2)
                            : lerpCol(COL_SUNSET_MID, COL_SUNSET_LOW, (k - 0.5f) * 2);
      fb.fillRect(0, y, 240, 6, c);
    }
    fb.fillCircle(60, GROUND_Y, 20, COL_SUN);
  } else {
    fb.fillCircle(178, 74, 12, COL_SUN);
    // Nuvole (lente)
    for (int k = 0; k < 2; k++) {
      int x = (int)scrollX(k * 150, 12, t, 300);
      int y = 82 + k * 18;
      fb.fillCircle(x, y, 9, COL_WHITE);
      fb.fillCircle(x + 10, y - 4, 11, COL_WHITE);
      fb.fillCircle(x + 22, y, 9, COL_WHITE);
    }
  }

  // Alberi (velocita' media)
  for (int k = 0; k < 4; k++) {
    int x = (int)scrollX(k * 80 + 20, 40, t, 320);
    fb.fillRect(x - 3, GROUND_Y - 26, 6, 26, th == TH_TRICK ? tree : COL_TRUNK);
    fb.fillCircle(x, GROUND_Y - 34, 15, tree);
    fb.fillCircle(x - 9, GROUND_Y - 26, 9, tree);
    fb.fillCircle(x + 9, GROUND_Y - 26, 9, tree);
  }

  // Prato e sentiero (veloce, come i passi)
  fb.fillRect(0, GROUND_Y, 240, 240 - GROUND_Y, grass);
  fb.fillRect(0, GROUND_Y + 2, 240, 12, COL_PATH);
  for (int k = 0; k < 9; k++) {
    int x = (int)scrollX(k * 30, 60, t, 270);
    fb.fillRect(x, GROUND_Y + 7, 12, 2, COL_PATHD);
  }
}

void drawLeg(float hipX, float hipY, float ph) {
  float swing = 9 * sinf(ph);
  float lift  = sinf(ph) > 0 ? 4 * sinf(ph) : 0;
  float fx = hipX + swing, fy = GROUND_Y + 4 - lift;
  fb.drawWideLine(hipX, hipY, fx, fy, 3.5, COL_CAT);
  fb.fillEllipse((int)fx + 2, (int)fy, 5, 3, COL_CAT);
}

void drawCatWalk(unsigned long now, int th) {
  float t = now / 1000.0f;
  float ph = t * 10;                         // ritmo dei passi
  int bob = (int)(2 * fabsf(sinf(ph)));
  int by = 152 - bob;
  int hx = 156, hy = 128 - bob;

  // Alone viola di notte (come nella scena ferma), altrimenti il gatto nero sparisce sul cielo scuro
  if (th == TH_TRICK) {
    fb.fillEllipse(116, by, 44, 24, COL_GLOW);
    fb.fillCircle(hx, hy, 26, COL_GLOW);
    fb.fillTriangle(hx - 20, hy - 8, hx - 13, hy - 41, hx + 3, hy - 20, COL_GLOW);
    fb.fillTriangle(hx - 1, hy - 18, hx + 9, hy - 41, hx + 21, hy - 8, COL_GLOW);
    fb.fillEllipse(116, GROUND_Y + 2, 44, 6, COL_GLOW);   // zampe
  }

  // Zampe lato lontano (dietro al corpo)
  drawLeg(98,  by + 8, ph);
  drawLeg(142, by + 8, ph + PI);

  // Coda alta e contenta
  float sw = 5 * sinf(t * 4);
  float px[4] = {80, 66, 62 + sw * 0.5f, 70 + sw};
  float py[4] = {144, 128, 106, 92};
  drawTail(px, py, 4.5);

  // Corpo
  fb.fillEllipse(116, by, 40, 20, COL_CAT);

  // Zampe lato vicino
  drawLeg(88,  by + 8, ph + PI);
  drawLeg(132, by + 8, ph);

  // Testa di profilo
  fb.fillTriangle(hx - 16, hy - 10, hx - 12, hy - 36, hx + 1, hy - 20, COL_CAT);
  fb.fillTriangle(hx + 1, hy - 18, hx + 9, hy - 36, hx + 17, hy - 10, COL_CAT);
  fb.fillTriangle(hx + 4, hy - 18, hx + 9, hy - 31, hx + 13, hy - 14, COL_PINK);
  fb.fillCircle(hx, hy, 22, COL_CAT);
  fb.fillEllipse(hx + 14, hy + 6, 10, 8, COL_CAT);   // muso
  drawBow(hx - 3, hy - 21, 7);                       // fiocco tra le orecchie

  // Occhio
  uint16_t eyeCol = th == TH_TRICK ? COL_EYE_RED : COL_EYE;
  if (now < blinkUntil) {
    fb.drawWideLine(hx + 4, hy - 3, hx + 14, hy - 3, 1.5, eyeCol);
  } else {
    fb.fillEllipse(hx + 9, hy - 3, 5, 6, eyeCol);
    fb.fillEllipse(hx + 11, hy - 2, 2, 5, COL_CAT);
    fb.fillCircle(hx + 12, hy - 6, 1, COL_WHITE);
  }

  // Naso, baffi
  fb.fillTriangle(hx + 22, hy + 1, hx + 25, hy + 4, hx + 21, hy + 6, COL_PINK);
  fb.drawLine(hx + 14, hy + 8,  hx + 38, hy + 4,  COL_WHISK);
  fb.drawLine(hx + 14, hy + 11, hx + 38, hy + 13, COL_WHISK);

  // Collare e guinzaglio verso il padrone (fuori schermo, in alto a sinistra)
  fb.fillRoundRect(hx - 20, hy + 2, 7, 18, 3, COL_COLLAR);
  float x0 = hx - 17, y0 = hy + 10;
  float cx = 60, cy = 150 + 4 * sinf(t * 3);
  float x1 = 20, y1 = 40;
  float lx = x0, ly = y0;
  for (int i = 1; i <= 16; i++) {
    float u = i / 16.0f, v = 1 - u;
    float nx = v * v * x0 + 2 * v * u * cx + u * u * x1;
    float ny = v * v * y0 + 2 * v * u * cy + u * u * y1;
    fb.drawWideLine(lx, ly, nx, ny, 1.2, COL_LEASH);
    lx = nx; ly = ny;
  }
}

// Rampa 0 -> 1 -> 0 per abbassare/rialzare la testa
float ease(float p) {
  if (p < 0.15f) return p / 0.15f;
  if (p > 0.85f) return (1 - p) / 0.15f;
  return 1;
}

void drawOverlays(unsigned long now) {
  if (now < bubbleUntil) drawSpeech();
  if (phase == PH_HUNT) {
    char c[8];
    snprintf(c, sizeof(c), "%d/%d", huntCount, N_BUSTE);
    drawSideBox(c);
  } else if (phase == PH_BONUS && SHOW_BONUS_TIMER && !bonusExpired) {
    unsigned long left = BONUS_DURATION_MIN * MIN_MS - bonusElapsed;
    char c[8];
    snprintf(c, sizeof(c), "%lu:%02lu", left / (60 * MIN_MS), (left / MIN_MS) % 60);
    drawSideBox(c);
  }
  if (listening) drawListenPaw(208, 120);
  if (ui == UI_MODAL) drawModal();
}

void renderCat(unsigned long now) {
  float t = now / 1000.0f;
  float p = anim ? (since(animStart)) / (float)animDur : 0;
  if (p > 1) p = 1;
  int th = currentTheme();

  if (anim == A_WALK) {
    drawScenery(th, t);
    drawCatWalk(now, th);
    fb.fillArc(CX, CY, 170, 120, 0, 360, COL_OUT);
    fb.drawCircle(CX, CY, 119, COL_RIM);
    drawBars(now);
    drawOverlays(now);
    fb.pushSprite(0, 0);
    return;
  }

  // Triste solo nel gioco libero (in fase 1 le barre partono vuote, nella caccia non si vedono)
  bool sad = false;
  if (phase == PH_FREE) {
    for (int i = 0; i < 4; i++) if (barVal[i] < LOW_THRESHOLD) sad = true;
  }
  int eyes = sad ? EYES_SAD : EYES_OPEN;
  if (now < blinkUntil) eyes = EYES_BLINK;
  int dx = 0, dy = 0;
  bool mouthOpen = false;

  if (anim == A_EAT) {
    dy = (int)(12 * ease(p));
    eyes = EYES_HAPPY;
    mouthOpen = (p > 0.15f && p < 0.85f) && ((now / 180) % 2);
  } else if (anim == A_PET) {
    dx = (int)(4 * sinf(t * 8));
    eyes = EYES_HAPPY;
  } else if (anim == A_CLEAN) {
    dx = (int)(2 * sinf(t * 25));
    eyes = EYES_BLINK;
  }

  drawBackground(th, t);
  drawBars(now);
  drawCat(now, th, dx, dy, eyes, mouthOpen, sad && anim == A_NONE);

  if (anim == A_EAT)   drawBowl(1 - p);
  if (anim == A_PET)   drawHearts(p, t);
  if (anim == A_CLEAN) drawBubbles(p, t);

  if (anim == A_NONE) drawIcons(now);
  drawOverlays(now);
  fb.pushSprite(0, 0);
}

// Attesa prima dell'avvio di una fase: Salem dorme (senza mostrare il tempo)
void renderWait(unsigned long now) {
  float t = now / 1000.0f;
  drawBackground(TH_NORMAL, t);
  drawCat(now, TH_NORMAL, 0, 6, EYES_BLINK, false, false);
  fb.setFont(&fonts::FreeSansBold9pt7b);
  fb.setTextColor(COL_NAVY);
  fb.setTextDatum(lgfx::middle_center);
  for (int i = 0; i < 3; i++) {
    float ph = fmodf(t * 0.5f + i / 3.0f, 1.0f);
    fb.drawString(i == 1 ? "Z" : "z", 160 + (int)(ph * 30), 70 - (int)(ph * 40));
  }
  if (listening) drawListenPaw(208, 120);
  fb.pushSprite(0, 0);
}

// Lucchetto della caccia: 3 cifre, gira = cambia cifra, click = cifra successiva
void drawPadlock(int x, int y) {
  fb.fillArc(x, y - 6, 18, 12, 180, 360, COL_GREY);
  fb.fillRect(x - 18, y - 6, 6, 8, COL_GREY);
  fb.fillRect(x + 12, y - 6, 6, 8, COL_GREY);
  fb.fillRoundRect(x - 24, y, 48, 36, 6, COL_GOLD);
  fb.fillCircle(x, y + 14, 5, COL_CAT);
  fb.fillRect(x - 2, y + 16, 5, 10, COL_CAT);
}

void renderLock(unsigned long now) {
  drawBackground(currentTheme(), now / 1000.0f);
  fb.fillCircle(CX, CY, 104, COL_BG);
  drawPadlock(CX, 44);
  bool err = now < lockErrUntil;
  if (!err && lockErrUntil) {   // fine del rosso: si riprova da capo
    lockErrUntil = 0;
    lockPos = 0;
    lockDigits[0] = lockDigits[1] = lockDigits[2] = 0;
  }
  fb.setFont(&fonts::FreeSansBold18pt7b);
  fb.setTextDatum(lgfx::middle_center);
  for (int i = 0; i < 3; i++) {
    int x = CX - 52 + i * 52;
    fb.fillRoundRect(x - 22, 106, 44, 56, 8, COL_WHITE);
    uint16_t border = err ? COL_ERR : (i == lockPos ? COL_SEL : COL_CAT);
    fb.drawRoundRect(x - 22, 106, 44, 56, 8, border);
    if (i == lockPos && !err) fb.drawRoundRect(x - 21, 107, 42, 54, 7, border);
    char d[2] = {(char)('0' + lockDigits[i]), 0};
    fb.setTextColor(err ? COL_ERR : COL_CAT);
    fb.drawString(d, x, 135);
  }
  if (listening) drawListenPaw(CX, 196);
  fb.pushSprite(0, 0);
}

// =====================================================================
//  MAPPA (immagine in mappa.h)
// =====================================================================
void renderMap() {
  fb.startWrite();
  for (int y = 0; y < 240; y++) {
    for (int x = 0; x < 240; x++) fb.writePixel(x, y, (uint16_t)mappa[y * 240 + x]);
  }
  fb.endWrite();
  fb.fillArc(CX, CY, 170, 120, 0, 360, COL_OUT);   // gli angoli dell'immagine restano fuori dal cerchio
  fb.drawCircle(CX, CY, 119, COL_RIM);
  fb.pushSprite(0, 0);
}

// =====================================================================
//  MENU STAFF
// =====================================================================
enum { M_BONUS, M_HUNT, M_SKIP, M_FREE, M_CLOCK, M_TEAM, M_STATUS, M_CLEAR_HUNT,
       M_TEST1, M_TEST2, M_RESET, M_EXIT, M_COUNT };

void menuLabel(int i, char* out, size_t n) {
  switch (i) {
    case M_BONUS: snprintf(out, n, "AVVIA FASE 1"); break;
    case M_HUNT: snprintf(out, n, "AVVIA FASE 2"); break;
    case M_SKIP: snprintf(out, n, "SALTA AVANTI"); break;
    case M_FREE: snprintf(out, n, "GIOCO LIBERO"); break;
    case M_CLOCK: snprintf(out, n, "OROLOGIO"); break;
    case M_TEAM: snprintf(out, n, "%d %s", team + 1, SQUADRE[team]); break;
    case M_STATUS: snprintf(out, n, "STATO"); break;
    case M_CLEAR_HUNT: snprintf(out, n, "AZZERA CACCIA"); break;
    case M_TEST1: snprintf(out, n, "TEST SERR. 1"); break;
    case M_TEST2: snprintf(out, n, "TEST SERR. 2"); break;
    case M_RESET: snprintf(out, n, millis() < resetArmedUntil ? "SICURO? CLICK" : "RESET TOTALE"); break;
    default: snprintf(out, n, "ESCI"); break;
  }
}

void openPin() {
  ui = UI_PIN;
  pinPos = 0;
  for (int i = 0; i < 4; i++) pinDigits[i] = 0;
  pinLast = millis();
  anim = A_NONE;
}

void pinClick() {
  pinLast = millis();
  if (++pinPos < 4) return;
  char typed[5];
  snprintf(typed, sizeof(typed), "%d%d%d%d", pinDigits[0], pinDigits[1], pinDigits[2], pinDigits[3]);
  if (strcmp(typed, STAFF_PIN) == 0) {
    ui = UI_MENU;
    menuSel = 0;
    menuInfo[0] = 0;
    menuLast = millis();
  } else {
    ui = UI_MAIN;   // PIN sbagliato: si torna al gioco senza messaggi
    Serial.println("PIN sbagliato");
  }
}

void closeMenu() {
  ui = UI_MAIN;
  buildItems();
  clockDirty = true;
  saveNow();
}

// SALTA AVANTI: fa avanzare quello che e' in corso (attesa, richieste, caccia). Utile per lo staff e per le prove.
void skipAhead() {
  if (phase == PH_WAIT) {
    startPhaseNow(waitFor);
    closeMenu();
    return;
  }
  if (phase == PH_BONUS && !bonusUnlocked && !bonusExpired) {
    if (!requestActive) {                 // la richiesta successiva arriva subito
      activateRequest();
      ui = UI_MENU;
    } else {                              // la richiesta in corso conta come fatta
      barVal[QUEST_ORDER[questIdx]] = clampBar(barVal[QUEST_ORDER[questIdx]] + QUEST_STEP);
      questIdx++;
      requestActive = false;
      questWait = 0;
      if (questIdx >= N_RICHIESTE) {
        for (int i = 0; i < 4; i++) barVal[i] = 100;
        bonusUnlocked = true;
        closeMenu();
        showModal(TXT_BONUS_MSG);
        return;
      }
      activateRequest();
    }
    markSave();
    snprintf(menuInfo, sizeof(menuInfo), "RICHIESTA %d/%d", questIdx + 1, N_RICHIESTE);
    return;
  }
  if (phase == PH_HUNT && huntState == HS_WAIT) {
    huntState = HS_COUNT;
    huntWait = 0;
    markSave();
    snprintf(menuInfo, sizeof(menuInfo), "ATTESA CACCIA FINITA");
    return;
  }
  if (phase == PH_HUNT && huntState == HS_COUNT && huntCount < N_BUSTE - 1) {   // l'ottava si fa a mano
    hunt[huntCount] = PERCORSI[team][huntCount];
    huntCount++;
    markSave();
    snprintf(menuInfo, sizeof(menuInfo), "CACCIA %d/%d", huntCount, N_BUSTE);
    return;
  }
  snprintf(menuInfo, sizeof(menuInfo), "NIENTE DA SALTARE");
}

void menuSelect() {
  menuLast = millis();
  switch (menuSel) {
    case M_BONUS: schedulePhase(PH_BONUS); closeMenu(); break;
    case M_HUNT: schedulePhase(PH_HUNT); closeMenu(); break;
    case M_SKIP: skipAhead(); break;
    case M_FREE: setPhase(PH_FREE); closeMenu(); break;
    case M_CLOCK: setPhase(PH_CLOCK); closeMenu(); break;
    case M_TEAM: team = (team + 1) % N_SQUADRE; markSave(); break;
    case M_STATUS: ui = UI_STATUS; break;
    case M_CLEAR_HUNT:
      huntCount = 0; huntState = HS_COUNT; huntWait = 0;
      markSave();
      snprintf(menuInfo, sizeof(menuInfo), "CACCIA AZZERATA");
      break;
    case M_TEST1: if (!awaiting) { snprintf(menuInfo, sizeof(menuInfo), "INVIO..."); sendRequest(REQ_TEST1); } break;
    case M_TEST2: if (!awaiting) { snprintf(menuInfo, sizeof(menuInfo), "INVIO..."); sendRequest(REQ_TEST2); } break;
    case M_RESET:
      if (millis() < resetArmedUntil) {
        int keepTeam = team;
        resetAll();
        team = keepTeam;
        resetArmedUntil = 0;
        buildItems();
        snprintf(menuInfo, sizeof(menuInfo), "RESET FATTO");
        markSave();
      } else {
        resetArmedUntil = millis() + 3000;
      }
      break;
    default: closeMenu(); break;
  }
}

void renderMenu() {
  fb.fillSprite(COL_OUT);
  fb.fillCircle(CX, CY, 119, COL_NAVY);
  fb.setTextDatum(lgfx::middle_center);
  fb.setFont(&fonts::Font2);
  fb.setTextColor(COL_GOLD);
  fb.drawString("STAFF", CX, 40);

  char buf[32];
  fb.setTextColor(COL_GREY);
  menuLabel(wrap(menuSel - 1, M_COUNT), buf, sizeof(buf));
  fb.drawString(buf, CX, 82);
  menuLabel(wrap(menuSel + 1, M_COUNT), buf, sizeof(buf));
  fb.drawString(buf, CX, 158);

  fb.fillRoundRect(22, 104, 196, 32, 10, COL_WHITE);
  fb.setFont(&fonts::FreeSansBold9pt7b);
  menuLabel(menuSel, buf, sizeof(buf));
  if (fb.textWidth(buf) > 186) fb.setFont(&fonts::Font2);   // nomi di squadra lunghi
  fb.setTextColor(COL_NAVY);
  fb.drawString(buf, CX, 120);

  fb.setFont(&fonts::Font2);
  fb.setTextColor(COL_WHITE);
  snprintf(buf, sizeof(buf), "%s - SQ.%d", PHASE_NAMES[phase], team + 1);
  fb.drawString(buf, CX, 190);
  if (menuInfo[0]) {
    fb.setTextColor(COL_BELL);
    fb.drawString(menuInfo, CX, 208);
  }
  fb.pushSprite(0, 0);
}

void renderStatus() {
  fb.fillSprite(COL_OUT);
  fb.fillCircle(CX, CY, 119, COL_NAVY);
  fb.setFont(&fonts::Font2);
  fb.setTextDatum(lgfx::middle_center);
  fb.setTextColor(COL_WHITE);
  char l[40];
  int y = 36;
  const int STEP = 16;
  if (phase == PH_WAIT) {
    unsigned long left = START_DELAY_MIN * MIN_MS - waitElapsed;
    snprintf(l, sizeof(l), "ATTESA %s: %lu:%02lu", PHASE_NAMES[waitFor], left / MIN_MS, (left / 1000UL) % 60);
  } else {
    snprintf(l, sizeof(l), "FASE: %s", PHASE_NAMES[phase]);
  }
  fb.drawString(l, CX, y); y += STEP;
  snprintf(l, sizeof(l), "SQ.%d %s", team + 1, SQUADRE[team]);                     fb.drawString(l, CX, y); y += STEP;
  snprintf(l, sizeof(l), "BARRE %d %d %d %d", barVal[0], barVal[1], barVal[2], barVal[3]); fb.drawString(l, CX, y); y += STEP;
  if (requestActive) snprintf(l, sizeof(l), "RICHIESTE %d/%d IN CORSO", questIdx, N_RICHIESTE);
  else snprintf(l, sizeof(l), "RICHIESTE %d/%d (%lu min)", questIdx, N_RICHIESTE, questWait / MIN_MS);
  fb.drawString(l, CX, y); y += STEP;
  snprintf(l, sizeof(l), "BONUS %lu/%lu min %s", bonusElapsed / MIN_MS, BONUS_DURATION_MIN,
           bonusExpired ? "SCADUTO" : (bonusUnlocked ? "SBLOCC." : ""));
  fb.drawString(l, CX, y); y += STEP;
  snprintf(l, sizeof(l), "CASSETTA 1: %s", box1Open ? "APERTA" : "CHIUSA");       fb.drawString(l, CX, y); y += STEP;
  const char* hs = huntState == HS_LOCK ? "LUCCHETTO" : (huntState == HS_WAIT ? "ATTESA" : "");
  if (huntState == HS_WAIT) snprintf(l, sizeof(l), "CACCIA %d/%d ATTESA %lu min", huntCount, N_BUSTE, huntWait / MIN_MS);
  else snprintf(l, sizeof(l), "CACCIA %d/%d %s", huntCount, N_BUSTE, hs);
  fb.drawString(l, CX, y); y += STEP;
  snprintf(l, sizeof(l), "MAPPA: %s", mapUnlocked ? "SI" : "NO");                 fb.drawString(l, CX, y); y += STEP;
  snprintf(l, sizeof(l), "CASSETTA 2: %s", box2Open ? "APERTA" : "CHIUSA");       fb.drawString(l, CX, y); y += STEP;
#ifdef SALEM_TEST_SCARICA
  snprintf(l, sizeof(l), "PROTEZIONE SCARICA: %d VOLTE", brownoutSleeps);
#else
  snprintf(l, sizeof(l), "RADIO A RICHIESTA %s SONNO %s", RADIO_ON_DEMAND ? "SI" : "NO", SLEEP_ENABLED ? "SI" : "NO");
#endif
  fb.setTextColor(COL_GREY);
  fb.drawString(l, CX, y); y += STEP;
  fb.drawString("click", CX, 214);
  fb.pushSprite(0, 0);
}

void renderPin() {
  fb.fillSprite(COL_OUT);
  fb.fillCircle(CX, CY, 119, COL_NAVY);
  fb.setTextDatum(lgfx::middle_center);
  fb.setFont(&fonts::Font2);
  fb.setTextColor(COL_GOLD);
  fb.drawString("PIN", CX, 70);
  fb.setFont(&fonts::FreeSansBold12pt7b);
  for (int i = 0; i < 4; i++) {
    int x = CX - 54 + i * 36;
    fb.fillRoundRect(x - 15, 100, 30, 40, 6, COL_WHITE);
    if (i == pinPos) fb.drawRoundRect(x - 16, 99, 32, 42, 7, COL_SEL);
    char d[2] = {(char)('0' + pinDigits[i]), 0};
    fb.setTextColor(COL_NAVY);
    fb.drawString(d, x, 121);
  }
  fb.pushSprite(0, 0);
}

// =====================================================================
//  OROLOGIO (quadrante da bitmap)
// =====================================================================
// Ogni pixel convertito con la stessa funzione RGB() del gatto, cosi' i colori sono uguali
void drawDialBitmap() {
  fb.startWrite();
  for (int y = 0; y < 240; y++) {
    for (int x = 0; x < 240; x++) {
      uint32_t c = quadrante[y * 240 + x];
      uint16_t c565 = RGB(c >> 16, c >> 8, c);
      fb.writePixel(x, y, c565);
    }
  }
  fb.endWrite();
}

void drawHand(float angle_deg, int length, float thickness, uint16_t color) {
  float rad = radians(angle_deg - 90);
  fb.drawWideLine(CX, CY, CX + length * cosf(rad), CY + length * sinf(rad), thickness, color);
}

void renderClock() {
  drawDialBitmap();

  float secAngle  = seconds * 6.0f;
  float minAngle  = minutes * 6.0f + seconds * 0.1f;
  float hourAngle = (hours % 12) * 30.0f + minutes * 0.5f;

  // La lancetta selezionata e' rossa
  drawHand(hourAngle, 50, 3.5, hand == 0 ? COL_SEL : COL_HAND);
  drawHand(minAngle,  78, 2.5, hand == 1 ? COL_SEL : COL_HAND);
  drawHand(secAngle,  92, 1.0, hand == 2 ? COL_SEL : COL_HAND);
  fb.fillCircle(CX, CY, 5, COL_HAND);
  fb.fillCircle(CX, CY, 2, COL_GOLD);

  if (statusUntil && millis() < statusUntil) {
    fb.fillRoundRect(CX - 60, CY + 30, 120, 34, 8, statusColor);
    fb.setFont(&fonts::FreeSerifBold12pt7b);
    fb.setTextColor(COL_WHITE);
    fb.setTextDatum(lgfx::middle_center);
    fb.drawString(statusText, CX, CY + 47);
  }
  if (listening) drawListenPaw(CX, 196);

  fb.pushSprite(0, 0);
}

// Testo di passaggio prima del quadrante (TXT_CLOCK_INTRO)
void renderIntro() {
  fb.fillSprite(COL_OUT);
  fb.fillCircle(CX, CY, 119, COL_NAVY);
  fb.drawCircle(CX, CY, 119, COL_GOLD);
  fb.setFont(&fonts::FreeSerifBold12pt7b);
  fb.setTextColor(COL_GOLD);
  fb.setTextDatum(lgfx::middle_center);
  char l[4][LINE_LEN];
  int n = wrapText(TXT_CLOCK_INTRO, 190, l, 4);
  int y0 = CY - (n - 1) * 15;
  for (int i = 0; i < n; i++) fb.drawString(l[i], CX, y0 + i * 30);
  fb.pushSprite(0, 0);
}

// =====================================================================
//  COMBINAZIONI SEGRETE
// =====================================================================
void startListening(unsigned long now) {
  listening = true;
  listenStart = now;
  lastSym = now;
  codeLen = 0;
  codeBuf[0] = 0;
  clockDirty = true;
}

void addSymbol(char c, unsigned long now) {
  if (codeLen < (int)sizeof(codeBuf) - 1) {
    codeBuf[codeLen++] = c;
    codeBuf[codeLen] = 0;
  }
  lastSym = now;
  clockDirty = true;
}

void finishListening() {
  listening = false;
  clockDirty = true;
  Serial.printf("Combinazione: %s\n", codeBuf);
  if (strcmp(codeBuf, CODE_STAFF) == 0) {
    openPin();
  } else if (phase == PH_FREE && strcmp(codeBuf, CODE_TO_CLOCK) == 0 && (mapUnlocked || !CLOCK_ONLY_AFTER_MAP)) {
    setPhase(PH_CLOCK);
    ui = UI_INTRO;   // prima il testo di passaggio, poi il quadrante
    introAt = millis();
  } else if (phase == PH_CLOCK && strcmp(codeBuf, CODE_TO_CAT) == 0 && (box2Open || !CAT_ONLY_AFTER_BOX2)) {
    setPhase(PH_FREE);
    say(TXT_HELLO, 2000);
  }
}

// =====================================================================
//  PULSANTE E CORONA
// =====================================================================
void onRelease(unsigned long held, unsigned long now) {
  switch (ui) {
    case UI_MODAL:  modalNext(); return;
    case UI_MAP:    closeMap(); return;
    case UI_MENU:   menuSelect(); return;
    case UI_STATUS: ui = UI_MENU; menuLast = now; return;
    case UI_PIN:    pinClick(); return;
    case UI_INTRO:  return;
  }
  // UI_MAIN
  if (phase == PH_WAIT) return;   // durante l'attesa la corona non fa niente
  if (phase == PH_CLOCK) {
    if (held < SEND_MIN_MS) {
      hand = (hand + 1) % 3;
      clockDirty = true;
    } else if (!awaiting) {
      sendRequest(REQ_BOX2);
    }
    return;
  }
  if (phase == PH_HUNT && huntState == HS_LOCK) { lockClick(now); return; }
  if (held < SEND_MIN_MS) onCatClick(now);
}

void handleButton(unsigned long now) {
  static bool swConsumed = false;
  static unsigned long swT = 0;
  bool sw = (digitalRead(PIN_ENC_SW) == LOW);

  if (sw && !swDown) {
    swDown = true;
    swConsumed = false;
    swT = now;
    // Schermo spento: questa pressione serve solo a riaccenderlo, non fa nient'altro
    if (!lightOn) { setLight(true); swConsumed = true; }
    lastInput = now;
  }
  if (swDown) lastInput = now;
  unsigned long held = swDown ? now - swT : 0;

  // Pressione lunga = inizia ad ascoltare una combinazione (solo nella schermata principale)
  if (swDown && !swConsumed && !listening && ui == UI_MAIN && held >= HOLD_MS) {
    swConsumed = true;
    startListening(now);
  }

  if (!sw && swDown) {
    swDown = false;
    if (held < 40 || swConsumed) return;
    applyRelease(held, now);
  }

  if (listening && !swDown) {
    if ((codeLen == 0 && since(listenStart) > LISTEN_MS) || (codeLen > 0 && since(lastSym) > CODE_GAP_MS)) {
      finishListening();
    }
  }
}

// Rilascio del pulsante dopo "held" ms (usato dalla corona vera e dall'autotest)
void applyRelease(unsigned long held, unsigned long now) {
  if (listening) { addSymbol(held >= LONG_CLICK_MS ? '-' : '.', now); return; }
  onRelease(held, now);
}

void handleRotation() {
  static int32_t acc = 0;
  noInterrupts();
  acc += encDelta;
  encDelta = 0;
  interrupts();

  int steps = acc / STEPS_PER_DETENT;
  if (steps == 0) return;
  acc -= steps * STEPS_PER_DETENT;
  if (INVERT_DIR) steps = -steps;
  lastInput = millis();
  // Schermo spento: girare la corona lo riaccende soltanto
  if (!lightOn) { setLight(true); return; }
  applyRotation(steps);
}

// Scatti della corona (usato dalla corona vera e dall'autotest)
void applyRotation(int steps) {
  if (listening) return;

  if (ui == UI_MENU) {
    menuSel = wrap(menuSel + steps, M_COUNT);
    menuLast = millis();
    resetArmedUntil = 0;
    return;
  }
  if (ui == UI_PIN) {
    pinDigits[pinPos] = wrap(pinDigits[pinPos] + steps, 10);
    pinLast = millis();
    return;
  }
  if (ui != UI_MAIN || phase == PH_WAIT) return;

  if (phase == PH_CLOCK) {
    if (hand == 0)      hours   = wrap(hours + steps, 12);   // quadrante a 12 ore: 9 e 21 sono la stessa posizione
    else if (hand == 1) minutes = wrap(minutes + steps, 60);
    else                seconds = wrap(seconds + steps, 60);
    clockDirty = true;
    markSave();
  } else if (phase == PH_HUNT && huntState == HS_LOCK) {
    if (millis() >= lockErrUntil) lockDigits[lockPos] = wrap(lockDigits[lockPos] + steps, 10);
  } else if (anim == A_NONE) {
    sel = wrap(sel + steps, nItems);
  }
}

#ifdef SALEM_AUTOTEST   // solo nella versione di test (vedi autotest.h)
#include "autotest.h"
#endif

// =====================================================================
//  SONNO LEGGERO (schermo spento)
// =====================================================================
// Collegato al PC (USB con dati): non dorme, cosi' il caricamento funziona sempre.
// A batteria o col powerbank (che non passa dalla USB della SuperMini) dorme normalmente.
bool canSleep() {
  return SLEEP_ENABLED && !lightOn && ui == UI_MAIN && anim == A_NONE && !awaiting
         && !listening && !swDown && !finalAt && !usb_serial_jtag_is_connected();
}

// Dorme fino a SLEEP_CHUNK_MS o finche' non si tocca la corona.
// In light sleep il C3 si sveglia sul LIVELLO del pin: si imposta l'opposto di quello attuale.
void lightSleep() {
  if (saveDirty) saveNow();
  esp_task_wdt_reset();
  Serial.flush();

  int a = digitalRead(PIN_ENC_A), b = digitalRead(PIN_ENC_B), s = digitalRead(PIN_ENC_SW);
  detachInterrupt(digitalPinToInterrupt(PIN_ENC_A));
  detachInterrupt(digitalPinToInterrupt(PIN_ENC_B));
  gpio_wakeup_enable((gpio_num_t)PIN_ENC_A,  a ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
  gpio_wakeup_enable((gpio_num_t)PIN_ENC_B,  b ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
  gpio_wakeup_enable((gpio_num_t)PIN_ENC_SW, s ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  esp_sleep_enable_timer_wakeup(SLEEP_CHUNK_MS * 1000ULL);

  gpio_hold_en((gpio_num_t)PIN_BLK);    // retroilluminazione bloccata spenta durante il sonno
  esp_light_sleep_start();
  gpio_hold_dis((gpio_num_t)PIN_BLK);

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  gpio_wakeup_disable((gpio_num_t)PIN_ENC_A);
  gpio_wakeup_disable((gpio_num_t)PIN_ENC_B);
  gpio_wakeup_disable((gpio_num_t)PIN_ENC_SW);
  attachEncoder();
  esp_task_wdt_reset();

  // Svegliato dalla corona: si riaccende e basta (il primo tocco non conta).
  // Se il pulsante e' ancora premuto ci pensa handleButton (che consuma quella pressione).
  if (cause == ESP_SLEEP_WAKEUP_GPIO && digitalRead(PIN_ENC_SW) == HIGH) lightWake();
}

// =====================================================================
//  SETUP E LOOP
// =====================================================================
void setup() {
  // Batteria scarica: dopo un riavvio per tensione bassa non si riparte subito, altrimenti
  // schermo e processore si prendono tutta la corrente ridotta del caricatore e la batteria
  // non si riprende mai. 60 s di sonno profondo con la retroilluminazione spenta, poi si riprova.
  gpio_hold_dis((gpio_num_t)PIN_BLK);   // al risveglio dal sonno profondo il pin era bloccato
  if (esp_reset_reason() == ESP_RST_BROWNOUT) {
    pinMode(PIN_BLK, OUTPUT);
    digitalWrite(PIN_BLK, BLK_ACTIVE_HIGH ? LOW : HIGH);
    gpio_hold_en((gpio_num_t)PIN_BLK);
    gpio_deep_sleep_hold_en();
    esp_sleep_enable_timer_wakeup(60ULL * 1000000ULL);
#ifdef SALEM_TEST_SCARICA
    brownoutSleeps++;
#endif
    esp_deep_sleep_start();
  }

  Serial.begin(115200);
  delay(1000);
  Serial.printf("Sprite: %s (blocco libero prima: %u byte, dopo: %u byte)\n",
                spriteEarly.ok ? "ok" : "ERRORE, memoria insufficiente", spriteLargestBefore,
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  pinMode(PIN_ENC_SW, INPUT_PULLUP);
  attachEncoder();

  display.init();
  pinMode(PIN_BLK, OUTPUT);
  digitalWrite(PIN_BLK, BLK_ACTIVE_HIGH ? HIGH : LOW);
  lightOn = true;
  lastInput = millis();

  display.setRotation(SCREEN_ROTATION);

  // Nel sonno leggero i pin restano come sono: senza questo il C3 li rilascia, la retroilluminazione
  // si riaccende da sola (circa 20 mA), la corona perde i pull-up e il reset del display resta libero.
  const int PIN_FISSI[] = {0, 1, 2, 3, 4, PIN_BLK, PIN_ENC_A, PIN_ENC_B, PIN_ENC_SW};
  for (int p : PIN_FISSI) gpio_sleep_sel_dis((gpio_num_t)p);

  if (!RADIO_ON_DEMAND) radioStart();

  prefs.begin("salem", false);
  loadState();
  buildItems();

  randomSeed(esp_random());
  blinkAt = millis() + 3000;
  nextComplaint = millis() + COMPLAINT_SEC * 1000UL;
  if (phase == PH_BONUS && requestActive) sayRequest();
  else if (phase == PH_FREE || phase == PH_HUNT) say(TXT_HELLO, 2500);
  Serial.printf("Pronto: fase %s, squadra %d (%s), RAM libera %u byte\n",
                PHASE_NAMES[phase], team + 1, SQUADRE[team], (unsigned)ESP.getFreeHeap());
  Serial.printf("Motivo dell'ultimo riavvio: %d (1=accensione, 4=crash, 5/6/7=blocco, 9=BROWNOUT)\n",
                (int)esp_reset_reason());
#ifdef SALEM_AUTOTEST
  atSetup();
#endif
  Serial.printf("Risparmio: sonno %s, radio solo all'invio %s\n",
                SLEEP_ENABLED ? "SI" : "NO", RADIO_ON_DEMAND ? "SI" : "NO");

  // Watchdog: se il programma si blocca, l'orologio riparte da solo (lo stato e' salvato)
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t wdt;
  wdt.timeout_ms = 5000;
  wdt.idle_core_mask = 0;
  wdt.trigger_panic = true;
  esp_task_wdt_reconfigure(&wdt);
#else
  esp_task_wdt_init(5, true);
#endif
  esp_task_wdt_add(NULL);
}

void loop() {
  esp_task_wdt_reset();
  unsigned long now = millis();
  static unsigned long lastT = now;
  unsigned long dt = now - lastT;   // comprende anche il tempo passato nel sonno leggero
  lastT = now;

  handleRotation();
  handleButton(now);
  radioLoop();
#ifdef SALEM_AUTOTEST
  atLoop();
#endif

  // Menu e PIN: si chiudono da soli dopo 30 s senza toccarli
  if ((ui == UI_MENU || ui == UI_STATUS) && since(menuLast) > 30000) closeMenu();
  if (ui == UI_PIN && since(pinLast) > 30000) ui = UI_MAIN;
  // Scheda: ogni pagina si chiude da sola dopo 20 s
  if (ui == UI_MODAL && since(modalAt) > 20000) ui = UI_MAIN;
  // Testo prima dell'orologio
  if (ui == UI_INTRO && since(introAt) >= CLOCK_INTRO_MS) { ui = UI_MAIN; clockDirty = true; }

  updateTimers(dt);
  if (ui != UI_MENU && ui != UI_STATUS) updateBars(now);

  // Salvataggio: subito dopo i cambiamenti (max ogni 2 s) e comunque ogni 60 s
  if ((saveDirty && since(lastSave) > 2000) || since(lastSave) > 60000) saveNow();

  // Retroilluminazione: si spegne dopo BACKLIGHT_TIMEOUT_S senza toccare la corona
#ifdef SALEM_TEST_SCARICA
  lastInput = millis();   // prova della scarica: schermo sempre acceso, la batteria muore in 2-3 ore
#endif
  if (lightOn && since(lastInput) > BACKLIGHT_TIMEOUT_S * 1000UL) {
    setLight(false);
    if (ui == UI_MAP) ui = UI_MAIN;   // la mappa si rivede dall'icona MAPPA
    Serial.println("Schermo spento (inattivo)");
  }

  // Cassetta 2 aperta: dopo qualche secondo di "APERTO" si torna al gioco libero (nessun messaggio)
  if (phase == PH_CLOCK && finalAt && since(finalAt) >= FINAL_DELAY_MS) {
    finalAt = 0;
    statusUntil = 0;
    setPhase(PH_FREE);
    saveNow();
    say(TXT_HELLO, 2000);
  }

  if (anim != A_NONE && since(animStart) >= animDur) onAnimEnd();
  if (now >= blinkAt) {
    blinkUntil = now + 140;
    blinkAt = now + random(2500, 6000);
  }
  complaints(now);

  // Schermo spento: si dorme (se possibile) e non si disegna
  if (!lightOn) {
    if (canSleep()) lightSleep();
    else delay(10);
    return;
  }

  // Disegno
  static unsigned long lastFrame = 0;
  unsigned long frameMs = 40;
  if (ui == UI_MENU) frameMs = 60;
  else if (ui == UI_STATUS) frameMs = 200;
  else if (ui == UI_MAP || ui == UI_PIN || ui == UI_INTRO) frameMs = 250;
  if (since(lastFrame) < frameMs) return;
  lastFrame = millis();

#ifdef SALEM_AUTOTEST
  if (atDraw()) return;
#endif

  switch (ui) {
    case UI_MENU:   renderMenu(); return;
    case UI_STATUS: renderStatus(); return;
    case UI_PIN:    renderPin(); return;
    case UI_MAP:    renderMap(); return;
    case UI_INTRO:  renderIntro(); return;
  }
  if (phase == PH_CLOCK) {
    if (statusUntil && millis() > statusUntil) { statusUntil = 0; clockDirty = true; }
    if (clockDirty) { clockDirty = false; renderClock(); }
    return;
  }
  if (phase == PH_WAIT) { renderWait(now); return; }
  if (phase == PH_HUNT && huntState == HS_LOCK) { renderLock(now); return; }
  renderCat(now);   // comprende la scheda (UI_MODAL) sopra al gatto
}
