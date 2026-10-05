/*
  muse_wifi.ino — Muse gadget add-on for the shotStopper grinder firmware
  (tatemazer/AcaiaArduinoBLE, examples/shotStopper).

  Adds WiFi + a tiny HTTP API on top of the stock shot-by-weight loop so Muse
  (via Home Link on the LAN) can dose the grinder and read shot state.
  The shotStopper core loop is untouched.

  INSTALL:
    1. Drop this file into the examples/shotStopper/ sketch folder.
    2. In shotStopper.ino setup(), after BLE.advertise(), add:
         museWifiSetup();
    3. In shotStopper.ino loop(), first line, add:
         museWifiLoop();
    4. Install the WiFiManager library (tzapu/WiFiManager) via the
       Arduino Library Manager. No other new dependencies.

  FIRST BOOT: the board creates an AP named "shotStopper-Setup".
  Join it, pick your WiFi network, done. Credentials persist.

  API (port 80, also at http://shotstopper-grinder.local):
    GET  /status  -> scale link, brewing state, live weight, goal, timer
    POST /target  -> {"g": 18.5}  sets dose target (10-200g), persists to EEPROM,
                     mirrors the BLE goal-weight characteristic
    GET  /last    -> final weight, goal, duration, end reason of last grind

  Assumes classic ESP32 (shotStopper default pin map).
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <WiFiManager.h>

static WebServer museServer(80);

// Last-grind summary, captured on the brewing true->false transition.
struct MuseLastGrind {
  float finalWeightG = 0;
  uint8_t goalG = 0;
  float durationS = 0;
  const char* endReason = "none";
  bool valid = false;
};
static MuseLastGrind museLast;
static bool musePrevBrewing = false;

static const char* museEndReason(ENDTYPE e) {
  switch (e) {
    case ENDTYPE::WEIGHT:     return "weight";
    case ENDTYPE::TIME:       return "time";
    case ENDTYPE::BUTTON:     return "button";
    case ENDTYPE::DISCONNECT: return "disconnect";
    default:                  return "unknown";
  }
}

static void museHandleStatus() {
  char buf[256];
  snprintf(buf, sizeof(buf),
    "{\"scale_connected\":%s,\"brewing\":%s,"
    "\"weight_g\":%.2f,\"goal_g\":%d,\"shot_timer_s\":%.1f,"
    "\"firmware\":\"shotstopper-muse/1.0\"}",
    scale.isConnected() ? "true" : "false",
    shot.brewing ? "true" : "false",
    currentWeight, goalWeight, shot.shotTimer);
  museServer.send(200, "application/json", buf);
}

static void museHandleTarget() {
  if (!museServer.hasArg("plain")) {
    museServer.send(400, "application/json", "{\"error\":\"missing body\"}");
    return;
  }
  String body = museServer.arg("plain");
  // Minimal JSON parse: look for "g": <number>
  int gPos = body.indexOf("\"g\"");
  if (gPos < 0) {
    museServer.send(400, "application/json", "{\"error\":\"expected {\\\"g\\\": <grams>}\"}");
    return;
  }
  float g = body.substring(body.indexOf(':', gPos) + 1).toFloat();
  // Mirror the stock validation: 10-200g, 1-byte EEPROM
  if (g < 10 || g > 200) {
    museServer.send(400, "application/json", "{\"error\":\"goal out of range (10-200g)\"}");
    return;
  }
  goalWeight = (uint8_t)g;
  EEPROM.write(WEIGHT_ADDR, goalWeight);
  EEPROM.commit();
  weightCharacteristic.writeValue(goalWeight);  // keep BLE clients in sync
  Serial.print("goal weight updated via HTTP to ");
  Serial.println(goalWeight);

  char buf[64];
  snprintf(buf, sizeof(buf), "{\"goal_g\":%d}", goalWeight);
  museServer.send(200, "application/json", buf);
}

static void museHandleLast() {
  char buf[192];
  snprintf(buf, sizeof(buf),
    "{\"valid\":%s,\"final_weight_g\":%.2f,\"goal_g\":%d,"
    "\"duration_s\":%.1f,\"end\":\"%s\"}",
    museLast.valid ? "true" : "false",
    museLast.finalWeightG, museLast.goalG,
    museLast.durationS, museLast.endReason);
  museServer.send(200, "application/json", buf);
}

void museWifiSetup() {
  WiFiManager wm;
  wm.setConnectTimeout(20);
  // Blocks into the captive portal only when no saved network connects.
  if (!wm.autoConnect("shotStopper-Setup")) {
    Serial.println("WiFi setup failed, rebooting");
    ESP.restart();
  }
  Serial.print("WiFi connected, IP: ");
  Serial.println(WiFi.localIP());

  if (MDNS.begin("shotstopper-grinder")) {
    Serial.println("mDNS: http://shotstopper-grinder.local");
  }

  museServer.on("/status", HTTP_GET, museHandleStatus);
  museServer.on("/target", HTTP_POST, museHandleTarget);
  museServer.on("/last", HTTP_GET, museHandleLast);
  museServer.onNotFound([]() {
    museServer.send(404, "application/json", "{\"error\":\"not found\"}");
  });
  museServer.begin();
  Serial.println("Muse HTTP API on port 80");
}

void museWifiLoop() {
  museServer.handleClient();

  // Capture the last-grind summary on the brewing true -> false edge.
  // (Reads the stock Shot struct; no changes to shotStopper.ino needed.)
  if (musePrevBrewing && !shot.brewing) {
    museLast.goalG = goalWeight;
    museLast.durationS = shot.end_s;
    museLast.endReason = museEndReason(shot.end);
    if (shot.datapoints > 0) {
      museLast.finalWeightG = shot.weight[shot.datapoints - 1];
    } else {
      museLast.finalWeightG = currentWeight;
    }
    museLast.valid = true;
    Serial.print("grind finished: ");
    Serial.print(museLast.finalWeightG);
    Serial.print("g in ");
    Serial.print(museLast.durationS);
    Serial.println("s");
  }
  musePrevBrewing = shot.brewing;
}
