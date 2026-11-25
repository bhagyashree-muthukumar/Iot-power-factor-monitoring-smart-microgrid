#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <PZEM004Tv30.h>
#include <ArduinoJson.h>
#include <math.h>


// ========== WIFI SETTINGS ==========
const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// ========== FIREBASE SETTINGS ==========
const char* FIREBASE_API_KEY = "YOUR_FIREBASE_API_KEY";
const char* DATABASE_URL = "YOUR_DATABASE_URL";

// ================= PZEM =================
#define PZEM_RX 16
#define PZEM_TX 17
HardwareSerial pzemSerial(2);
PZEM004Tv30 pzem(pzemSerial, PZEM_RX, PZEM_TX);

// ================= RELAYS (active LOW) =================
const uint8_t RELAY_PIN[3] = {23, 22, 21};
#define RELAY_ACTIVE_LOW true

// Nominal capacitance values for each stage (uF)
const float CAP_UF[3] = {8.0f, 8.0f, 8.0f};

// ================= APFC CONTROL SETTINGS =================
// User specification:
// Target PF is 0.90.
// If PF < 0.90: Capacitors turn ON one by one to make PF >= 0.90.
// If PF >= 0.90: Any unneeded capacitor shuts down one by one to maintain PF ~ 0.90.
const float PF_SETPOINT          = 0.90f;   // Target power factor threshold
const float PF_STEPDOWN_MARGIN   = 0.89f;   // Only step down if estimated PF without the cap is >= 0.89
const float MIN_LOAD_CURRENT_A   = 0.08f;   // Below this -> no load connected, turn all caps OFF
const float MIN_LOAD_POWER_W     = 5.0f;    // Below this -> no active load
const float MAX_SAFE_VOLTAGE_V   = 255.0f;  // Overvoltage protection threshold
const float LOAD_CHANGE_DELTA_W  = 15.0f;   // Power change threshold to detect physical load changes

// Timing parameters
const unsigned long STEP_INTERVAL_MS    = 2500UL; // 2.5s between relay steps (allows PZEM to settle)
const unsigned long CONTROL_INTERVAL_MS = 1000UL; // Evaluate APFC logic every 1s
const unsigned long READING_MAX_AGE_MS  = 7000UL; // Freshness limit for sensor data
const unsigned long READ_INTERVAL_MS    = 1500UL; // PZEM read interval
const unsigned long UPLOAD_INTERVAL_MS  = 4000UL; // Regular Firebase upload interval

// ================= STATE VARIABLES =================
int currentStage = 0;          // Number of capacitor stages currently ON (0, 1, 2, or 3)
bool capOn[3] = {false, false, false};
int maxStageForCurrentLoad = 3; // Maximum stage allowed before overcompensation occurs
bool manualOverride = false;    // Allows manual control from Serial Monitor if needed

// Tracking for previous action
enum LastAction { ACT_NONE, ACT_STEPPED_UP, ACT_STEPPED_DOWN };
LastAction lastAction = ACT_NONE;
float pfBeforeAction = 0.0f;
float lastPowerReading = 0.0f;

// Timers
unsigned long lastRead = 0;
unsigned long lastUpload = 0;
unsigned long lastWiFiAttempt = 0;
unsigned long lastAuthAttempt = 0;
unsigned long lastControl = 0;
unsigned long lastStepTime = 0;
unsigned long lastValidReadingMs = 0;

// Auth
String idToken = "";
String refreshToken = "";
unsigned long tokenExpiry = 0;

// Measurements
float voltage = NAN;
float current = NAN;
float power = NAN;
float frequency = NAN;
float powerFactor = NAN;
bool haveValidReading = false;

// Forward declaration
bool sendReadingsToFirebase();

// ================= HELPER FUNCTIONS =================

// Estimate reactive power produced by a capacitor in VAR
float calculateCapVar(float uF, float v, float f = 50.0f) {
  if (uF <= 0.0f || v <= 0.0f) return 0.0f;
  if (f <= 40.0f || f >= 70.0f) f = 50.0f;
  return 2.0f * 3.14159265f * f * (uF * 1e-6f) * (v * v);
}

float totalUF() {
  float t = 0.0f;
  for (int i = 0; i < 3; i++) {
    if (capOn[i]) t += CAP_UF[i];
  }
  return t;
}

float totalCapVar() {
  float v = (isnan(voltage) || voltage < 50.0f) ? 230.0f : voltage;
  float f = (isnan(frequency) || frequency < 40.0f) ? 50.0f : frequency;
  float total = 0.0f;
  for (int i = 0; i < 3; i++) {
    if (capOn[i]) {
      total += calculateCapVar(CAP_UF[i], v, f);
    }
  }
  return total;
}

// Predicts what the power factor would be if we remove the top capacitor
float predictPFWithoutTopCap() {
  if (currentStage <= 0 || power <= 0.0f || voltage <= 0.0f) return 1.0f;

  float S = voltage * current;
  float Q_meas = (S > power) ? sqrtf(S * S - power * power) : 0.0f;

  float v = voltage;
  float f = (frequency >= 40.0f && frequency <= 70.0f) ? frequency : 50.0f;
  float q_one_cap = calculateCapVar(CAP_UF[currentStage - 1], v, f);

  // If measured Q is very small (< 50 VAR), system is at or near unity.
  // Removing one capacitor will shift reactive power by approx q_one_cap:
  float Q_pred;
  if (Q_meas < 50.0f) {
    Q_pred = q_one_cap;
  } else {
    Q_pred = Q_meas + q_one_cap;
  }

  float S_pred = sqrtf(power * power + Q_pred * Q_pred);
  if (S_pred <= 0.0f) return 1.0f;

  float pf_pred = power / S_pred;
  if (pf_pred > 1.0f) pf_pred = 1.0f;
  if (pf_pred < 0.0f) pf_pred = 0.0f;
  return pf_pred;
}

// ================= RELAY CONTROL =================
void setRelay(int idx, bool on) {
  if (idx < 0 || idx >= 3) return;
  digitalWrite(RELAY_PIN[idx], (on == RELAY_ACTIVE_LOW) ? LOW : HIGH);
  capOn[idx] = on;
}

void applyStage(int stage) {
  if (stage < 0) stage = 0;
  if (stage > 3) stage = 3;

  for (int i = 0; i < 3; i++) {
    bool shouldBeOn = (i < stage);
    setRelay(i, shouldBeOn);
  }
  currentStage = stage;
  lastStepTime = millis();

  Serial.printf(">>> RELAY UPDATE: Stage %d/3 active | Caps: [%s, %s, %s] (%.0f uF)\n",
                currentStage,
                capOn[0] ? "ON" : "OFF",
                capOn[1] ? "ON" : "OFF",
                capOn[2] ? "ON" : "OFF",
                totalUF());

  // Instantly push update to Firebase so dashboard updates in < 1 second!
  if (WiFi.status() == WL_CONNECTED && haveValidReading) {
    sendReadingsToFirebase();
  }
}

void allCapsOff() {
  applyStage(0);
}

void initializeRelays() {
  for (int i = 0; i < 3; i++) {
    digitalWrite(RELAY_PIN[i], RELAY_ACTIVE_LOW ? HIGH : LOW);
    pinMode(RELAY_PIN[i], OUTPUT);
    digitalWrite(RELAY_PIN[i], RELAY_ACTIVE_LOW ? HIGH : LOW);
    capOn[i] = false;
  }
  currentStage = 0;
  Serial.println("Relays initialized: All Capacitors OFF");
}

// ================= WIFI =================
bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi...");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000UL) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wi-Fi connected, IP: ");
    Serial.println(WiFi.localIP());
    return true;
  }
  Serial.println("Wi-Fi connection failed");
  return false;
}

// ================= HTTPS POST HELPER =================
bool httpsPost(const String& url, const String& contentType,
               const String& body, String& response) {
  WiFiClientSecure client;
  client.setInsecure(); // Prototype testing
  HTTPClient https;
  https.setTimeout(6000);
  if (!https.begin(client, url)) return false;
  https.addHeader("Content-Type", contentType);
  int status = https.POST(body);
  response = https.getString();
  https.end();
  return status >= 200 && status < 300;
}

bool signInAnonymously() {
  if (WiFi.status() != WL_CONNECTED) return false;
  String url = String("https://identitytoolkit.googleapis.com/v1/accounts:signUp?key=") + FIREBASE_API_KEY;
  String response;
  if (!httpsPost(url, "application/json", "{\"returnSecureToken\":true}", response)) {
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, response) ||
      !doc["idToken"].is<const char*>() ||
      !doc["refreshToken"].is<const char*>()) {
    return false;
  }
  idToken = doc["idToken"].as<String>();
  refreshToken = doc["refreshToken"].as<String>();
  unsigned long expiresIn = String(doc["expiresIn"] | "3600").toInt();
  tokenExpiry = millis() + expiresIn * 1000UL;
  Serial.println("Firebase auth successful");
  return true;
}

bool refreshFirebaseToken() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (refreshToken.length() == 0) return signInAnonymously();
  String url = String("https://securetoken.googleapis.com/v1/token?key=") + FIREBASE_API_KEY;
  String body = "grant_type=refresh_token&refresh_token=" + refreshToken;
  String response;
  if (!httpsPost(url, "application/x-www-form-urlencoded", body, response)) {
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, response) ||
      !doc["id_token"].is<const char*>() ||
      !doc["refresh_token"].is<const char*>()) {
    return false;
  }
  idToken = doc["id_token"].as<String>();
  refreshToken = doc["refresh_token"].as<String>();
  unsigned long expiresIn = String(doc["expires_in"] | "3600").toInt();
  tokenExpiry = millis() + expiresIn * 1000UL;
  Serial.println("Firebase token refreshed");
  return true;
}

bool ensureFirebaseAuth() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (idToken.length() == 0) return signInAnonymously();
  if (tokenExpiry == 0 || (long)(millis() - (tokenExpiry - 300000UL)) >= 0) {
    return refreshFirebaseToken();
  }
  return true;
}

// ================= READ PZEM =================
void readBus3() {
  float v = pzem.voltage();
  float i = pzem.current();
  float p = pzem.power();
  float pf = pzem.pf();
  float f = pzem.frequency();

  if (isnan(v) || isnan(i) || isnan(p) || isnan(pf)) {
    Serial.println("[PZEM] Reading returned NaN; keeping previous values");
    return;
  }

  if (v <= 0.0f || i < 0.0f || p < 0.0f || pf < 0.0f || pf > 1.0f) {
    return;
  }

  voltage = v;
  current = i;
  power = p;
  powerFactor = pf;
  if (!isnan(f) && f > 40.0f && f < 70.0f) {
    frequency = f;
  } else {
    frequency = 50.0f;
  }

  haveValidReading = true;
  lastValidReadingMs = millis();
}

// ================= AUTOMATIC POWER FACTOR CONTROL =================
void updatePFControl() {
  unsigned long now = millis();
  if (now - lastControl < CONTROL_INTERVAL_MS) return;
  lastControl = now;

  // If in manual override mode from Serial Monitor, bypass auto control
  if (manualOverride) return;

  // 1. Fail-safe: No valid sensor data for prolonged time
  if (!haveValidReading || (now - lastValidReadingMs > READING_MAX_AGE_MS)) {
    if (currentStage > 0) {
      Serial.println("[APFC SAFETY] Sensor timeout! Turning all capacitors OFF");
      allCapsOff();
      lastAction = ACT_NONE;
    }
    return;
  }

  // 2. Overvoltage Protection
  if (voltage > MAX_SAFE_VOLTAGE_V) {
    if (currentStage > 0) {
      Serial.printf("[APFC SAFETY] Overvoltage (%.1f V > %.1f V)! Disconnecting capacitors\n",
                    voltage, MAX_SAFE_VOLTAGE_V);
      allCapsOff();
      lastAction = ACT_NONE;
    }
    return;
  }

  // 3. No Load / Load Removed Detection
  if (current < MIN_LOAD_CURRENT_A || power < MIN_LOAD_POWER_W) {
    if (currentStage > 0) {
      Serial.printf("[APFC] No active load (I=%.3fA, P=%.1fW). Turning all capacitors OFF\n",
                    current, power);
      allCapsOff();
      lastAction = ACT_NONE;
      maxStageForCurrentLoad = 3;
    }
    return;
  }

  // Calculate apparent power S and reactive power Q
  float S = voltage * current;
  float Q_meas = (S > power) ? sqrtf(S * S - power * power) : 0.0f;
  float Q_caps = totalCapVar();

  // Print current status line
  Serial.printf("[BUS3] V: %.1fV | I: %.3fA | P: %.1fW | Q: %.1fVAR | PF: %.2f | Stage: %d/3 (%.0fuF, %.0fVAR)\n",
                voltage, current, power, Q_meas, powerFactor, currentStage, totalUF(), Q_caps);

  // 4. Physical Load Change Detection
  if (fabs(power - lastPowerReading) > LOAD_CHANGE_DELTA_W) {
    Serial.printf("[APFC] Load change detected (P: %.1f W -> %.1f W). Re-evaluating compensation.\n",
                  lastPowerReading, power);
    lastPowerReading = power;
    maxStageForCurrentLoad = 3; // Reset maximum allowed stage for new load
    lastAction = ACT_NONE;
  }

  // 5. Enforce settle delay between relay steps to allow PZEM reading to stabilize
  if (now - lastStepTime < STEP_INTERVAL_MS) {
    return;
  }

  // 6. Evaluate previous action outcome
  if (lastAction == ACT_STEPPED_UP) {
    // If adding a capacitor caused PF to drop by > 0.03, it caused overcompensation (leading)
    if (powerFactor < (pfBeforeAction - 0.03f)) {
      Serial.printf("[APFC] Stage %d caused overcompensation (PF dropped from %.2f to %.2f). Stepping back to Stage %d.\n",
                    currentStage, pfBeforeAction, powerFactor, currentStage - 1);
      int optimalStage = currentStage - 1;
      maxStageForCurrentLoad = optimalStage;
      applyStage(optimalStage);
      lastAction = ACT_NONE;
      return;
    }
    lastAction = ACT_NONE;
  } else if (lastAction == ACT_STEPPED_DOWN) {
    lastAction = ACT_NONE;
  }

  // ================= DECISION LOGIC =================

  // RULE 1: If PF < 0.90 -> Turn ON capacitors one by one to reach >= 0.90
  if (powerFactor < PF_SETPOINT) {
    if (currentStage < maxStageForCurrentLoad && currentStage < 3) {
      int nextStage = currentStage + 1;
      Serial.printf("[APFC] PF is %.2f (< %.2f). Stepping UP -> Turning ON Cap %d (Stage %d)\n",
                    powerFactor, PF_SETPOINT, nextStage, nextStage);
      pfBeforeAction = powerFactor;
      lastAction = ACT_STEPPED_UP;
      applyStage(nextStage);
      return;
    } else {
      // Maximum compensation for this load already applied
      return;
    }
  }

  // RULE 2: If PF >= 0.90 -> Check if any capacitor is unnecessary (step down one by one to maintain ~0.90)
  if (powerFactor >= PF_SETPOINT && currentStage > 0) {
    float predictedPF = predictPFWithoutTopCap();

    // If removing the top capacitor will still keep PF >= 0.89 (around 0.90):
    if (predictedPF >= PF_STEPDOWN_MARGIN) {
      Serial.printf("[APFC] PF is %.2f (>= %.2f). Capacitor %d is not needed (predicted PF without it = %.2f >= %.2f). Stepping DOWN -> Stage %d\n",
                    powerFactor, PF_SETPOINT, currentStage, predictedPF, PF_STEPDOWN_MARGIN, currentStage - 1);
      pfBeforeAction = powerFactor;
      lastAction = ACT_STEPPED_DOWN;
      applyStage(currentStage - 1);
      return;
    } else {
      // Capacitor is necessary to maintain PF >= 0.90. Hold steady!
      Serial.printf("[APFC] PF is %.2f (>= %.2f). Stage %d is optimal (removing it would drop PF to %.2f). Holding steady.\n",
                    powerFactor, PF_SETPOINT, currentStage, predictedPF);
      return;
    }
  }
}

// ================= SERIAL USER COMMANDS =================
void handleSerialCommands() {
  if (!Serial.available()) return;
  char cmd = Serial.read();

  if (cmd == '+' || cmd == 'u' || cmd == 'U') {
    manualOverride = true;
    int next = (currentStage < 3) ? currentStage + 1 : 3;
    Serial.printf("[MANUAL] Force Step UP -> Stage %d\n", next);
    applyStage(next);
  } else if (cmd == '-' || cmd == 'd' || cmd == 'D') {
    manualOverride = true;
    int prev = (currentStage > 0) ? currentStage - 1 : 0;
    Serial.printf("[MANUAL] Force Step DOWN -> Stage %d\n", prev);
    applyStage(prev);
  } else if (cmd == '0') {
    manualOverride = true;
    Serial.println("[MANUAL] Force All Capacitors OFF");
    allCapsOff();
  } else if (cmd == 'a' || cmd == 'A') {
    manualOverride = false;
    maxStageForCurrentLoad = 3;
    lastAction = ACT_NONE;
    lastStepTime = millis();
    Serial.println("[MODE] Resumed AUTOMATIC APFC Mode");
  } else if (cmd == '?') {
    Serial.println("\n--- BUS 3 APFC COMMANDS ---");
    Serial.println(" + / u : Step UP (turn ON next capacitor)");
    Serial.println(" - / d : Step DOWN (turn OFF top capacitor)");
    Serial.println(" 0     : Turn all capacitors OFF");
    Serial.println(" a     : Resume AUTOMATIC APFC mode");
    Serial.printf(" Current State: Stage %d/3 | PF=%.2f | Mode: %s\n\n",
                  currentStage, powerFactor, manualOverride ? "MANUAL" : "AUTO");
  }
}

// ================= FIREBASE UPLOAD =================
bool sendReadingsToFirebase() {
  if (!haveValidReading || millis() - lastValidReadingMs > READING_MAX_AGE_MS) {
    return false;
  }
  if (!ensureFirebaseAuth()) {
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  https.setTimeout(5000);

  String url = String(DATABASE_URL) + "/buses/bus3.json?auth=" + idToken;
  if (!https.begin(client, url)) return false;
  https.addHeader("Content-Type", "application/json");

  float S = voltage * current;
  float Q = (S > power) ? sqrtf(S * S - power * power) : 0.0f;

  JsonDocument doc;
  doc["voltage"] = voltage;
  doc["current"] = current;
  doc["power"] = power;
  doc["frequency"] = frequency;
  doc["powerFactor"] = powerFactor;
  doc["reactivePower_var"] = Q;
  doc["updatedAtMs"] = millis();
  doc["capacitorStage1"] = capOn[0];
  doc["capacitorStage2"] = capOn[1];
  doc["capacitorStage3"] = capOn[2];
  doc["totalCapacitance_uF"] = totalUF();

  String json;
  serializeJson(doc, json);

  int status = https.PUT(json);
  String response = https.getString();
  https.end();

  if (status >= 200 && status < 300) {
    lastUpload = millis();
    return true;
  }

  if (status == 401 || status == 403) {
    idToken = "";
    tokenExpiry = 0;
  }
  return false;
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n==================================================");
  Serial.println("   SMART MICROGRID - BUS 3 APFC SYSTEM");
  Serial.println("   Target PF: 0.90 | Automatic 1-by-1 UP & DOWN");
  Serial.println("==================================================");

  initializeRelays();
  pzemSerial.begin(9600, SERIAL_8N1, PZEM_RX, PZEM_TX);

  connectWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    signInAnonymously();
  }

  lastStepTime = millis();
  lastPowerReading = 0.0f;
  Serial.println("[SYSTEM READY] Send '?' in Serial Monitor for commands.\n");
}

// ================= MAIN LOOP =================
void loop() {
  unsigned long now = millis();

  // Handle Serial user commands (manual +/- / auto mode)
  handleSerialCommands();

  // Reconnect WiFi if disconnected
  if (WiFi.status() != WL_CONNECTED && now - lastWiFiAttempt >= 10000UL) {
    lastWiFiAttempt = now;
    connectWiFi();
  }

  // Refresh Firebase auth if needed
  if (WiFi.status() == WL_CONNECTED && idToken.length() == 0 &&
      now - lastAuthAttempt >= 10000UL) {
    lastAuthAttempt = now;
    signInAnonymously();
  }

  // Read PZEM-004T sensor periodically
  if (now - lastRead >= READ_INTERVAL_MS) {
    lastRead = now;
    readBus3();
  }

  // Run APFC decision logic
  updatePFControl();

  // Regular telemetry upload to Firebase
  if (WiFi.status() == WL_CONNECTED && now - lastUpload >= UPLOAD_INTERVAL_MS) {
    sendReadingsToFirebase();
  }

  delay(10);
}
