/**
 * @file main.cpp
 * @brief ESP32-C3 MeteoRadar Display for SHMÚ (Slovakia)
 * @details Fetches live meteorological radar images, crops/resamples them 
 *          dynamically around a custom GPS coordinate, and overlays national 
 *          borders, city markers, and distance rings on a round GC9A01 display.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SPIFFS.h>
#include <Preferences.h>
#include <WiFiManager.h>
#include <PNGdec.h>
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

/**
 * @brief Slovakia Border Vector Array (Longitude, Latitude)
 * MODIFY HERE: You can add, remove, or adjust coordinate points 
 * to refine the border shape or adapt this for another country.
 */
static const float SK_BORDER[][2] = {
  {16.96, 48.48}, {16.85, 48.28}, {17.06, 48.14}, {17.16, 48.02}, {17.65, 47.78},
  {18.10, 47.76}, {18.30, 47.78}, {18.75, 47.79}, {18.82, 48.08}, {19.00, 48.16},
  {19.82, 48.08}, {20.07, 48.27}, {20.45, 48.50}, {20.61, 48.55}, {21.05, 48.52},
  {21.68, 48.37}, {22.15, 48.44}, {22.14, 48.16}, {22.56, 49.08}, {22.38, 49.12},
  {21.31, 49.42}, {20.85, 49.40}, {20.65, 49.41}, {20.10, 49.33}, {19.58, 49.44},
  {19.22, 49.52}, {18.84, 49.51}, {18.45, 49.45}, {18.06, 49.18}, {17.85, 48.95},
  {17.62, 48.87}, {17.32, 48.85}, {17.07, 48.77}, {16.96, 48.48}
};
static constexpr size_t SK_BORDER_COUNT = sizeof(SK_BORDER) / sizeof(SK_BORDER[0]);

/**
 * @brief Major Cities Database (Name, Latitude, Longitude)
 * MODIFY HERE: Add or remove cities/towns to display on the map overlay.
 */
struct City {
  const char* name;
  float lat;
  float lon;
};

static const City CITIES[] = {
  {"BA", 48.1486, 17.1077}, // Bratislava
  {"TT", 48.3775, 17.5883}, // Trnava
  {"NR", 48.3061, 18.0864}, // Nitra
  {"TN", 48.8945, 18.0444}, // Trenčín
  {"ZA", 49.2231, 18.7397}, // Žilina
  {"BB", 48.7363, 19.1462}, // Banská Bystrica
  {"PO", 48.9984, 21.2393}, // Prešov
  {"KE", 48.7164, 21.2611}, // Košice
  {"BJ", 49.2918, 21.2727}, // Bardejov
  {"PP", 49.0595, 20.2978}, // Poprad
  {"MI", 48.7547, 21.9195}, // Michalovce
  {"LC", 48.3294, 19.6648}  // Lučenec
};
static constexpr size_t CITY_COUNT = sizeof(CITIES) / sizeof(CITIES[0]);

// Bounding box structure for map coordinate cropping
struct CropBox {
  int x1, y1, x2, y2;
  int w() const { return x2 - x1 + 1; }
  int h() const { return y2 - y1 + 1; }
};

CropBox crop;
uint16_t line565[RADAR_IMG_W];
uint16_t outLine[TFT_W];

String lastPngName;
uint32_t lastUpdate = 0;
Preferences prefs;

// Default runtime configuration states (overwritten by Preferences or WiFiManager)
float centerLat = atof(DEFAULT_CENTER_LAT);
float centerLon = atof(DEFAULT_CENTER_LON);
int timeOffsetHours = DEFAULT_TIME_OFFSET_HOURS;

// Available zoom steps in kilometers radius from center
static const float ZOOM_LEVELS_KM[] = {10.0f, 25.0f, 50.0f, 100.0f};
static constexpr int ZOOM_LEVEL_COUNT = sizeof(ZOOM_LEVELS_KM) / sizeof(ZOOM_LEVELS_KM[0]);
int zoomIndex = 1;
float currentRadiusKm = atof(DEFAULT_RADIUS_KM_TEXT);

// Button debounce and long-press state tracking variables
bool lastButtonReading = HIGH;
bool stableButtonState = HIGH;
uint32_t lastButtonChangeMs = 0;
uint32_t buttonPressStartMs = 0;
bool longPressHandled = false;
static constexpr uint32_t BUTTON_DEBOUNCE_MS = 50;

// Forward Declarations
bool renderRadar();


// ==========================================
// 2. GEOGRAPHIC & PROJECTION MAPPING HELPERS
// ==========================================

/**
 * @brief Converts geographic Longitude to image pixel X coordinate.
 */
int lonToX(float lon) {
  return roundf((lon - LON_LEFT) * (RADAR_IMG_W - 1) / (LON_RIGHT - LON_LEFT));
}

/**
 * @brief Converts geographic Latitude to image pixel Y coordinate.
 */
int latToY(float lat) {
  return roundf((LAT_TOP - lat) * (RADAR_IMG_H - 1) / (LAT_TOP - LAT_BOTTOM));
}

/**
 * @brief Maps global map pixels to circular screen coordinates (X axis).
 */
float mapXToScreenX(float mapX) {
  return (mapX - crop.x1) * (float)TFT_W / (float)crop.w();
}

/**
 * @brief Maps global map pixels to circular screen coordinates (Y axis).
 */
float mapYToScreenY(float mapY) {
  return (mapY - crop.y1) * (float)TFT_H / (float)crop.h();
}

/**
 * @brief Computes the dynamic pixel crop window based on center GPS and radius.
 */
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


// ==========================================
// 3. UI & STATUS DISPLAY FUNCTIONS
// ==========================================

/**
 * @brief Renders multiline status messages centered on the screen.
 */
void showStatus(const String& text) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  int y = 80;
  int start = 0;

  while (true) {
    int pos = text.indexOf('\n', start);
    String line = (pos == -1) ? text.substring(start) : text.substring(start, pos);

    tft.setTextDatum(middle_center);
    tft.drawString(line, TFT_W / 2, y);
    y += 20;

    if (pos == -1) break;
    start = pos + 1;
  }
}

/**
 * @brief Extracts and formats timestamp HH:MM from radar filename string.
 */
String getRadarTimeText(const String& filename) {
  const String prefix = "cmax.kruh.";
  int start = filename.indexOf(prefix);
  if (start < 0) return "--:--";

  int dateStart = start + prefix.length();
  if (filename.length() < dateStart + 13) return "--:--";

  String hhmm = filename.substring(dateStart + 9, dateStart + 13);
  if (hhmm.length() != 4) return "--:--";
  for (int i = 0; i < 4; i++) {
    if (!isDigit(hhmm[i])) return "--:--";
  }

  int hour = hhmm.substring(0, 2).toInt();
  int minute = hhmm.substring(2, 4).toInt();
  hour = (hour + timeOffsetHours) % 24;
  if (hour < 0) hour += 24;

  char out[6];
  snprintf(out, sizeof(out), "%02d:%02d", hour, minute);
  return String(out);
}


// ==========================================
// 4. SYSTEM RESET & PREFERENCES STORAGE
// ==========================================

/**
 * @brief Clears WiFi Manager settings and NVS preferences, then restarts.
 */
void resetSettingsAndRestart() {
  Serial.println("\n================================");
  Serial.println("Resetting WiFiManager and preferences...");
  Serial.println("================================");

  showStatus("Reset nastavenia...");

  WiFiManager wm;
  wm.resetSettings();

  prefs.begin("radar", false);
  prefs.clear();
  prefs.end();

  delay(1000);
  ESP.restart();
}

/**
 * @brief Checks if the physical button is held down at boot to trigger factory reset.
 */
void checkResetButtonAtBoot() {
  if (digitalRead(ZOOM_BUTTON_PIN) != LOW) return;

  Serial.println("Button held at boot - waiting for factory reset timeout...");
  showStatus("Drz pre reset");

  uint32_t start = millis();
  while (digitalRead(ZOOM_BUTTON_PIN) == LOW) {
    if (millis() - start >= RESET_HOLD_MS) {
      resetSettingsAndRestart();
    }
    delay(20);
  }
}

void initZoomLevel() {
  zoomIndex = 0;
  float bestDiff = fabsf(ZOOM_LEVELS_KM[0] - currentRadiusKm);
  for (int i = 1; i < ZOOM_LEVEL_COUNT; i++) {
    float diff = fabsf(ZOOM_LEVELS_KM[i] - currentRadiusKm);
    if (diff < bestDiff) {
      bestDiff = diff;
      zoomIndex = i;
    }
  }
  currentRadiusKm = ZOOM_LEVELS_KM[zoomIndex];
}

void nextZoomLevel() {
  zoomIndex = (zoomIndex + 1) % ZOOM_LEVEL_COUNT;
  currentRadiusKm = ZOOM_LEVELS_KM[zoomIndex];
  Serial.printf("Zoom: %.0f km\n", currentRadiusKm);
  renderRadar();
}

void saveRadiusToPrefs(float radiusKm) {
  prefs.begin("radar", false);
  prefs.putFloat("radius", radiusKm);
  prefs.end();
}

void saveConfigToPrefs(float lat, float lon, float radiusKm, int offsetHours) {
  prefs.begin("radar", false);
  prefs.putFloat("lat", lat);
  prefs.putFloat("lon", lon);
  prefs.putFloat("radius", radiusKm);
  prefs.putInt("tzOffset", offsetHours);
  prefs.end();
}

void loadPositionFromPrefs() {
  prefs.begin("radar", true);
  centerLat = prefs.getFloat("lat", atof(DEFAULT_CENTER_LAT));
  centerLon = prefs.getFloat("lon", atof(DEFAULT_CENTER_LON));
  currentRadiusKm = prefs.getFloat("radius", atof(DEFAULT_RADIUS_KM_TEXT));
  timeOffsetHours = prefs.getInt("tzOffset", DEFAULT_TIME_OFFSET_HOURS);
  prefs.end();

  Serial.printf("Position: %.6f, %.6f\n", centerLat, centerLon);
  Serial.printf("Zoom: %.0f km\n", currentRadiusKm);
  Serial.printf("Time Offset: %+d h\n", timeOffsetHours);
}

/**
 * @brief Handles short press (zoom toggle) and long press (factory reset).
 */
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
        saveRadiusToPrefs(currentRadiusKm);
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


// ==========================================
// 5. WIFI & CONFIGURATION PORTAL
// ==========================================

/**
 * @brief Connects to WiFi or launches AP Portal with configuration input fields.
 */
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  delay(100);

  showStatus("ESP MeteoRadar v1.0\nSHMU Slovensko\nWiFi portal...");
  delay(500);

  char latBuf[16], lonBuf[16], radiusBuf[8], offsetBuf[8];
  snprintf(latBuf, sizeof(latBuf), "%.6f", centerLat);
  snprintf(lonBuf, sizeof(lonBuf), "%.6f", centerLon);
  snprintf(radiusBuf, sizeof(radiusBuf), "%.0f", currentRadiusKm);
  snprintf(offsetBuf, sizeof(offsetBuf), "%d", timeOffsetHours);

  WiFiManager wm;
  wm.setConfigPortalTimeout(300);
  wm.setConnectTimeout(15);
  wm.setBreakAfterConfig(true);

  wm.setAPCallback([](WiFiManager *myWiFiManager) {
    showStatus("WiFi Portal aktivny!\n\nPripojte sa na WiFi:\nESPMeteoRadar\n\nIP: 192.168.4.1");
  });

  WiFiManagerParameter p_lat("lat", "Zemepisná šírka / latitude", latBuf, sizeof(latBuf));
  WiFiManagerParameter p_lon("lon", "Zemepisná dĺžka / longitude", lonBuf, sizeof(lonBuf));
  WiFiManagerParameter p_radius("radius", "Predvolený rozsah km", radiusBuf, sizeof(radiusBuf));
  WiFiManagerParameter p_offset("offset", "Časový offset hodín (+2 leto, +1 zima)", offsetBuf, sizeof(offsetBuf));
  
  wm.addParameter(&p_lat);
  wm.addParameter(&p_lon);
  wm.addParameter(&p_radius);
  wm.addParameter(&p_offset);

  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  bool ok = wm.autoConnect("ESPMeteoRadar");

  if (!ok || WiFi.status() != WL_CONNECTED) {
    showStatus("WiFi chyba\nPodrz tlacidlo 3s\npre reset");
    return;
  }

  float newLat = atof(p_lat.getValue());
  float newLon = atof(p_lon.getValue());
  float newRadius = atof(p_radius.getValue());
  int newOffset = atoi(p_offset.getValue());

  if (newLat > 46.5f && newLat < 50.5f) centerLat = newLat;
  if (newLon > 14.0f && newLon < 23.5f) centerLon = newLon;
  if (newRadius == 10.0f || newRadius == 25.0f || newRadius == 50.0f || newRadius == 100.0f) {
    currentRadiusKm = newRadius;
  }
  if (newOffset >= -12 && newOffset <= 14) {
    timeOffsetHours = newOffset;
  }
  
  saveConfigToPrefs(centerLat, centerLon, currentRadiusKm, timeOffsetHours);
  initZoomLevel();
}


// ==========================================
// 6. API PARSING & RADAR DOWNLOAD
// ==========================================

String extractRadarTimestamp(const String& filename) {
  const String prefix = "cmax.kruh.";
  int start = filename.indexOf(prefix);
  if (start < 0) return "";

  int dateStart = start + prefix.length();
  if (filename.length() < dateStart + 13) return "";

  String date = filename.substring(dateStart, dateStart + 8);
  String hhmm = filename.substring(dateStart + 9, dateStart + 13);

  if (filename[dateStart + 8] != '.') return "";
  for (size_t i = 0; i < date.length(); i++) if (!isDigit(date[i])) return "";
  for (size_t i = 0; i < hhmm.length(); i++) if (!isDigit(hhmm[i])) return "";

  return date + hhmm;
}

String findLatestPngNameInText(const String& text, String& newestTs, int& foundCount) {
  const String prefix = "cmax.kruh.";
  String latest;
  int pos = 0;

  while (true) {
    int idx = text.indexOf(prefix, pos);
    if (idx < 0) break;

    int end = text.indexOf(".png", idx);
    if (end < 0) break;

    String name = text.substring(idx, end + 4);
    String ts = extractRadarTimestamp(name);
    if (!ts.isEmpty()) {
      foundCount++;
      if (ts > newestTs) {
        newestTs = ts;
        latest = name;
      }
    }
    pos = end + 4;
  }
  return latest;
}

String findLatestPngNameFromHttpStream(HTTPClient& http) {
  WiFiClient* stream = http.getStreamPtr();
  const int contentLength = http.getSize();

  String window, latest, newestTs;
  int foundCount = 0, bytesRead = 0;
  uint32_t lastDataMs = millis();
  uint8_t buf[512];

  while (http.connected() && (contentLength < 0 || bytesRead < contentLength)) {
    size_t avail = stream->available();
    if (avail) {
      size_t toRead = (avail < sizeof(buf)) ? avail : sizeof(buf);
      int n = stream->readBytes(buf, toRead);
      if (n <= 0) break;

      bytesRead += n;
      lastDataMs = millis();
      window += String((const char*)buf, n);

      String candidate = findLatestPngNameInText(window, newestTs, foundCount);
      if (!candidate.isEmpty()) latest = candidate;

      if (window.length() > 300) {
        window = window.substring(window.length() - 200);
      }
    } else {
      if (millis() - lastDataMs > 10000) break;
      delay(1);
    }
  }

  String candidate = findLatestPngNameInText(window, newestTs, foundCount);
  if (!candidate.isEmpty()) latest = candidate;
  return latest;
}

/**
 * @brief Securely downloads the latest radar image frame from the SHMÚ directory.
 */
bool downloadLatestRadar() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(10000);

  HTTPClient http;
  http.setTimeout(15000);

  if (!http.begin(client, SHMU_API_URL)) return false;
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  String latest = findLatestPngNameFromHttpStream(http);
  http.end();

  if (latest.isEmpty()) return false;
  if (latest == lastPngName && SPIFFS.exists(RADAR_FILE)) return true;

  String url = String(SHMU_BASE_URL) + latest;
  if (!http.begin(client, url)) return false;
  code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  File f = SPIFFS.open(RADAR_FILE, "w");
  if (!f) {
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buf[1024];
  int total = http.getSize();
  int written = 0;

  while (http.connected() && (total < 0 || written < total)) {
    size_t avail = stream->available();
    if (avail) {
      size_t toRead = (avail < sizeof(buf)) ? avail : sizeof(buf);
      int n = stream->readBytes(buf, toRead);
      f.write(buf, n);
      written += n;
    } else {
      delay(1);
    }
  }

  f.close();
  http.end();
  lastPngName = latest;
  return written > 0;
}


// ==========================================
// 7. PNG DECODER & RESAMPLING CALLBACKS
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

/**
 * @brief Memory-optimized Line-by-Line Decoder & Resampler.
 * Stretches source rows to fill target vertical spans without gaps, 
 * using zero large heap allocations.
 */
int drawPngLine(PNGDRAW* pDraw) {
  int srcY = pDraw->y;
  if (srcY < crop.y1 || srcY > crop.y2) return 1;

  png.getLineAsRGB565(pDraw, line565, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);

  float startScreenY = mapYToScreenY((float)srcY);
  float endScreenY = mapYToScreenY((float)srcY + 1.0f);

  int syMin = constrain((int)floorf(startScreenY), 0, TFT_H - 1);
  int syMax = constrain((int)ceilf(endScreenY), syMin, TFT_H - 1);

  for (int dx = 0; dx < TFT_W; dx++) {
    float srcX = (float)crop.x1 + ((float)dx / (float)TFT_W) * (float)crop.w();
    int srcXInt = constrain((int)srcX, 0, RADAR_IMG_W - 1);
    outLine[dx] = line565[srcXInt];
  }

  for (int sy = syMin; sy <= syMax; sy++) {
    tft.pushImage(0, sy, TFT_W, 1, outLine);
  }

  return 1;
}


// ==========================================
// 8. GRAPHIC OVERLAYS & RENDERING
// ==========================================

/**
 * @brief Draws vector national border lines scaled to the current crop window.
 */
void drawSlovakiaBorder() {
  for (size_t i = 0; i < SK_BORDER_COUNT - 1; i++) {
    float mapX1 = (float)lonToX(SK_BORDER[i][0]);
    float mapY1 = (float)latToY(SK_BORDER[i][1]);
    float mapX2 = (float)lonToX(SK_BORDER[i+1][0]);
    float mapY2 = (float)latToY(SK_BORDER[i+1][1]);

    int sx1 = (int)mapXToScreenX(mapX1);
    int sy1 = (int)mapYToScreenY(mapY1);
    int sx2 = (int)mapXToScreenX(mapX2);
    int sy2 = (int)mapYToScreenY(mapY2);

    tft.drawLine(sx1, sy1, sx2, sy2, TFT_CYAN);
  }
}

/**
 * @brief Renders city name labels and red crosshair markers within viewport bounds.
 */
void drawCitiesOverlay() {
  tft.setTextDatum(middle_center);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);

  for (size_t i = 0; i < CITY_COUNT; i++) {
    float mapX = (float)lonToX(CITIES[i].lon);
    float mapY = (float)latToY(CITIES[i].lat);

    int sx = (int)mapXToScreenX(mapX);
    int sy = (int)mapYToScreenY(mapY);

    if (sx >= 10 && sx <= TFT_W - 10 && sy >= 10 && sy <= TFT_H - 10) {
      tft.drawLine(sx - 3, sy, sx + 3, sy, TFT_RED);
      tft.drawLine(sx, sy - 3, sx, sy + 3, TFT_RED);
      tft.drawString(CITIES[i].name, sx, sy - 8);
    }
  }
}

/**
 * @brief Renders HUD rings, center crosshairs, timestamp, and zoom indicator.
 */
void drawOverlay() {
  int cx = TFT_W / 2;
  int cy = TFT_H / 2;

  drawSlovakiaBorder();
  drawCitiesOverlay();

  tft.drawCircle(cx, cy, TFT_W / 2 - 2, TFT_DARKGREY);
  tft.drawCircle(cx, cy, TFT_W / 4, TFT_DARKGREY);
  tft.drawLine(cx - 8, cy, cx + 8, cy, TFT_WHITE);
  tft.drawLine(cx, cy - 8, cx, cy + 8, TFT_WHITE);

  tft.setTextDatum(top_center);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(String((int)currentRadiusKm) + " km", cx, 4);

  tft.setTextDatum(bottom_center);
  tft.drawString(getRadarTimeText(lastPngName), cx, TFT_H - 4);
}

/**
 * @brief Decodes and renders the radar map frame directly onto the display.
 */
bool renderRadar() {
  if (!SPIFFS.exists(RADAR_FILE)) return false;

  crop = makeCrop(centerLat, centerLon, currentRadiusKm);
  tft.fillScreen(TFT_BLACK);

  int rc = png.open(RADAR_FILE, pngOpen, pngClose, pngRead, pngSeek, drawPngLine);
  if (rc != PNG_SUCCESS) {
    showStatus("PNG chyba");
    return false;
  }

  rc = png.decode(nullptr, 0);
  png.close();

  if (rc != PNG_SUCCESS) {
    showStatus("Decode chyba");
    return false;
  }

  drawOverlay();
  return true;
}


// ==========================================
// 9. MAIN ARDUINO ENTRY POINTS
// ==========================================

void setup() {
  Serial.begin(115200);
  delay(500);

  tft.init();
  tft.setRotation(0);
  tft.setBrightness(180);
  tft.setFont(&fonts::Font2);

  pinMode(ZOOM_BUTTON_PIN, INPUT_PULLUP);
  checkResetButtonAtBoot();

  loadPositionFromPrefs();
  initZoomLevel();
  showStatus("ESP MeteoRadar");

  if (!SPIFFS.begin(true)) {
    showStatus("Chyba SPIFFS!");
    delay(3000);
  }

  connectWiFi();
  crop = makeCrop(centerLat, centerLon, currentRadiusKm);

  if (downloadLatestRadar()) {
    renderRadar();
  } else {
    showStatus("Stiahnutie zlyhalo");
  }

  lastUpdate = millis();
}

void loop() {
  // Periodic background check for new radar imagery based on interval defined in config.h
  if (millis() - lastUpdate >= UPDATE_INTERVAL_MS) {
    lastUpdate = millis();
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.reconnect();
      delay(2000);
      if (WiFi.status() != WL_CONNECTED) connectWiFi();
    }
    if (downloadLatestRadar()) renderRadar();
  }
  
  handleZoomButton();
  delay(20);
}