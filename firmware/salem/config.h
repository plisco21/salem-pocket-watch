// =====================================================================
//  SALEM — FILE DI CONFIGURAZIONE (specifiche versione 2, 25 ottobre 2026)
//  Tutto quello che si personalizza sta qui. Il resto del codice non va toccato.
//  Testi: niente lettere accentate (il carattere del display non le ha):
//  scrivere E', PERCHE', DIRA'.
// =====================================================================
#pragma once

// ---------------------------------------------------------------------
//  DISPLAY E CORONA
// ---------------------------------------------------------------------
const bool PANEL_INVERT     = true;    // false se i colori risultano invertiti (gatto non nero)
const bool PANEL_RGB_ORDER  = false;   // false = colori giusti (quadrante marrone, fondo beige)
const bool INVERT_DIR       = false;   // true se la corona gira al contrario
const int  STEPS_PER_DETENT = 4;       // impulsi dell'encoder per ogni scatto
const int  SCREEN_ROTATION  = 1;       // 0 = normale, 1 = 90 gradi orario, 2 = 180, 3 = 270
// Velocita' SPI del display: 27 MHz = animazioni fluide. Se compaiono righe o colori
// sbagliati (fili lunghi nel case), scendere a 20 o 10 MHz.
const uint32_t SPI_FREQ     = 27000000;

// Retroilluminazione (BLK del display su GPIO5)
const int  PIN_BLK          = 5;
const bool BLK_ACTIVE_HIGH  = true;    // false se lo schermo si spegne invece di accendersi
const unsigned long BACKLIGHT_TIMEOUT_S = 60;   // si spegne dopo questi secondi senza toccare la corona

// ---------------------------------------------------------------------
//  RISPARMIO ENERGETICO
//  Durante le prove col cavo USB conviene SLEEP_ENABLED = false: nel sonno
//  il monitor seriale si stacca. Se qualcosa non va, false su tutti e due
//  riporta al comportamento di prima (radio sempre accesa, niente sonno).
// ---------------------------------------------------------------------
const bool SLEEP_ENABLED   = true;      // sonno leggero a schermo spento
const bool RADIO_ON_DEMAND = true;     // radio accesa solo durante un invio
const unsigned long SLEEP_CHUNK_MS = 3000;   // dorme a tratti (sotto i 5 s del watchdog)

// ---------------------------------------------------------------------
//  RADIO (ESP-NOW verso le serrature)
// ---------------------------------------------------------------------
const wifi_power_t TX_POWER = WIFI_POWER_8_5dBm;  // SuperMini: oltre ~11 dBm spesso non trasmette
const int  SEND_TRIES        = 5;       // tentativi per ogni invio
const unsigned long RETRY_MS = 250;     // pausa tra i tentativi
const unsigned long REPLY_TIMEOUT_MS = 2000;


// ---------------------------------------------------------------------
//  AVVIO DELLE FASI DAL MENU STAFF
//  AVVIA FASE 1 / AVVIA FASE 2 partono dopo questa attesa (Salem dorme).
//  SALTA AVANTI nel menu le fa partire subito (e fa avanzare richieste e caccia).
// ---------------------------------------------------------------------
const unsigned long START_DELAY_MIN = 10;

// ---------------------------------------------------------------------
//  BARRE DEL GATTO (0-100) nel gioco libero e nella caccia
//  Ordine: cibo, affetto, pulizia, svago
// ---------------------------------------------------------------------
const int BAR_START[4]        = {60, 50, 70, 50};   // valori dopo il reset totale
const int BAR_INC[4]          = {10, 10, 10, 10};   // quanto sale ogni mossa (1/10 di barra)
const int BAR_DECAY_SEC[4]    = {15, 12, 24, 20};   // ogni quanti secondi scende di 1 punto (piena -> vuota in 20-40 min)
const int FULL_HOLD_SEC       = 60;                 // a 100 resta ferma per questi secondi
const int WALK_CLEAN_COST     = 5;                  // lo spasso sporca un po'
const int WALK_FOOD_COST      = 3;                  // e mette un po' di fame
const int LOW_THRESHOLD       = 30;                 // sotto questo valore si lamenta ed e' triste
const int COMPLAINT_SEC       = 20;                 // ogni quanto si lamenta

// ---------------------------------------------------------------------
//  FASE 1 — QUEST A RICHIESTE
//  Barre vuote all'avvio. Salem chiede una mossa; fatta quella, la barra sale
//  di QUEST_STEP e la richiesta successiva arriva QUEST_GAP_MIN minuti dopo.
//  Le mosse non richieste non riempiono (Salem dice TXT_NOT_NOW).
//  Barre ferme. Dopo BONUS_DURATION_MIN il bonus sparisce in silenzio.
// ---------------------------------------------------------------------
const int N_RICHIESTE = 16;
const uint8_t QUEST_ORDER[N_RICHIESTE] = {   // esempio
  CIBO, CIBO, SPASSO, CIBO,
  PULIZIA, SPASSO, PULIZIA, PULIZIA,
  CAREZZA, CAREZZA, CAREZZA, PULIZIA,
  CAREZZA, SPASSO, CIBO, SPASSO,
};
const int  QUEST_STEP               = 25;    // 1/4 di barra per ogni richiesta fatta
const unsigned long QUEST_GAP_MIN   = 12;    // minuti tra la mossa fatta e la richiesta successiva
const unsigned long REQUEST_REPEAT_SEC = 20; // il fumetto della richiesta si ripete
const unsigned long BONUS_DURATION_MIN = 240;   // 4 ore dall'avvio (dopo l'attesa)
const bool SHOW_BONUS_TIMER   = false;          // true = tempo rimasto visibile sul display
// Combinazione davanti alla cassetta 1: quella mossa, N click entro la finestra
const uint8_t BONUS_MOVE      = CAREZZA;
const int  BONUS_CLICKS       = 3;
const unsigned long BONUS_WINDOW_MS = 3000;

// ---------------------------------------------------------------------
//  FASE 2 — CACCIA DOLCETTO / SCHERZETTO
//  Percorso di ogni squadra: 8 passi {scenario, mossa}, in ordine.
//  Scenari: DOLCETTO, SCHERZETTO   Mosse: CIBO, CAREZZA, PULIZIA, SPASSO ("gioca")
// ---------------------------------------------------------------------
const uint8_t START_SCENARIO = DOLCETTO;   // scenario all'avvio della caccia

// ESEMPIO: nomi, password e percorsi veri vanno scelti per il proprio gioco
const char* SQUADRE[N_SQUADRE] = {
  "TEAM 1", "TEAM 2", "TEAM 3", "TEAM 4",
  "TEAM 5", "TEAM 6", "TEAM 7", "TEAM 8",
};

// Password del lucchetto (3 cifre) dopo le 8 mosse giuste
const char* PASSWORD[N_SQUADRE] = {
  "111", "222", "333", "444", "555", "666", "777", "888",
};

const Passo PERCORSI[N_SQUADRE][N_BUSTE] = {
  // 1 TEAM 1
  {{DOLCETTO, PULIZIA}, {SCHERZETTO, CAREZZA}, {DOLCETTO, SPASSO}, {SCHERZETTO, CIBO},
   {DOLCETTO, CIBO}, {SCHERZETTO, CIBO}, {DOLCETTO, PULIZIA}, {SCHERZETTO, CIBO}},
  // 2 TEAM 2
  {{SCHERZETTO, CAREZZA}, {DOLCETTO, CIBO}, {SCHERZETTO, CIBO}, {DOLCETTO, SPASSO},
   {SCHERZETTO, SPASSO}, {DOLCETTO, CIBO}, {SCHERZETTO, CAREZZA}, {DOLCETTO, CIBO}},
  // 3 TEAM 3
  {{DOLCETTO, SPASSO}, {SCHERZETTO, CIBO}, {DOLCETTO, CIBO}, {SCHERZETTO, CAREZZA},
   {DOLCETTO, CIBO}, {SCHERZETTO, SPASSO}, {DOLCETTO, CIBO}, {SCHERZETTO, CAREZZA}},
  // 4 TEAM 4
  {{SCHERZETTO, CIBO}, {DOLCETTO, CAREZZA}, {SCHERZETTO, PULIZIA}, {DOLCETTO, SPASSO},
   {SCHERZETTO, CAREZZA}, {DOLCETTO, CIBO}, {SCHERZETTO, PULIZIA}, {DOLCETTO, CAREZZA}},
  // 5 TEAM 5
  {{DOLCETTO, CIBO}, {SCHERZETTO, CAREZZA}, {DOLCETTO, PULIZIA}, {SCHERZETTO, CIBO},
   {DOLCETTO, CIBO}, {SCHERZETTO, CIBO}, {DOLCETTO, CAREZZA}, {SCHERZETTO, SPASSO}},
  // 6 TEAM 6
  {{SCHERZETTO, SPASSO}, {DOLCETTO, PULIZIA}, {SCHERZETTO, SPASSO}, {DOLCETTO, SPASSO},
   {SCHERZETTO, PULIZIA}, {DOLCETTO, PULIZIA}, {SCHERZETTO, CAREZZA}, {DOLCETTO, CAREZZA}},
  // 7 TEAM 7
  {{DOLCETTO, CAREZZA}, {SCHERZETTO, CIBO}, {DOLCETTO, PULIZIA}, {SCHERZETTO, SPASSO},
   {DOLCETTO, PULIZIA}, {SCHERZETTO, SPASSO}, {DOLCETTO, PULIZIA}, {SCHERZETTO, CIBO}},
  // 8 TEAM 8
  {{SCHERZETTO, CIBO}, {DOLCETTO, SPASSO}, {SCHERZETTO, CAREZZA}, {DOLCETTO, PULIZIA},
   {SCHERZETTO, CAREZZA}, {DOLCETTO, SPASSO}, {SCHERZETTO, SPASSO}, {DOLCETTO, CIBO}},
};

// Ordine sbagliato: si torna a 0/8 e per questi minuti le mosse non contano
const unsigned long HUNT_ERROR_WAIT_MIN = 10;
// Password sbagliata: si riprova solo la password (le 8 mosse restano valide)

// ---------------------------------------------------------------------
//  COMBINAZIONI SEGRETE
//  Si tiene premuta la corona per HOLD_MS (compare una zampina), poi si fa
//  la sequenza: '.' = click corto, '-' = click lungo (almeno LONG_CLICK_MS).
// ---------------------------------------------------------------------
const char* CODE_STAFF    = "-.-";          // menu staff (poi il PIN), funziona ovunque
const char* STAFF_PIN     = "0000";         // PIN a 4 cifre del menu staff (cambiarlo!)
const char* CODE_TO_CLOCK = "..-..";        // gioco libero -> orologio (esempio)
const char* CODE_TO_CAT   = "-...-";        // orologio -> gattino
const bool CLOCK_ONLY_AFTER_MAP   = true;   // l'orologio si sblocca solo dopo la mappa
const bool CAT_ONLY_AFTER_BOX2    = true;   // si torna al gatto solo dopo la cassetta 2
const unsigned long HOLD_MS       = 3000;   // pressione per iniziare una combinazione
const unsigned long LONG_CLICK_MS = 400;    // click lungo
const unsigned long LISTEN_MS     = 5000;   // tempo per iniziare la sequenza
const unsigned long CODE_GAP_MS   = 1500;   // pausa che chiude la sequenza

// ---------------------------------------------------------------------
//  OROLOGIO
// ---------------------------------------------------------------------
const int CLOCK_START_H = 12, CLOCK_START_M = 0, CLOCK_START_S = 0;
const unsigned long SEND_MIN_MS = 1000;      // pressione minima per inviare l'orario
const unsigned long CLOCK_INTRO_MS = 5500;   // quanto resta TXT_CLOCK_INTRO prima del quadrante
const unsigned long FINAL_DELAY_MS = 2500;   // quanto resta "APERTO" prima di tornare al gatto
// L'orario giusto si imposta nello sketch della serratura (TARGET_H/M/S in lock.ino).

// ---------------------------------------------------------------------
//  FRASI
//  "Fumetto" = una o due righe brevi (max ~13 caratteri per riga).
//  "Scheda"  = messaggio lungo, a pagine: click = pagina successiva.
// ---------------------------------------------------------------------
// Fumetti (Salem e' femmina: SPORCA, PULITA, CURATA)
const char* TXT_HELLO       = "MIAO!";
const char* TXT_ACTION[4]   = {"GNAM GNAM!", "PURRR...", "CHE PULITA!", "ANDIAMO!"};
const char* TXT_LOW[4]      = {"HO FAME...", "COCCOLE?", "SONO SPORCA", "USCIAMO?"};
const char* TXT_REQUEST[4]  = {"HO FAME!", "UNA CAREZZA?", "MI SENTO TANTO SPORCA!", "ANDIAMO FUORI?"};
const char* TXT_NOT_NOW     = "NON ORA...";
const char* TXT_WALK_BACK   = "CHE BELLO!";
const char* TXT_SWEET       = "DOLCETTO!";
const char* TXT_TRICK       = "SCHERZETTO!";
const char* TXT_GO_CLOSER   = "AVVICINATI!";
const char* TXT_NO_RADIO    = "NON TI SENTO";
// Schede
const char* TXT_BONUS_MSG   = "GRAZIE PER AVERMI CURATA. ORA CERCA LA CASSETTA E ACCAREZZAMI 3 VOLTE";
const char* TXT_QUEST_OK    = "UN PICCOLO DONO PER QUELLO CHE HAI FATTO PER ME.";
const char* TXT_MAP_OK      = "";   // scheda dopo la mappa ("" = nessuna, solo l'immagine)
const char* TXT_CLOCK_INTRO = "CHE ORE SONO?";
