/**
 * @file main.cpp
 * @brief ESP32-C3 MeteoRadar + ADSB Plane Radar (Slovakia)
 * @details Obsahuje obnovené NVS ukladanie, WiFiManager parametre (vrátane nastavenia intervalu karuselu), 
 *          dynamické filtrovanie miest pri počasí a zelené radarové pozadie s Edge Dots pre lietadlá.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SPIFFS.h>
#include <Preferences.h>
#include <WiFiManager.h>
#include <PNGdec.h>
#include <ArduinoJson.h>
#include <math.h>

#include "config.h"
#include "display_gc9a01.h"

// ==========================================
// 1. GLOBAL OBJECTS & CONFIGURATION DATA
// ==========================================
LGFX tft;
PNG png;
File pngFile;

static const char* RADAR_FILE = "/radar.png";

// Detailnejšia hranica SR (zahustený polygón pre režim Počasia)
static const float SK_BORDER[][2] = {
  {16.96, 48.48}, {16.90, 48.38}, {16.85, 48.28}, {16.95, 48.21}, {17.06, 48.14},
  {17.11, 48.08}, {17.16, 48.02}, {17.40, 47.90}, {17.65, 47.78}, {17.87, 47.77},
  {18.10, 47.76}, {18.20, 47.77}, {18.30, 47.78}, {18.52, 47.78}, {18.75, 47.79},
  {18.78, 47.93}, {18.82, 48.08}, {18.91, 48.12}, {19.00, 48.16}, {19.41, 48.12},
  {19.82, 48.08}, {19.94, 48.17}, {20.07, 48.27}, {20.26, 48.38}, {20.45, 48.50},
  {20.53, 48.52}, {20.61, 48.55}, {20.83, 48.53}, {21.05, 48.52}, {21.36, 48.44},
  {21.68, 48.37}, {21.91, 48.40}, {22.15, 48.44}, {22.14, 48.30}, {22.14, 48.16},
  {22.35, 48.62}, {22.56, 49.08}, {22.47, 49.10}, {22.38, 49.12}, {21.84, 49.27},
  {21.31, 49.42}, {21.08, 49.41}, {20.85, 49.40}, {20.75, 49.40}, {20.65, 49.41},
  {20.37, 49.37}, {20.10, 49.33}, {19.84, 49.38}, {19.58, 49.44}, {19.40, 49.48},
  {19.22, 49.52}, {19.03, 49.51}, {18.84, 49.51}, {18.64, 49.48}, {18.45, 49.45},
  {18.25, 49.31}, {18.06, 49.18}, {17.95, 49.06}, {17.85, 48.95}, {17.73, 48.91},
  {17.62, 48.87}, {17.47, 48.86}, {17.32, 48.85}, {17.19, 48.81}, {17.07, 48.77},
  {17.01, 48.62}, {16.96, 48.48}
};
static constexpr size_t SK_BORDER_COUNT = sizeof(SK_BORDER) / sizeof(SK_BORDER[0]);

// Štruktúra miest s príznakom priority zobrazenia (isMajor)
struct City { const char* name; float lat; float lon; bool isMajor; };
static const City CITIES[] = {
  {"BA", 48.1486, 17.1077, true},  {"TT", 48.3775, 17.5883, false},
  {"NR", 48.3061, 18.0864, true},  {"TN", 48.8945, 18.0444, false},
  {"ZA", 49.2231, 18.7397, true},  {"BB", 48.7363, 19.1462, true},
  {"PO", 48.9984, 21.2393, true},  {"KE", 48.7164, 21.2611, true},
  {"BJ", 49.2918, 21.2727, false}, {"PP", 49.0595, 20.2978, false},
  {"MI", 48.7547, 21.9195, false}, {"LC", 48.3294, 19.6648, false}
};
static constexpr size_t CITY_COUNT = sizeof(CITIES) / sizeof(CITIES[0]);

struct CropBox { int x1, y1, x2, y2; int w() const { return x2 - x1 + 1; } int h() const { return y2 - y1 + 1; } };
CropBox crop;
uint16_t line565[RADAR_IMG_W];
uint16_t outLine[TFT_W];

String lastPngName;
Preferences prefs;

float centerLat = atof(DEFAULT_CENTER_LAT);
float centerLon = atof(DEFAULT_CENTER_LON);
int timeOffsetHours = DEFAULT_TIME_OFFSET_HOURS;
int carouselIntervalSec = 30; // Predvolený interval prepínania v sekundách
uint32_t carouselIntervalMs = 30000;

static const float ZOOM_LEVELS_KM[] = {10.0f, 25.0f, 50.0f, 100.0f, 250.0f};
static constexpr int ZOOM_LEVEL_COUNT = sizeof(ZOOM_LEVELS_KM) / sizeof(ZOOM_LEVELS_KM[0]);
int zoomIndex = 1;
float currentRadiusKm = atof(DEFAULT_RADIUS_KM_TEXT);

// Button variables
bool lastButtonReading = HIGH;
bool stableButtonState = HIGH;
uint32_t lastButtonChangeMs = 0;
uint32_t buttonPressStartMs = 0;
bool longPressHandled = false;
static constexpr uint32_t BUTTON_DEBOUNCE_MS = 50;

// === CAROUSEL STATE MACHINE VARIABLES ===
enum AppMode { MODE_WEATHER, MODE_PLANES };
AppMode currentMode = MODE_WEATHER;

uint32_t lastWeatherUpdateMs = 0;
uint32_t lastCarouselSwitchMs = 0;
uint32_t lastPlaneFetchMs = 0;

static constexpr uint32_t PLANE_FETCH_INTERVAL_MS = 10000;

struct AircraftData {
  float lat;
  float lon;
  float track;
  float nose_deg;
  float gs_knots;
  float vrate_fpm;
  bool is_mil;
  char route[8];
  char callsign[9];
  char type[5];
  char alt[12];
};

// Forward Declarations
bool renderRadar();
void fetchAndDrawPlanes();
void drawWeatherOverlay(bool showTime);
void drawPlaneRadarGrid();


// ==========================================
// 2. GEOGRAPHIC & PROJECTION MAPPING HELPERS
// ==========================================

int lonToX(float lon) { return roundf((lon - LON_LEFT) * (RADAR_IMG_W - 1) / (LON_RIGHT - LON_LEFT)); }
int latToY(float lat) { return roundf((LAT_TOP - lat) * (RADAR_IMG_H - 1) / (LAT_TOP - LAT_BOTTOM)); }
float mapXToScreenX(float mapX) { return (mapX - crop.x1) * (float)TFT_W / (float)crop.w(); }
float mapYToScreenY(float mapY) { return (mapY - crop.y1) * (float)TFT_H / (float)crop.h(); }

CropBox makeCrop(float lat, float lon, float radiusKm) {
  float degLat = radiusKm / 111.32f;
  float degLon = radiusKm / (111.32f * cosf(lat * DEG_TO_RAD));
  int x1 = lonToX(lon - degLon);
  int x2 = lonToX(lon + degLon);
  int y1 = latToY(lat + degLat);
  int y2 = latToY(lat - degLat);
  x1 = constrain(x1, 0, RADAR_IMG_W - 1); x2 = constrain(x2, 0, RADAR_IMG_W - 1);
  y1 = constrain(y1, 0, RADAR_IMG_H - 1); y2 = constrain(y2, 0, RADAR_IMG_H - 1);
  if (x2 < x1) std::swap(x1, x2);
  if (y2 < y1) std::swap(y1, y2);
  return {x1, y1, x2, y2};
}

// ==========================================
// UI, PREFERENCES, WIFI
// ==========================================

void showStatus(const String& text) {
  tft.setFont(&fonts::Font0);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  int y = 80;
  int start = 0;
  while (true) {
    int pos = text.indexOf('\n', start);
    String line = (pos == -1) ? text.substring(start) : text.substring(start, pos);
    tft.setTextDatum(textdatum_t::middle_center);
    tft.drawString(line, TFT_W / 2, y);
    y += 12;
    if (pos == -1) break;
    start = pos + 1;
  }
}

String getRadarTimeText(const String& filename) {
  const String prefix = "cmax.kruh.";
  int start = filename.indexOf(prefix);
  if (start < 0) return "--:--";
  int dateStart = start + prefix.length();
  if (filename.length() < dateStart + 13) return "--:--";
  String hhmm = filename.substring(dateStart + 9, dateStart + 13);
  int hour = hhmm.substring(0, 2).toInt();
  int minute = hhmm.substring(2, 4).toInt();
  hour = (hour + timeOffsetHours) % 24;
  if (hour < 0) hour += 24;
  char out[6];
  snprintf(out, sizeof(out), "%02d:%02d", hour, minute);
  return String(out);
}

void resetSettingsAndRestart() {
  showStatus("Reset nastavenia...");
  WiFiManager wm; wm.resetSettings();
  prefs.begin("radar", false); prefs.clear(); prefs.end();
  delay(1000); ESP.restart();
}

void checkResetButtonAtBoot() {
  if (digitalRead(ZOOM_BUTTON_PIN) != LOW) return;
  showStatus("Drz pre reset");
  uint32_t start = millis();
  while (digitalRead(ZOOM_BUTTON_PIN) == LOW) {
    if (millis() - start >= RESET_HOLD_MS) resetSettingsAndRestart();
    delay(20);
  }
}

void nextZoomLevel() {
  zoomIndex = (zoomIndex + 1) % ZOOM_LEVEL_COUNT;
  currentRadiusKm = ZOOM_LEVELS_KM[zoomIndex];
  crop = makeCrop(centerLat, centerLon, currentRadiusKm);
  
  if (currentMode == MODE_WEATHER) renderRadar();
  else fetchAndDrawPlanes();
}

void handleZoomButton() {
  bool reading = digitalRead(ZOOM_BUTTON_PIN);
  if (reading != lastButtonReading) {
    lastButtonChangeMs = millis();
    lastButtonReading = reading;
  }
  if ((millis() - lastButtonChangeMs) > BUTTON_DEBOUNCE_MS && reading != stableButtonState) {
    stableButtonState = reading;
    if (stableButtonState == LOW) {
      buttonPressStartMs = millis();
      longPressHandled = false;
    } else {
      if (!longPressHandled) {
        nextZoomLevel();
        prefs.begin("radar", false);
        prefs.putFloat("radius", currentRadiusKm);
        prefs.end();
      }
    }
  }
  if (stableButtonState == LOW && !longPressHandled && buttonPressStartMs > 0) {
    if (millis() - buttonPressStartMs >= RESET_HOLD_MS) {
      longPressHandled = true;
      resetSettingsAndRestart();
    }
  }
}

void connectWiFi() {
  WiFi.mode(WIFI_STA); delay(100);
  showStatus("ESP MeteoRadar v1.2\nPripajam WiFi...");

  // Načítanie uložených hodnôt pre predvyplnenie
  prefs.begin("radar", true);
  String curLat = String(prefs.getFloat("lat", atof(DEFAULT_CENTER_LAT)), 4);
  String curLon = String(prefs.getFloat("lon", atof(DEFAULT_CENTER_LON)), 4);
  String curRad = String((int)prefs.getFloat("radius", atof(DEFAULT_RADIUS_KM_TEXT)));
  String curOff = String(prefs.getInt("offset", DEFAULT_TIME_OFFSET_HOURS));
  String curCar = String(prefs.getInt("car_int", 30));
  prefs.end();

  WiFiManagerParameter custom_lat("lat", "Zemepisna sirka (Lat)", curLat.c_str(), 10);
  WiFiManagerParameter custom_lon("lon", "Zemepisna dlzka (Lon)", curLon.c_str(), 10);
  WiFiManagerParameter custom_rad("radius", "Predvoleny rozsah (km)", curRad.c_str(), 5);
  WiFiManagerParameter custom_off("offset", "Casovy offset (hodiny)", curOff.c_str(), 3);
  WiFiManagerParameter custom_car("car_int", "Interval karuselu (sekundy)", curCar.c_str(), 4);

  WiFiManager wm;
  wm.setConfigPortalTimeout(300);
  wm.setConnectTimeout(15);
  wm.setBreakAfterConfig(true);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  wm.addParameter(&custom_lat);
  wm.addParameter(&custom_lon);
  wm.addParameter(&custom_rad);
  wm.addParameter(&custom_off);
  wm.addParameter(&custom_car);

  if (!wm.autoConnect("ESPMeteoRadar")) {
    showStatus("WiFi chyba\nPodrz tlacidlo 3s\npre reset");
    return;
  }

  // Uloženie nových hodnôt z portálu do NVS
  prefs.begin("radar", false);
  if (strlen(custom_lat.getValue()) > 0) {
    centerLat = atof(custom_lat.getValue());
    prefs.putFloat("lat", centerLat);
  }
  if (strlen(custom_lon.getValue()) > 0) {
    centerLon = atof(custom_lon.getValue());
    prefs.putFloat("lon", centerLon);
  }
  if (strlen(custom_rad.getValue()) > 0) {
    currentRadiusKm = atof(custom_rad.getValue());
    prefs.putFloat("radius", currentRadiusKm);
  }
  if (strlen(custom_off.getValue()) > 0) {
    timeOffsetHours = atoi(custom_off.getValue());
    prefs.putInt("offset", timeOffsetHours);
  }
  if (strlen(custom_car.getValue()) > 0) {
    carouselIntervalSec = atoi(custom_car.getValue());
    if (carouselIntervalSec < 5) carouselIntervalSec = 5; // Bezpečnostné minimum 5s
    prefs.putInt("car_int", carouselIntervalSec);
  }
  prefs.end();

  carouselIntervalMs = (uint32_t)carouselIntervalSec * 1000;
}


// ==========================================
// SHMÚ RADAR DOWNLOAD & PARSING
// ==========================================

String findLatestPngNameInText(const String& text, String& newestTs) {
  const String prefix = "cmax.kruh.";
  String latest;
  int pos = 0;
  while (true) {
    int idx = text.indexOf(prefix, pos);
    if (idx < 0) break;
    int end = text.indexOf(".png", idx);
    if (end < 0) break;
    String name = text.substring(idx, end + 4);
    String ts = name.substring(name.indexOf(prefix) + prefix.length(), name.indexOf(prefix) + prefix.length() + 13);
    if (ts > newestTs) { newestTs = ts; latest = name; }
    pos = end + 4;
  }
  return latest;
}

bool downloadLatestRadar() {
  if (WiFi.status() != WL_CONNECTED) return false;
  
  for (int attempt = 1; attempt <= 2; attempt++) {
    WiFiClientSecure client; client.setInsecure(); client.setHandshakeTimeout(15000);
    HTTPClient http; http.setTimeout(15000);

    if (http.begin(client, SHMU_API_URL)) {
      if (http.GET() == HTTP_CODE_OK) {
        String window, latest, newestTs;
        WiFiClient* stream = http.getStreamPtr();
        uint8_t buf[512];
        while (http.connected()) {
          size_t avail = stream->available();
          if (avail) {
            size_t toRead = (avail < sizeof(buf)) ? avail : sizeof(buf);
            int n = stream->readBytes(buf, toRead);
            window += String((const char*)buf, n);
            String candidate = findLatestPngNameInText(window, newestTs);
            if (!candidate.isEmpty()) latest = candidate;
            if (window.length() > 300) window = window.substring(window.length() - 200);
          } else delay(1);
          if (stream->available() == 0 && !http.connected()) break;
        }
        http.end(); client.stop();

        if (!latest.isEmpty()) {
          if (latest == lastPngName && SPIFFS.exists(RADAR_FILE)) return true;
          String url = String(SHMU_BASE_URL) + latest;
          HTTPClient httpImg; httpImg.setTimeout(25000);
          if (httpImg.begin(client, url) && httpImg.GET() == HTTP_CODE_OK) {
            File f = SPIFFS.open(RADAR_FILE, "w");
            if (f) {
              httpImg.writeToStream(&f);
              f.close();
              lastPngName = latest;
              return true;
            }
          }
        }
      }
    }
    client.stop();
    if (attempt < 2) delay(3000);
  }
  return false;
}

// ==========================================
// ADSB PLANE RADAR MODULE
// ==========================================

void drawAircraftSymbol(int x, int y, float heading_deg, float track_deg, float gs_knots, bool is_mil) {
  if (gs_knots > 0.0f) {
    constexpr float kDegToRad = 0.01745329252f;
    float rad = track_deg * kDegToRad;
    int len = constrain((int)(gs_knots * 0.06f), 6, 18);
    int ex = x + (int)roundf(sinf(rad) * len);
    int ey = y - (int)roundf(cosf(rad) * len);
    tft.drawWideLine(x, y, ex, ey, 1, tft.color565(255, 255, 255));
  }

  constexpr float kDegToRad = 0.01745329252f;
  float rad = heading_deg * kDegToRad;
  float sin_h = sinf(rad);
  float cos_h = cosf(rad);

  int tip_x = x + (int)roundf(sin_h * 5);
  int tip_y = y - (int)roundf(cos_h * 5);
  int base_x = x - (int)roundf(sin_h * 2);
  int base_y = y + (int)roundf(cos_h * 2);
  int wing_x = (int)roundf(cos_h * 3);
  int wing_y = (int)roundf(sin_h * 3);

  uint16_t symbolColor = is_mil ? tft.color565(255, 0, 0) : tft.color565(0, 120, 255);

  tft.fillTriangle(tip_x, tip_y, base_x + wing_x, base_y + wing_y, base_x - wing_x, base_y - wing_y, symbolColor);
}

void drawAircraftTag(int x, int y, const AircraftData& ac) {
  tft.setFont(&fonts::Font0);
  bool tagOnRight = x < (TFT_W / 2);
  int anchorX = tagOnRight ? (x + 8) : (x - 8);
  
  bool hasRoute = (ac.route[0] != '\0');
  int yOffset = (y > (TFT_H / 2)) ? (hasRoute ? -28 : -21) : 4;
  int currentY = y + yOffset;

  tft.setTextDatum(tagOnRight ? textdatum_t::top_left : textdatum_t::top_right);

  if (hasRoute) {
    tft.setTextColor(tft.color565(255, 130, 255), TFT_BLACK);
    tft.drawString(ac.route, anchorX, currentY);
    currentY += 7;
  }

  uint16_t callsignColor = ac.is_mil ? tft.color565(255, 60, 60) : tft.color565(255, 255, 255);
  tft.setTextColor(callsignColor, TFT_BLACK);
  tft.drawString(ac.callsign, anchorX, currentY);
  currentY += 7;

  tft.setTextColor(tft.color565(100, 200, 255), TFT_BLACK);
  tft.drawString(ac.type, anchorX, currentY);
  currentY += 7;

  tft.setTextColor(tft.color565(255, 255, 0), TFT_BLACK);
  tft.drawString(ac.alt, anchorX, currentY);

  if (ac.vrate_fpm >= 150.0f) {
    int ax = tagOnRight ? (anchorX + tft.textWidth(ac.alt) + 3) : (anchorX - tft.textWidth(ac.alt) - 6);
    tft.fillTriangle(ax, currentY + 4, ax + 2, currentY + 1, ax + 4, currentY + 4, tft.color565(0, 255, 0));
  } else if (ac.vrate_fpm <= -150.0f) {
    int ax = tagOnRight ? (anchorX + tft.textWidth(ac.alt) + 3) : (anchorX - tft.textWidth(ac.alt) - 6);
    tft.fillTriangle(ax, currentY + 1, ax + 2, currentY + 4, ax + 4, currentY + 1, tft.color565(255, 0, 0));
  }
}

void drawEdgeIndicator(int mapX, int mapY, bool is_mil) {
  int cx = TFT_W / 2;
  int cy = TFT_H / 2;

  float dx = (float)(mapX - crop.x1) * TFT_W / crop.w() - cx;
  float dy = (float)(mapY - crop.y1) * TFT_H / crop.h() - cy;

  float angle = atan2f(dy, dx);
  int edgeX = cx + (int)(roundf(cosf(angle) * 112.0f));
  int edgeY = cy + (int)(roundf(sinf(angle) * 112.0f));

  uint16_t dotColor = is_mil ? tft.color565(255, 0, 0) : tft.color565(255, 140, 0);

  tft.fillCircle(edgeX, edgeY, 3, dotColor);
  tft.drawCircle(edgeX, edgeY, 3, TFT_BLACK);
}

void fetchAndDrawPlanes() {
  if (WiFi.status() != WL_CONNECTED) return;

  float radiusNm = currentRadiusKm / 1.852f;
  String url = "https://opendata.adsb.fi/api/v3/lat/" + String(centerLat, 4) + "/lon/" + String(centerLon, 4) + "/dist/" + String(radiusNm, 1);

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(10000);

  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(10000);

  if (http.begin(client, url)) {
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      
      JsonDocument filter;
      filter["ac"][0]["lat"] = true;
      filter["ac"][0]["lon"] = true;
      filter["ac"][0]["track"] = true;
      filter["ac"][0]["true_heading"] = true;
      filter["ac"][0]["gs"] = true;
      filter["ac"][0]["flight"] = true;
      filter["ac"][0]["t"] = true;
      filter["ac"][0]["alt_baro"] = true;
      filter["ac"][0]["baro_rate"] = true;
      filter["ac"][0]["dbFlags"] = true;
      filter["ac"][0]["orig_iata"] = true;
      filter["ac"][0]["dest_iata"] = true;

      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
      
      if (!err) {
        JsonArray acList = doc["ac"].as<JsonArray>();
        
        tft.fillScreen(TFT_BLACK);
        
        drawPlaneRadarGrid();

        tft.setFont(&fonts::Font0);
        tft.setTextDatum(textdatum_t::bottom_center);
        tft.setTextColor(tft.color565(0, 255, 0), TFT_BLACK);
        tft.drawString("Lietadiel: " + String(acList.size()), TFT_W / 2, TFT_H - 4);

        int cx = TFT_W / 2;
        int cy = TFT_H / 2;

        for (JsonObject plane : acList) {
          AircraftData ac;
          ac.lat = plane["lat"].as<float>();
          ac.lon = plane["lon"].as<float>();
          ac.track = plane["track"] | 0.0f;
          ac.nose_deg = plane["true_heading"] | ac.track;
          ac.gs_knots = plane["gs"] | 0.0f;
          ac.vrate_fpm = plane["baro_rate"] | 0.0f;

          int dbFlags = plane["dbFlags"] | 0;
          ac.is_mil = (dbFlags & 1) != 0;

          const char* orig = plane["orig_iata"] | "";
          const char* dest = plane["dest_iata"] | "";

          if (strlen(orig) > 0 && strlen(dest) > 0) {
            snprintf(ac.route, sizeof(ac.route), "%s-%s", orig, dest);
          } else {
            ac.route[0] = '\0';
          }

          const char* fl = plane["flight"] | "";
          strlcpy(ac.callsign, fl, sizeof(ac.callsign));
          if (ac.callsign[0] == '\0') strlcpy(ac.callsign, "NOCALL", sizeof(ac.callsign));

          const char* typeStr = plane["t"] | "";
          strlcpy(ac.type, typeStr, sizeof(ac.type));

          if (plane["alt_baro"].is<const char*>() && strcmp(plane["alt_baro"].as<const char*>(), "ground") == 0) {
            strlcpy(ac.alt, "GND", sizeof(ac.alt));
          } else {
            float altFeet = plane["alt_baro"] | 0.0f;
            int altMeters = (int)(altFeet * 0.3048f);
            snprintf(ac.alt, sizeof(ac.alt), "%dm", altMeters);
          }

          int mapX = lonToX(ac.lon);
          int mapY = latToY(ac.lat);
          int sx = (int)mapXToScreenX(mapX);
          int sy = (int)mapYToScreenY(mapY);

          float distFromCenter = sqrtf((float)((sx - cx) * (sx - cx) + (sy - cy) * (sy - cy)));

          if (distFromCenter <= 112.0f) {
            drawAircraftSymbol(sx, sy, ac.nose_deg, ac.track, ac.gs_knots, ac.is_mil);
            if (currentRadiusKm <= 50) {
              drawAircraftTag(sx, sy, ac);
            }
          } else {
            drawEdgeIndicator(mapX, mapY, ac.is_mil);
          }
        }
      }
    }
    http.end();
  }
  client.stop();
}


// ==========================================
// GRAPHIC OVERLAYS & RENDERING
// ==========================================

void drawPlaneRadarGrid() {
  int cx = TFT_W / 2;
  int cy = TFT_H / 2;
  uint16_t gridColor = tft.color565(0, 200, 0);     
  uint16_t dimGridColor = tft.color565(0, 80, 0);   

  tft.drawLine(cx - 110, cy, cx + 110, cy, dimGridColor);
  tft.drawLine(cx, cy - 110, cx, cy + 110, dimGridColor);

  tft.drawCircle(cx, cy, 35, gridColor);
  tft.drawCircle(cx, cy, 70, gridColor);
  tft.drawCircle(cx, cy, 105, gridColor);

  tft.setFont(&fonts::Font0);
  tft.setTextDatum(textdatum_t::middle_center);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("N", cx, 8);
  tft.drawString("S", cx, TFT_H - 8);
  tft.drawString("W", 8, cy);
  tft.drawString("E", TFT_W - 8, cy);

  tft.setTextDatum(textdatum_t::middle_right);
  tft.setTextColor(tft.color565(0, 255, 0), TFT_BLACK);
  tft.drawString(String((int)currentRadiusKm) + "km", TFT_W - 16, cy);
}

void drawWeatherOverlay(bool showTime) {
  int cx = TFT_W / 2;
  int cy = TFT_H / 2;

  tft.setFont(&fonts::Font0);

  for (size_t i = 0; i < SK_BORDER_COUNT - 1; i++) {
    int sx1 = (int)mapXToScreenX(lonToX(SK_BORDER[i][0]));
    int sy1 = (int)mapYToScreenY(latToY(SK_BORDER[i][1]));
    int sx2 = (int)mapXToScreenX(lonToX(SK_BORDER[i+1][0]));
    int sy2 = (int)mapYToScreenY(latToY(SK_BORDER[i+1][1]));
    tft.drawLine(sx1, sy1, sx2, sy2, TFT_CYAN);
  }

  tft.setTextDatum(textdatum_t::middle_center);
  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  for (size_t i = 0; i < CITY_COUNT; i++) {
    if (currentRadiusKm >= 100.0f && !CITIES[i].isMajor) continue;

    int sx = (int)mapXToScreenX(lonToX(CITIES[i].lon));
    int sy = (int)mapYToScreenY(latToY(CITIES[i].lat));
    if (sx >= 10 && sx <= TFT_W - 10 && sy >= 10 && sy <= TFT_H - 10) {
      tft.drawLine(sx - 2, sy, sx + 2, sy, TFT_RED);
      tft.drawLine(sx, sy - 2, sx, sy + 2, TFT_RED);
      tft.drawString(CITIES[i].name, sx, sy - 6);
    }
  }

  tft.drawCircle(cx, cy, TFT_W / 2 - 2, TFT_DARKGREY);
  tft.drawCircle(cx, cy, TFT_W / 4, TFT_DARKGREY);
  tft.drawLine(cx - 6, cy, cx + 6, cy, TFT_WHITE);
  tft.drawLine(cx, cy - 6, cx, cy + 6, TFT_WHITE);

  tft.setTextDatum(textdatum_t::top_center);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(String((int)currentRadiusKm) + " km", cx, 4);

  if (showTime) {
    tft.setTextDatum(textdatum_t::bottom_center);
    tft.drawString(getRadarTimeText(lastPngName), cx, TFT_H - 4);
  }
}

// ==========================================
// PNG DECODER & RESAMPLING CALLBACKS
// ==========================================

void* pngOpen(const char* filename, int32_t* size) {
  pngFile = SPIFFS.open(filename, "r");
  if (!pngFile) return nullptr;
  *size = pngFile.size();
  return &pngFile;
}

void pngClose(void* handle) {
  if (pngFile) pngFile.close();
}

int32_t pngRead(PNGFILE* handle, uint8_t* buffer, int32_t length) {
  return pngFile.read(buffer, length);
}

int32_t pngSeek(PNGFILE* handle, int32_t position) {
  return pngFile.seek(position) ? position : -1;
}

int drawPngLine(PNGDRAW* pDraw) {
  int srcY = pDraw->y;
  if (srcY < crop.y1 || srcY > crop.y2) return 1;
  png.getLineAsRGB565(pDraw, line565, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);
  float startScreenY = mapYToScreenY((float)srcY);
  float endScreenY = mapYToScreenY((float)srcY + 1.0f);
  int syMin = constrain((int)floorf(startScreenY), 0, (int)(TFT_H - 1));
  int syMax = constrain((int)ceilf(endScreenY), syMin, (int)(TFT_H - 1));

  for (int dx = 0; dx < TFT_W; dx++) {
    int srcXInt = constrain((int)mapXToScreenX(crop.x1 + ((float)dx / TFT_W) * crop.w()), 0, RADAR_IMG_W - 1);
    outLine[dx] = line565[constrain((int)(crop.x1 + ((float)dx / TFT_W) * crop.w()), 0, RADAR_IMG_W - 1)];
  }

  for (int sy = syMin; sy <= syMax; sy++) {
    tft.pushImage(0, sy, TFT_W, 1, outLine);
  }
  return 1;
}

bool renderRadar() {
  if (!SPIFFS.exists(RADAR_FILE)) return false;
  tft.fillScreen(TFT_BLACK);
  if (png.open(RADAR_FILE, pngOpen, pngClose, pngRead, pngSeek, drawPngLine) == PNG_SUCCESS) {
    png.decode(nullptr, 0);
    png.close();
  }
  drawWeatherOverlay(true);
  return true;
}

// ==========================================
// 9. MAIN ARDUINO ENTRY POINTS
// ==========================================

void setup() {
  Serial.begin(115200);
  tft.init(); tft.setRotation(0); tft.setBrightness(180); 
  
  tft.setFont(&fonts::Font0);

  pinMode(ZOOM_BUTTON_PIN, INPUT_PULLUP);
  
  prefs.begin("radar", true);
  centerLat = prefs.getFloat("lat", atof(DEFAULT_CENTER_LAT));
  centerLon = prefs.getFloat("lon", atof(DEFAULT_CENTER_LON));
  currentRadiusKm = prefs.getFloat("radius", atof(DEFAULT_RADIUS_KM_TEXT));
  timeOffsetHours = prefs.getInt("offset", DEFAULT_TIME_OFFSET_HOURS);
  carouselIntervalSec = prefs.getInt("car_int", 30);
  prefs.end();

  if (carouselIntervalSec < 5) carouselIntervalSec = 5;
  carouselIntervalMs = (uint32_t)carouselIntervalSec * 1000;

  checkResetButtonAtBoot();
  SPIFFS.begin(true);
  connectWiFi();
  
  crop = makeCrop(centerLat, centerLon, currentRadiusKm);

  tft.fillScreen(TFT_BLACK);
  drawWeatherOverlay(false);
  tft.setFont(&fonts::Font0);
  tft.setTextDatum(textdatum_t::bottom_center);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString("Nacitavam data...", TFT_W / 2, TFT_H - 12);

  if (downloadLatestRadar()) {
    renderRadar();
  }
  
  lastWeatherUpdateMs = millis();
  lastCarouselSwitchMs = millis();
}

void loop() {
  uint32_t now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    delay(1000);
  }

  // Prepínanie režimov podľa nastaveného intervalu
  if (now - lastCarouselSwitchMs >= carouselIntervalMs) {
    lastCarouselSwitchMs = now;
    currentMode = (currentMode == MODE_WEATHER) ? MODE_PLANES : MODE_WEATHER;
    
    if (currentMode == MODE_WEATHER) {
      Serial.println("Karusel: Prepínam na POČASIE");
      renderRadar();
    } else {
      Serial.println("Karusel: Prepínam na LIETADLÁ");
      fetchAndDrawPlanes();
      lastPlaneFetchMs = now;
    }
  }

  if (currentMode == MODE_WEATHER) {
    if (now - lastWeatherUpdateMs >= UPDATE_INTERVAL_MS) {
      lastWeatherUpdateMs = now;
      if (downloadLatestRadar()) renderRadar();
    }
  } else if (currentMode == MODE_PLANES) {
    if (now - lastPlaneFetchMs >= PLANE_FETCH_INTERVAL_MS) {
      lastPlaneFetchMs = now;
      fetchAndDrawPlanes();
    }
  }
  
  handleZoomButton();
  delay(20);
}