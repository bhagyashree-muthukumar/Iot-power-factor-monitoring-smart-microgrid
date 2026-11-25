
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

// ========== PZEM UART ==========
#define PZEM_RX 16
#define PZEM_TX 17

HardwareSerial pzemSerial(2);
PZEM004Tv30 pzem(pzemSerial, PZEM_RX, PZEM_TX);

// ========== BUS 2 RELAYS ==========
// Preserved from your working sketch.
#define RELAY1_PIN   23
#define RELAY2_PIN   22
#define RELAY3_PIN   21
#define RELAY_CB_PIN 19

const bool RELAY_ACTIVE_LOW = true;

// ========== TIMERS ==========
unsigned long lastRead = 0;
unsigned long lastUpload = 0;
unsigned long lastWiFiAttempt = 0;
unsigned long lastAuthAttempt = 0;

String idToken = "";
String refreshToken = "";
unsigned long tokenExpiry = 0;

// ========== MEASUREMENTS ==========
float voltage = NAN;
float current = NAN;
float frequency = NAN;
float power = NAN;
float powerFactor = NAN;
bool haveValidReading = false;

// ========== RELAY FUNCTIONS ==========
void relayOn(uint8_t pin) {
  digitalWrite(pin, RELAY_ACTIVE_LOW ? LOW : HIGH);
}

void relayOff(uint8_t pin) {
  digitalWrite(pin, RELAY_ACTIVE_LOW ? HIGH : LOW);
}

void initializeRelays() {
  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);
  pinMode(RELAY3_PIN, OUTPUT);
  pinMode(RELAY_CB_PIN, OUTPUT);

  // Preserve existing Bus 2 startup behavior.
  relayOn(RELAY1_PIN);
  relayOn(RELAY2_PIN);
  relayOn(RELAY3_PIN);
  relayOn(RELAY_CB_PIN);

  Serial.println("Bus 2: bulb relay ON commands applied");
  Serial.println("Bus 2: breaker close command applied");
}

// ========== WIFI CONNECTION ==========
bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting Bus 2 to hotspot...");

  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - start < 20000) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Bus 2 Wi-Fi connected");
    Serial.print("Bus 2 IP: ");
    Serial.println(WiFi.localIP());
    return true;
  }

  Serial.println("Bus 2 Wi-Fi connection failed");
  return false;
}

// ========== HTTPS POST HELPER ==========
bool httpsPost(
  const String& url,
  const String& contentType,
  const String& body,
  String& response
) {
  WiFiClientSecure client;

  // For prototype testing only: server certificate is not verified.
  // Use CA certificate validation for a production deployment.
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

  Serial.printf("Authentication HTTP status: %d\n", status);

  https.end();

  return status >= 200 && status < 300;
}

// ========== FIREBASE ANONYMOUS SIGN-IN ==========
bool signInAnonymously() {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

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

  if (deserializeJson(doc, response) ||
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

  tokenExpiry = millis() + expiresIn * 1000UL;

  Serial.println("Bus 2 Firebase authentication successful");
  return true;
}

// ========== REFRESH FIREBASE TOKEN ==========
bool refreshFirebaseToken() {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

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

  if (deserializeJson(doc, response) ||
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

  tokenExpiry = millis() + expiresIn * 1000UL;

  Serial.println("Bus 2 Firebase token refreshed");
  return true;
}

// ========== ENSURE FIREBASE AUTH ==========
bool ensureFirebaseAuth() {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  if (idToken.length() == 0) {
    return signInAnonymously();
  }

  // Refresh five minutes before token expiry.
  if (tokenExpiry == 0 ||
      (long)(millis() - (tokenExpiry - 300000UL)) >= 0) {
    return refreshFirebaseToken();
  }

  return true;
}

// ========== READ BUS 2 PZEM ==========
void readBus2() {
  float v = pzem.voltage();
  float i = pzem.current();
  float f = pzem.frequency();
  float p = pzem.power();
  float pf = pzem.pf();

  if (isnan(v) || isnan(i) || isnan(f) ||
      isnan(p) || isnan(pf) ||
      v <= 0 || i < 0 || p < 0 ||
      pf < 0 || pf > 1.0) {
    Serial.println(
      "Bus 2 PZEM read error; check meter connections"
    );
    return;
  }

  voltage = v;
  current = i;
  frequency = f;
  power = p;
  powerFactor = pf;
  haveValidReading = true;

  Serial.printf(
    "BUS2 V=%.2f V | I=%.3f A | F=%.2f Hz | "
    "P=%.2f W | PF=%.2f\n",
    voltage, current, frequency, power, powerFactor
  );
}

// ========== UPLOAD BUS 2 TO FIREBASE ==========
bool sendReadingsToFirebase() {
  if (!haveValidReading) {
    Serial.println("No valid Bus 2 reading to upload");
    return false;
  }

  if (!ensureFirebaseAuth()) {
    Serial.println("Bus 2 Firebase authentication unavailable");
    return false;
  }

  WiFiClientSecure client;

  // For prototype testing only; certificate verification is disabled.
  client.setInsecure();

  HTTPClient https;
  https.setTimeout(8000);

  String url =
    String(DATABASE_URL) +
    "/buses/bus2.json?auth=" + idToken;

  if (!https.begin(client, url)) {
    Serial.println("Firebase HTTPS begin failed");
    return false;
  }

  https.addHeader("Content-Type", "application/json");

  JsonDocument doc;
  doc["voltage"] = voltage;
  doc["current"] = current;
  doc["frequency"] = frequency;
  doc["power"] = power;
  doc["powerFactor"] = powerFactor;
  doc["updatedAtMs"] = millis();

  String json;
  serializeJson(doc, json);

  // Updates only the Bus 2 node.
  int status = https.PUT(json);
  String response = https.getString();

  Serial.printf("Bus 2 Firebase HTTP status: %d\n", status);

  if (status >= 200 && status < 300) {
    Serial.println("Bus 2 data uploaded successfully");
    https.end();
    return true;
  }

  Serial.println("Firebase error response:");
  Serial.println(response);

  https.end();

  if (status == 401 || status == 403) {
    idToken = "";
    tokenExpiry = 0;
  }

  return false;
}

// ========== SETUP ==========
void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println("\n===== BUS 2 FIREBASE STARTING =====");

  // Relay behavior remains unchanged.
  initializeRelays();

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

  if (millis() - lastRead >= 2000) {
    lastRead = millis();
    readBus2();
  }

  if (WiFi.status() == WL_CONNECTED &&
      millis() - lastUpload >= 5000) {
    lastUpload = millis();
    sendReadingsToFirebase();
  }

  delay(10);
}
