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

  FIRST BOOT: the board opens an AP named "shotStopper-Setup" for 3 minutes.
  Join it, pick your WiFi network, done. Credentials persist. Shot stopping
  runs the whole time: WiFi never blocks setup() or loop().

  API (port 80, also at http://shotstopper-grinder.local):
    GET  /status  -> scale link, brewing state, live weight, goal, timer
    POST /target  -> {"g": 36}  sets the yield goal (10-200g, whole grams),
                     persists to EEPROM, mirrors the BLE goal-weight characteristic
    GET  /last    -> final weight, goal, duration, end reason of last shot

  OTA: once on WiFi, the board accepts ArduinoOTA uploads as
  "shotstopper-grinder". Put  #define MUSE_OTA_PASSWORD "..."  in a
  gitignored ota_secret.h next to this file to require a password.
  Uploads are refused while a shot is brewing.

  Pin maps come from shotStopper.ino (ESP32-C3 for the shotStopper PCB).

  BUILD: WiFi + BLE no longer fit the default 1.25MB app slot. Use the
  "No FS 4MB (2MB APP x2)" partition scheme, which keeps OTA. Switch style can
  be set without editing shotStopper.ino, e.g. for a GS3 AV:
    arduino-cli compile --fqbn esp32:esp32:esp32c3:PartitionScheme=no_fs \
      --library ../.. --build-property "compiler.cpp.extra_flags=-DMOMENTARY=true" .
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>
#if __has_include("ota_secret.h")
#include "ota_secret.h"
#endif

static WebServer museServer(80);
static WiFiManager museWm;
static bool museNetStarted = false;

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

// Start mDNS, OTA and the HTTP API once the station link is up. Deferred so
// the API never fights WiFiManager's setup portal for port 80.
static void museStartNetServices() {
  Serial.print("WiFi connected, IP: ");
  Serial.println(WiFi.localIP());

  ArduinoOTA.setHostname("shotstopper-grinder");  // also starts mDNS
#ifdef MUSE_OTA_PASSWORD
  ArduinoOTA.setPassword(MUSE_OTA_PASSWORD);
#endif
  ArduinoOTA.onStart([]() {
    digitalWrite(OUT, LOW);  // release the brew relay before flashing
    Serial.println("OTA update starting");
  });
  ArduinoOTA.onError([](ota_error_t e) {
    Serial.printf("OTA error %u\n", e);
  });
  ArduinoOTA.begin();
  MDNS.addService("http", "tcp", 80);
  Serial.println("mDNS: http://shotstopper-grinder.local");

  museServer.on("/status", HTTP_GET, museHandleStatus);
  museServer.on("/target", HTTP_POST, museHandleTarget);
  museServer.on("/last", HTTP_GET, museHandleLast);
  museServer.onNotFound([]() {
    museServer.send(404, "application/json", "{\"error\":\"not found\"}");
  });
  museServer.begin();
  Serial.println("Muse HTTP API on port 80");
  museNetStarted = true;
}

void museWifiSetup() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  museWm.setConfigPortalBlocking(false);
  museWm.setConfigPortalTimeout(180);  // close the setup AP after 3 min
  museWm.setConnectTimeout(10);
  // With saved credentials this waits up to 10s for the link; otherwise it
  // opens the setup AP and returns immediately. museWifiLoop() finishes up.
  museWm.autoConnect("shotStopper-Setup");
}

void museWifiLoop() {
  museWm.process();  // drives the non-blocking setup portal
  if (!museNetStarted && WiFi.status() == WL_CONNECTED &&
      !museWm.getConfigPortalActive()) {
    museStartNetServices();
  }
  if (museNetStarted) {
    // Never reflash mid-shot: the pump relay is live.
    if (!shot.brewing) ArduinoOTA.handle();
    museServer.handleClient();
  }

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
