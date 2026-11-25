
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

// ========== PZEM UART PINS ==========
#define PZEM_RX 16
#define PZEM_TX 17

// ========== BUS 1 BULB RELAYS ==========
// Preserve the existing active-low relay behavior.
#define RELAY1_PIN 23
#define RELAY2_PIN 22
#define RELAY3_PIN 21
#define RELAY_ACTIVE_LOW true

HardwareSerial pzemSerial(2);
PZEM004Tv30 pzem(pzemSerial, PZEM_RX, PZEM_TX);

String idToken = "";
String refreshToken = "";

unsigned long tokenExpiry = 0;
unsigned long lastPzemRead = 0;
unsigned long lastUpload = 0;
unsigned long lastAuthAttempt = 0;
unsigned long lastWiFiAttempt = 0;

float voltage = NAN;
float current = NAN;
float power = NAN;
float powerFactor = NAN;

bool haveValidReading = false;

// ========== RELAY CONTROL ==========
void setRelay(uint8_t pin, bool turnOn) {
  digitalWrite(
    pin,
    (turnOn == RELAY_ACTIVE_LOW) ? LOW : HIGH
  );
}

void initBulbRelays() {
  // Preserve the existing Bus 1 startup sequence.
  digitalWrite(RELAY1_PIN, LOW);
  digitalWrite(RELAY2_PIN, LOW);
  digitalWrite(RELAY3_PIN, LOW);

  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);
  pinMode(RELAY3_PIN, OUTPUT);

  setRelay(RELAY1_PIN, true);
  setRelay(RELAY2_PIN, true);
  setRelay(RELAY3_PIN, true);

  Serial.println("Bus 1 relay ON commands applied");
}

// ========== WIFI ==========
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to Wi-Fi");

  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - start < 20000) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Wi-Fi connected");
    Serial.print("ESP32 IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Wi-Fi connection failed");
  }
}

// ========== HTTPS POST HELPER ==========
bool httpsPost(
  const String& url,
  const String& contentType,
  const String& body,
  String& response
) {
  WiFiClientSecure client;

  // Prototype convenience only. This does not verify the
  // server certificate. Use CA certificate validation for
  // a production or safety-critical deployment.
  client.setInsecure();

  HTTPClient https;
  https.setTimeout(8000);

  if (!https.begin(client, url)) {
    Serial.println("HTTPS begin failed");
    return false;
  }

  https.addHeader("Content-Type", contentType);

  int status = https.POST(body);
  response = https.getString();

  Serial.printf("HTTPS POST status: %d\n", status);

  https.end();

  return status >= 200 && status < 300;
}

// ========== ANONYMOUS SIGN-IN ==========
bool signInAnonymously() {
  if (WiFi.status() != WL_CONNECTED) return false;

  String url =
    String("https://identitytoolkit.googleapis.com/v1/accounts:signUp?key=")
    + FIREBASE_API_KEY;

  String response;

  bool ok = httpsPost(
    url,
    "application/json",
    "{\"returnSecureToken\":true}",
    response
  );

  if (!ok) {
    Serial.println("Anonymous sign-in failed:");
    Serial.println(response);
    return false;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, response);

  if (error ||
      !doc["idToken"].is<const char*>() ||
      !doc["refreshToken"].is<const char*>()) {
    Serial.println("Could not parse sign-in response");
    Serial.println(response);
    return false;
  }

  idToken = doc["idToken"].as<String>();
  refreshToken = doc["refreshToken"].as<String>();

  unsigned long expiresIn =
    String(doc["expiresIn"] | "3600").toInt();

  tokenExpiry = millis() + (expiresIn * 1000UL);

  Serial.println("Firebase anonymous sign-in successful");
  return true;
}

// ========== REFRESH TOKEN ==========
bool refreshFirebaseToken() {
  if (WiFi.status() != WL_CONNECTED) return false;

  if (refreshToken.length() == 0) {
    return signInAnonymously();
  }

  String url =
    String("https://securetoken.googleapis.com/v1/token?key=")
    + FIREBASE_API_KEY;

  String body =
    "grant_type=refresh_token&refresh_token=" + refreshToken;

  String response;

  bool ok = httpsPost(
    url,
    "application/x-www-form-urlencoded",
    body,
    response
  );

  if (!ok) {
    Serial.println("Token refresh failed:");
    Serial.println(response);
    return false;
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, response);

  if (error ||
      !doc["id_token"].is<const char*>() ||
      !doc["refresh_token"].is<const char*>()) {
    Serial.println("Could not parse token refresh response");
    Serial.println(response);
    return false;
  }

  idToken = doc["id_token"].as<String>();
  refreshToken = doc["refresh_token"].as<String>();

  unsigned long expiresIn =
    String(doc["expires_in"] | "3600").toInt();

  tokenExpiry = millis() + (expiresIn * 1000UL);

  Serial.println("Firebase token refreshed");
  return true;
}

// ========== ENSURE AUTHENTICATION ==========
bool ensureFirebaseAuth() {
  if (WiFi.status() != WL_CONNECTED) return false;

  if (idToken.length() == 0) {
    return signInAnonymously();
  }

  // Refresh five minutes before expiry.
  if ((long)(millis() - (tokenExpiry - 300000UL)) >= 0) {
    return refreshFirebaseToken();
  }

  return true;
}

// ========== READ BUS 1 PZEM ==========
void readBus1() {
  float v = pzem.voltage();
  float i = pzem.current();
  float p = pzem.power();
  float pf = pzem.pf();

  if (isnan(v) || isnan(i) || isnan(p) || isnan(pf) ||
      v <= 0 || i < 0 || p < 0 || pf < 0 || pf > 1.0) {
    Serial.println("Bus 1 PZEM reading invalid");
    return;
  }

  voltage = v;
  current = i;
  power = p;
  powerFactor = pf;
  haveValidReading = true;

  Serial.printf(
    "Bus 1: V=%.1f V, I=%.3f A, P=%.1f W, PF=%.2f\n",
    voltage, current, power, powerFactor
  );
}

// ========== UPLOAD BUS 1 TO FIREBASE ==========
bool uploadBus1() {
  if (!haveValidReading) {
    Serial.println("No valid Bus 1 reading to upload");
    return false;
  }

  if (!ensureFirebaseAuth()) {
    Serial.println("Firebase authentication unavailable");
    return false;
  }

  WiFiClientSecure client;

  // Prototype convenience only; see certificate warning above.
  client.setInsecure();

  HTTPClient https;
  https.setTimeout(8000);

  String url =
    String(DATABASE_URL) +
    "/buses/bus1.json?auth=" + idToken;

  if (!https.begin(client, url)) {
    Serial.println("Firebase HTTPS begin failed");
    return false;
  }

  https.addHeader("Content-Type", "application/json");

  JsonDocument doc;
  doc["voltage"] = voltage;
  doc["current"] = current;
  doc["power"] = power;
  doc["powerFactor"] = powerFactor;
  doc["updatedAtMs"] = millis();

  String json;
  serializeJson(doc, json);

  // PUT replaces only /buses/bus1, not the other bus nodes.
  int status = https.PUT(json);
  String response = https.getString();

  Serial.printf("Firebase upload HTTP status: %d\n", status);

  if (status >= 200 && status < 300) {
    Serial.println("Bus 1 uploaded to Firebase successfully");
    https.end();
    return true;
  }

  Serial.println("Firebase response:");
  Serial.println(response);

  https.end();

  // If token was rejected, try signing in again next time.
  if (status == 401 || status == 403) {
    idToken = "";
    tokenExpiry = 0;
  }

  return false;
}

// ========== SETUP ==========
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("=== BUS 1 FIREBASE STARTING ===");

  initBulbRelays();

  pzemSerial.begin(9600, SERIAL_8N1, PZEM_RX, PZEM_TX);

  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    signInAnonymously();
  }
}

// ========== MAIN LOOP ==========
void loop() {
  if (WiFi.status() != WL_CONNECTED &&
      millis() - lastWiFiAttempt >= 10000) {
    lastWiFiAttempt = millis();
    connectWiFi();
  }

  if (WiFi.status() == WL_CONNECTED &&
      idToken.length() == 0 &&
      millis() - lastAuthAttempt >= 10000) {
    lastAuthAttempt = millis();
    signInAnonymously();
  }

  if (millis() - lastPzemRead >= 2000) {
    lastPzemRead = millis();
    readBus1();
  }

  if (WiFi.status() == WL_CONNECTED &&
      millis() - lastUpload >= 5000) {
    lastUpload = millis();
    uploadBus1();
  }

  delay(10);
}
