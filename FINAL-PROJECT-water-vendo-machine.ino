#include <WiFi.h>
#include <HTTPClient.h>
#include <LiquidCrystal_I2C.h>
#include <Wire.h>
#include <time.h>
#include <Preferences.h>
#include <string.h>

// -------------------- LCD --------------------
LiquidCrystal_I2C lcd(0x27, 16, 2);

// -------------------- WIFI --------------------
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// true  = WiFi + Firebase enabled
// false = full offline mode, logs saved locally only
#define WIFI_ENABLED false

// -------------------- FIREBASE --------------------
#define FIREBASE_PROJECT_ID "YOUR_FIREBASE_PROJECT_ID"
#define FIREBASE_API_KEY "YOUR_FIREBASE_API_KEY"

// -------------------- DEBUG --------------------
#define COIN_DEBUG true
#define LCD_DEBUG_PULSES false

// -------------------- VENDING UX --------------------
// false = one cup per selection, keeps remaining balance
// true  = auto-dispense as many cups as balance allows
#define AUTO_MULTI_CUP false

// -------------------- ESP32 30-PIN SAFE PINS --------------------
#define SDA_PIN 21
#define SCL_PIN 22

#define coinSlotPin 34   // GPIO34 needs external 10k pull-up to 3.3V
#define pumpPin 26
#define coolerPin 27

#define btnCold 18
#define btnRegular 19
#define resetBtn 23

// -------------------- CONSTANTS --------------------
const int maxCups = 50;

const int regularPrice = 3;
const int coldPrice = 5;

const unsigned long pumpDuration = 11500;
const unsigned long pumpTimeout = 20000;
const unsigned long cupGapDuration = 700;

// New: cooler runs first for 10 seconds before pump starts
const unsigned long coldPreCoolDuration = 120000;

const int coinPulseTimeout = 300;
const unsigned long coinRejectedDisplayTime = 1500;

#define MAX_OFFLINE_LOGS 100

// -------------------- COIN RULES --------------------
struct CoinRule {
  const char *label;
  int minPulse;
  int maxPulse;
  int value;
};

CoinRule coinRules[] = {
  {"1 PESO", 1, 2, 1},
  {"5 PESO", 4, 10, 5},
  {"10 PESO", 15, 20, 10}
};

const int coinRuleCount = sizeof(coinRules) / sizeof(coinRules[0]);

// -------------------- FSM --------------------
enum State {
  IDLE,
  ACCEPTING_COINS,
  PRE_COOLING,
  DISPENSING,
  CUP_GAP,
  COIN_REJECTED,
  OUT_OF_WATER,
  ERROR_STATE
};

State currentState = ERROR_STATE;
State lastDisplayedState = ERROR_STATE;
State resumeAfterMessage = IDLE;

// -------------------- BUTTON --------------------
struct ButtonState {
  uint8_t pin;
  int last;
  int current;
  bool pressed;
};

ButtonState coldButton = {btnCold, HIGH, HIGH, false};
ButtonState regularButton = {btnRegular, HIGH, HIGH, false};
ButtonState resetButton = {resetBtn, HIGH, HIGH, false};

// -------------------- OFFLINE TRANSACTION --------------------
struct TransactionLog {
  int amount;
  bool coldSelected;
  bool timeSynced;
  uint32_t uptimeMs;
  char timestamp[25];
};

TransactionLog offlineQueue[MAX_OFFLINE_LOGS];
int offlineCount = 0;

// -------------------- GLOBAL VARIABLES --------------------
Preferences prefs;
bool prefsReady = false;

volatile int pulseBuffer = 0;
volatile bool coinInputEnabled = false;

int coinPulseCount = 0;
unsigned long coinLastPulseTime = 0;
unsigned long coinDebugTimer = 0;

int currentCoins = 0;
int cupsToDispense = 0;
int cupsSold = 0;

uint32_t totalSales = 0;
uint32_t lostLogCount = 0;

bool isCold = false;
bool wifiConnected = false;
bool timeIsSynced = false;
bool flushPending = false;

unsigned long pumpStartTime = 0;
unsigned long preCoolStartTime = 0;
unsigned long cupGapStartTime = 0;
unsigned long lastButtonCheckTime = 0;
unsigned long lastWifiCheckTime = 0;
unsigned long messageStartTime = 0;

int lastRejectedPulseCount = 0;
char errorMessage[17] = "System Error";

// LCD cache
int lastDisplayedCoins = -9999;
int lastDisplayedCupsToDispense = -9999;
int lastDisplayedCupsSold = -9999;
int lastDisplayedPulseCount = -9999;
int lastDisplayedRejectedPulse = -9999;
int lastDisplayedPreCoolSeconds = -9999;

// -------------------- FUNCTION DECLARATIONS --------------------
void setState(State newState);
void resetCoinDetector();
void forceLCDRefresh();
void saveRuntimeState();
bool addOfflineTransaction(const TransactionLog &tx);
void flushOfflineQueue();
bool syncTime(unsigned long timeoutMs);
void validatePayment();
void beginWaterCycle();
void beginPreCooling();
void beginDispensing();
void handlePreCooling();

// -------------------- ISR --------------------
void IRAM_ATTR coinISR() {
  if (!coinInputEnabled) return;

  static unsigned long lastInterruptTime = 0;
  unsigned long now = millis();

  if (now - lastInterruptTime > 5) {
    pulseBuffer++;
  }

  lastInterruptTime = now;
}

// -------------------- STATE NAME --------------------
const char *stateName(State state) {
  switch (state) {
    case IDLE: return "IDLE";
    case ACCEPTING_COINS: return "ACCEPTING_COINS";
    case PRE_COOLING: return "PRE_COOLING";
    case DISPENSING: return "DISPENSING";
    case CUP_GAP: return "CUP_GAP";
    case COIN_REJECTED: return "COIN_REJECTED";
    case OUT_OF_WATER: return "OUT_OF_WATER";
    case ERROR_STATE: return "ERROR_STATE";
    default: return "UNKNOWN";
  }
}

// -------------------- LCD HELPERS --------------------
void printLCDLine(int row, const char *text) {
  char buffer[17];

  strncpy(buffer, text, 16);
  buffer[16] = '\0';

  lcd.setCursor(0, row);
  lcd.print(buffer);

  int len = strlen(buffer);
  for (int i = len; i < 16; i++) {
    lcd.print(' ');
  }
}

void forceLCDRefresh() {
  lastDisplayedState = ERROR_STATE;
  lastDisplayedCoins = -9999;
  lastDisplayedCupsToDispense = -9999;
  lastDisplayedCupsSold = -9999;
  lastDisplayedPulseCount = -9999;
  lastDisplayedRejectedPulse = -9999;
  lastDisplayedPreCoolSeconds = -9999;
}

// -------------------- COIN DETECTOR RESET --------------------
void resetCoinDetector() {
  noInterrupts();
  pulseBuffer = 0;
  interrupts();

  coinPulseCount = 0;
  coinLastPulseTime = 0;
}

// -------------------- CENTRALIZED STATE TRANSITION --------------------
void setState(State newState) {
  if (currentState == newState) {
    forceLCDRefresh();
    return;
  }

  State oldState = currentState;

  // Exit actions
  coinInputEnabled = false;
  resetCoinDetector();

  // Keep outputs active only during PRE_COOLING or DISPENSING.
  // PRE_COOLING: cooler ON, pump OFF
  // DISPENSING: pump ON, cooler ON only if cold
  if (newState != DISPENSING && newState != PRE_COOLING) {
    digitalWrite(pumpPin, LOW);
    digitalWrite(coolerPin, LOW);
  }

  currentState = newState;

  // Entry actions
  if (newState == IDLE || newState == ACCEPTING_COINS) {
    resetCoinDetector();
    coinInputEnabled = true;
  } else {
    coinInputEnabled = false;
  }

  if (COIN_DEBUG) {
    Serial.print("STATE: ");
    Serial.print(stateName(oldState));
    Serial.print(" -> ");
    Serial.println(stateName(newState));
  }

  forceLCDRefresh();
}

// -------------------- PREFERENCES --------------------
void saveRuntimeState() {
  if (!prefsReady) return;

  prefs.putInt("balance", currentCoins);
  prefs.putInt("cupsSold", cupsSold);
  prefs.putUInt("totalSales", totalSales);
  prefs.putUInt("lostLogs", lostLogCount);
}

void persistOfflineQueue() {
  if (!prefsReady) return;

  prefs.putInt("offCount", offlineCount);

  if (offlineCount > 0) {
    prefs.putBytes("offQueue", offlineQueue, sizeof(TransactionLog) * offlineCount);
  } else {
    prefs.remove("offQueue");
  }
}

void loadPersistentData() {
  prefsReady = prefs.begin("vendo", false);

  if (!prefsReady) {
    Serial.println("Preferences failed");
    return;
  }

  currentCoins = prefs.getInt("balance", 0);
  cupsSold = prefs.getInt("cupsSold", 0);
  totalSales = prefs.getUInt("totalSales", 0);
  lostLogCount = prefs.getUInt("lostLogs", 0);

  offlineCount = prefs.getInt("offCount", 0);

  if (offlineCount < 0 || offlineCount > MAX_OFFLINE_LOGS) {
    offlineCount = 0;
  }

  size_t loadedBytes = prefs.getBytes("offQueue", offlineQueue, sizeof(offlineQueue));
  int loadedItems = loadedBytes / sizeof(TransactionLog);

  if (loadedItems < offlineCount) {
    offlineCount = loadedItems;
  }

  Serial.print("Loaded balance: ");
  Serial.println(currentCoins);

  Serial.print("Loaded cups sold: ");
  Serial.println(cupsSold);

  Serial.print("Loaded offline logs: ");
  Serial.println(offlineCount);
}

// -------------------- TIME --------------------
bool getISOTime(char *buffer, size_t bufferSize) {
  time_t now;
  time(&now);

  if (now < 1700000000) {
    strncpy(buffer, "1970-01-01T00:00:00Z", bufferSize);
    buffer[bufferSize - 1] = '\0';
    return false;
  }

  struct tm timeInfo;
  gmtime_r(&now, &timeInfo);

  strftime(buffer, bufferSize, "%Y-%m-%dT%H:%M:%SZ", &timeInfo);
  return true;
}

bool syncTime(unsigned long timeoutMs) {
  if (!WIFI_ENABLED || !wifiConnected) return false;

  Serial.println("Syncing NTP time...");

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  unsigned long start = millis();

  while (millis() - start < timeoutMs) {
    time_t now;
    time(&now);

    if (now > 1700000000) {
      timeIsSynced = true;
      Serial.println("Time synced");
      return true;
    }

    delay(200);
  }

  timeIsSynced = false;
  Serial.println("Time sync failed");
  return false;
}

// -------------------- WIFI --------------------
void connectWiFi() {
  if (!WIFI_ENABLED) {
    Serial.println("WiFi disabled");
    wifiConnected = false;
    return;
  }

  Serial.println("Connecting to WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;

  while (WiFi.status() != WL_CONNECTED && attempts < 10) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  wifiConnected = (WiFi.status() == WL_CONNECTED);

  if (wifiConnected) {
    Serial.println("\nWiFi connected");
    syncTime(10000);
    flushPending = true;
  } else {
    Serial.println("\nWiFi failed");
  }
}

void checkWifiStatus() {
  if (!WIFI_ENABLED) return;

  if (millis() - lastWifiCheckTime > 10000) {
    lastWifiCheckTime = millis();

    bool previous = wifiConnected;
    wifiConnected = (WiFi.status() == WL_CONNECTED);

    if (!wifiConnected) {
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }

    if (!previous && wifiConnected) {
      Serial.println("WiFi reconnected");
      syncTime(5000);
      flushPending = true;
    }
  }
}

void maybeFlushOfflineQueue() {
  if (!WIFI_ENABLED) return;
  if (!wifiConnected) return;
  if (!flushPending && offlineCount == 0) return;
  if (currentState != IDLE) return;

  flushPending = false;
  flushOfflineQueue();
}

// -------------------- FIREBASE --------------------
TransactionLog makeTransaction(int amount, bool coldSelected) {
  TransactionLog tx;

  tx.amount = amount;
  tx.coldSelected = coldSelected;
  tx.uptimeMs = millis();
  tx.timeSynced = getISOTime(tx.timestamp, sizeof(tx.timestamp));

  return tx;
}

bool postTransactionToFirebase(const TransactionLog &tx) {
  if (!WIFI_ENABLED || !wifiConnected) return false;

  char url[300];

  snprintf(
    url,
    sizeof(url),
    "https://firestore.googleapis.com/v1/projects/%s/databases/(default)/documents/waterLogs?key=%s",
    FIREBASE_PROJECT_ID,
    FIREBASE_API_KEY
  );

  char json[600];

  snprintf(
    json,
    sizeof(json),
    "{"
      "\"fields\":{"
        "\"amount\":{\"integerValue\":\"%d\"},"
        "\"isCold\":{\"booleanValue\":%s},"
        "\"timestamp\":{\"timestampValue\":\"%s\"},"
        "\"timeSynced\":{\"booleanValue\":%s},"
        "\"clientUptimeMs\":{\"integerValue\":\"%lu\"}"
      "}"
    "}",
    tx.amount,
    tx.coldSelected ? "true" : "false",
    tx.timestamp,
    tx.timeSynced ? "true" : "false",
    (unsigned long)tx.uptimeMs
  );

  HTTPClient http;
  http.setTimeout(5000);

  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  int code = http.POST((uint8_t *)json, strlen(json));

  http.end();

  Serial.print("Firebase HTTP code: ");
  Serial.println(code);

  return (code >= 200 && code < 300);
}

bool addOfflineTransaction(const TransactionLog &tx) {
  if (offlineCount >= MAX_OFFLINE_LOGS) {
    lostLogCount++;
    saveRuntimeState();

    Serial.println("Offline queue full");
    return false;
  }

  offlineQueue[offlineCount] = tx;
  offlineCount++;

  persistOfflineQueue();

  Serial.print("Saved offline transaction. Queue: ");
  Serial.println(offlineCount);

  return true;
}

void flushOfflineQueue() {
  if (!WIFI_ENABLED || !wifiConnected || offlineCount == 0) return;

  Serial.println("Flushing offline queue...");

  int sent = 0;

  while (sent < offlineCount) {
    if (!postTransactionToFirebase(offlineQueue[sent])) {
      break;
    }

    sent++;
  }

  if (sent > 0) {
    for (int i = sent; i < offlineCount; i++) {
      offlineQueue[i - sent] = offlineQueue[i];
    }

    offlineCount -= sent;
    persistOfflineQueue();
  }

  Serial.print("Offline queue remaining: ");
  Serial.println(offlineCount);
}

bool recordTransaction(int price, bool coldSelected) {
  if (WIFI_ENABLED && wifiConnected && !timeIsSynced) {
    syncTime(3000);
  }

  TransactionLog tx = makeTransaction(price, coldSelected);

  totalSales += price;
  saveRuntimeState();

  if (WIFI_ENABLED && wifiConnected) {
    if (postTransactionToFirebase(tx)) {
      flushPending = true;
      return true;
    }
  }

  return addOfflineTransaction(tx);
}

// -------------------- LCD UPDATE --------------------
void updateLCD() {
  int preCoolSecondsRemaining = -1;

  if (currentState == PRE_COOLING) {
    unsigned long elapsed = millis() - preCoolStartTime;

    if (elapsed >= coldPreCoolDuration) {
      preCoolSecondsRemaining = 0;
    } else {
      preCoolSecondsRemaining = (coldPreCoolDuration - elapsed + 999) / 1000;
    }
  }

  if (
    currentState == lastDisplayedState &&
    currentCoins == lastDisplayedCoins &&
    cupsToDispense == lastDisplayedCupsToDispense &&
    cupsSold == lastDisplayedCupsSold &&
    coinPulseCount == lastDisplayedPulseCount &&
    lastRejectedPulseCount == lastDisplayedRejectedPulse &&
    preCoolSecondsRemaining == lastDisplayedPreCoolSeconds
  ) {
    return;
  }

  lastDisplayedState = currentState;
  lastDisplayedCoins = currentCoins;
  lastDisplayedCupsToDispense = cupsToDispense;
  lastDisplayedCupsSold = cupsSold;
  lastDisplayedPulseCount = coinPulseCount;
  lastDisplayedRejectedPulse = lastRejectedPulseCount;
  lastDisplayedPreCoolSeconds = preCoolSecondsRemaining;

  char line[17];

  if (currentState == IDLE) {
    if (currentCoins > 0) {
      snprintf(line, sizeof(line), "Balance: P%d", currentCoins);
      printLCDLine(0, line);
      printLCDLine(1, "Select Water");
    } else {
      printLCDLine(0, "Insert/Select");
      printLCDLine(1, "Cold | Regular");
    }
  }

  else if (currentState == ACCEPTING_COINS) {
    int price = isCold ? coldPrice : regularPrice;

    snprintf(line, sizeof(line), "%s P%d", isCold ? "Cold" : "Regular", price);
    printLCDLine(0, line);

    if (LCD_DEBUG_PULSES && coinPulseCount > 0) {
      snprintf(line, sizeof(line), "Pulse: %d", coinPulseCount);
    } else {
      snprintf(line, sizeof(line), "Paid: %d/%d", currentCoins, price);
    }

    printLCDLine(1, line);
  }

  else if (currentState == PRE_COOLING) {
    printLCDLine(0, "Cooling water...");
    snprintf(line, sizeof(line), "Pump in: %ds", preCoolSecondsRemaining);
    printLCDLine(1, line);
  }

  else if (currentState == DISPENSING) {
    printLCDLine(0, "Dispensing...");
    snprintf(line, sizeof(line), "Left:%d Bal:%d", cupsToDispense, currentCoins);
    printLCDLine(1, line);
  }

  else if (currentState == CUP_GAP) {
    printLCDLine(0, "Next cup...");
    snprintf(line, sizeof(line), "Left: %d", cupsToDispense);
    printLCDLine(1, line);
  }

  else if (currentState == COIN_REJECTED) {
    printLCDLine(0, "Invalid Coin");
    snprintf(line, sizeof(line), "Pulse: %d", lastRejectedPulseCount);
    printLCDLine(1, line);
  }

  else if (currentState == OUT_OF_WATER) {
    printLCDLine(0, "OUT OF WATER");
    printLCDLine(1, "Press RESET");
  }

  else if (currentState == ERROR_STATE) {
    printLCDLine(0, "SYSTEM ERROR");
    printLCDLine(1, errorMessage);
  }
}

// -------------------- COIN CLASSIFIER --------------------
bool classifyCoin(int pulseCount, int &coinValue, const char *&coinLabel) {
  for (int i = 0; i < coinRuleCount; i++) {
    if (
      pulseCount >= coinRules[i].minPulse &&
      pulseCount <= coinRules[i].maxPulse
    ) {
      coinValue = coinRules[i].value;
      coinLabel = coinRules[i].label;
      return true;
    }
  }

  coinValue = 0;
  coinLabel = "UNKNOWN";
  return false;
}

void enterCoinRejected(int pulseCount) {
  lastRejectedPulseCount = pulseCount;
  messageStartTime = millis();

  if (currentState == ACCEPTING_COINS) {
    resumeAfterMessage = ACCEPTING_COINS;
  } else {
    resumeAfterMessage = IDLE;
  }

  setState(COIN_REJECTED);
}

void processCoins() {
  if (currentState != IDLE && currentState != ACCEPTING_COINS) {
    resetCoinDetector();
    return;
  }

  unsigned long now = millis();

  noInterrupts();
  int detected = pulseBuffer;
  pulseBuffer = 0;
  interrupts();

  if (detected > 0) {
    coinPulseCount += detected;
    coinLastPulseTime = now;

    if (COIN_DEBUG) {
      Serial.print("RAW PULSE: ");
      Serial.print(detected);
      Serial.print(" | TOTAL: ");
      Serial.println(coinPulseCount);
    }
  }

  if (coinPulseCount > 0 && (now - coinLastPulseTime > coinPulseTimeout)) {
    int coinValue = 0;
    const char *coinLabel = "UNKNOWN";

    bool validCoin = classifyCoin(coinPulseCount, coinValue, coinLabel);

    if (COIN_DEBUG) {
      Serial.println("------ COIN ANALYSIS ------");
      Serial.print("Final Pulse Count: ");
      Serial.println(coinPulseCount);
    }

    if (validCoin) {
      currentCoins += coinValue;
      saveRuntimeState();

      if (COIN_DEBUG) {
        Serial.print("VALID: ");
        Serial.println(coinLabel);
        Serial.print("ADDED: ");
        Serial.println(coinValue);
        Serial.print("BALANCE: ");
        Serial.println(currentCoins);
      }

      resetCoinDetector();
    } else {
      if (COIN_DEBUG) {
        Serial.println("INVALID COIN / UNKNOWN RANGE");
      }

      int rejectedPulse = coinPulseCount;
      resetCoinDetector();
      enterCoinRejected(rejectedPulse);
    }

    if (COIN_DEBUG) {
      Serial.println("---------------------------");
    }
  }

  if (COIN_DEBUG && millis() - coinDebugTimer > 5000) {
    coinDebugTimer = millis();

    Serial.print("STATE: ");
    Serial.print(stateName(currentState));
    Serial.print(" | BALANCE: ");
    Serial.print(currentCoins);
    Serial.print(" | OFFLINE LOGS: ");
    Serial.println(offlineCount);
  }
}

// -------------------- PAYMENT --------------------
void beginPreCooling() {
  setState(PRE_COOLING);

  preCoolStartTime = millis();

  digitalWrite(pumpPin, LOW);
  digitalWrite(coolerPin, HIGH);

  Serial.println("Cold pre-cooling started");
  Serial.println("Cooler: ON | Pump: OFF");
}

void beginDispensing() {
  setState(DISPENSING);

  pumpStartTime = millis();

  digitalWrite(pumpPin, HIGH);

  if (isCold) {
    digitalWrite(coolerPin, HIGH);
    Serial.println("Cold dispensing started");
    Serial.println("Cooler: ON | Pump: ON");
  } else {
    digitalWrite(coolerPin, LOW);
    Serial.println("Regular dispensing started");
    Serial.println("Cooler: OFF | Pump: ON");
  }
}

void beginWaterCycle() {
  if (isCold) {
    beginPreCooling();
  } else {
    beginDispensing();
  }
}

void validatePayment() {
  if (currentState != ACCEPTING_COINS) return;

  if (cupsSold >= maxCups) {
    setState(OUT_OF_WATER);
    return;
  }

  int price = isCold ? coldPrice : regularPrice;

  if (currentCoins < price) return;

  int capacityRemaining = maxCups - cupsSold;

  if (capacityRemaining <= 0) {
    setState(OUT_OF_WATER);
    return;
  }

  if (AUTO_MULTI_CUP) {
    int affordableCups = currentCoins / price;

    if (affordableCups > capacityRemaining) {
      affordableCups = capacityRemaining;
    }

    cupsToDispense = affordableCups;
    currentCoins -= cupsToDispense * price;
  } else {
    cupsToDispense = 1;
    currentCoins -= price;
  }

  saveRuntimeState();

  Serial.print("Payment accepted. Cups queued: ");
  Serial.println(cupsToDispense);

  beginWaterCycle();
}

// -------------------- DISPENSING --------------------
void enterError(const char *message) {
  strncpy(errorMessage, message, 16);
  errorMessage[16] = '\0';

  setState(ERROR_STATE);
}

void handlePreCooling() {
  unsigned long elapsed = millis() - preCoolStartTime;

  if (elapsed >= coldPreCoolDuration) {
    Serial.println("Pre-cooling finished");
    Serial.println("Starting pump now");

    beginDispensing();
  }
}

void handleDispensing() {
  unsigned long elapsed = millis() - pumpStartTime;

  if (elapsed >= pumpTimeout) {
    digitalWrite(pumpPin, LOW);
    digitalWrite(coolerPin, LOW);

    enterError("Pump Timeout");
    return;
  }

  if (elapsed >= pumpDuration) {
    digitalWrite(pumpPin, LOW);
    digitalWrite(coolerPin, LOW);

    int price = isCold ? coldPrice : regularPrice;

    cupsSold++;
    cupsToDispense--;

    saveRuntimeState();

    Serial.print("Cup dispensed. Cups sold: ");
    Serial.println(cupsSold);

    if (!recordTransaction(price, isCold)) {
      enterError("Log Queue Full");
      return;
    }

    if (cupsSold >= maxCups) {
      setState(OUT_OF_WATER);
      return;
    }

    if (AUTO_MULTI_CUP && cupsToDispense > 0) {
      cupGapStartTime = millis();
      setState(CUP_GAP);
    } else {
      cupsToDispense = 0;
      setState(IDLE);
    }
  }
}

void handleCupGap() {
  if (millis() - cupGapStartTime >= cupGapDuration) {
    beginWaterCycle();
  }
}

void handleCoinRejected() {
  if (millis() - messageStartTime >= coinRejectedDisplayTime) {
    setState(resumeAfterMessage);
  }
}

// -------------------- BUTTONS --------------------
void updateButton(ButtonState &button) {
  button.current = digitalRead(button.pin);
  button.pressed = (button.last == HIGH && button.current == LOW);
  button.last = button.current;
}

void selectWater(bool coldSelected) {
  isCold = coldSelected;

  if (currentState != ACCEPTING_COINS) {
    setState(ACCEPTING_COINS);
  } else {
    forceLCDRefresh();
  }

  Serial.println(coldSelected ? "Cold selected" : "Regular selected");

  validatePayment();
}

void handleResetButton() {
  Serial.println("Reset pressed");

  digitalWrite(pumpPin, LOW);
  digitalWrite(coolerPin, LOW);

  currentCoins = 0;
  cupsToDispense = 0;
  cupsSold = 0;

  resetCoinDetector();
  saveRuntimeState();

  setState(IDLE);
}

void readButtons() {
  if (millis() - lastButtonCheckTime < 50) return;
  lastButtonCheckTime = millis();

  updateButton(coldButton);
  updateButton(regularButton);
  updateButton(resetButton);

  if (resetButton.pressed) {
    handleResetButton();
    return;
  }

  if (
    currentState == IDLE ||
    currentState == ACCEPTING_COINS
  ) {
    if (coldButton.pressed) {
      selectWater(true);
    }

    if (regularButton.pressed) {
      selectWater(false);
    }
  }
}

// -------------------- SETUP --------------------
void setup() {
  Serial.begin(115200);
  Serial.println("=== WATER VENDO SYSTEM START ===");

  Wire.begin(SDA_PIN, SCL_PIN);

  lcd.init();
  lcd.backlight();

  printLCDLine(0, "System Starting");
  printLCDLine(1, "Please wait...");

  pinMode(coinSlotPin, INPUT); // GPIO34 requires external 10k pull-up

  pinMode(pumpPin, OUTPUT);
  pinMode(coolerPin, OUTPUT);

  pinMode(btnCold, INPUT_PULLUP);
  pinMode(btnRegular, INPUT_PULLUP);
  pinMode(resetBtn, INPUT_PULLUP);

  digitalWrite(pumpPin, LOW);
  digitalWrite(coolerPin, LOW);

  loadPersistentData();

  attachInterrupt(digitalPinToInterrupt(coinSlotPin), coinISR, FALLING);

  connectWiFi();

  if (cupsSold >= maxCups) {
    setState(OUT_OF_WATER);
  } else {
    setState(IDLE);
  }

  updateLCD();
}

// -------------------- LOOP --------------------
void loop() {
  readButtons();
  checkWifiStatus();

  switch (currentState) {
    case IDLE:
      processCoins();
      break;

    case ACCEPTING_COINS:
      processCoins();
      validatePayment();
      break;

    case PRE_COOLING:
      handlePreCooling();
      break;

    case DISPENSING:
      handleDispensing();
      break;

    case CUP_GAP:
      handleCupGap();
      break;

    case COIN_REJECTED:
      handleCoinRejected();
      break;

    case OUT_OF_WATER:
      break;

    case ERROR_STATE:
      break;
  }

  maybeFlushOfflineQueue();
  updateLCD();
}
