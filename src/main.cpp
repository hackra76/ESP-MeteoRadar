/**
 * =======================================================================================
 * @file main.cpp
 * @brief ESP32-C3 MeteoRadar + ADS-B Plane Radar (Slovakia)
 * @author hackra76 / Antigravity AI
 * @version 1.4.0
 * 
 * @details 
 * Multifunkčný radar pre okrúhly GC9A01 240x240 displej a ESP32-C3 SuperMini.
 * 
 * Nové a pokročilé funkcie vo v1.4.0:
 * 1. 🔘 MULTI-CLICK TLAČIDLO:
 *    - 1x klik: Cyklická zmena zoomu (10, 25, 50, 100, 250 km).
 *    - 2x klik (Dvojklik): Okamžité manuálne prepnutie režimu (Počasie <-> Lietadlá).
 *    - Dlhé podržanie (3s): Továrenský reset WiFi a NVS pamäte.
 * 
 * 2. ✈️ PLYNULÁ EXTRAPOLÁCIA POHYBU LIETADIEL (Dead Reckoning):
 *    - Plynulý pohyb lietadiel po displeji každú sekundu na základe kurzu a rýchlosti.
 *    - Odstraňuje 10-sekundové skoky medzi ADS-B dopytmi.
 * 
 * 3. 🌙 NOČNÝ REŽIM (Auto-Dimming):
 *    - Automatické zníženie jasu displeja počas noci (22:00 - 06:00) podľa NTP času.
 * 
 * 4. 🌐 LOKÁLNY WEB DASHBOARD (Web Server na porte 80):
 *    - Moderné webové rozhranie s dark-mode dizajnom dostupné na IP adrese dosky.
 *    - Živý zoznam sledovaných lietadiel (Callsign, Trasa, Typ, Rýchlosť, Výška).
 *    - Diaľkové ovládanie zoomu, prepínania režimov a úprava GPS súradníc bez resetu.
 * =======================================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <SPIFFS.h>
#include <Preferences.h>
#include <WiFiManager.h>
#include <PNGdec.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <math.h>

#include "config.h"
#include "display_gc9a01.h"
#include "ui_font.h"

static const char* CURRENT_VERSION = "v1.5.0";

// =======================================================================================
// 1. GLOBÁLNE INŠTANCIE & DÁTOVÉ ŠTRUKTÚRY
// =======================================================================================

LGFX tft;                 ///< Ovládač displeja LovyanGFX
LGFX_Sprite canvas(&tft); ///< Dynamický offscreen buffer (Double Buffering)
bool canvasReady = false;
PNG png;                  ///< PNG dekodér pre SHMÚ radarové snímky
File pngFile;             ///< Súborový deskriptor pre SPIFFS
Preferences prefs;        ///< Trvalé úložisko NVS pre konfiguráciu
WebServer server(80);     ///< Lokálny webový server na porte 80

void ensureCanvas() {
  if (!canvasReady) {
    canvas.setColorDepth(8);
    if (canvas.createSprite(TFT_W, TFT_H)) {
      canvas.loadFont(ui_font_vlw, lgfx::IFont::font_type_t::ft_vlw);
      canvas.setTextSize(0.80f);
      canvasReady = true;
    }
  }
}

void releaseCanvas() {
  if (canvasReady) {
    canvas.deleteSprite();
    canvasReady = false;
  }
}

static const char* RADAR_FILE = "/radar.png";

// Polygón štátnej hranice Slovenskej republiky (zahustené GPS body)
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

// Zoznam miest pre orientáciu na mape (isMajor = zobrazené aj pri veľkom odzoomovaní)
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

// Výrez (Crop) radarového obrazu
struct CropBox { 
  int x1, y1, x2, y2; 
  int w() const { return x2 - x1 + 1; } 
  int h() const { return y2 - y1 + 1; } 
};
CropBox crop;

// Vyrovnávacie pamäte pre dekódovanie PNG riadkov
uint16_t line565[RADAR_IMG_W];
uint16_t outLine[TFT_W];

// Globálne premenné pre konfiguráciu a stav
String lastPngName;
float centerLat = atof(DEFAULT_CENTER_LAT);
float centerLon = atof(DEFAULT_CENTER_LON);
int timeOffsetHours = DEFAULT_TIME_OFFSET_HOURS;
int carouselIntervalSec = 30;
uint32_t carouselIntervalMs = 30000;
bool carouselEnabled = true;

// Nočný režim
bool nightModeEnabled = true;
int nightStartHour = 22; // 22:00
int nightEndHour = 6;    // 06:00
bool isNightActive = false;

// Úrovne priblíženia (km)
static const float ZOOM_LEVELS_KM[] = {10.0f, 25.0f, 50.0f, 100.0f, 250.0f};
static constexpr int ZOOM_LEVEL_COUNT = sizeof(ZOOM_LEVELS_KM) / sizeof(ZOOM_LEVELS_KM[0]);
int zoomIndex = 2; // Predvolene 50 km
float currentRadiusKm = atof(DEFAULT_RADIUS_KM_TEXT);

// Stavový automat aplikácie (Karusel)
enum AppMode { MODE_WEATHER, MODE_PLANES };
AppMode currentMode = MODE_WEATHER;

uint32_t lastWeatherUpdateMs = 0;
uint32_t lastCarouselSwitchMs = 0;
uint32_t lastPlaneFetchMs = 0;
uint32_t lastPlaneRedrawMs = 0;
uint32_t lastPlaneFetchFixMs = 0;
static constexpr uint32_t PLANE_FETCH_INTERVAL_MS = 10000;
static constexpr uint32_t PLANE_REDRAW_INTERVAL_MS = 1000; // Plynulé prekreslenie každú 1s

// Dátový model lietadla pre vykreslenie štítku
struct AircraftData {
  float lat;
  float lon;
  float track;
  float nose_deg;
  float gs_knots;
  float vrate_fpm;
  bool is_mil;
  char route[10];    ///< Formát letísk napr. "VIE>AMS"
  char callsign[9];  ///< Volací znak napr. "KLM1902"
  char type[5];      ///< Typ ICAO napr. "A21N"
  char alt[12];      ///< Výška v metroch napr. "10250m"
};

static constexpr size_t MAX_AIRCRAFT = 32;
AircraftData aircraftList[MAX_AIRCRAFT];
size_t aircraftCount = 0;

// Dopredné deklarácie funkcií
bool downloadLatestRadar();
bool renderRadar();
void fetchPlanesData();
void drawPlanes();
void drawWeatherOverlay(bool showTime);
void drawPlaneRadarGrid();
void setupWebServer();


// =======================================================================================
// 2. GEOGRAFICKÉ & PROJEKČNÉ FUNKCIE (MAPPING)
// =======================================================================================

inline int lonToX(float lon) { 
  return roundf((lon - LON_LEFT) * (RADAR_IMG_W - 1) / (LON_RIGHT - LON_LEFT)); 
}

inline int latToY(float lat) { 
  return roundf((LAT_TOP - lat) * (RADAR_IMG_H - 1) / (LAT_TOP - LAT_BOTTOM)); 
}

inline float mapXToScreenX(float mapX) { 
  return (mapX - crop.x1) * (float)TFT_W / (float)crop.w(); 
}

inline float mapYToScreenY(float mapY) { 
  return (mapY - crop.y1) * (float)TFT_H / (float)crop.h(); 
}

CropBox makeCrop(float lat, float lon, float radiusKm) {
  float degLat = radiusKm / 111.32f;
  float degLon = radiusKm / (111.32f * cosf(lat * DEG_TO_RAD));
  
  int x1 = lonToX(lon - degLon);
  int x2 = lonToX(lon + degLon);
  int y1 = latToY(lat + degLat);
  int y2 = latToY(lat - degLat);
  
  x1 = constrain(x1, 0, RADAR_IMG_W - 1); 
  x2 = constrain(x2, 0, RADAR_IMG_W - 1);
  y1 = constrain(y1, 0, RADAR_IMG_H - 1); 
  y2 = constrain(y2, 0, RADAR_IMG_H - 1);
  
  if (x2 < x1) std::swap(x1, x2);
  if (y2 < y1) std::swap(y1, y2);
  
  return {x1, y1, x2, y2};
}


// =======================================================================================
// 3. POUŽÍVATEĽSKÉ ROZHRANIE, NASTAVENIA & NOČNÝ REŽIM
// =======================================================================================

void showStatus(const String& text) {
  tft.setTextSize(0.75f);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  int y = 75;
  int start = 0;
  while (true) {
    int pos = text.indexOf('\n', start);
    String line = (pos == -1) ? text.substring(start) : text.substring(start, pos);
    tft.setTextDatum(textdatum_t::middle_center);
    tft.drawString(line, TFT_W / 2, y);
    y += 18;
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

String getCurrentSystemTimeText() {
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  if (t->tm_year < 100) return "--:--";
  char out[6];
  snprintf(out, sizeof(out), "%02d:%02d", t->tm_hour, t->tm_min);
  return String(out);
}

void resetSettingsAndRestart() {
  showStatus("Reset nastavenia...");
  WiFiManager wm; 
  wm.resetSettings();
  prefs.begin("radar", false); 
  prefs.clear(); 
  prefs.end();
  delay(1000); 
  ESP.restart();
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

/** Kontrola a automatická úprava jasu podľa nočného režimu */
void updateNightMode() {
  if (!nightModeEnabled) {
    if (isNightActive) {
      isNightActive = false;
      tft.setBrightness(180);
    }
    return;
  }
  
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  if (t->tm_year < 100) return; // NTP ešte nie je zosynchronizované

  int hour = t->tm_hour;
  bool shouldBeNight = (hour >= nightStartHour || hour < nightEndHour);
  if (shouldBeNight != isNightActive) {
    isNightActive = shouldBeNight;
    tft.setBrightness(isNightActive ? 30 : 180);
    Serial.printf("Nočný režim: %s (jas %d)\n", isNightActive ? "AKTÍVNY" : "VYPNUTÝ", isNightActive ? 30 : 180);
  }
}

/** Zmena zoomu */
void setZoomIndex(int newIndex) {
  zoomIndex = (newIndex >= 0 && newIndex < ZOOM_LEVEL_COUNT) ? newIndex : 2;
  currentRadiusKm = ZOOM_LEVELS_KM[zoomIndex];
  crop = makeCrop(centerLat, centerLon, currentRadiusKm);
  
  prefs.begin("radar", false);
  prefs.putFloat("radius", currentRadiusKm);
  prefs.end();

  if (currentMode == MODE_WEATHER) {
    renderRadar();
  } else {
    lastPlaneFetchMs = millis();
    fetchPlanesData();
    drawPlanes();
  }
}

/** Prepnutie režimu aplikácie */
void setAppMode(AppMode newMode) {
  currentMode = newMode;
  lastCarouselSwitchMs = millis();
  if (currentMode == MODE_WEATHER) {
    releaseCanvas();
    renderRadar();
  } else {
    ensureCanvas();
    drawPlanes();
    lastPlaneFetchMs = millis();
    fetchPlanesData();
    drawPlanes();
  }
}

/** 
 * Inteligentná obsluha tlačidla:
 * - 1 klik = Zmena zoomu
 * - 2 kliky = Prepnutie režimu (Počasie / Lietadlá)
 * - Dlhé podržanie = Továrenský reset
 */
void handleButton() {
  static bool lastBtnState = HIGH;
  static uint32_t pressStartMs = 0;
  static uint32_t lastReleaseMs = 0;
  static int pendingClicks = 0;
  static constexpr uint32_t DOUBLE_CLICK_GAP_MS = 350;

  bool btnState = digitalRead(ZOOM_BUTTON_PIN);
  uint32_t now = millis();

  // Stlačenie tlačidla
  if (lastBtnState == HIGH && btnState == LOW) {
    pressStartMs = now;
  }
  // Držanie tlačidla
  else if (btnState == LOW) {
    if (now - pressStartMs >= RESET_HOLD_MS) {
      resetSettingsAndRestart();
      return;
    }
  }
  // Uvoľnenie tlačidla
  else if (lastBtnState == LOW && btnState == HIGH) {
    uint32_t duration = now - pressStartMs;
    if (duration >= 30 && duration < RESET_HOLD_MS) {
      pendingClicks++;
      lastReleaseMs = now;
    }
  }
  lastBtnState = btnState;

  // Vyhodnotenie kliknutí po uplynutí okna pre dvojklik
  if (pendingClicks > 0 && (now - lastReleaseMs >= DOUBLE_CLICK_GAP_MS)) {
    if (pendingClicks == 1) {
      // 1x Klik = Zmena zoomu
      setZoomIndex((zoomIndex + 1) % ZOOM_LEVEL_COUNT);
    } else if (pendingClicks >= 2) {
      // 2x Klik = Okamžité prepnutie režimu
      setAppMode((currentMode == MODE_WEATHER) ? MODE_PLANES : MODE_WEATHER);
    }
    pendingClicks = 0;
  }
}

void connectWiFi() {
  WiFi.mode(WIFI_STA); 
  delay(100);
  showStatus("ESP MeteoRadar v1.5\nPripajam WiFi...");

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
    if (carouselIntervalSec < 5) carouselIntervalSec = 5;
    prefs.putInt("car_int", carouselIntervalSec);
  }
  prefs.end();

  carouselIntervalMs = (uint32_t)carouselIntervalSec * 1000;
  configTime(timeOffsetHours * 3600, 0, "pool.ntp.org", "time.nist.gov");

  String ipStr = WiFi.localIP().toString();
  Serial.println("\n=======================================================");
  Serial.printf("WiFi Pripojené! IP adresa: %s\n", ipStr.c_str());
  if (MDNS.begin("espmeteoradar")) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("Web Dashboard: http://espmeteoradar.local alebo http://" + ipStr);
  }
  Serial.println("=======================================================\n");

  showStatus("WiFi Pripojene!\n\nIP: " + ipStr + "\nespmeteoradar.local");
  delay(2500);
}


// =======================================================================================
// 4. LOKÁLNY WEB DASHBOARD (EMBEDDED WEB SERVER)
// =======================================================================================

const char HTML_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="sk">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>ESP MeteoRadar & Plane Dashboard</title>
  <style>
    :root {
      --bg: #0d1117;
      --card-bg: rgba(22, 27, 34, 0.85);
      --border: #30363d;
      --accent: #58a6ff;
      --accent-green: #3fb950;
      --accent-magenta: #d2a8ff;
      --accent-orange: #f0883e;
      --text: #c9d1d9;
      --text-bright: #f0f6fc;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Helvetica, Arial, sans-serif; }
    body { background: var(--bg); color: var(--text); padding: 16px; min-height: 100vh; display: flex; flex-direction: column; align-items: center; }
    .container { width: 100%; max-width: 680px; }
    header { text-align: center; margin-bottom: 20px; }
    header h1 { color: var(--text-bright); font-size: 1.6rem; display: flex; align-items: center; justify-content: center; gap: 8px; }
    header p { color: #8b949e; font-size: 0.9rem; margin-top: 4px; }
    .card { background: var(--card-bg); border: 1px solid var(--border); border-radius: 12px; padding: 16px; margin-bottom: 16px; box-shadow: 0 4px 12px rgba(0,0,0,0.3); backdrop-filter: blur(8px); }
    .card h2 { font-size: 1.1rem; color: var(--text-bright); margin-bottom: 12px; display: flex; align-items: center; justify-content: space-between; }
    .grid-stats { display: grid; grid-template-columns: repeat(auto-fit, minmax(130px, 1fr)); gap: 10px; }
    .stat-box { background: rgba(0,0,0,0.3); padding: 10px; border-radius: 8px; border: 1px solid var(--border); text-align: center; }
    .stat-box .label { font-size: 0.75rem; color: #8b949e; text-transform: uppercase; letter-spacing: 0.5px; }
    .stat-box .val { font-size: 1.15rem; font-weight: bold; color: var(--accent); margin-top: 4px; }
    .stat-box .sub { font-size: 0.75rem; color: #8b949e; margin-top: 3px; }
    .btn-group { display: flex; flex-wrap: wrap; gap: 8px; margin-top: 10px; }
    button, input[type="submit"] { background: #21262d; color: var(--text-bright); border: 1px solid var(--border); padding: 8px 14px; border-radius: 6px; cursor: pointer; font-size: 0.9rem; transition: all 0.2s; }
    button:hover, input[type="submit"]:hover { background: #30363d; border-color: #8b949e; }
    button.active { background: #1f6feb; border-color: #58a6ff; color: #fff; }
    table { width: 100%; border-collapse: collapse; margin-top: 8px; font-size: 0.85rem; }
    th, td { padding: 8px 6px; text-align: left; border-bottom: 1px solid var(--border); }
    th { color: #8b949e; font-weight: 600; }
    .route { color: var(--accent-magenta); font-weight: bold; }
    .speed { color: var(--accent-green); }
    .alt { color: #e3b341; }
    .form-group { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin-bottom: 10px; }
    label { font-size: 0.8rem; color: #8b949e; display: block; margin-bottom: 4px; }
    input[type="text"], input[type="number"], input[type="password"], select { width: 100%; background: #0d1117; border: 1px solid var(--border); padding: 8px; border-radius: 6px; color: var(--text-bright); font-size: 0.9rem; }
    select { cursor: pointer; }
    .badge { padding: 3px 6px; border-radius: 4px; font-size: 0.7rem; font-weight: bold; background: rgba(56, 139, 253, 0.15); color: var(--accent); }
    .badge-green { background: rgba(63, 185, 80, 0.15); color: var(--accent-green); }
  </style>
</head>
<body>
  <div class="container">
    <header>
      <h1>🛰️ ESP MeteoRadar & Plane Dashboard</h1>
      <p>ESP32-C3 SuperMini • GC9A01 240x240 LCD</p>
    </header>

    <!-- KARTA 1: AKTUÁLNY STAV RADARU -->
    <div class="card">
      <h2>📊 Aktuálny stav radaru <span class="badge badge-green" id="live-badge">ŽIVÉ DÁTA</span></h2>
      <div class="grid-stats">
        <div class="stat-box"><div class="label">Režim</div><div class="val" id="mode-val">--</div><div class="sub" id="car-sub">Karusel: ZAP</div></div>
        <div class="stat-box"><div class="label">Zoom</div><div class="val" id="zoom-val">-- km</div><div class="sub">Rozsah radaru</div></div>
        <div class="stat-box"><div class="label">Lietadlá</div><div class="val" id="planes-count">0</div><div class="sub">V okruhu</div></div>
        <div class="stat-box"><div class="label">WiFi Signál</div><div class="val" id="wifi-rssi">-- dBm</div><div class="sub" id="wifi-pct">Kvalita: -- %</div></div>
      </div>
    </div>

    <!-- KARTA 2: STAV SYSTÉMU & HARDVÉRU -->
    <div class="card">
      <h2>🖥️ Systém & Stav hardvéru</h2>
      <div class="grid-stats">
        <div class="stat-box">
          <div class="label">Procesor (CPU)</div>
          <div class="val" id="sys-cpu">160 MHz</div>
          <div class="sub" id="sys-temp">Teplota: -- °C</div>
        </div>
        <div class="stat-box">
          <div class="label">Voľná RAM</div>
          <div class="val" id="sys-ram">-- KB</div>
          <div class="sub" id="sys-ram-sub">z 320 KB</div>
        </div>
        <div class="stat-box">
          <div class="label">Flash Pamäť</div>
          <div class="val" id="sys-flash">4 MB</div>
          <div class="sub" id="sys-heap-min">Min RAM: -- KB</div>
        </div>
        <div class="stat-box">
          <div class="label">Doba behu</div>
          <div class="val" id="sys-uptime">00:00:00</div>
          <div class="sub" id="sys-ip">IP: --</div>
        </div>
      </div>
    </div>

    <!-- KARTA 3: RÝCHLE OVLÁDANIE -->
    <div class="card">
      <h2>🎮 Rýchle ovládanie radaru</h2>
      <label>Zmena mierky (Zoom):</label>
      <div class="btn-group">
        <button onclick="setZoom(0)" id="zbtn-0">10 km</button>
        <button onclick="setZoom(1)" id="zbtn-1">25 km</button>
        <button onclick="setZoom(2)" id="zbtn-2">50 km</button>
        <button onclick="setZoom(3)" id="zbtn-3">100 km</button>
        <button onclick="setZoom(4)" id="zbtn-4">250 km</button>
      </div>
      <label style="margin-top: 14px;">Manuálne prepnutie režimu:</label>
      <div class="btn-group">
        <button onclick="setMode('weather')" id="mbtn-weather">🌦️ Počasie (SHMÚ)</button>
        <button onclick="setMode('planes')" id="mbtn-planes">✈️ Lietadlá (ADS-B)</button>
        <button onclick="toggleCarousel()" id="mbtn-car">🔄 Karusel</button>
      </div>
    </div>

    <!-- KARTA 4: TABUĽKA LIETADIEL -->
    <div class="card">
      <h2>✈️ Zoznam lietadiel v dosahu radaru</h2>
      <div style="overflow-x: auto;">
        <table>
          <thead>
            <tr><th>Let / Trasa</th><th>Typ</th><th>Rýchlosť</th><th>Výška</th><th>Pozícia</th></tr>
          </thead>
          <tbody id="planes-tbody">
            <tr><td colspan="5" style="text-align:center; color:#8b949e;">Načítavam zoznam lietadiel...</td></tr>
          </tbody>
        </table>
      </div>
    </div>

    <!-- KARTA 5: NASTAVENIA POLOHY & RADARU -->
    <div class="card">
      <h2>📍 Nastavenia polohy a radaru</h2>
      
      <div style="margin-bottom: 10px;">
        <label>Rýchly výber slovenského mesta:</label>
        <select onchange="onCityPreset(this)">
          <option value="">-- Zvoľte mesto pre automatické vyplnenie --</option>
          <option value="48.1486,17.1077">Bratislava (48.1486, 17.1077)</option>
          <option value="48.7164,21.2611">Košice (48.7164, 21.2611)</option>
          <option value="48.9984,21.2393">Prešov (48.9984, 21.2393)</option>
          <option value="49.2231,18.7397">Žilina (49.2231, 18.7397)</option>
          <option value="48.7363,19.1462">Banská Bystrica (48.7363, 19.1462)</option>
          <option value="48.3061,18.0864">Nitra (48.3061, 18.0864)</option>
          <option value="48.3775,17.5883">Trnava (48.3775, 17.5883)</option>
          <option value="48.8945,18.0444">Trenčín (48.8945, 18.0444)</option>
          <option value="49.0595,20.2978">Poprad (49.0595, 20.2978)</option>
          <option value="49.2918,21.2727">Bardejov (49.2918, 21.2727)</option>
          <option value="48.7547,21.9195">Michalovce (48.7547, 21.9195)</option>
          <option value="48.6690,19.1230">Zvolen (48.6690, 19.1230)</option>
          <option value="48.3294,19.6648">Lučenec (48.3294, 19.6648)</option>
          <option value="49.0806,19.3004">Ružomberok (49.0806, 19.3004)</option>
          <option value="49.0645,18.9228">Martin (49.0645, 18.9228)</option>
          <option value="48.7712,18.6253">Prievidza (48.7712, 18.6253)</option>
        </select>
      </div>

      <button type="button" onclick="useMyLocation()" id="btn-gps" style="background:#1f6feb; border-color:#58a6ff; width:100%; margin-bottom:12px; font-weight:600;">
        📍 Zistiť moju polohu (GPS / Sieťová IP)
      </button>

      <form onsubmit="saveSettings(event)">
        <div class="form-group">
          <div><label>Zemepisná šírka (Lat):</label><input type="text" name="lat" id="inp-lat" required oninput="userIsEditing=true"></div>
          <div><label>Zemepisná dĺžka (Lon):</label><input type="text" name="lon" id="inp-lon" required oninput="userIsEditing=true"></div>
        </div>
        <div class="form-group">
          <div><label>Interval karuselu (s):</label><input type="number" name="car_int" id="inp-car" min="5" max="300" required oninput="userIsEditing=true"></div>
          <div><label>Časový offset (h):</label><input type="number" name="offset" id="inp-off" min="-12" max="12" required oninput="userIsEditing=true"></div>
        </div>
        <div style="display: flex; gap: 8px; margin-top: 8px;">
          <input type="submit" id="btn-save-cfg" value="💾 Uložiť nastavenia" style="background:#238636; border-color:#2ea043; flex:1;">
          <button type="button" onclick="rebootEsp()" style="background:#da3633; border-color:#f85149;">🔄 Reštart</button>
        </div>
      </form>
    </div>

    <!-- KARTA 6: ZMENA WI-FI -->
    <div class="card">
      <h2>📶 Zmena Wi-Fi siete <button type="button" onclick="scanWifi()" id="btn-scan" style="font-size:0.8rem; padding:4px 10px;">🔍 Vyhľadať siete</button></h2>
      <form onsubmit="changeWifi(event)">
        <div style="margin-bottom: 10px;">
          <label>Dostupné Wi-Fi siete v okolí:</label>
          <select id="wifi-select" onchange="onWifiSelect(this)" style="margin-bottom: 6px;">
            <option value="">-- Kliknite na Vyhľadať siete alebo zadajte ručne nižšie --</option>
          </select>
          <label>Názov siete (SSID):</label>
          <input type="text" id="wifi-ssid" placeholder="Napr. MojaDomacaWiFi" required>
        </div>
        <div style="margin-bottom: 12px;">
          <label>Heslo k Wi-Fi sieti:</label>
          <input type="password" id="wifi-pass" placeholder="Heslo (ponechajte prázdne pre otvorenú sieť)">
        </div>
        <input type="submit" id="btn-save-wifi" value="💾 Pripojiť k novej Wi-Fi" style="background:#238636; border-color:#2ea043; width:100%;">
      </form>
    </div>

    <!-- KARTA 7: AKTUALIZÁCIA FIRMVÉRU (OTA) -->
    <div class="card">
      <h2>🚀 Aktualizácia firmvéru (OTA)</h2>
      <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:12px;">
        <div><b>Aktuálna verzia:</b> <span id="ota-cur-ver" style="color:#58a6ff; font-weight:bold;">v1.5.0</span></div>
        <button type="button" onclick="checkOta()" id="btn-check-ota" style="font-size:0.8rem; padding:6px 12px; background:#1f6feb; border-color:#58a6ff;">🔍 Skontrolovať GitHub</button>
      </div>

      <div id="ota-info-box" style="display:none; background:#0d1117; border:1px solid #30363d; border-radius:8px; padding:12px; margin-bottom:14px;">
        <div id="ota-status-text" style="font-weight:600; margin-bottom:6px;"></div>
        <div id="ota-release-notes" style="font-size:0.85rem; color:#8b949e; margin-bottom:10px; max-height:100px; overflow-y:auto; white-space:pre-wrap;"></div>
        <button type="button" onclick="startGithubOta()" id="btn-start-ota" style="background:#238636; border-color:#2ea043; width:100%; font-weight:bold; display:none;">
          ⬇️ Stiahnuť a aktualizovať z GitHubu
        </button>
      </div>

      <hr style="border:0; border-top:1px solid #30363d; margin:14px 0;">

      <label>Alebo nahrať lokálny súbor (.bin):</label>
      <form id="upload-form" onsubmit="uploadLocalOta(event)">
        <input type="file" id="ota-file" accept=".bin" required style="margin-bottom:8px;">
        <input type="submit" id="btn-upload-ota" value="📁 Nahrať firmvér z PC/mobilu" style="background:#30363d; border-color:#8b949e; width:100%;">
      </form>
    </div>
  </div>

  <script>
    let userIsEditing = false;

    function formatUptime(sec) {
      const d = Math.floor(sec / 86400);
      const h = Math.floor((sec % 86400) / 3600);
      const m = Math.floor((sec % 3600) / 60);
      const s = sec % 60;
      if (d > 0) return d + 'd ' + h + 'h ' + m + 'm';
      return (h < 10 ? '0' : '') + h + ':' + (m < 10 ? '0' : '') + m + ':' + (s < 10 ? '0' : '') + s;
    }

    function rssiToPct(rssi) {
      if (rssi <= -100) return 0;
      if (rssi >= -50) return 100;
      return 2 * (rssi + 100);
    }

    async function loadData() {
      try {
        const res = await fetch('/api/status');
        const d = await res.json();
        
        // Stav radaru
        document.getElementById('mode-val').innerText = d.mode === 0 ? '🌦️ Počasie' : '✈️ Lietadlá';
        document.getElementById('car-sub').innerText = d.car_en ? 'Karusel: ZAP (' + d.car_int + 's)' : 'Karusel: VYP';
        document.getElementById('zoom-val').innerText = d.radius + ' km';
        document.getElementById('planes-count').innerText = d.planes ? d.planes.length : 0;
        document.getElementById('wifi-rssi').innerText = d.rssi + ' dBm';
        document.getElementById('wifi-pct').innerText = 'Kvalita: ' + rssiToPct(d.rssi) + ' % (' + (d.ssid || '') + ')';

        // Stav systému & Hardvér
        document.getElementById('sys-cpu').innerText = (d.cpu_mhz || 160) + ' MHz';
        document.getElementById('sys-temp').innerText = 'Teplota: ' + (d.temp ? d.temp.toFixed(1) : '--') + ' °C';
        document.getElementById('sys-ram').innerText = d.heap_free + ' KB';
        const ramPct = Math.round((d.heap_free / (d.heap_total || 320)) * 100);
        document.getElementById('sys-ram-sub').innerText = 'Voľných ' + ramPct + ' % (z ' + (d.heap_total || 320) + ' KB)';
        document.getElementById('sys-flash').innerText = (d.flash_size || 4) + ' MB Flash';
        document.getElementById('sys-heap-min').innerText = 'Min RAM: ' + (d.heap_min || d.heap_free) + ' KB';
        document.getElementById('sys-uptime').innerText = formatUptime(d.uptime || 0);
        document.getElementById('sys-ip').innerText = 'IP: ' + (d.ip || '');

        // Zoom button active state
        for (let i = 0; i < 5; i++) {
          const btn = document.getElementById('zbtn-' + i);
          if (btn) btn.className = (d.zoom_idx === i) ? 'active' : '';
        }
        document.getElementById('mbtn-weather').className = (d.mode === 0) ? 'active' : '';
        document.getElementById('mbtn-planes').className = (d.mode === 1) ? 'active' : '';
        document.getElementById('mbtn-car').innerText = d.car_en ? '🔄 Karusel: ZAP' : '⏸️ Karusel: VYP';

        // Form fields (iba ak používateľ práve neupravuje formulár)
        if (!userIsEditing && document.activeElement.tagName !== 'INPUT' && document.activeElement.tagName !== 'SELECT') {
          document.getElementById('inp-lat').value = (typeof d.lat === 'number') ? d.lat.toFixed(4) : d.lat;
          document.getElementById('inp-lon').value = (typeof d.lon === 'number') ? d.lon.toFixed(4) : d.lon;
          document.getElementById('inp-car').value = d.car_int;
          document.getElementById('inp-off').value = d.offset;
        }

        // Planes table
        const tbody = document.getElementById('planes-tbody');
        if (!d.planes || d.planes.length === 0) {
          tbody.innerHTML = '<tr><td colspan="5" style="text-align:center; color:#8b949e;">V okruhu ' + d.radius + ' km nie sú žiadne lietadlá</td></tr>';
        } else {
          let html = '';
          for (const p of d.planes) {
            const displayId = p.route ? '<span class="route">' + p.route + '</span> (' + p.cs + ')' : '<b>' + p.cs + '</b>';
            const kmh = Math.round(p.gs * 1.852);
            html += '<tr><td>' + displayId + '</td><td>' + (p.t || '-') + '</td><td class="speed">' + kmh + ' km/h</td><td class="alt">' + p.alt + '</td><td>' + p.lat.toFixed(3) + ', ' + p.lon.toFixed(3) + '</td></tr>';
          }
          tbody.innerHTML = html;
        }
      } catch (e) {
        console.error(e);
      } finally {
        setTimeout(loadData, 3000);
      }
    }

    async function setZoom(idx) {
      await fetch('/api/set?zoom=' + idx);
      loadData();
    }
    async function setMode(m) {
      await fetch('/api/set?mode=' + m);
      loadData();
    }
    async function toggleCarousel() {
      await fetch('/api/set?toggle_car=1');
      loadData();
    }
    async function rebootEsp() {
      if (confirm('Naozaj reštartovať ESP MeteoRadar?')) {
        await fetch('/api/reboot');
        alert('ESP sa reštartuje...');
      }
    }

    function onCityPreset(sel) {
      if (!sel.value) return;
      userIsEditing = true;
      const parts = sel.value.split(',');
      document.getElementById('inp-lat').value = parseFloat(parts[0]).toFixed(4);
      document.getElementById('inp-lon').value = parseFloat(parts[1]).toFixed(4);
    }

    async function saveSettings(e) {
      e.preventDefault();
      const btn = document.getElementById('btn-save-cfg');
      const old = btn.value;
      btn.value = '⏳ Ukladám...';
      btn.disabled = true;

      const lat = document.getElementById('inp-lat').value;
      const lon = document.getElementById('inp-lon').value;
      const car_int = document.getElementById('inp-car').value;
      const offset = document.getElementById('inp-off').value;

      try {
        await fetch('/api/save', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: 'lat=' + encodeURIComponent(lat) + '&lon=' + encodeURIComponent(lon) + '&car_int=' + encodeURIComponent(car_int) + '&offset=' + encodeURIComponent(offset)
        });
        userIsEditing = false;
        btn.value = '✅ Uložené!';
        setTimeout(() => { btn.value = old; btn.disabled = false; }, 2500);
        loadData();
      } catch (err) {
        alert('Chyba pri ukladaní: ' + err);
        btn.value = old;
        btn.disabled = false;
      }
    }

    async function useMyLocation() {
      const btn = document.getElementById('btn-gps');
      const old = btn.innerText;
      btn.innerText = '⏳ Zisťujem polohu...';
      userIsEditing = true;

      if (navigator.geolocation && window.isSecureContext) {
        navigator.geolocation.getCurrentPosition(
          (pos) => {
            applyCoords(pos.coords.latitude, pos.coords.longitude, 'GPS');
          },
          async (err) => {
            console.warn('GPS zlyhalo, prepínam na IP geolokáciu...', err);
            await fetchIpLocation();
          },
          { enableHighAccuracy: true, timeout: 8000 }
        );
      } else {
        await fetchIpLocation();
      }

      async function fetchIpLocation() {
        btn.innerText = '⏳ Zisťujem polohu cez sieť...';
        try {
          const res = await fetch('https://ipwho.is/');
          const d = await res.json();
          if (d && d.success && d.latitude && d.longitude) {
            applyCoords(d.latitude, d.longitude, (d.city || 'IP'));
            return;
          }
        } catch (e) {}

        try {
          const res2 = await fetch('https://freeipapi.com/api/json');
          const d2 = await res2.json();
          if (d2 && d2.latitude && d2.longitude) {
            applyCoords(d2.latitude, d2.longitude, (d2.cityName || 'IP'));
            return;
          }
        } catch (e2) {}

        alert('Automatické zistenie polohy cez sieť zlyhalo. Vyberte prosím mesto zo zoznamu vyššie.');
        btn.innerText = old;
      }

      function applyCoords(lat, lon, src) {
        const fLat = parseFloat(lat).toFixed(4);
        const fLon = parseFloat(lon).toFixed(4);
        document.getElementById('inp-lat').value = fLat;
        document.getElementById('inp-lon').value = fLon;
        btn.innerText = '✅ Nastavené: ' + src + ' (' + fLat + ', ' + fLon + ')';
        setTimeout(() => { btn.innerText = old; }, 4000);
      }
    }

    async function scanWifi() {
      const btn = document.getElementById('btn-scan');
      const old = btn.innerText;
      btn.innerText = '⏳ Skenujem...';
      try {
        const res = await fetch('/api/scan');
        const list = await res.json();
        const sel = document.getElementById('wifi-select');
        sel.innerHTML = '<option value="">-- Vyberte nájdenú sieť (' + list.length + ') --</option>';
        list.forEach(w => {
          const opt = document.createElement('option');
          opt.value = w.ssid;
          opt.innerText = w.ssid + ' (' + w.rssi + ' dBm' + (w.enc ? ' 🔒' : '') + ')';
          sel.appendChild(opt);
        });
        btn.innerText = '✅ Nájdených: ' + list.length;
      } catch (e) {
        alert('Chyba pri skenovaní sietí: ' + e);
        btn.innerText = old;
      }
      setTimeout(() => { btn.innerText = old; }, 3000);
    }

    function onWifiSelect(sel) {
      if (sel.value) {
        document.getElementById('wifi-ssid').value = sel.value;
        document.getElementById('wifi-pass').focus();
      }
    }

    async function changeWifi(e) {
      e.preventDefault();
      const ssid = document.getElementById('wifi-ssid').value;
      const pass = document.getElementById('wifi-pass').value;
      if (!confirm('Naozaj chcete prepnúť zariadenie na Wi-Fi sieť "' + ssid + '"?')) return;
      const btn = document.getElementById('btn-save-wifi');
      btn.disabled = true;
      btn.value = '⏳ Pripájam k novej sieti...';
      try {
        await fetch('/api/wifi', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: 'ssid=' + encodeURIComponent(ssid) + '&pass=' + encodeURIComponent(pass)
        });
        alert('Prihlasovacie údaje boli odoslané. ESP sa pripája k sieti "' + ssid + '".\nSkontrolujte novú IP adresu na displeji zariadenia.');
      } catch (err) {
        alert('Odoslanie zlyhalo: ' + err);
        btn.disabled = false;
        btn.value = '💾 Pripojiť k novej Wi-Fi';
      }
    }

    async function checkOta() {
      const btn = document.getElementById('btn-check-ota');
      const box = document.getElementById('ota-info-box');
      const statusTxt = document.getElementById('ota-status-text');
      const notes = document.getElementById('ota-release-notes');
      const startBtn = document.getElementById('btn-start-ota');
      
      btn.disabled = true;
      btn.innerText = '⏳ Kontrolujem...';
      box.style.display = 'block';
      statusTxt.innerText = 'Pripájam k serveru GitHub...';
      notes.innerText = '';
      startBtn.style.display = 'none';

      try {
        const res = await fetch('/api/ota/check');
        const d = await res.json();
        
        if (d.has_update) {
          statusTxt.innerHTML = '🎉 <span style="color:#2ea043;">Dostupná nová verzia: ' + d.latest_version + '</span> (vaša: ' + d.current_version + ')';
          notes.innerText = d.notes || d.name || '';
          startBtn.style.display = 'block';
          startBtn.innerText = '🚀 Aktualizovať na ' + d.latest_version;
        } else {
          statusTxt.innerHTML = '✅ <span style="color:#58a6ff;">Používate najnovšiu verziu ' + d.current_version + '</span>';
          notes.innerText = d.name ? ('Posledný release: ' + d.name) : '';
          startBtn.style.display = 'block';
          startBtn.innerText = '🔄 Preinštalovať ' + d.current_version + ' z GitHubu';
        }
      } catch (err) {
        statusTxt.innerHTML = '❌ <span style="color:#f85149;">Chyba pri kontrole: ' + err + '</span>';
      } finally {
        btn.disabled = false;
        btn.innerText = '🔍 Skontrolovať GitHub';
      }
    }

    async function startGithubOta() {
      if (!confirm('Naozaj spustiť OTA aktualizáciu z GitHubu?\nPočas aktualizácie nevypínajte napájanie zariadenia!')) return;
      const startBtn = document.getElementById('btn-start-ota');
      startBtn.disabled = true;
      startBtn.innerText = '⏳ Sťahujem a inštalujem... Sledujte displej ESP32';
      try {
        await fetch('/api/ota/github', { method: 'POST' });
        alert('OTA aktualizácia bola spustená!\nESP32 sťahuje firmvér a po dokončení sa automaticky reštartuje.');
      } catch (e) {
        alert('Chyba pri spustení: ' + e);
        startBtn.disabled = false;
      }
    }

    async function uploadLocalOta(e) {
      e.preventDefault();
      const fileInp = document.getElementById('ota-file');
      if (!fileInp.files || fileInp.files.length === 0) return;
      if (!confirm('Naozaj nahrať vybraný firmvér "' + fileInp.files[0].name + '"?')) return;

      const btn = document.getElementById('btn-upload-ota');
      btn.disabled = true;
      btn.value = '⏳ Nahrávam firmvér do ESP...';

      const formData = new FormData();
      formData.append('firmware', fileInp.files[0]);

      try {
        const res = await fetch('/api/ota/upload', {
          method: 'POST',
          body: formData
        });
        if (res.ok) {
          alert('Firmvér úspešne nahraný!\nESP32 sa reštartuje...');
        } else {
          alert('Chyba pri nahrávaní súboru.');
          btn.disabled = false;
          btn.value = '📁 Nahrať firmvér z PC/mobilu';
        }
      } catch (err) {
        alert('Zlyhalo: ' + err);
        btn.disabled = false;
        btn.value = '📁 Nahrať firmvér z PC/mobilu';
      }
    }

    loadData();
  </script>
</body>
</html>
)rawliteral";

void handleWebRoot() {
  server.send_P(200, "text/html", HTML_PAGE);
}

void handleApiStatus() {
  JsonDocument doc;
  doc["mode"] = (int)currentMode;
  doc["radius"] = (int)currentRadiusKm;
  doc["zoom_idx"] = zoomIndex;
  doc["lat"] = centerLat;
  doc["lon"] = centerLon;
  doc["car_int"] = carouselIntervalSec;
  doc["car_en"] = carouselEnabled;
  doc["offset"] = timeOffsetHours;
  doc["rssi"] = WiFi.RSSI();
  doc["ssid"] = WiFi.SSID();
  doc["ip"] = WiFi.localIP().toString();
  doc["mac"] = WiFi.macAddress();
  doc["heap_free"] = (int)(ESP.getFreeHeap() / 1024);
  doc["heap_total"] = (int)(ESP.getHeapSize() / 1024);
  doc["heap_min"] = (int)(ESP.getMinFreeHeap() / 1024);
  doc["flash_size"] = (int)(ESP.getFlashChipSize() / (1024 * 1024));
  doc["cpu_mhz"] = getCpuFrequencyMhz();
  doc["temp"] = roundf(temperatureRead() * 10.0f) / 10.0f;
  doc["uptime"] = millis() / 1000;

  JsonArray pArr = doc["planes"].to<JsonArray>();
  for (size_t i = 0; i < aircraftCount; i++) {
    JsonObject obj = pArr.add<JsonObject>();
    obj["cs"] = aircraftList[i].callsign;
    if (aircraftList[i].route[0] != '\0') obj["route"] = aircraftList[i].route;
    obj["t"] = aircraftList[i].type;
    obj["gs"] = aircraftList[i].gs_knots;
    obj["alt"] = aircraftList[i].alt;
    obj["lat"] = aircraftList[i].lat;
    obj["lon"] = aircraftList[i].lon;
  }

  String jsonStr;
  serializeJson(doc, jsonStr);
  server.send(200, "application/json", jsonStr);
}

void handleApiSet() {
  if (server.hasArg("zoom")) {
    setZoomIndex(server.arg("zoom").toInt());
  }
  if (server.hasArg("mode")) {
    String m = server.arg("mode");
    if (m == "weather") setAppMode(MODE_WEATHER);
    else if (m == "planes") setAppMode(MODE_PLANES);
  }
  if (server.hasArg("toggle_car")) {
    carouselEnabled = !carouselEnabled;
  }
  server.send(200, "text/plain", "OK");
}

void handleApiSave() {
  if (server.hasArg("lat") && server.hasArg("lon")) {
    centerLat = server.arg("lat").toFloat();
    centerLon = server.arg("lon").toFloat();
    if (server.hasArg("car_int")) carouselIntervalSec = constrain(server.arg("car_int").toInt(), 5, 300);
    if (server.hasArg("offset")) timeOffsetHours = server.arg("offset").toInt();

    carouselIntervalMs = (uint32_t)carouselIntervalSec * 1000;
    crop = makeCrop(centerLat, centerLon, currentRadiusKm);

    prefs.begin("radar", false);
    prefs.putFloat("lat", centerLat);
    prefs.putFloat("lon", centerLon);
    prefs.putInt("car_int", carouselIntervalSec);
    prefs.putInt("offset", timeOffsetHours);
    prefs.end();

    configTime(timeOffsetHours * 3600, 0, "pool.ntp.org", "time.nist.gov");
    
    if (currentMode == MODE_WEATHER) {
      if (downloadLatestRadar()) renderRadar();
      else renderRadar();
    } else {
      lastPlaneFetchMs = millis();
      fetchPlanesData();
      drawPlanes();
    }
  }
  server.send(200, "text/plain", "OK");
}

void handleApiScan() {
  int n = WiFi.scanNetworks();
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i).length() == 0) continue;
    JsonObject obj = arr.add<JsonObject>();
    obj["ssid"] = WiFi.SSID(i);
    obj["rssi"] = WiFi.RSSI(i);
    obj["enc"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
  }
  String jsonStr;
  serializeJson(doc, jsonStr);
  server.send(200, "application/json", jsonStr);
  WiFi.scanDelete();
}

void handleApiWifi() {
  if (server.hasArg("ssid")) {
    String newSsid = server.arg("ssid");
    String newPass = server.hasArg("pass") ? server.arg("pass") : "";
    server.send(200, "text/plain", "OK");
    delay(500);

    showStatus("Pripajam k novej\nWiFi:\n" + newSsid);
    WiFi.disconnect(true);
    delay(300);
    WiFi.begin(newSsid.c_str(), newPass.c_str());

    uint32_t startMs = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startMs < 15000) {
      delay(300);
    }
  }
}

void handleApiReboot() {
  server.send(200, "text/plain", "OK");
  delay(500);
  ESP.restart();
}

void handleApiOtaCheck() {
  if (WiFi.status() != WL_CONNECTED) {
    server.send(503, "application/json", "{\"error\":\"WiFi nie je pripojené\"}");
    return;
  }

  releaseCanvas(); // Uvoľníme RAM pre bezpečný TLS handshake

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(8000);
  HTTPClient http;
  http.setTimeout(10000);
  http.setUserAgent("ESP32-MeteoRadar");

  JsonDocument doc;
  doc["current_version"] = CURRENT_VERSION;
  doc["has_update"] = false;
  doc["latest_version"] = CURRENT_VERSION;
  doc["name"] = "";
  doc["notes"] = "";

  if (http.begin(client, "https://api.github.com/repos/hackra76/ESP-MeteoRadar/releases/latest")) {
    int code = http.GET();
    if (code == HTTP_CODE_OK) {
      JsonDocument filter;
      filter["tag_name"] = true;
      filter["name"] = true;
      filter["body"] = true;

      JsonDocument ghDoc;
      DeserializationError err = deserializeJson(ghDoc, http.getStream(), DeserializationOption::Filter(filter));
      if (!err) {
        String tag = ghDoc["tag_name"] | "";
        String name = ghDoc["name"] | "";
        String body = ghDoc["body"] | "";

        doc["latest_version"] = tag;
        doc["name"] = name;
        doc["notes"] = body;
        doc["has_update"] = (tag.length() > 0 && tag != String(CURRENT_VERSION));
      }
    }
    http.end();
  }
  client.stop();

  String jsonStr;
  serializeJson(doc, jsonStr);
  server.send(200, "application/json", jsonStr);
}

void handleApiOtaGithub() {
  if (WiFi.status() != WL_CONNECTED) {
    server.send(503, "application/json", "{\"error\":\"WiFi nie je pripojené\"}");
    return;
  }

  server.send(200, "application/json", "{\"status\":\"starting\"}");
  delay(300);

  releaseCanvas();
  showStatus("OTA Aktualizacia...\nPripajam GitHub...");

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(12000);

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(35000);
  http.setUserAgent("ESP32-MeteoRadar");

  String url = "https://github.com/hackra76/ESP-MeteoRadar/releases/latest/download/firmware.bin";

  if (http.begin(client, url)) {
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      int totalLen = http.getSize();
      WiFiClient* stream = http.getStreamPtr();

      if (Update.begin(totalLen > 0 ? totalLen : UPDATE_SIZE_UNKNOWN, U_FLASH)) {
        size_t written = 0;
        uint8_t buff[1024];
        int lastPct = -1;

        while (http.connected() && (written < (size_t)totalLen || totalLen <= 0)) {
          size_t avail = stream->available();
          if (avail) {
            size_t toRead = (avail < sizeof(buff)) ? avail : sizeof(buff);
            int n = stream->readBytes(buff, toRead);
            if (n > 0) {
              Update.write(buff, n);
              written += n;

              if (totalLen > 0) {
                int pct = (int)((written * 100) / totalLen);
                if (pct != lastPct && pct % 5 == 0) {
                  lastPct = pct;
                  showStatus("OTA Aktualizacia...\nStahujem: " + String(pct) + " %");
                }
              }
            }
          } else {
            delay(1);
          }
          if (totalLen > 0 && written >= (size_t)totalLen) break;
          if (stream->available() == 0 && !http.connected()) break;
        }

        if (Update.end(true)) {
          showStatus("OTA Dokoncena!\nRestartujem...");
          delay(1500);
          ESP.restart();
          return;
        } else {
          showStatus("Chyba zapisu OTA:\n" + String(Update.errorString()));
          delay(3000);
        }
      } else {
        showStatus("Chyba inicializacie\nOTA Update");
        delay(3000);
      }
    } else {
      showStatus("Chyba stahovania\nHTTP: " + String(httpCode));
      delay(3000);
    }
    http.end();
  }
  client.stop();
}

void handleApiOtaUploadLoop() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    releaseCanvas();
    showStatus("Manualna OTA...\nPripravujem...");
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
      showStatus("Chyba OTA start!");
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      showStatus("Chyba zapisu OTA!");
    } else {
      static int lastUploadPct = -1;
      int pct = (upload.totalSize > 0) ? (int)((upload.currentSize * 100) / upload.totalSize) : 0;
      if (pct != lastUploadPct && pct % 10 == 0) {
        lastUploadPct = pct;
        showStatus("Manualna OTA...\n" + String(pct) + " %");
      }
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      showStatus("OTA Dokoncena!\nRestartujem...");
    } else {
      showStatus("Chyba OTA END!");
    }
  }
}

void handleApiOtaUploadDone() {
  server.sendHeader("Connection", "close");
  if (Update.hasError()) {
    server.send(500, "text/plain", "Chyba OTA aktualizacie");
  } else {
    server.send(200, "text/plain", "OK - Restartujem ESP...");
    delay(1000);
    ESP.restart();
  }
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleWebRoot);
  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/api/set", HTTP_GET, handleApiSet);
  server.on("/api/save", HTTP_POST, handleApiSave);
  server.on("/api/scan", HTTP_GET, handleApiScan);
  server.on("/api/wifi", HTTP_POST, handleApiWifi);
  server.on("/api/reboot", HTTP_GET, handleApiReboot);
  server.on("/api/ota/check", HTTP_GET, handleApiOtaCheck);
  server.on("/api/ota/github", HTTP_POST, handleApiOtaGithub);
  server.on("/api/ota/upload", HTTP_POST, handleApiOtaUploadDone, handleApiOtaUploadLoop);
  server.begin();
  Serial.println("Web server spustený na porte 80!");
}


// =======================================================================================
// 5. SHMÚ METEORADAR (SŤAHOVANIE & SPRACOVANIE)
// =======================================================================================

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
    if (ts > newestTs) { 
      newestTs = ts; 
      latest = name; 
    }
    pos = end + 4;
  }
  return latest;
}

bool downloadLatestRadar() {
  if (WiFi.status() != WL_CONNECTED) return false;
  
  releaseCanvas(); // Uvoľníme 58 KB RAM pre maximálny priestor pre TLS

  for (int attempt = 1; attempt <= 2; attempt++) {
    WiFiClientSecure client; 
    client.setInsecure();
    client.setHandshakeTimeout(10000);
    HTTPClient http; 
    http.setTimeout(10000);

    if (http.begin(client, SHMU_API_URL)) {
      if (http.GET() == HTTP_CODE_OK) {
        String resp = http.getString();
        http.end(); 
        client.stop();

        String newestTs;
        String latest = findLatestPngNameInText(resp, newestTs);

        if (!latest.isEmpty()) {
          if (latest == lastPngName && SPIFFS.exists(RADAR_FILE)) return true;
          String url = String(SHMU_BASE_URL) + latest;
          WiFiClientSecure clientImg;
          clientImg.setInsecure();
          clientImg.setHandshakeTimeout(15000);
          HTTPClient httpImg; 
          httpImg.setTimeout(25000);
          if (httpImg.begin(clientImg, url) && httpImg.GET() == HTTP_CODE_OK) {
            File f = SPIFFS.open(RADAR_FILE, "w");
            if (f) {
              httpImg.writeToStream(&f);
              f.close();
              lastPngName = latest;
              httpImg.end();
              clientImg.stop();
              return true;
            }
          }
          httpImg.end();
          clientImg.stop();
        }
      } else {
        http.end();
        client.stop();
      }
    } else {
      client.stop();
    }
    if (attempt < 2) delay(2000);
  }
  return false;
}


// =======================================================================================
// 6. ADS-B LETECKÝ RADAR & VRS LETOVÉ TRASY
// =======================================================================================

static constexpr int kAircraftNoseLenPx = 8;
static constexpr int kAircraftTailLenPx = 3;
static constexpr int kAircraftTailHalfPx = 4;
static constexpr float kVrateThresholdFpm = 128.0f;
static constexpr int kVrateArrowW = 8;
static constexpr int kVrateArrowH = 8;
static constexpr int kVrateArrowGapPx = 3;
static constexpr int kTypeSpeedGapPx = 4;
static constexpr int kAircraftLabelGapPx = 3;

inline void applyTagStyle()      { tft.setTextSize(0.80f); }
inline void applyCardinalStyle() { tft.setTextSize(0.75f); }
inline void applyScaleStyle()    { tft.setTextSize(0.75f); }
inline void applyCityStyle()     { tft.setTextSize(0.80f); }

// ---- Statické vyhľadávanie letových trás (VRS standing-data) ----
static constexpr char kRouteBaseUrl[] = "https://vrs-standing-data.adsb.lol/routes/";
static constexpr size_t kRouteCacheSize = 48;

struct RouteCacheEntry {
  char callsign[9];
  char route[10];
  bool used;
};
static RouteCacheEntry s_route_cache[kRouteCacheSize] = {};
static size_t s_route_cache_next = 0;

static const char* routeCacheFind(const char* callsign) {
  for (size_t i = 0; i < kRouteCacheSize; ++i) {
    if (s_route_cache[i].used && strcmp(s_route_cache[i].callsign, callsign) == 0) {
      return s_route_cache[i].route;
    }
  }
  return nullptr;
}

static void routeCachePut(const char* callsign, const char* route) {
  RouteCacheEntry& e = s_route_cache[s_route_cache_next];
  s_route_cache_next = (s_route_cache_next + 1) % kRouteCacheSize;
  e.used = true;
  strlcpy(e.callsign, callsign, sizeof(e.callsign));
  strlcpy(e.route, route, sizeof(e.route));
}

static void buildRouteDisplay(const char* codes, char* out, size_t out_len) {
  out[0] = '\0';
  if (codes == nullptr || strcmp(codes, "unknown") == 0) return;
  const char* dash = strchr(codes, '-');
  if (dash == nullptr) return;
  const char* last = strrchr(codes, '-') + 1;
  size_t first_len = (size_t)(dash - codes);
  if (first_len > 4) first_len = 4;
  size_t last_len = strnlen(last, 4);
  if (first_len + 1 + last_len + 1 > out_len) return;
  memcpy(out, codes, first_len);
  out[first_len] = '>';
  memcpy(out + first_len + 1, last, last_len);
  out[first_len + 1 + last_len] = '\0';
}

static void fetchRouteForCallsign(const char* cs, char* out_route, size_t out_len) {
  out_route[0] = '\0';
  if (cs[0] == '\0' || strlen(cs) < 3) return;

  const char* cached = routeCacheFind(cs);
  if (cached != nullptr) {
    strlcpy(out_route, cached, out_len);
    return;
  }

  if (ESP.getFreeHeap() < 35000) return;

  char url[96];
  snprintf(url, sizeof(url), "%s%c%c/%s.json", kRouteBaseUrl, cs[0], cs[1], cs);

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(3000);
  HTTPClient http;
  http.setTimeout(3000);

  if (http.begin(client, url)) {
    int code = http.GET();
    if (code == HTTP_CODE_OK) {
      String resp = http.getString();
      JsonDocument filter;
      filter["_airport_codes_iata"] = true;
      JsonDocument doc;
      if (!deserializeJson(doc, resp, DeserializationOption::Filter(filter))) {
        const char* codes = doc["_airport_codes_iata"] | "";
        if (strlen(codes) > 0) {
          buildRouteDisplay(codes, out_route, out_len);
          routeCachePut(cs, out_route);
        } else {
          routeCachePut(cs, "");
        }
      }
    } else if (code == HTTP_CODE_NOT_FOUND) {
      routeCachePut(cs, "");
    }
    http.end();
  }
  client.stop();
}

int speedLineLengthPx(float gs_knots) {
  if (gs_knots <= 0.0f) return 0;
  constexpr float kKmPerKnotPerHorizon = 1.852f * 60.0f / 3600.0f;
  const float px = gs_knots * kKmPerKnotPerHorizon * 107.0f / 13.3f * (1.5f / 5.0f);
  const int len = (int)(px + 0.5f);
  return (len < 2) ? 2 : len;
}

void drawAircraftSymbol(LovyanGFX& target, int x, int y, float heading_deg, float track_deg, float gs_knots, bool is_mil) {
  constexpr float kDegToRad = 0.01745329252f;
  const float rad_h = heading_deg * kDegToRad;
  const float sin_h = sinf(rad_h);
  const float cos_h = cosf(rad_h);

  const int tip_x = x + (int)roundf(sin_h * (float)kAircraftNoseLenPx);
  const int tip_y = y - (int)roundf(cos_h * (float)kAircraftNoseLenPx);

  const int base_x = x - (int)roundf(sin_h * (float)kAircraftTailLenPx);
  const int base_y = y + (int)roundf(cos_h * (float)kAircraftTailLenPx);

  const int wing_x = (int)roundf(cos_h * (float)kAircraftTailHalfPx);
  const int wing_y = (int)roundf(sin_h * (float)kAircraftTailHalfPx);

  const int len = speedLineLengthPx(gs_knots);
  if (len > 0) {
    const float rad_t = track_deg * kDegToRad;
    const int ex = tip_x + (int)roundf(sinf(rad_t) * (float)len);
    const int ey = tip_y - (int)roundf(cosf(rad_t) * (float)len);
    target.drawWideLine(tip_x, tip_y, ex, ey, 1, target.color565(255, 255, 255));
  }

  uint16_t symbolColor = is_mil ? target.color565(255, 0, 0) : target.color565(0, 120, 255);
  target.fillTriangle(tip_x, tip_y, base_x + wing_x, base_y + wing_y, base_x - wing_x, base_y - wing_y, symbolColor);
}

const char* tagTopLine(const AircraftData& ac) {
  if (ac.route[0] != '\0') return ac.route;
  return ac.callsign;
}

void formatTypePart(const AircraftData& ac, char* out, size_t out_len) {
  const int kmh = (int)lroundf(ac.gs_knots * 1.852f);
  if (ac.type[0] != '\0' && kmh > 0) {
    snprintf(out, out_len, "%s,", ac.type);
  } else if (ac.type[0] != '\0') {
    snprintf(out, out_len, "%s", ac.type);
  } else {
    out[0] = '\0';
  }
}

void formatSpeedPart(const AircraftData& ac, char* out, size_t out_len) {
  const int kmh = (int)lroundf(ac.gs_knots * 1.852f);
  if (kmh > 0) {
    snprintf(out, out_len, "%d", kmh);
  } else {
    out[0] = '\0';
  }
}

int vrateDirection(const AircraftData& ac) {
  if (isnan(ac.vrate_fpm)) return 0;
  if (ac.vrate_fpm >= kVrateThresholdFpm) return 1;
  if (ac.vrate_fpm <= -kVrateThresholdFpm) return -1;
  return 0;
}

void drawVRateArrow(LovyanGFX& target, int x, int ly, int line_h, int dir) {
  const int ty = ly + (line_h - kVrateArrowH) / 2;
  if (dir > 0) {
    target.fillTriangle(x + kVrateArrowW / 2, ty, x, ty + kVrateArrowH, x + kVrateArrowW, ty + kVrateArrowH, target.color565(30, 220, 30));
  } else if (dir < 0) {
    target.fillTriangle(x + kVrateArrowW / 2, ty + kVrateArrowH, x, ty, x + kVrateArrowW, ty, target.color565(235, 40, 40));
  }
}

int measureTagBlockWidth(LovyanGFX& target, const AircraftData& ac) {
  target.setTextSize(0.80f);
  int max_w = 0;
  const char* top = tagTopLine(ac);
  if (top[0] != '\0') {
    int w = target.textWidth(top);
    if (w > max_w) max_w = w;
  }
  char type_part[12];
  char speed_part[10];
  formatTypePart(ac, type_part, sizeof(type_part));
  formatSpeedPart(ac, speed_part, sizeof(speed_part));
  int w2 = 0;
  if (type_part[0] != '\0') w2 += target.textWidth(type_part);
  if (speed_part[0] != '\0') w2 += target.textWidth(speed_part);
  if (type_part[0] != '\0' && speed_part[0] != '\0') w2 += kTypeSpeedGapPx;
  if (w2 > max_w) max_w = w2;

  if (ac.alt[0] != '\0') {
    int w3 = target.textWidth(ac.alt);
    if (vrateDirection(ac) != 0) w3 += kVrateArrowGapPx + kVrateArrowW;
    if (w3 > max_w) max_w = w3;
  }
  return max_w;
}

void drawAircraftTag(LovyanGFX& target, int x, int y, const AircraftData& ac) {
  target.setTextSize(0.80f);
  const int line_h = target.fontHeight();
  const int block_w = measureTagBlockWidth(target, ac);
  const int block_h = line_h * 3;
  int ly = y - block_h / 2;

  const int symbol_half = kAircraftNoseLenPx + kAircraftTailHalfPx;
  const bool tag_on_right = x < (TFT_W / 2);
  int anchor_x = 0;

  if (tag_on_right) {
    anchor_x = x + symbol_half + kAircraftLabelGapPx;
    anchor_x = std::min(anchor_x, TFT_W - block_w - 2);
    target.setTextDatum(textdatum_t::top_left);
  } else {
    anchor_x = x - symbol_half - kAircraftLabelGapPx;
    anchor_x = std::max(anchor_x, block_w + 2);
    target.setTextDatum(textdatum_t::top_right);
  }
  ly = constrain(ly, 2, TFT_H - block_h - 2);

  // Riadok 1: Trasa letísk (fialová) alebo Callsign (biela/červená)
  const char* top = tagTopLine(ac);
  if (top[0] != '\0') {
    uint16_t col = ac.is_mil ? target.color565(255, 60, 60) : (ac.route[0] != '\0' ? target.color565(255, 130, 255) : TFT_WHITE);
    target.setTextColor(col, TFT_BLACK);
    target.drawString(top, anchor_x, ly);
  }
  ly += line_h;

  // Riadok 2: Typ lietadla (modrá) + Rýchlosť v km/h (svetlozelená)
  char type_part[12];
  char speed_part[10];
  formatTypePart(ac, type_part, sizeof(type_part));
  formatSpeedPart(ac, speed_part, sizeof(speed_part));
  const int w_type = (type_part[0] != '\0') ? target.textWidth(type_part) : 0;
  const int w_speed = (speed_part[0] != '\0') ? target.textWidth(speed_part) : 0;

  if (tag_on_right) {
    if (type_part[0] != '\0') {
      target.setTextColor(target.color565(90, 200, 255), TFT_BLACK);
      target.drawString(type_part, anchor_x, ly);
    }
    if (speed_part[0] != '\0') {
      target.setTextColor(target.color565(150, 235, 150), TFT_BLACK);
      target.drawString(speed_part, anchor_x + w_type + kTypeSpeedGapPx, ly);
    }
  } else {
    if (speed_part[0] != '\0') {
      target.setTextColor(target.color565(150, 235, 150), TFT_BLACK);
      target.drawString(speed_part, anchor_x, ly);
    }
    if (type_part[0] != '\0') {
      target.setTextColor(target.color565(90, 200, 255), TFT_BLACK);
      target.drawString(type_part, anchor_x - w_speed - kTypeSpeedGapPx, ly);
    }
  }
  ly += line_h;

  // Riadok 3: Výška v metroch (žltá) + Šípka stúpania/klesania
  if (ac.alt[0] != '\0') {
    target.setTextColor(target.color565(255, 255, 0), TFT_BLACK);
    target.drawString(ac.alt, anchor_x, ly);
    const int dir = vrateDirection(ac);
    if (dir != 0) {
      const int w_alt = target.textWidth(ac.alt);
      int ax = tag_on_right ? (anchor_x + w_alt + kVrateArrowGapPx) : (anchor_x - w_alt - kVrateArrowGapPx - kVrateArrowW);
      drawVRateArrow(target, ax, ly, line_h, dir);
    }
  }
}

void drawEdgeIndicator(LovyanGFX& target, int mapX, int mapY, bool is_mil) {
  int cx = TFT_W / 2;
  int cy = TFT_H / 2;

  float dx = (float)(mapX - crop.x1) * TFT_W / crop.w() - cx;
  float dy = (float)(mapY - crop.y1) * TFT_H / crop.h() - cy;

  float angle = atan2f(dy, dx);
  int edgeX = cx + (int)(roundf(cosf(angle) * 114.0f));
  int edgeY = cy + (int)(roundf(sinf(angle) * 114.0f));

  uint16_t dotColor = is_mil ? target.color565(255, 0, 0) : target.color565(255, 140, 0);

  target.fillCircle(edgeX, edgeY, 4, dotColor);
  target.drawCircle(edgeX, edgeY, 4, TFT_BLACK);
}

/** Stiahnutie zoznamu lietadiel z ADS-B API */
void fetchPlanesData() {
  if (WiFi.status() != WL_CONNECTED) return;

  releaseCanvas(); // Uvoľníme 58 KB RAM pre bezpečný a stabilný TLS handshake!

  float fetchRadiusKm = currentRadiusKm * 1.35f;
  if (fetchRadiusKm > 320.0f) fetchRadiusKm = 320.0f;
  float radiusNm = fetchRadiusKm / 1.852f;
  String url = "https://opendata.adsb.fi/api/v3/lat/" + String(centerLat, 4) + "/lon/" + String(centerLon, 4) + "/dist/" + String(radiusNm, 1);

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(8000);

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

      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
      
      http.end();
      client.stop(); // <--- OKAMŽITÉ UZAVRETIE SPOJENIA

      if (!err) {
        JsonArray acList = doc["ac"].as<JsonArray>();
        size_t count = 0;

        for (JsonObject plane : acList) {
          if (count >= MAX_AIRCRAFT) break;

          AircraftData ac;
          ac.lat = plane["lat"].as<float>();
          ac.lon = plane["lon"].as<float>();
          ac.track = plane["track"] | 0.0f;
          ac.nose_deg = plane["true_heading"] | ac.track;
          ac.gs_knots = plane["gs"] | 0.0f;
          ac.vrate_fpm = plane["baro_rate"] | 0.0f;

          int dbFlags = plane["dbFlags"] | 0;
          ac.is_mil = (dbFlags & 1) != 0;

          const char* fl = plane["flight"] | "";
          strlcpy(ac.callsign, fl, sizeof(ac.callsign));
          
          size_t csLen = strlen(ac.callsign);
          while (csLen > 0 && ac.callsign[csLen - 1] == ' ') {
            ac.callsign[--csLen] = '\0';
          }
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

          // Rýchle načítanie z pamäte cache
          ac.route[0] = '\0';
          if (strcmp(ac.callsign, "NOCALL") != 0 && !ac.is_mil) {
            const char* cached = routeCacheFind(ac.callsign);
            if (cached != nullptr) {
              strlcpy(ac.route, cached, sizeof(ac.route));
            }
          }

          aircraftList[count++] = ac;
        }

        aircraftCount = count;
        lastPlaneFetchFixMs = millis();

        // Samostatné sekvenčné dohľadanie trás maximálne pre 2 lietadlá (po úplnom uzavretí ADS-B spojenia!)
        int routesFetched = 0;
        for (size_t i = 0; i < aircraftCount && routesFetched < 2; i++) {
          if (aircraftList[i].route[0] == '\0' && strcmp(aircraftList[i].callsign, "NOCALL") != 0 && !aircraftList[i].is_mil) {
            fetchRouteForCallsign(aircraftList[i].callsign, aircraftList[i].route, sizeof(aircraftList[i].route));
            routesFetched++;
          }
        }
      }
    } else {
      http.end();
      client.stop();
    }
  } else {
    client.stop();
  }
}

/** Vykreslenie radarovej mriežky s mestami a kružnicami */
void drawPlaneRadarGrid(LovyanGFX& target) {
  int cx = TFT_W / 2;
  int cy = TFT_H / 2;

  // 1. Hranica SR
  for (size_t i = 0; i < SK_BORDER_COUNT - 1; i++) {
    int sx1 = (int)mapXToScreenX(lonToX(SK_BORDER[i][0]));
    int sy1 = (int)mapYToScreenY(latToY(SK_BORDER[i][1]));
    int sx2 = (int)mapXToScreenX(lonToX(SK_BORDER[i+1][0]));
    int sy2 = (int)mapYToScreenY(latToY(SK_BORDER[i+1][1]));
    target.drawLine(sx1, sy1, sx2, sy2, TFT_CYAN);
  }

  // 2. Filtrovanie a zobrazenie miest
  target.setTextSize(0.80f);
  target.setTextDatum(textdatum_t::bottom_center);
  target.setTextColor(TFT_ORANGE, TFT_BLACK);
  for (size_t i = 0; i < CITY_COUNT; i++) {
    if (currentRadiusKm >= 100.0f && !CITIES[i].isMajor) continue;

    int sx = (int)mapXToScreenX(lonToX(CITIES[i].lon));
    int sy = (int)mapYToScreenY(latToY(CITIES[i].lat));
    if (sx >= 10 && sx <= TFT_W - 10 && sy >= 10 && sy <= TFT_H - 10) {
      target.drawLine(sx - 2, sy, sx + 2, sy, TFT_RED);
      target.drawLine(sx, sy - 2, sx, sy + 2, TFT_RED);
      target.drawString(CITIES[i].name, sx, sy - 3);
    }
  }

  // 3. Koncentrické zelené kruhy a kríž
  uint16_t gridColor = target.color565(0, 200, 0);     
  uint16_t dimGridColor = target.color565(0, 80, 0);   

  target.drawLine(cx - 110, cy, cx + 110, cy, dimGridColor);
  target.drawLine(cx, cy - 110, cx, cy + 110, dimGridColor);

  target.drawCircle(cx, cy, 35, gridColor);
  target.drawCircle(cx, cy, 70, gridColor);
  target.drawCircle(cx, cy, 105, gridColor);

  // 4. Svetové strany (N, S, W, E)
  target.setTextSize(0.75f);
  target.setTextDatum(textdatum_t::middle_center);
  target.setTextColor(TFT_WHITE, TFT_BLACK);
  target.drawString("N", cx, 8);
  target.drawString("S", cx, TFT_H - 8);
  target.drawString("W", 8, cy);
  target.drawString("E", TFT_W - 8, cy);

  // 5. Mierka a čas
  target.setTextSize(0.75f);
  target.setTextDatum(textdatum_t::top_center);
  target.setTextColor(target.color565(0, 255, 0), TFT_BLACK);
  target.drawString(String((int)currentRadiusKm) + " km", cx, 4);

  target.setTextDatum(textdatum_t::bottom_center);
  target.setTextColor(TFT_WHITE, TFT_BLACK);
  target.drawString(getCurrentSystemTimeText(), cx, TFT_H - 4);
}

/** Vykreslenie radaru lietadiel s plynulou extrapoláciou pohybu (Double Buffering) */
void drawPlanes() {
  ensureCanvas();
  LovyanGFX& target = canvasReady ? static_cast<LovyanGFX&>(canvas) : static_cast<LovyanGFX&>(tft);

  target.fillScreen(TFT_BLACK);
  drawPlaneRadarGrid(target);

  int cx = TFT_W / 2;
  int cy = TFT_H / 2;

  // Výpočet uplynutého času od posledného sieťového fixu
  float dt_s = (lastPlaneFetchFixMs > 0) ? (float)(millis() - lastPlaneFetchFixMs) / 1000.0f : 0.0f;
  if (dt_s > 30.0f) dt_s = 30.0f;

  for (size_t i = 0; i < aircraftCount; i++) {
    AircraftData ac = aircraftList[i];

    // Plynulá extrapolácia pozície pozdĺž kurzu podľa rýchlosti lietadla
    if (dt_s > 0.0f && ac.gs_knots > 0.0f && !isnan(ac.track)) {
      float dist_km = ac.gs_knots * 1.852f * dt_s / 3600.0f;
      float dLat = dist_km * cosf(ac.track * DEG_TO_RAD) / 111.32f;
      float cosLat = cosf(ac.lat * DEG_TO_RAD);
      float dLon = (fabsf(cosLat) > 0.01f) ? (dist_km * sinf(ac.track * DEG_TO_RAD) / (111.32f * cosLat)) : 0.0f;
      ac.lat += dLat;
      ac.lon += dLon;
    }

    int mapX = lonToX(ac.lon);
    int mapY = latToY(ac.lat);
    int sx = (int)mapXToScreenX(mapX);
    int sy = (int)mapYToScreenY(mapY);

    float distFromCenter = sqrtf((float)((sx - cx) * (sx - cx) + (sy - cy) * (sy - cy)));

    if (distFromCenter <= 106.0f) {
      drawAircraftSymbol(target, sx, sy, ac.nose_deg, ac.track, ac.gs_knots, ac.is_mil);
      if (currentRadiusKm <= 50) {
        drawAircraftTag(target, sx, sy, ac);
      }
    } else {
      drawEdgeIndicator(target, mapX, mapY, ac.is_mil);
    }
  }

  if (canvasReady) {
    canvas.pushSprite(0, 0);
  }
}

void drawWeatherOverlay(bool showTime) {
  int cx = TFT_W / 2;
  int cy = TFT_H / 2;

  // Hranice SR
  for (size_t i = 0; i < SK_BORDER_COUNT - 1; i++) {
    int sx1 = (int)mapXToScreenX(lonToX(SK_BORDER[i][0]));
    int sy1 = (int)mapYToScreenY(latToY(SK_BORDER[i][1]));
    int sx2 = (int)mapXToScreenX(lonToX(SK_BORDER[i+1][0]));
    int sy2 = (int)mapYToScreenY(latToY(SK_BORDER[i+1][1]));
    tft.drawLine(sx1, sy1, sx2, sy2, TFT_CYAN);
  }

  // Zobrazenie miest
  applyCityStyle();
  tft.setTextDatum(textdatum_t::bottom_center);
  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  for (size_t i = 0; i < CITY_COUNT; i++) {
    if (currentRadiusKm >= 100.0f && !CITIES[i].isMajor) continue;

    int sx = (int)mapXToScreenX(lonToX(CITIES[i].lon));
    int sy = (int)mapYToScreenY(latToY(CITIES[i].lat));
    if (sx >= 10 && sx <= TFT_W - 10 && sy >= 10 && sy <= TFT_H - 10) {
      tft.drawLine(sx - 2, sy, sx + 2, sy, TFT_RED);
      tft.drawLine(sx, sy - 2, sx, sy + 2, TFT_RED);
      tft.drawString(CITIES[i].name, sx, sy - 3);
    }
  }

  // Kruhy a zameriavač pre meteoradar
  tft.drawCircle(cx, cy, TFT_W / 2 - 2, TFT_DARKGREY);
  tft.drawCircle(cx, cy, TFT_W / 4, TFT_DARKGREY);
  tft.drawLine(cx - 6, cy, cx + 6, cy, TFT_WHITE);
  tft.drawLine(cx, cy - 6, cx, cy + 6, TFT_WHITE);

  applyScaleStyle();
  tft.setTextDatum(textdatum_t::top_center);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(String((int)currentRadiusKm) + " km", cx, 4);

  if (showTime) {
    tft.setTextDatum(textdatum_t::bottom_center);
    tft.drawString(getRadarTimeText(lastPngName), cx, TFT_H - 4);
  }
}


// =======================================================================================
// 8. PNG DEKÓDER & SPIFFS CALLBACK FUNKCIE
// =======================================================================================

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

  const float xRatio = (float)crop.w() / (float)TFT_W;
  for (int dx = 0; dx < TFT_W; dx++) {
    int srcX = crop.x1 + (int)(dx * xRatio);
    if (srcX < 0) srcX = 0;
    else if (srcX >= RADAR_IMG_W) srcX = RADAR_IMG_W - 1;
    outLine[dx] = line565[srcX];
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


// =======================================================================================
// 9. ŠTART A HLAVNÝ CYKLUS (SETUP & LOOP)
// =======================================================================================

void setup() {
  Serial.begin(115200);
  
  tft.init(); 
  tft.setRotation(0); 
  tft.setBrightness(180); 
  tft.loadFont(ui_font_vlw, lgfx::IFont::font_type_t::ft_vlw);
  tft.setTextSize(0.80f);

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
  setupWebServer();
  
  crop = makeCrop(centerLat, centerLon, currentRadiusKm);

  tft.fillScreen(TFT_BLACK);
  drawWeatherOverlay(false);
  tft.setTextSize(0.75f);
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

  // Spracovanie tlačidla (1x klik, 2x klik, dlhé podržanie)
  handleButton();

  // Spracovanie požiadaviek lokálneho webového servera
  server.handleClient();

  // Kontrola nočného režimu (Auto-Dimming)
  updateNightMode();

  // Automatické opätovné pripojenie k WiFi v prípade výpadku
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    delay(1000);
  }

  // Prepínanie režimov v Karuseli (ak je karusel zapnutý)
  if (carouselEnabled && (now - lastCarouselSwitchMs >= carouselIntervalMs)) {
    lastCarouselSwitchMs = now;
    setAppMode((currentMode == MODE_WEATHER) ? MODE_PLANES : MODE_WEATHER);
  }

  // Periodické aktualizácie podľa aktívneho režimu
  if (currentMode == MODE_WEATHER) {
    if (now - lastWeatherUpdateMs >= UPDATE_INTERVAL_MS) {
      lastWeatherUpdateMs = now;
      if (downloadLatestRadar()) renderRadar();
    }
  } else if (currentMode == MODE_PLANES) {
    // Sťahovanie čerstvých ADS-B dát každých 10s
    if (now - lastPlaneFetchMs >= PLANE_FETCH_INTERVAL_MS) {
      lastPlaneFetchMs = now;
      fetchPlanesData();
    }
    // Plynulé extrapolované prekresľovanie lietadiel každú sekundu
    if (now - lastPlaneRedrawMs >= PLANE_REDRAW_INTERVAL_MS) {
      lastPlaneRedrawMs = now;
      drawPlanes();
    }
  }
  
  delay(10);
}