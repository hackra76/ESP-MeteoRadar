/**
 * =======================================================================================
 * @file main.cpp
 * @brief ESP32-C3 MeteoRadar + ADS-B Plane Radar (Slovakia)
 * @author hackra76 / Antigravity AI
 * @version 1.3.0
 * 
 * @details 
 * Multifunkčný radar pre okrúhly GC9A01 240x240 displej a ESP32-C3 SuperMini.
 * 
 * Hlavné funkcie:
 * 1. METEORADAR (SHMÚ):
 *    - Automatické sťahovanie najnovšieho radarového kompozitu (cmax.kruh) zo SHMÚ.
 *    - Inteligentný streaming a resamplovanie PNG obrazu (800x550) do výrezu pre displej.
 *    - Vykreslenie hraníc SR a dynamické zobrazenie miest podľa zvoleného zoomu.
 * 
 * 2. ADS-B AIR RADAR (Lietadlá v reálnom čase):
 *    - Živé sledovanie lietadiel v okolí cez ADS-B open data feed (adsb.fi).
 *    - Automatické zisťovanie letových trás (ODKIAĽ > KAM, napr. VIE>AMS) cez VRS databázu.
 *    - Kruhová vyrovnávacia pamäť (Route Cache) pre rýchle zobrazenie a úsporu dát.
 *    - Indikácia smeru, kurzu, rýchlosti, výšky v metroch a vertikálnej rýchlosti (stúpanie/klesanie).
 *    - Edge Dots (oranžové/červené body na okraji) pre lietadlá mimo aktuálneho kruhu.
 * 
 * 3. SYSTÉM & OVLÁDANIE:
 *    - Automatický karusel (prepínanie medzi počasím a lietadlami v nastaviteľnom intervale).
 *    - Hardvérové prerušenie (ISR) pre tlačidlo na GPIO9:
 *      - Krátke stlačenie = cyklická zmena mierky (10, 25, 50, 100, 250 km).
 *      - Dlhé podržanie (3s) = reset WiFi a vymazanie NVS pamäte do továrenských nastavení.
 *    - Integrovaný WiFiManager konfiguračný portál (nastavenie Lat, Lon, Zoom, Offset, Karusel).
 * =======================================================================================
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
#include "ui_font.h"

// =======================================================================================
// 1. GLOBÁLNE INŠTANCIE & DÁTOVÉ ŠTRUKTÚRY
// =======================================================================================
LGFX tft;                 ///< Inštancia ovládača displeja LovyanGFX
PNG png;                  ///< PNG dekodér pre SHMÚ radarové snímky
File pngFile;             ///< Súborový deskriptor pre SPIFFS
Preferences prefs;        ///< Trvalé úložisko NVS pre konfiguráciu

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

// Úrovne priblíženia (km)
static const float ZOOM_LEVELS_KM[] = {10.0f, 25.0f, 50.0f, 100.0f, 250.0f};
static constexpr int ZOOM_LEVEL_COUNT = sizeof(ZOOM_LEVELS_KM) / sizeof(ZOOM_LEVELS_KM[0]);
int zoomIndex = 2; // Predvolene 50 km
float currentRadiusKm = atof(DEFAULT_RADIUS_KM_TEXT);

// Riadenie hardvérového prerušenia tlačidla (ISR)
volatile bool zoomRequested = false;
volatile uint32_t lastBtnInterruptMs = 0;
static constexpr uint32_t DEBOUNCE_DELAY_MS = 200;

// Stavový automat aplikácie (Karusel)
enum AppMode { MODE_WEATHER, MODE_PLANES };
AppMode currentMode = MODE_WEATHER;

uint32_t lastWeatherUpdateMs = 0;
uint32_t lastCarouselSwitchMs = 0;
uint32_t lastPlaneFetchMs = 0;
static constexpr uint32_t PLANE_FETCH_INTERVAL_MS = 10000;

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

// Dopredné deklarácie funkcií
bool renderRadar();
void fetchAndDrawPlanes();
void drawWeatherOverlay(bool showTime);
void drawPlaneRadarGrid();


// =======================================================================================
// 2. HARDVÉROVÉ PRERUŠENIE PRE TLAČIDLO (ISR)
// =======================================================================================
void IRAM_ATTR buttonISR() {
  uint32_t now = millis();
  if (now - lastBtnInterruptMs > DEBOUNCE_DELAY_MS) {
    zoomRequested = true;
    lastBtnInterruptMs = now;
  }
}


// =======================================================================================
// 3. GEOGRAFICKÉ & PROJEKČNÉ FUNKCIE (MAPPING)
// =======================================================================================

/** Prepočet zemepisnej dĺžky na X súradnicu v zdrojovom PNG snímku */
inline int lonToX(float lon) { 
  return roundf((lon - LON_LEFT) * (RADAR_IMG_W - 1) / (LON_RIGHT - LON_LEFT)); 
}

/** Prepočet zemepisnej šírky na Y súradnicu v zdrojovom PNG snímku */
inline int latToY(float lat) { 
  return roundf((LAT_TOP - lat) * (RADAR_IMG_H - 1) / (LAT_TOP - LAT_BOTTOM)); 
}

/** Mapovanie X súradnice z výrezu na fyzický pixel displeja */
inline float mapXToScreenX(float mapX) { 
  return (mapX - crop.x1) * (float)TFT_W / (float)crop.w(); 
}

/** Mapovanie Y súradnice z výrezu na fyzický pixel displeja */
inline float mapYToScreenY(float mapY) { 
  return (mapY - crop.y1) * (float)TFT_H / (float)crop.h(); 
}

/** Výpočet orezového okna (CropBox) na základe stredu a polomeru v km */
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
// 4. POUŽÍVATEĽSKÉ ROZHRANIE, NASTAVENIA & WIFI
// =======================================================================================

/** Zobrazenie viacriadkového stavového hlásenia v strede obrazovky */
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

/** Získanie času snímky z názvu súboru SHMÚ (UTC + časový offset) */
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

/** Získanie aktuálneho systémového času z NTP */
String getCurrentSystemTimeText() {
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  if (t->tm_year < 100) return "--:--";
  char out[6];
  snprintf(out, sizeof(out), "%02d:%02d", t->tm_hour, t->tm_min);
  return String(out);
}

/** Továrenský reset konfigurácie WiFi a NVS */
void resetSettingsAndRestart() {
  detachInterrupt(digitalPinToInterrupt(ZOOM_BUTTON_PIN));
  showStatus("Reset nastavenia...");
  WiFiManager wm; 
  wm.resetSettings();
  prefs.begin("radar", false); 
  prefs.clear(); 
  prefs.end();
  delay(1000); 
  ESP.restart();
}

/** Kontrola podržania tlačidla pri štarte pre vyvolanie resetu */
void checkResetButtonAtBoot() {
  if (digitalRead(ZOOM_BUTTON_PIN) != LOW) return;
  showStatus("Drz pre reset");
  uint32_t start = millis();
  while (digitalRead(ZOOM_BUTTON_PIN) == LOW) {
    if (millis() - start >= RESET_HOLD_MS) resetSettingsAndRestart();
    delay(20);
  }
}

/** Spracovanie požiadavky na zmenu mierky (zoom) alebo reset pri dlhom podržaní */
void processZoomRequest() {
  if (!zoomRequested) return;
  zoomRequested = false;

  if (digitalRead(ZOOM_BUTTON_PIN) == LOW) {
    uint32_t holdStart = millis();
    while (digitalRead(ZOOM_BUTTON_PIN) == LOW) {
      if (millis() - holdStart >= RESET_HOLD_MS) {
        resetSettingsAndRestart();
        return;
      }
      delay(10);
    }
  }

  // Zmena úrovne zoomu
  zoomIndex = (zoomIndex + 1) % ZOOM_LEVEL_COUNT;
  currentRadiusKm = ZOOM_LEVELS_KM[zoomIndex];
  crop = makeCrop(centerLat, centerLon, currentRadiusKm);
  
  // Uloženie zvoleného zoomu do NVS
  prefs.begin("radar", false);
  prefs.putFloat("radius", currentRadiusKm);
  prefs.end();

  if (currentMode == MODE_WEATHER) renderRadar();
  else fetchAndDrawPlanes();
}

/** Pripojenie k WiFi sieti cez WiFiManager s vlastnými parametrami */
void connectWiFi() {
  WiFi.mode(WIFI_STA); 
  delay(100);
  showStatus("ESP MeteoRadar v1.3\nPripajam WiFi...");

  // Načítanie existujúcich hodnôt z NVS pamäte
  prefs.begin("radar", true);
  String curLat = String(prefs.getFloat("lat", atof(DEFAULT_CENTER_LAT)), 4);
  String curLon = String(prefs.getFloat("lon", atof(DEFAULT_CENTER_LON)), 4);
  String curRad = String((int)prefs.getFloat("radius", atof(DEFAULT_RADIUS_KM_TEXT)));
  String curOff = String(prefs.getInt("offset", DEFAULT_TIME_OFFSET_HOURS));
  String curCar = String(prefs.getInt("car_int", 30));
  prefs.end();

  // Definícia konfiguračných polí pre WiFiManager portál
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

  // Uloženie zadaných hodnôt po úspešnom odoslaní z portálu
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
}


// =======================================================================================
// 5. SHMÚ METEORADAR (SŤAHOVANIE & SPRACOVANIE)
// =======================================================================================

/** Vyhľadanie najnovšieho názvu radarového súboru cmax.kruh.*.png v HTML odpovedi */
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

/** Stiahnutie najnovšieho PNG radarového kompozitu do SPIFFS */
bool downloadLatestRadar() {
  if (WiFi.status() != WL_CONNECTED) return false;
  
  for (int attempt = 1; attempt <= 2; attempt++) {
    WiFiClientSecure client; 
    client.setInsecure(); 
    client.setHandshakeTimeout(15000);
    HTTPClient http; 
    http.setTimeout(15000);

    if (http.begin(client, SHMU_API_URL)) {
      if (http.GET() == HTTP_CODE_OK) {
        String window, latest, newestTs;
        WiFiClient* stream = http.getStreamPtr();
        uint8_t buf[512];
        while (http.connected()) {
          processZoomRequest();
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
        http.end(); 
        client.stop();

        if (!latest.isEmpty()) {
          if (latest == lastPngName && SPIFFS.exists(RADAR_FILE)) return true;
          String url = String(SHMU_BASE_URL) + latest;
          HTTPClient httpImg; 
          httpImg.setTimeout(25000);
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


// =======================================================================================
// 6. ADS-B LETECKÝ RADAR & VRS LETOVÉ TRASY
// =======================================================================================

// Konfigurácia symbolov a typografie lietadiel
static constexpr int kAircraftNoseLenPx = 8;
static constexpr int kAircraftTailLenPx = 3;
static constexpr int kAircraftTailHalfPx = 4;
static constexpr float kVrateThresholdFpm = 128.0f;
static constexpr int kVrateArrowW = 8;
static constexpr int kVrateArrowH = 8;
static constexpr int kVrateArrowGapPx = 3;
static constexpr int kTypeSpeedGapPx = 4;
static constexpr int kAircraftLabelGapPx = 3;

// Typografické štýly
inline void applyTagStyle()      { tft.setTextSize(0.80f); }
inline void applyCardinalStyle() { tft.setTextSize(0.75f); }
inline void applyScaleStyle()    { tft.setTextSize(0.75f); }
inline void applyCityStyle()     { tft.setTextSize(0.80f); }

// ---- Statické vyhľadávanie letových trás (VRS standing-data) ----
// Dopytuje statickú databázu letových plánov: https://vrs-standing-data.adsb.lol/routes/<prvé 2 písmená>/<Callsign>.json
static constexpr char kRouteBaseUrl[] = "https://vrs-standing-data.adsb.lol/routes/";
static constexpr size_t kRouteCacheSize = 48;

struct RouteCacheEntry {
  char callsign[9];
  char route[10];
  bool used;
};
static RouteCacheEntry s_route_cache[kRouteCacheSize] = {};
static size_t s_route_cache_next = 0;

/** Nájdenie trasy v lokálnej pamäti RAM podľa volacieho znaku */
static const char* routeCacheFind(const char* callsign) {
  for (size_t i = 0; i < kRouteCacheSize; ++i) {
    if (s_route_cache[i].used && strcmp(s_route_cache[i].callsign, callsign) == 0) {
      return s_route_cache[i].route;
    }
  }
  return nullptr;
}

/** Uloženie nájdenej (alebo prázdnej) trasy do kruhovej keše */
static void routeCachePut(const char* callsign, const char* route) {
  RouteCacheEntry& e = s_route_cache[s_route_cache_next];
  s_route_cache_next = (s_route_cache_next + 1) % kRouteCacheSize;
  e.used = true;
  strlcpy(e.callsign, callsign, sizeof(e.callsign));
  strlcpy(e.route, route, sizeof(e.route));
}

/** Prevod formátu z JSON ("VIE-AMS" / "LOWW-EHAM") na prehľadné "VIE>AMS" */
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

/** Stiahnutie trasy zo servera VRS pre konkrétny let */
static void fetchRouteForCallsign(const char* cs, char* out_route, size_t out_len) {
  out_route[0] = '\0';
  if (cs[0] == '\0' || strlen(cs) < 3) return;

  const char* cached = routeCacheFind(cs);
  if (cached != nullptr) {
    strlcpy(out_route, cached, out_len);
    return;
  }

  // Ochrana pred nedostatkom pamäte pre bezpečný TLS handshake
  if (ESP.getFreeHeap() < 40000) return;

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
      routeCachePut(cs, ""); // Negatívna keš - neopakovať dopyt pre neznáme lety
    }
    http.end();
  }
  client.stop();
}

/** Výpočet dĺžky vektora rýchlosti lietadla v pixeloch */
int speedLineLengthPx(float gs_knots) {
  if (gs_knots <= 0.0f) return 0;
  constexpr float kKmPerKnotPerHorizon = 1.852f * 60.0f / 3600.0f;
  const float px = gs_knots * kKmPerKnotPerHorizon * 107.0f / 13.3f * (1.5f / 5.0f);
  const int len = (int)(px + 0.5f);
  return (len < 2) ? 2 : len;
}

/** Vykreslenie ikony lietadla (trojuholník s vektorom rýchlosti) */
void drawAircraftSymbol(int x, int y, float heading_deg, float track_deg, float gs_knots, bool is_mil) {
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
    tft.drawWideLine(tip_x, tip_y, ex, ey, 1, tft.color565(255, 255, 255));
  }

  uint16_t symbolColor = is_mil ? tft.color565(255, 0, 0) : tft.color565(0, 120, 255);
  tft.fillTriangle(tip_x, tip_y, base_x + wing_x, base_y + wing_y, base_x - wing_x, base_y - wing_y, symbolColor);
}

/** Vráti text pre prvý riadok štítku (prednostne trasu, inak callsign) */
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

void drawVRateArrow(int x, int ly, int line_h, int dir) {
  const int ty = ly + (line_h - kVrateArrowH) / 2;
  if (dir > 0) {
    tft.fillTriangle(x + kVrateArrowW / 2, ty, x, ty + kVrateArrowH, x + kVrateArrowW, ty + kVrateArrowH, tft.color565(30, 220, 30));
  } else if (dir < 0) {
    tft.fillTriangle(x + kVrateArrowW / 2, ty + kVrateArrowH, x, ty, x + kVrateArrowW, ty, tft.color565(235, 40, 40));
  }
}

int measureTagBlockWidth(const AircraftData& ac) {
  applyTagStyle();
  int max_w = 0;
  const char* top = tagTopLine(ac);
  if (top[0] != '\0') {
    int w = tft.textWidth(top);
    if (w > max_w) max_w = w;
  }
  char type_part[12];
  char speed_part[10];
  formatTypePart(ac, type_part, sizeof(type_part));
  formatSpeedPart(ac, speed_part, sizeof(speed_part));
  int w2 = 0;
  if (type_part[0] != '\0') w2 += tft.textWidth(type_part);
  if (speed_part[0] != '\0') w2 += tft.textWidth(speed_part);
  if (type_part[0] != '\0' && speed_part[0] != '\0') w2 += kTypeSpeedGapPx;
  if (w2 > max_w) max_w = w2;

  if (ac.alt[0] != '\0') {
    int w3 = tft.textWidth(ac.alt);
    if (vrateDirection(ac) != 0) w3 += kVrateArrowGapPx + kVrateArrowW;
    if (w3 > max_w) max_w = w3;
  }
  return max_w;
}

/** Vykreslenie informačného 3-riadkového štítku lietadla */
void drawAircraftTag(int x, int y, const AircraftData& ac) {
  applyTagStyle();
  const int line_h = tft.fontHeight();
  const int block_w = measureTagBlockWidth(ac);
  const int block_h = line_h * 3;
  int ly = y - block_h / 2;

  const int symbol_half = kAircraftNoseLenPx + kAircraftTailHalfPx;
  const bool tag_on_right = x < (TFT_W / 2);
  int anchor_x = 0;

  if (tag_on_right) {
    anchor_x = x + symbol_half + kAircraftLabelGapPx;
    anchor_x = std::min(anchor_x, TFT_W - block_w - 2);
    tft.setTextDatum(textdatum_t::top_left);
  } else {
    anchor_x = x - symbol_half - kAircraftLabelGapPx;
    anchor_x = std::max(anchor_x, block_w + 2);
    tft.setTextDatum(textdatum_t::top_right);
  }
  ly = constrain(ly, 2, TFT_H - block_h - 2);

  // Riadok 1: Trasa letísk (fialová) alebo Callsign (biela/červená)
  const char* top = tagTopLine(ac);
  if (top[0] != '\0') {
    uint16_t col = ac.is_mil ? tft.color565(255, 60, 60) : (ac.route[0] != '\0' ? tft.color565(255, 130, 255) : TFT_WHITE);
    tft.setTextColor(col, TFT_BLACK);
    tft.drawString(top, anchor_x, ly);
  }
  ly += line_h;

  // Riadok 2: Typ lietadla (modrá) + Rýchlosť v km/h (svetlozelená)
  char type_part[12];
  char speed_part[10];
  formatTypePart(ac, type_part, sizeof(type_part));
  formatSpeedPart(ac, speed_part, sizeof(speed_part));
  const int w_type = (type_part[0] != '\0') ? tft.textWidth(type_part) : 0;
  const int w_speed = (speed_part[0] != '\0') ? tft.textWidth(speed_part) : 0;

  if (tag_on_right) {
    if (type_part[0] != '\0') {
      tft.setTextColor(tft.color565(90, 200, 255), TFT_BLACK);
      tft.drawString(type_part, anchor_x, ly);
    }
    if (speed_part[0] != '\0') {
      tft.setTextColor(tft.color565(150, 235, 150), TFT_BLACK);
      tft.drawString(speed_part, anchor_x + w_type + kTypeSpeedGapPx, ly);
    }
  } else {
    if (speed_part[0] != '\0') {
      tft.setTextColor(tft.color565(150, 235, 150), TFT_BLACK);
      tft.drawString(speed_part, anchor_x, ly);
    }
    if (type_part[0] != '\0') {
      tft.setTextColor(tft.color565(90, 200, 255), TFT_BLACK);
      tft.drawString(type_part, anchor_x - w_speed - kTypeSpeedGapPx, ly);
    }
  }
  ly += line_h;

  // Riadok 3: Výška v metroch (žltá) + Šípka stúpania/klesania
  if (ac.alt[0] != '\0') {
    tft.setTextColor(tft.color565(255, 255, 0), TFT_BLACK);
    tft.drawString(ac.alt, anchor_x, ly);
    const int dir = vrateDirection(ac);
    if (dir != 0) {
      const int w_alt = tft.textWidth(ac.alt);
      int ax = tag_on_right ? (anchor_x + w_alt + kVrateArrowGapPx) : (anchor_x - w_alt - kVrateArrowGapPx - kVrateArrowW);
      drawVRateArrow(ax, ly, line_h, dir);
    }
  }
}

/** Vykreslenie indikátora (Edge Dot) pre lietadlá za okrajom radaru */
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

/** Stiahnutie zoznamu lietadiel z ADS-B API a vykreslenie radaru */
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

      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
      
      if (!err) {
        JsonArray acList = doc["ac"].as<JsonArray>();
        
        tft.fillScreen(TFT_BLACK);
        drawPlaneRadarGrid();

        int cx = TFT_W / 2;
        int cy = TFT_H / 2;
        int routesFetchedThisCycle = 0;

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

          const char* fl = plane["flight"] | "";
          strlcpy(ac.callsign, fl, sizeof(ac.callsign));
          
          // Orezanie koncových medzier z volacieho znaku
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

          int mapX = lonToX(ac.lon);
          int mapY = latToY(ac.lat);
          int sx = (int)mapXToScreenX(mapX);
          int sy = (int)mapYToScreenY(mapY);

          float distFromCenter = sqrtf((float)((sx - cx) * (sx - cx) + (sy - cy) * (sy - cy)));

          // Zistenie trasy letísk (ODKIAĽ>KAM) cez cache alebo statické VRS súbory
          ac.route[0] = '\0';
          if (strcmp(ac.callsign, "NOCALL") != 0 && !ac.is_mil) {
            const char* cached = routeCacheFind(ac.callsign);
            if (cached != nullptr) {
              strlcpy(ac.route, cached, sizeof(ac.route));
            } else if (distFromCenter <= 112.0f && routesFetchedThisCycle < 2) {
              fetchRouteForCallsign(ac.callsign, ac.route, sizeof(ac.route));
              routesFetchedThisCycle++;
            }
          }

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


// =======================================================================================
// 7. GRAFICKÉ PREKRYTIE & VYKRESLOVANIE RADARU
// =======================================================================================

/** Vykreslenie radarovej mriežky, kružníc a mapy SR pre letecký režim */
void drawPlaneRadarGrid() {
  int cx = TFT_W / 2;
  int cy = TFT_H / 2;

  // 1. Hranica SR (azúrová linka)
  for (size_t i = 0; i < SK_BORDER_COUNT - 1; i++) {
    int sx1 = (int)mapXToScreenX(lonToX(SK_BORDER[i][0]));
    int sy1 = (int)mapYToScreenY(latToY(SK_BORDER[i][1]));
    int sx2 = (int)mapXToScreenX(lonToX(SK_BORDER[i+1][0]));
    int sy2 = (int)mapYToScreenY(latToY(SK_BORDER[i+1][1]));
    tft.drawLine(sx1, sy1, sx2, sy2, TFT_CYAN);
  }

  // 2. Filtrovanie a zobrazenie miest
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

  // 3. Koncentrické zelené kruhy a kríž
  uint16_t gridColor = tft.color565(0, 200, 0);     
  uint16_t dimGridColor = tft.color565(0, 80, 0);   

  tft.drawLine(cx - 110, cy, cx + 110, cy, dimGridColor);
  tft.drawLine(cx, cy - 110, cx, cy + 110, dimGridColor);

  tft.drawCircle(cx, cy, 35, gridColor);
  tft.drawCircle(cx, cy, 70, gridColor);
  tft.drawCircle(cx, cy, 105, gridColor);

  // 4. Svetové strany (N, S, W, E)
  applyCardinalStyle();
  tft.setTextDatum(textdatum_t::middle_center);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("N", cx, 8);
  tft.drawString("S", cx, TFT_H - 8);
  tft.drawString("W", 8, cy);
  tft.drawString("E", TFT_W - 8, cy);

  // 5. Mierka a čas aktualizácie
  applyScaleStyle();
  tft.setTextDatum(textdatum_t::top_center);
  tft.setTextColor(tft.color565(0, 255, 0), TFT_BLACK);
  tft.drawString(String((int)currentRadiusKm) + " km", cx, 4);

  tft.setTextDatum(textdatum_t::bottom_center);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(getCurrentSystemTimeText(), cx, TFT_H - 4);
}

/** Vykreslenie prekrytia pre meteoradar SHMÚ */
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

/** Optimalizovaný resampler pre prevod riadku PNG do rozlíšenia displeja */
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

/** Vykreslenie meteoradaru zo SPIFFS */
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
  
  // Inicializácia displeja GC9A01
  tft.init(); 
  tft.setRotation(0); 
  tft.setBrightness(180); 
  tft.loadFont(ui_font_vlw, lgfx::IFont::font_type_t::ft_vlw);
  tft.setTextSize(0.80f);

  // Inicializácia tlačidla
  pinMode(ZOOM_BUTTON_PIN, INPUT_PULLUP);
  
  // Načítanie preferencií
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

  // Pripojenie hardvérového prerušenia
  attachInterrupt(digitalPinToInterrupt(ZOOM_BUTTON_PIN), buttonISR, FALLING);
  
  crop = makeCrop(centerLat, centerLon, currentRadiusKm);

  // Zobrazenie úvodnej obrazovky
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

  // Spracovanie stlačenia tlačidla
  processZoomRequest();

  // Automatické opätovné pripojenie k WiFi v prípade výpadku
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    delay(1000);
  }

  // Prepínanie režimov v Karuseli (Počasie <-> Lietadlá)
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

  // Periodické aktualizácie podľa aktívneho režimu
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
  
  delay(10);
}