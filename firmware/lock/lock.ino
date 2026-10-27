// SERRATURA — ESP32-C3 + relè su GPIO4 + ESP-NOW, collegata al firmware Salem (salem/salem.ino)
// Dopo ogni apertura la scheda si riavvia da sola, e se si blocca il watchdog la fa
// ripartire entro 3 s con la serratura chiusa (protezione contro i disturbi del solenoide).
//
// Stesso sketch per le due cassette: cambia solo CASSETTA qui sotto.
//   CASSETTA 1 = bonus della fase 1: si apre con la combinazione del gatto, SOLO DA VICINO
//   CASSETTA 2 = orologio della fase 4: si apre con l'orario giusto
// Relè su NO: a riposo NESSUNA corrente = serratura CHIUSA.
//
// Arduino IDE > Strumenti: Scheda ESP32C3 Dev Module, USB CDC On Boot: Enabled

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_task_wdt.h>

// ================== IMPOSTAZIONI ==================
const int CASSETTA = 1;                   // 1 = cassetta bonus, 2 = cassetta orologio

// Cassetta 1: si apre solo se il segnale dell'orologio e' piu' forte di questo (dBm).
// Il Monitor seriale stampa il segnale di ogni messaggio: avvicinati alla distanza
// voluta e copia qui il valore che leggi.
const int RSSI_MIN = -90;

// Cassetta 2: orario giusto (ore sul quadrante 1-12, minuti, secondi)
// Esempio: 10:10:10 sul quadrante
const int  TARGET_H = 10;
const int  TARGET_M = 10;
const int  TARGET_S = 10;
const bool USE_SECONDS = true;            // false = contano solo ore e minuti

const int  PIN_RELE = 4;
const bool RELAY_ACTIVE_HIGH = true;      // se il relè scatta al contrario, metti false
const unsigned long OPEN_MS     = 5000;   // quanto resta aperta (5 s)
const unsigned long MAX_OPEN_MS = 6000;   // sicurezza: non resta mai alimentata piu' di cosi' (il solenoide scalda)

const bool RESTART_AFTER_OPEN = true;     // riavvia la scheda dopo ogni apertura
const unsigned long RESTART_DELAY_MS = 500;  // pausa tra chiusura e riavvio
const uint32_t WDT_MS = 3000;             // se la scheda si blocca, riparte entro questo tempo

const wifi_power_t TX_POWER = WIFI_POWER_8_5dBm;  // uguale a quella degli orologi
// ==================================================

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

volatile bool newMsg = false;
volatile uint8_t rtype, rh, rm, rs, rseq;
volatile int rrssi = 0;
volatile uint8_t rmac[6];
volatile uint32_t rxAny = 0;   // tutti i pacchetti ESP-NOW arrivati, anche quelli scartati

bool isOpen = false;
unsigned long openedAt = 0, restartAt = 0;

// Ultima richiesta servita, per riconoscere i doppioni
uint8_t lastMac[6] = {0};
uint8_t lastSeq = 0;
uint8_t lastRes = RES_WRONG;
unsigned long lastAt = 0;
bool haveLast = false;

void relayOn()  { digitalWrite(PIN_RELE, RELAY_ACTIVE_HIGH ? HIGH : LOW); }
void relayOff() { digitalWrite(PIN_RELE, RELAY_ACTIVE_HIGH ? LOW : HIGH); }

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  const uint8_t *mac = info->src_addr;
  int rssi = info->rx_ctrl ? info->rx_ctrl->rssi : 0;
#else
void onRecv(const uint8_t *mac, const uint8_t *data, int len) {
  int rssi = 0;   // il core 2.x non dà la potenza del segnale: niente controllo di vicinanza
#endif
  rxAny++;
  if (len != sizeof(Msg) || newMsg) return;
  Msg m;
  memcpy(&m, data, sizeof(Msg));
  if (m.magic != MAGIC || m.box != CASSETTA) return;
  if (m.type != MSG_OPEN && m.type != MSG_TEST) return;
  rtype = m.type; rh = m.h; rm = m.m; rs = m.s; rseq = m.seq;
  rrssi = rssi;
  for (int i = 0; i < 6; i++) rmac[i] = mac[i];
  newMsg = true;
}

// Esito della consegna dell'ultima risposta (conferma radio dall'orologio)
volatile int sendStatus = -1;   // -1 in attesa, 1 consegnata, 0 persa

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
void onSent(const esp_now_send_info_t *info, esp_now_send_status_t status) {
#else
void onSent(const uint8_t *mac, esp_now_send_status_t status) {
#endif
  sendStatus = (status == ESP_NOW_SEND_SUCCESS) ? 1 : 0;
}

// Risposta solo all'orologio che ha scritto
void sendReply(const uint8_t *mac, uint8_t seq, uint8_t res) {
  if (!esp_now_is_peer_exist(mac)) {
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
  }
  Msg r;
  r.magic = MAGIC;
  r.type = MSG_REPLY;
  r.box = CASSETTA;
  r.h = res;
  r.m = 0;
  r.s = 0;
  r.seq = seq;
  sendStatus = -1;
  esp_now_send(mac, (uint8_t*)&r, sizeof(r));
}

// Prima di aprire: manda la risposta e aspetta che l'orologio confermi di averla ricevuta
// (fino a 3 tentativi). Cosi' l'orologio dice "APERTO" anche se il colpo del solenoide
// poi disturba la scheda. Ritorna true se la conferma e' arrivata.
bool sendReplyConfirmed(const uint8_t *mac, uint8_t seq, uint8_t res) {
  for (int attempt = 0; attempt < 3; attempt++) {
    sendReply(mac, seq, res);
    unsigned long t0 = millis();
    while (sendStatus < 0 && millis() - t0 < 100) delay(1);
    if (sendStatus == 1) return true;
  }
  return false;
}

void apri() {
  Serial.println(">>> APERTA");
  relayOn();
  isOpen = true;
  openedAt = millis();
}

void chiudi() {
  relayOff();
  isOpen = false;
  Serial.println(">>> richiusa");
  if (RESTART_AFTER_OPEN) restartAt = millis() + RESTART_DELAY_MS;
}

void initEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_ps(WIFI_PS_NONE);                    // radio sempre sveglia: col risparmio perde pacchetti
  WiFi.setTxPower(TX_POWER);                        // SuperMini: a piena potenza spesso non trasmette
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);   // stesso canale degli orologi
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW: errore di avvio");
    return;
  }
  esp_now_register_recv_cb(onRecv);
  esp_now_register_send_cb(onSent);
}

void startWatchdog() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t cfg;
  cfg.timeout_ms = WDT_MS;
  cfg.idle_core_mask = 0;
  cfg.trigger_panic = true;
  esp_task_wdt_reconfigure(&cfg);
#else
  esp_task_wdt_init(WDT_MS / 1000, true);
#endif
  esp_task_wdt_add(NULL);
}

// Decide se aprire. Le prove dal menu staff (MSG_TEST) aprono sempre.
uint8_t evaluate(uint8_t type, int h, int m, int s, int rssi) {
  if (type == MSG_TEST) return RES_OPEN;
  if (CASSETTA == 1) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    if (rssi < RSSI_MIN) return RES_FAR;
#endif
    return RES_OPEN;
  }
  bool ok = (h % 12 == TARGET_H % 12) && (m == TARGET_M) && (!USE_SECONDS || s == TARGET_S);
  return ok ? RES_OPEN : RES_WRONG;
}

void setup() {
  // Prima il livello "spento", poi il pin come uscita: la serratura parte chiusa
  digitalWrite(PIN_RELE, RELAY_ACTIVE_HIGH ? LOW : HIGH);
  pinMode(PIN_RELE, OUTPUT);
  relayOff();

  Serial.begin(115200);
  delay(300);
  Serial.printf("Boot, motivo reset: %d (1=accensione, 3=riavvio voluto, 4=crash, 5/6/7=blocco, 9=BROWNOUT)\n",
                (int)esp_reset_reason());
  Serial.printf("CASSETTA %d - serratura CHIUSA (nessuna corrente).\n", CASSETTA);
  if (CASSETTA == 1) Serial.printf("Si apre solo con segnale >= %d dBm\n", RSSI_MIN);
  else Serial.printf("Combinazione: %02d:%02d:%02d\n", TARGET_H, TARGET_M, TARGET_S);

  initEspNow();
  startWatchdog();
}

void loop() {
  esp_task_wdt_reset();   // "sono vivo": se non arriva entro WDT_MS, la scheda si riavvia

  // Chiusura senza bloccare nulla
  if (isOpen && (millis() - openedAt >= OPEN_MS || millis() - openedAt >= MAX_OPEN_MS)) {
    chiudi();
  }

  // Riavvio pulito dopo l'apertura
  if (restartAt && (long)(millis() - restartAt) >= 0) {
    Serial.println("Riavvio dopo l'apertura");
    Serial.flush();
    ESP.restart();
  }

  // Richiesta da un orologio
  if (newMsg) {
    uint8_t mac[6];
    for (int i = 0; i < 6; i++) mac[i] = rmac[i];
    uint8_t seq = rseq, type = rtype;
    int h = rh, m = rm, s = rs, rssi = rrssi;

    bool duplicate = haveLast && seq == lastSeq && memcmp(mac, lastMac, 6) == 0 && millis() - lastAt < 5000;
    if (duplicate) {
      // Stesso invio ripetuto: la risposta di prima si era persa, la rimando senza riaprire
      sendReply(mac, seq, lastRes);
      Serial.printf("Ripetizione seq %d -> rimando risposta\n", seq);
    } else {
      uint8_t res = evaluate(type, h, m, s, rssi);
      bool busy = isOpen || restartAt;
      const char* RES_TXT[3] = {"sbagliato", "GIUSTO", "TROPPO LONTANO"};
      Serial.printf("Ricevuto %s %02d:%02d:%02d (seq %d, segnale %d dBm) -> %s%s\n",
                    type == MSG_TEST ? "TEST" : "codice", h, m, s, seq, rssi,
                    RES_TXT[res], busy && res == RES_OPEN ? " (gia' aperta)" : "");
      // Risposta PRIMA di aprire, con conferma di consegna: l'orologio sa che e' aperta
      // anche se subito dopo il colpo del solenoide disturba la scheda
      bool delivered = sendReplyConfirmed(mac, seq, res);
      Serial.printf("Risposta all'orologio: %s\n", delivered ? "consegnata" : "NON confermata");
      if (res == RES_OPEN && !busy) apri();
      memcpy(lastMac, mac, 6);
      lastSeq = seq;
      lastRes = res;
      lastAt = millis();
      haveLast = true;
    }
    newMsg = false;
  }

  // Test manuale da seriale
  if (Serial.available()) {
    char c = Serial.read();
    if (c == '1' && !isOpen && !restartAt) apri();
  }

  // Battito: se smette di comparire, la scheda e' bloccata
  static unsigned long hb = 0;
  if (millis() - hb > 5000) {
    hb = millis();
    Serial.printf("vivo (pacchetti radio ricevuti: %lu)\n", (unsigned long)rxAny);
  }
}
