// ═══════════════════════════════════════════════════════════════
//  main.ino  —  Sats ATM firmware
//
//  Hardware:  ESP32 + HX-916 coin acceptor via PC817 optocoupler
//  Display:   Android tablet (browser at http://192.168.4.1)
//  API:       Blink Lightning wallet (GraphQL over HTTPS)
//
//  Flow:
//    Boot → load saved credentials → connect WiFi (STA+AP)
//         → fetch rate + starting balance
//         → serve web UI → accept coins → create invoice
//         → poll for payment → reset
//
//  First boot (no saved credentials):
//    Boot → AP only → serve settings screen → operator configures
//         → save to NVS → reboot → normal flow
// ═══════════════════════════════════════════════════════════════

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "config.h"
#include "ui.h"           // HTML served to tablet (see coin_atm_esp32.h)

// ── Objects ──────────────────────────────────────────────────────
WebServer    server(80);
Preferences  prefs;
WiFiClientSecure client;

// ── Saved credentials (loaded from NVS on boot) ──────────────────
String savedSSID;
String savedPassword;
String savedWalletId;
String savedApiKey;

// ── Runtime state ────────────────────────────────────────────────
enum State { IDLE, COINS_INSERTED, CREATE_INVOICE, SHOW_QR, PAID, ERROR_STATE };
volatile State  appState       = IDLE;
volatile int    pulseCount     = 0;
volatile ulong  lastPulseTime  = 0;

float   creditEUR        = 0.0f;
int     creditSats       = 0;
int     satsPerEUR       = 0;           // fetched on boot
long    startingBalance  = 0;           // fetched on boot (gauge reference)
long    currentBalance   = 0;
String  paymentRequest   = "";          // lnbc... invoice string
String  paymentHash      = "";          // for payment polling
ulong   lastPollTime     = 0;
ulong   lastBalancePoll  = 0;
bool    wifiConnected    = false;
bool    settingsMode     = false;       // true = no saved creds on boot

// ── Coin ISR ─────────────────────────────────────────────────────
void IRAM_ATTR onCoinPulse() {
  if (appState == IDLE || appState == COINS_INSERTED) {
    if (pulseCount < MAX_PULSES) pulseCount++;
    lastPulseTime = millis();
    appState = COINS_INSERTED;
  }
}

// ═══════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  Serial.println("\n\n⚡ Sats ATM booting...");

  // ── Load saved credentials ──────────────────────────────────
  prefs.begin("satm", false);
  savedSSID     = prefs.getString("ssid",     "");
  savedPassword = prefs.getString("password", "");
  savedWalletId = prefs.getString("walletId", "");
  savedApiKey   = prefs.getString("apiKey",   "");
  prefs.end();

  // ── Start AP (always — tablet always connects here) ──────────
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.printf("AP started: %s  IP: %s\n", AP_SSID, AP_IP);

  // ── TLS: PoC mode — operator supervised ──────────────────────
  client.setInsecure();

  // ── Try STA connection if credentials saved ───────────────────
  if (savedSSID.length() > 0 && savedWalletId.length() > 0) {
    Serial.printf("Connecting to WiFi: %s\n", savedSSID.c_str());
    WiFi.begin(savedSSID.c_str(), savedPassword.c_str());
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500); Serial.print("."); attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
      wifiConnected = true;
      Serial.printf("\nConnected. IP: %s\n", WiFi.localIP().toString().c_str());
      fetchBootData();
    } else {
      Serial.println("\nWiFi connection failed — settings required");
      settingsMode = true;
    }
  } else {
    Serial.println("No credentials saved — showing settings screen");
    settingsMode = true;
  }

  // ── Coin interrupt ────────────────────────────────────────────
  pinMode(COIN_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(COIN_PIN), onCoinPulse, FALLING);

  // ── Web server routes ─────────────────────────────────────────
  setupRoutes();
  server.begin();
  Serial.println("Web server started");
}

// ═══════════════════════════════════════════════════════════════
//  MAIN LOOP
// ═══════════════════════════════════════════════════════════════
void loop() {
  server.handleClient();

  if (settingsMode || !wifiConnected) return;

  // ── Coin timeout check ────────────────────────────────────────
  if (appState == COINS_INSERTED &&
      (millis() - lastPulseTime) > INTER_COIN_MS) {
    float euros = (float)pulseCount / (float)PULSES_PER_EUR;
    if (euros > MAX_CREDIT_EUR) euros = MAX_CREDIT_EUR;
    creditEUR  = euros;
    creditSats = (int)(euros * satsPerEUR);
    pulseCount = 0;
    Serial.printf("Coins finalised: €%.2f = %d sats\n", creditEUR, creditSats);
    appState = CREATE_INVOICE;
  }

  // ── Create invoice ────────────────────────────────────────────
  if (appState == CREATE_INVOICE) {
    if (createInvoice()) {
      appState = SHOW_QR;
      Serial.println("Invoice created — showing QR");
    } else {
      Serial.println("Invoice creation failed");
      appState = ERROR_STATE;
    }
  }

  // ── Poll payment status ───────────────────────────────────────
  if (appState == SHOW_QR &&
      (millis() - lastPollTime) > POLL_INTERVAL_MS) {
    lastPollTime = millis();
    if (checkPayment()) {
      appState = PAID;
      Serial.println("Payment confirmed!");
    }
  }

  // ── Poll wallet balance (background) ─────────────────────────
  if (appState == SHOW_QR || appState == IDLE) {
    if ((millis() - lastBalancePoll) > BALANCE_POLL_MS) {
      lastBalancePoll = millis();
      fetchBalance();
    }
  }

  // ── Auto-reset after PAID ─────────────────────────────────────
  // Reset is triggered by the tablet after showing confirmation,
  // but we also reset server-side state here after 5s
  static ulong paidTime = 0;
  if (appState == PAID) {
    if (paidTime == 0) paidTime = millis();
    if ((millis() - paidTime) > 5000) {
      resetState();
      paidTime = 0;
    }
  }
}

// ═══════════════════════════════════════════════════════════════
//  BOOT DATA FETCH
// ═══════════════════════════════════════════════════════════════
void fetchBootData() {
  Serial.println("Fetching boot data from Blink...");
  satsPerEUR      = fetchSatsPerEUR();
  startingBalance = fetchBalance();
  currentBalance  = startingBalance;
  Serial.printf("Rate: 1 EUR = %d sats\n", satsPerEUR);
  Serial.printf("Starting balance: %ld sats\n", startingBalance);
}

// ── Fetch EUR → sats rate ─────────────────────────────────────
int fetchSatsPerEUR() {
  String query = "{\"query\":\"{ btcPriceList(range: ONE_DAY) { price { base offset } } }\"}";
  String resp  = blinkPost(query);
  if (resp.length() == 0) return 1600;   // fallback rate

  StaticJsonDocument<1024> doc;
  if (deserializeJson(doc, resp)) return 1600;

  // base is BTC price in USD cents × 10^offset
  // We approximate EUR ≈ USD for PoC (operator can adjust PULSES_PER_EUR)
  float btcUSD = doc["data"]["btcPriceList"][0]["price"]["base"].as<float>();
  int   offset = doc["data"]["btcPriceList"][0]["price"]["offset"].as<int>();
  while (offset < 0) { btcUSD /= 10.0f; offset++; }
  int sats = (int)(100000000.0f / btcUSD);
  return (sats > 0) ? sats : 1600;
}

// ── Fetch wallet balance ──────────────────────────────────────
long fetchBalance() {
  String query = "{\"query\":\"{ me { defaultAccount { wallets { balance } } } }\","
                 "\"operationName\":null}";
  String resp  = blinkPost(query);
  if (resp.length() == 0) return currentBalance;

  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, resp)) return currentBalance;

  JsonArray wallets = doc["data"]["me"]["defaultAccount"]["wallets"];
  for (JsonObject w : wallets) {
    if (w["balance"].as<long>() >= 0) {
      currentBalance = w["balance"].as<long>();
      return currentBalance;
    }
  }
  return currentBalance;
}

// ═══════════════════════════════════════════════════════════════
//  INVOICE + PAYMENT
// ═══════════════════════════════════════════════════════════════
bool createInvoice() {
  String query = String("{\"query\":\"mutation { lnInvoiceCreate(input: { walletId: \\\"") +
                 savedWalletId + "\\\" amount: " + String(creditSats) +
                 " }) { invoice { paymentRequest paymentHash } errors { message } } }\"}";

  String resp = blinkPost(query);
  if (resp.length() == 0) return false;

  StaticJsonDocument<1024> doc;
  if (deserializeJson(doc, resp)) return false;

  JsonObject inv = doc["data"]["lnInvoiceCreate"]["invoice"];
  if (inv.isNull()) return false;

  paymentRequest = inv["paymentRequest"].as<String>();
  paymentHash    = inv["paymentHash"].as<String>();
  return (paymentRequest.length() > 0 && paymentHash.length() > 0);
}

bool checkPayment() {
  String query = String("{\"query\":\"{ lnInvoicePaymentStatus(input: { paymentRequest: \\\"") +
                 paymentRequest + "\\\" }) { status } }\"}";

  String resp = blinkPost(query);
  if (resp.length() == 0) return false;

  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, resp)) return false;

  String status = doc["data"]["lnInvoicePaymentStatus"]["status"].as<String>();
  return (status == "PAID");
}

// ═══════════════════════════════════════════════════════════════
//  BLINK HTTP POST
// ═══════════════════════════════════════════════════════════════
String blinkPost(const String& payload) {
  if (!client.connect("api.blink.sv", 443)) {
    Serial.println("Blink connect failed");
    return "";
  }

  client.println("POST /graphql HTTP/1.1");
  client.println("Host: api.blink.sv");
  client.println("Content-Type: application/json");
  client.println("X-API-KEY: " + savedApiKey);
  client.println("Content-Length: " + String(payload.length()));
  client.println("Connection: close");
  client.println();
  client.print(payload);

  // Wait for response
  ulong t = millis();
  while (!client.available() && millis() - t < 5000) delay(10);

  // Skip HTTP headers
  while (client.connected() || client.available()) {
    String line = client.readStringUntil('\n');
    if (line == "\r") break;
  }

  // Read body
  String body = "";
  while (client.available()) body += (char)client.read();
  client.stop();
  return body;
}

// ═══════════════════════════════════════════════════════════════
//  STATE RESET
// ═══════════════════════════════════════════════════════════════
void resetState() {
  appState       = IDLE;
  pulseCount     = 0;
  creditEUR      = 0.0f;
  creditSats     = 0;
  paymentRequest = "";
  paymentHash    = "";
  lastPollTime   = 0;
  Serial.println("Session reset → IDLE");
}

// ═══════════════════════════════════════════════════════════════
//  WEB SERVER ROUTES
// ═══════════════════════════════════════════════════════════════
void setupRoutes() {

  // ── Main UI ────────────────────────────────────────────────
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", getUI());    // see ui.h
  });

  // ── State endpoint (tablet polls this) ────────────────────
  server.on("/state", HTTP_GET, []() {
    String gaugeLevel = "high";
    if (startingBalance > 0) {
      float pct = (float)currentBalance / (float)startingBalance;
      if      (pct <= GAUGE_LOW)  gaugeLevel = (pct <= 0.05f) ? "critical" : "low";
      else if (pct <= GAUGE_MID)  gaugeLevel = "low";
      else if (pct <= GAUGE_HIGH) gaugeLevel = "mid";
    }

    String stateStr;
    switch (appState) {
      case IDLE:          stateStr = "IDLE";    break;
      case COINS_INSERTED:stateStr = "COINS";   break;
      case CREATE_INVOICE:stateStr = "CREATING";break;
      case SHOW_QR:       stateStr = "SHOW_QR"; break;
      case PAID:          stateStr = "PAID";    break;
      default:            stateStr = "ERROR";   break;
    }

    String json = "{";
    json += "\"state\":\""    + stateStr                    + "\",";
    json += "\"invoice\":\""  + paymentRequest              + "\",";
    json += "\"sats\":"       + String(creditSats)          + ",";
    json += "\"credit\":"     + String(creditEUR, 2)        + ",";
    json += "\"gauge\":\""    + gaugeLevel                  + "\",";
    json += "\"rate\":"       + String(satsPerEUR)          + ",";
    json += "\"settings\":"   + String(settingsMode ? "true" : "false");
    json += "}";

    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", json);
  });

  // ── Settings save endpoint ────────────────────────────────
  server.on("/settings", HTTP_POST, []() {
    if (!server.hasArg("plain")) {
      server.send(400, "application/json", "{\"error\":\"no body\"}");
      return;
    }

    StaticJsonDocument<512> doc;
    if (deserializeJson(doc, server.arg("plain"))) {
      server.send(400, "application/json", "{\"error\":\"invalid json\"}");
      return;
    }

    String newSSID     = doc["ssid"]     | "";
    String newPassword = doc["password"] | "";
    String newWallet   = doc["walletId"] | "";
    String newApiKey   = doc["apiKey"]   | "";

    if (newSSID.length() == 0 || newWallet.length() == 0 || newApiKey.length() == 0) {
      server.send(400, "application/json", "{\"error\":\"missing fields\"}");
      return;
    }

    // Test WiFi connection before saving
    Serial.printf("Testing WiFi: %s\n", newSSID.c_str());
    WiFi.begin(newSSID.c_str(), newPassword.c_str());
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
      delay(500); attempts++;
    }

    if (WiFi.status() != WL_CONNECTED) {
      WiFi.disconnect();
      server.send(200, "application/json", "{\"success\":false,\"error\":\"WiFi connection failed\"}");
      return;
    }

    // Save to NVS
    prefs.begin("satm", false);
    prefs.putString("ssid",     newSSID);
    prefs.putString("password", newPassword);
    prefs.putString("walletId", newWallet);
    prefs.putString("apiKey",   newApiKey);
    prefs.end();

    server.send(200, "application/json", "{\"success\":true}");
    Serial.println("Settings saved — rebooting in 2s");
    delay(2000);
    ESP.restart();
  });

  // ── Clear settings (factory reset) ───────────────────────
  server.on("/reset", HTTP_POST, []() {
    prefs.begin("satm", false);
    prefs.clear();
    prefs.end();
    server.send(200, "application/json", "{\"success\":true}");
    delay(1000);
    ESP.restart();
  });

  server.onNotFound([]() {
    server.send(404, "text/plain", "Not found");
  });
}
