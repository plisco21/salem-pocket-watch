// =====================================================================
//  AUTOTEST — SOLO VERSIONE DI TEST (non entra nel firmware normale)
//  Si compila solo con -DSALEM_AUTOTEST (vedi tools/upload.ps1). Senza quel flag questo file
//  non entra nello sketch e la versione pulita non contiene niente di tutto questo.
//
//  All'avvio l'orologio gioca da solo AT_CICLI volte tutto il percorso (fase 1, cassetta 1,
//  caccia, lucchetto, mappa, orologio, cassetta 2, menu staff) e controlla lo stato a ogni passo.
//  Durante il test la radio e' simulata (le serrature non si aprono). Alla fine: schermata
//  verde/rossa, click = gioco normale con radio vera (per provare le serrature dal menu).
// =====================================================================
#pragma once

const int  AT_CICLI         = 5;
const bool AT_RADIO_VERA    = false;   // true = usa le serrature vere durante il test
const bool AT_ATTENDI_CLICK = false;   // true = parte solo dopo un click (per riprenderlo)
const unsigned long AT_PASSO_MS  = 150;   // pausa tra un passo e l'altro (1500 per il video)
const bool AT_LENTO = false;              // true = corona scatto per scatto e pausa dopo ogni click
const unsigned long AT_SCATTO_MS = 140;
const unsigned long AT_CLICK_MS  = 700;
const int AT_TARGET_H = 10, AT_TARGET_M = 10, AT_TARGET_S = 10;   // orario della cassetta 2 (come lock.ino)

int atCycle = 0, atStep = 0, atSub = 0;
unsigned long atT = 0, atNextAct = 0;
int atFails = 0, atChecks = 0;
bool atStarted = false, atDone = false, atHuntRight = false, atShowResult = true;
char atLastFail[64] = "";
int atBars[4];
Passo atPath[N_BUSTE];

void atGo(int step) { atStep = step; atSub = 0; atT = millis(); }
void atNext() { atGo(atStep + 1); }
bool atElapsed(unsigned long ms) { return since(atT) >= ms; }

void atCheck(bool ok, const char* what) {
  atChecks++;
  if (ok) return;
  atFails++;
  snprintf(atLastFail, sizeof(atLastFail), "C%d P%d %s", atCycle + 1, atStep, what);
  Serial.printf("AUTOTEST FAIL ciclo %d passo %d: %s (fase %d ui %d)\n", atCycle + 1, atStep, what, phase, ui);
}

// true quando si puo' andare avanti: condizione vera, oppure tempo scaduto (= FAIL)
bool atWaitFor(bool cond, unsigned long timeout, const char* what) {
  if (cond) return true;
  if (atElapsed(timeout)) { atCheck(false, what); return true; }
  return false;
}

// Ridisegna subito la schermata attuale (modalita' lenta: si vede ogni scatto e ogni click)
void atRedraw() {
  esp_task_wdt_reset();
  unsigned long now = millis();
  switch (ui) {
    case UI_MENU:   renderMenu(); return;
    case UI_PIN:    renderPin(); return;
    case UI_MAP:    renderMap(); return;
    case UI_INTRO:  renderIntro(); return;
    case UI_STATUS: renderStatus(); return;
  }
  if (phase == PH_CLOCK) { renderClock(); return; }
  if (phase == PH_WAIT) { renderWait(now); return; }
  if (phase == PH_HUNT && huntState == HS_LOCK) { renderLock(now); return; }
  renderCat(now);
}

void atPause(unsigned long ms) {
  unsigned long t0 = millis();
  while (millis() - t0 < ms) { atRedraw(); delay(30); }
}

void atClick(unsigned long held) {
  lastInput = millis();
  applyRelease(held, millis());
  if (AT_LENTO) atPause(AT_CLICK_MS);
}

void atRotate(int steps) {
  lastInput = millis();
  if (!steps) return;
  if (!AT_LENTO) { applyRotation(steps); return; }
  int dir = steps > 0 ? 1 : -1;
  for (int i = 0; i != steps; i += dir) {   // uno scatto alla volta, come una corona vera
    applyRotation(dir);
    atRedraw();
    delay(AT_SCATTO_MS);
  }
}

int atItemIndex(int it) {
  for (int i = 0; i < nItems; i++) if (items[i] == it) return i;
  return -1;
}

void atSelect(int it) {
  int i = atItemIndex(it);
  if (i < 0) { atCheck(false, "icona non presente"); return; }
  atRotate(i - sel);
}

void atMorse(const char* code) {
  startListening(millis());
  for (const char* c = code; *c; c++) atClick(*c == '-' ? 600 : 120);
}

// Scheda aperta: click finche' si chiude (a pagine)
bool atCloseModal() {
  if (ui != UI_MODAL) return true;
  if (atSub++ > 8) { atCheck(false, "scheda non si chiude"); ui = UI_MAIN; return true; }
  atClick(120);
  return ui != UI_MODAL;
}

void atRun() {
  lightWake();   // schermo sempre acceso durante il test
  if (!atStarted) {
    if (!AT_ATTENDI_CLICK || digitalRead(PIN_ENC_SW) == LOW) {
      while (digitalRead(PIN_ENC_SW) == LOW) { esp_task_wdt_reset(); delay(10); }
      atStarted = true;
      atNextAct = millis() + 1000;
      Serial.println("AUTOTEST: via!");
    }
    return;
  }
  if ((long)(millis() - atNextAct) < 0) return;
  atNextAct = millis() + AT_PASSO_MS;

  switch (atStep) {
    // ---------- inizio ciclo ----------
    case 0: {
      int t = atCycle % N_SQUADRE;
      resetAll();
      team = t;
      ui = UI_MAIN;
      listening = false;
      setPhase(PH_FREE);
      atHuntRight = false;
      Serial.printf("=== AUTOTEST ciclo %d/%d - squadra %d %s ===\n", atCycle + 1, AT_CICLI, team + 1, SQUADRE[team]);
      atNext();
      break;
    }
    // ---------- fase 1 ----------
    case 1:
      schedulePhase(PH_BONUS);
      atCheck(phase == PH_WAIT, "attesa fase 1");
      atNext();
      break;
    case 2:
      atClick(120);
      atCheck(phase == PH_WAIT && anim == A_NONE, "corona ferma durante l'attesa");
      waitElapsed = START_DELAY_MIN * MIN_MS;   // l'attesa scade
      atNext();
      break;
    case 3:
      if (atWaitFor(phase == PH_BONUS, 2000, "fase 1 partita dopo l'attesa")) {
        atCheck(requestActive && questIdx == 0, "prima richiesta subito");
        atCheck(!barVal[0] && !barVal[1] && !barVal[2] && !barVal[3], "barre vuote all'avvio");
        atNext();
      }
      break;
    case 4: {   // mossa sbagliata: NON ORA, nessuna barra sale
      if (anim != A_NONE) break;
      int wrong = (QUEST_ORDER[questIdx] + 1) % 4;
      memcpy(atBars, barVal, sizeof(atBars));
      atSelect(wrong);
      atClick(120);
      atCheck(strcmp(bubbleText, TXT_NOT_NOW) == 0, "NON ORA su mossa non richiesta");
      atCheck(memcmp(atBars, barVal, sizeof(atBars)) == 0 && anim == A_NONE, "mossa non richiesta non riempie");
      atNext();
      break;
    }
    case 5: {   // mossa richiesta
      int want = QUEST_ORDER[questIdx];
      int before = barVal[want], q = questIdx;
      atSelect(want);
      atClick(120);
      atCheck(questIdx == q + 1, "richiesta contata");
      atCheck(barVal[want] == min(100, before + QUEST_STEP), "barra +1/4");
      atCheck(anim != A_NONE, "animazione della mossa");
      atNext();
      break;
    }
    case 6:
      if (atWaitFor(anim == A_NONE, 9000, "fine animazione")) atNext();
      break;
    case 7:
      if (questIdx >= N_RICHIESTE) { atGo(9); break; }
      atCheck(!requestActive, "nessuna richiesta subito dopo la mossa");
      questWait = QUEST_GAP_MIN * MIN_MS;   // i 12 minuti scadono
      atNext();
      break;
    case 8:
      if (atWaitFor(requestActive, 2000, "richiesta successiva dopo 12 min")) atGo(4);
      break;
    case 9:
      if (atWaitFor(ui == UI_MODAL, 3000, "scheda del bonus")) {
        atCheck(bonusUnlocked, "bonus sbloccato");
        atCheck(barVal[0] == 100 && barVal[1] == 100 && barVal[2] == 100 && barVal[3] == 100, "barre piene");
        atCheck(modalPages() >= 1, "scheda con testo");
        atNext();
      }
      break;
    case 10:
      if (atCloseModal()) atNext();
      break;
    case 11:
      atSelect(IT_LETTER);
      atClick(120);
      atCheck(ui == UI_MODAL, "busta rilegge il messaggio");
      atNext();
      break;
    case 12:
      if (atCloseModal()) atNext();
      break;
    case 13:   // CAREZZA x3 entro 3 s davanti alla cassetta 1 (tutti nello stesso passo)
      if (anim != A_NONE) break;
      atSelect(BONUS_MOVE);
      for (int i = 0; i < BONUS_CLICKS; i++) atClick(120);
      atNext();
      break;
    case 14:
      if (atWaitFor(box1Open && ui == UI_MODAL, 3000, "cassetta 1 aperta + scheda")) {
        atCheck(phase == PH_FREE, "gioco libero dopo la cassetta 1");
        atNext();
      }
      break;
    case 15:
      if (atCloseModal()) atNext();
      break;
    // ---------- caccia ----------
    case 16:
      if (anim != A_NONE) break;
      schedulePhase(PH_HUNT);
      waitElapsed = START_DELAY_MIN * MIN_MS;
      atNext();
      break;
    case 17:
      if (atWaitFor(phase == PH_HUNT, 2000, "caccia partita")) {
        atCheck(huntCount == 0 && huntState == HS_COUNT, "caccia da 0/8");
        memcpy(atPath, PERCORSI[team], sizeof(atPath));
        atPath[0].scenario ^= 1;   // primo giro: ordine sbagliato apposta
        atNext();
      }
      break;
    case 18: {   // mossa atSub del percorso: scenario, poi mossa
      if (anim != A_NONE) break;
      const Passo &p = atPath[atSub];
      atSelect(p.scenario == DOLCETTO ? IT_SWEET : IT_TRICK);
      atClick(120);
      atCheck(scenario == p.scenario, "scenario scelto");
      atSelect(p.mossa);
      atClick(120);
      atSub++;
      atCheck(huntCount == atSub, "contatore caccia");
      if (atSub >= N_BUSTE) { atStep = 19; atT = millis(); }
      break;
    }
    case 19:
      if (!atHuntRight) {
        if (atWaitFor(huntState == HS_WAIT, 9000, "ordine sbagliato -> attesa")) {
          atCheck(huntCount == 0, "ordine sbagliato -> 0/8");
          atGo(20);
        }
      } else if (atWaitFor(huntState == HS_LOCK, 9000, "ordine giusto -> lucchetto")) {
        atGo(23);
      }
      break;
    case 20:
      if (anim != A_NONE) break;
      atSelect(IT_FOOD);
      atClick(120);
      atCheck(strcmp(bubbleText, TXT_NOT_NOW) == 0 && huntCount == 0, "mosse bloccate durante l'attesa");
      huntWait = HUNT_ERROR_WAIT_MIN * MIN_MS;   // i 10 minuti scadono
      atNext();
      break;
    case 21:
      if (atWaitFor(huntState == HS_COUNT, 2000, "fine attesa caccia")) {
        memcpy(atPath, PERCORSI[team], sizeof(atPath));
        atHuntRight = true;
        atGo(18);
      }
      break;
    case 23: {   // password sbagliata
      const char* pw = PASSWORD[team];
      int d[3] = {(pw[0] - '0' + 1) % 10, pw[1] - '0', pw[2] - '0'};
      for (int i = 0; i < 3; i++) { atRotate(d[i] - lockDigits[lockPos]); atClick(120); }
      atCheck(lockErrUntil > millis() && huntState == HS_LOCK, "password sbagliata rifiutata");
      atNext();
      break;
    }
    case 24:
      if (atWaitFor(lockErrUntil == 0 && lockPos == 0, 3000, "lucchetto di nuovo pronto")) atNext();
      break;
    case 25: {   // password giusta
      const char* pw = PASSWORD[team];
      for (int i = 0; i < 3; i++) { atRotate((pw[i] - '0') - lockDigits[lockPos]); atClick(120); }
      atCheck(mapUnlocked && ui == UI_MAP && phase == PH_FREE, "password giusta -> mappa");
      atNext();
      break;
    }
    case 26:
      if (!atElapsed(1500)) break;
      atClick(120);
      atCheck(ui == UI_MAIN, "mappa chiusa col click");
      atNext();
      break;
    case 27:
      atSelect(IT_MAP);
      atClick(120);
      atCheck(ui == UI_MAP, "mappa dall'icona MAPPA");
      atNext();
      break;
    case 28:
      if (!atElapsed(800)) break;
      atClick(120);
      atCheck(ui == UI_MAIN, "mappa richiusa");
      atNext();
      break;
    // ---------- orologio ----------
    case 29:
      atMorse(CODE_TO_CLOCK);
      atNext();
      break;
    case 30:
      if (atWaitFor(ui == UI_INTRO, 4000, "combinazione -> testo di passaggio")) {
        atCheck(phase == PH_CLOCK, "fase orologio");
        atNext();
      }
      break;
    case 31:
      if (atWaitFor(ui == UI_MAIN, 9000, "fine testo -> quadrante")) atNext();
      break;
    case 32:   // orario sbagliato (3:00:00)
      atRotate(3 - hours);
      atClick(1200);
      atNext();
      break;
    case 33:
      if (atWaitFor(!awaiting && strcmp(statusText, "ERRATO") == 0, 3000, "orario sbagliato -> ERRATO")) {
        atCheck(!box2Open, "cassetta 2 resta chiusa");
        atNext();
      }
      break;
    case 34:   // orario giusto (AT_TARGET): ore, click, minuti, click, secondi, pressione lunga
      atRotate(AT_TARGET_H - hours);
      atClick(120);
      atRotate(AT_TARGET_M - minutes);
      atClick(120);
      atRotate(AT_TARGET_S - seconds);
      atCheck(hours == AT_TARGET_H && minutes == AT_TARGET_M && seconds == AT_TARGET_S, "orario giusto impostato");
      atClick(1200);
      atNext();
      break;
    case 35:
      if (atWaitFor(box2Open, 3000, "cassetta 2 aperta")) atNext();
      break;
    case 36:
      if (atWaitFor(phase == PH_FREE, 5000, "ritorno al gatto dopo APERTO")) atNext();
      break;
    // ---------- menu staff ----------
    case 37:
      atMorse(CODE_STAFF);
      atNext();
      break;
    case 38:
      if (atWaitFor(ui == UI_PIN, 4000, "combinazione staff -> PIN")) {
        for (int i = 0; i < 4; i++) { atRotate((STAFF_PIN[i] - '0') - pinDigits[pinPos]); atClick(120); }
        atCheck(ui == UI_MENU, "PIN giusto -> menu");
        atNext();
      }
      break;
    case 39:
      atRotate(M_EXIT - menuSel);
      atClick(120);
      atCheck(ui == UI_MAIN, "ESCI dal menu");
      atNext();
      break;
    // ---------- fine ciclo ----------
    default:
      atCycle++;
      Serial.printf("=== fine ciclo %d: controlli %d, errori %d ===\n", atCycle, atChecks, atFails);
      if (atCycle >= AT_CICLI) {
        atDone = true;
        resetAll();
        team = 0;
        buildItems();
        saveNow();
        Serial.printf("AUTOTEST FINITO: %d cicli, %d controlli, %d errori%s%s\n", atCycle, atChecks, atFails,
                      atFails ? " - ultimo: " : "", atLastFail);
      } else {
        atGo(0);
      }
      break;
  }
}

// ---- Schermate ----
void renderAutotestStart() {
  fb.fillSprite(COL_OUT);
  fb.fillCircle(CX, CY, 119, COL_NAVY);
  fb.setTextDatum(lgfx::middle_center);
  fb.setTextColor(COL_WHITE);
  fb.setFont(&fonts::FreeSansBold12pt7b);
  fb.drawString("AUTOTEST", CX, 90);
  fb.setFont(&fonts::Font2);
  char l[32];
  snprintf(l, sizeof(l), "%d CICLI COMPLETI", AT_CICLI);
  fb.drawString(l, CX, 122);
  fb.setTextColor(COL_GOLD);
  fb.drawString("click per iniziare", CX, 150);
  fb.pushSprite(0, 0);
}

void renderAutotest() {
  bool ok = atFails == 0;
  fb.fillSprite(COL_OUT);
  fb.fillCircle(CX, CY, 119, ok ? COL_OK : COL_ERR);
  fb.setTextDatum(lgfx::middle_center);
  fb.setTextColor(COL_WHITE);
  fb.setFont(&fonts::FreeSansBold12pt7b);
  fb.drawString(ok ? "AUTOTEST OK" : "AUTOTEST ERRORI", CX, 60);
  fb.setFont(&fonts::Font2);
  char l[48];
  snprintf(l, sizeof(l), "%d CICLI - %d CONTROLLI", atCycle, atChecks);
  fb.drawString(l, CX, 95);
  snprintf(l, sizeof(l), "ERRORI: %d", atFails);
  fb.drawString(l, CX, 115);
  if (!ok) {
    char w[3][LINE_LEN];
    int n = wrapText(atLastFail, 180, w, 3);
    for (int i = 0; i < n; i++) fb.drawString(w[i], CX, 140 + i * 17);
  }
  fb.drawString("click = gioco normale", CX, 200);
  fb.pushSprite(0, 0);
}

// ---- Radio simulata (solo mentre il test gira) ----
unsigned long simReplyAt = 0;

bool atSimSend(int kind) {
  if (AT_RADIO_VERA || !atStarted || atDone) return false;
  Serial.printf("Invio SIMULATO cassetta %d orario %d:%02d:%02d\n",
                (kind == REQ_BOX1 || kind == REQ_TEST1) ? 1 : 2, hours, minutes, seconds);
  awaiting = true;
  sentAt = millis();
  tries = SEND_TRIES;
  simReplyAt = millis() + 300;
  return true;
}

// Stessa regola delle serrature vere: cassetta 2 = AT_TARGET (uguale a TARGET_H/M/S in lock.ino)
void atSimLoop() {
  if (!simReplyAt || (long)(millis() - simReplyAt) < 0) return;
  simReplyAt = 0;
  if (reqKind == REQ_BOX2) replyState = (hours % 12 == AT_TARGET_H && minutes == AT_TARGET_M && seconds == AT_TARGET_S) ? RES_OPEN : RES_WRONG;
  else replyState = RES_OPEN;
}

// ---- Collegamento con setup e loop di salem.ino ----
void atSetup() {
  Serial.printf("!!! VERSIONE DI TEST: autotest di %d cicli, radio %s durante il test !!!\n",
                AT_CICLI, AT_RADIO_VERA ? "VERA" : "SIMULATA");
}

void atLoop() {
  if (!atDone) { atRun(); return; }
  // Test finito: la schermata del risultato resta finche' non si fa click, poi gioco normale
  if (atShowResult && digitalRead(PIN_ENC_SW) == LOW) {
    while (digitalRead(PIN_ENC_SW) == LOW) { esp_task_wdt_reset(); delay(10); }
    atShowResult = false;
    lastInput = millis();
    Serial.println("AUTOTEST: risultato chiuso, gioco normale");
  }
}

bool atDraw() {
  if (atDone && atShowResult) { lightWake(); renderAutotest(); return true; }
  if (!atStarted) { renderAutotestStart(); return true; }
  return false;
}
