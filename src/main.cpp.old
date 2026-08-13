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

LGFX tft;
PNG png;
File pngFile;

static const char* RADAR_FILE = "/radar.png";

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

float centerLat = atof(DEFAULT_CENTER_LAT);
float centerLon = atof(DEFAULT_CENTER_LON);
int timeOffsetHours = DEFAULT_TIME_OFFSET_HOURS;

static const float ZOOM_LEVELS_KM[] = {10.0f, 25.0f, 50.0f, 100.0f};
static constexpr int ZOOM_LEVEL_COUNT = sizeof(ZOOM_LEVELS_KM) / sizeof(ZOOM_LEVELS_KM[0]);
int zoomIndex = 1;
float currentRadiusKm = atof(DEFAULT_RADIUS_KM_TEXT);

bool lastButtonReading = HIGH;
bool stableButtonState = HIGH;
uint32_t lastButtonChangeMs = 0;
uint32_t buttonPressStartMs = 0;
bool longPressHandled = false;
static constexpr uint32_t BUTTON_DEBOUNCE_MS = 50;

int lonToX(float lon) {
  return roundf((lon - LON_LEFT) * (RADAR_IMG_W - 1) / (LON_RIGHT - LON_LEFT));
}

int latToY(float lat) {
  return roundf((LAT_TOP - lat) * (RADAR_IMG_H - 1) / (LAT_TOP - LAT_BOTTOM));
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

/*
void showStatus(const String& text) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(middle_center);
  tft.drawString(text, TFT_W / 2, TFT_H / 2);
}
*/

void showStatus(const String& text){
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  int y = 80;
  int start = 0;

  while (true){
    int pos = text.indexOf('\n', start);

    String line;
    if (pos == -1)
      line = text.substring(start);
    else
      line = text.substring(start, pos);

    tft.setTextDatum(middle_center);
    tft.drawString(line, TFT_W / 2, y);
    y += 20;

    if (pos == -1)
      break;

    start = pos + 1;
  }
}


bool renderRadar();

String getRadarTimeText(const String& filename) {
  // Názov má tvar napr. cmax.kruh.20260811.1010.0.png
  // Čas v názve nechávame pre výber najnovšieho súboru bez zmeny,
  // offset sa použije iba pre zobrazenie na displeji.
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


void resetSettingsAndRestart() {
  Serial.println();
  Serial.println("================================");
  Serial.println("Dlhe podrzanie tlacidla - mazem WiFiManager a ulozenu konfiguraciu");
  Serial.println("================================");

  showStatus("Reset nastavenia...");

  WiFiManager wm;
  wm.resetSettings();        // zmaze ulozene WiFi udaje z NVS

  prefs.begin("radar", false);
  prefs.clear();             // zmaze ulozenu polohu a predvoleny zoom
  prefs.end();

  delay(1000);
  ESP.restart();
}

void checkResetButtonAtBoot() {
  if (digitalRead(ZOOM_BUTTON_PIN) != LOW) return;

  Serial.println("Tlacidlo drzane pri starte - cakam na dlhe podrzanie...");
  showStatus("Drz pre reset");

  uint32_t start = millis();
  while (digitalRead(ZOOM_BUTTON_PIN) == LOW) {
    if (millis() - start >= RESET_HOLD_MS) {
      resetSettingsAndRestart();
    }
    delay(20);
  }

  Serial.println("Tlacidlo pustene - reset sa nevykona");
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
      // Krátke stlačenie = prepnutie zoomu. Ak už prebehol dlhý stisk, zoom sa neprepína.
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

  Serial.printf("Poloha: %.6f, %.6f\n", centerLat, centerLon);
  Serial.printf("Predvolený zoom: %.0f km\n", currentRadiusKm);
  Serial.printf("Časový offset zobrazenia: %+d h\n", timeOffsetHours);
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  delay(100);

  showStatus("ESP MeteoRadar v1.0\nSHMU Slovensko\nWiFi portal...");
  delay(500);

  char latBuf[16];
  char lonBuf[16];
  char radiusBuf[8];
  char offsetBuf[8];
  snprintf(latBuf, sizeof(latBuf), "%.6f", centerLat);
  snprintf(lonBuf, sizeof(lonBuf), "%.6f", centerLon);
  snprintf(radiusBuf, sizeof(radiusBuf), "%.0f", currentRadiusKm);
  snprintf(offsetBuf, sizeof(offsetBuf), "%d", timeOffsetHours);

  WiFiManager wm;
  wm.setConfigPortalTimeout(300);
  wm.setConnectTimeout(15);
  wm.setBreakAfterConfig(true);

  // Ked sa vytvori WiFi AP, zobrazi sa to priamo na displeji
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

  Serial.println("Spúšťam WiFiManager...");
  bool ok = wm.autoConnect("ESPMeteoRadar");

  if (!ok || WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFiManager: nepripojené");
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

  Serial.print("WiFi OK, IP: ");
  Serial.println(WiFi.localIP());
  Serial.printf("Nastavená poloha: %.6f, %.6f, zoom %.0f km, offset %+d h\n", centerLat, centerLon, currentRadiusKm, timeOffsetHours);
}

String extractRadarTimestamp(const String& filename) {
  // Názov má tvar: cmax.kruh.20260811.1010.0.png
  const String prefix = "cmax.kruh.";
  int start = filename.indexOf(prefix);
  if (start < 0) return "";

  int dateStart = start + prefix.length();
  if (filename.length() < dateStart + 13) return "";

  String date = filename.substring(dateStart, dateStart + 8);
  String hhmm = filename.substring(dateStart + 9, dateStart + 13);

  if (filename[dateStart + 8] != '.') return "";
  for (int i = 0; i < date.length(); i++) if (!isDigit(date[i])) return "";
  for (int i = 0; i < hhmm.length(); i++) if (!isDigit(hhmm[i])) return "";

  return date + hhmm; // napr. 202608111010 - dobre sa porovnáva ako text
}

String findLatestPngNameInText(const String& text, String& newestTs, int& foundCount) {
  const String prefix = "cmax.kruh.";
  String latest;
  int pos = 0;

  while (true) {
    int idx = text.indexOf(prefix, pos);
    if (idx < 0) break;

    int end = text.indexOf(".png", idx);
    if (end < 0) break; // kandidát nie je v tomto kusu textu kompletný

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

  String window;
  String latest;
  String newestTs;
  int foundCount = 0;
  int bytesRead = 0;
  uint32_t lastDataMs = millis();
  uint8_t buf[512];

  Serial.println("Prechádzam API stream a hľadám najnovší timestamp v názve PNG...");

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

      // Necháme presah, kedy by bol na hranici chunku rozdelený názov súboru.
      if (window.length() > 300) {
        window = window.substring(window.length() - 200);
      }
    } else {
      if (millis() - lastDataMs > 10000) {
        Serial.println("Timeout pri čítaní API");
        break;
      }
      delay(1);
    }
  }

  // Doprohľadávanie zvyšku okna.
  String candidate = findLatestPngNameInText(window, newestTs, foundCount);
  if (!candidate.isEmpty()) latest = candidate;

  Serial.printf("API prečítané: %d B, nájdených kandidátov: %d\n", bytesRead, foundCount);
  if (!latest.isEmpty()) {
    Serial.print("Vybraný timestamp: ");
    Serial.println(newestTs);
  }
  return latest;
}

bool downloadLatestRadar() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(15000);
  Serial.println("Sťahujem radardata API SHMÚ...");

  if (!http.begin(client, SHMU_API_URL)) return false;
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("API HTTP chyba: %d\n", code);
    http.end();
    return false;
  }

  String latest = findLatestPngNameFromHttpStream(http);
  http.end();

  if (latest.isEmpty()) {
    Serial.println("Nenádený žiaden PNG súbor v SHMÚ API");
    return false;
  }

  Serial.println();
  Serial.println("================================");
  Serial.print("Vybraný najnovší PNG: ");
  Serial.println(latest);
  Serial.print("Čas snímky: ");
  Serial.println(getRadarTimeText(latest));
  Serial.println("================================");

  if (latest == lastPngName && SPIFFS.exists(RADAR_FILE)) {
    Serial.println("Súbor už je aktuálny");
    return true;
  }

  String url = String(SHMU_BASE_URL) + latest;
  Serial.print("Sťahujem: ");
  Serial.println(url);

  if (!http.begin(client, url)) return false;
  code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("PNG HTTP chyba: %d\n", code);
    http.end();
    return false;
  }

  File f = SPIFFS.open(RADAR_FILE, "w");
  if (!f) {
    Serial.println("Nemožno otvoriť súbor pre zápis");
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

  Serial.print("Stiahnutý súbor: ");
  Serial.println(latest);
  Serial.printf("Uložené %d B\n", written);
  Serial.print("Zobrazovaný čas snímky: ");
  Serial.println(getRadarTimeText(latest));
  return written > 0;
}

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

  int dstY = (int)((int64_t)(srcY - crop.y1) * TFT_H / crop.h());
  if (dstY < 0 || dstY >= TFT_H) return 1;

  png.getLineAsRGB565(pDraw, line565, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);

  for (int dx = 0; dx < TFT_W; dx++) {
    int srcX = crop.x1 + (int)((int64_t)dx * crop.w() / TFT_W);
    srcX = constrain(srcX, 0, RADAR_IMG_W - 1);
    outLine[dx] = line565[srcX];
  }

  tft.pushImage(0, dstY, TFT_W, 1, outLine);
  return 1;
}

void drawOverlay() {
  int cx = TFT_W / 2;
  int cy = TFT_H / 2;

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

bool renderRadar() {
  if (!SPIFFS.exists(RADAR_FILE)) return false;

  crop = makeCrop(centerLat, centerLon, currentRadiusKm);
  Serial.printf("Crop: x %d..%d, y %d..%d\n", crop.x1, crop.x2, crop.y1, crop.y2);

  tft.fillScreen(TFT_BLACK);

  int rc = png.open(RADAR_FILE, pngOpen, pngClose, pngRead, pngSeek, drawPngLine);
  if (rc != PNG_SUCCESS) {
    Serial.printf("PNG open chyba: %d\n", rc);
    showStatus("PNG chyba");
    return false;
  }

  Serial.printf("PNG: %d x %d\n", png.getWidth(), png.getHeight());
  rc = png.decode(nullptr, 0);
  png.close();

  if (rc != PNG_SUCCESS) {
    Serial.printf("PNG decode chyba: %d\n", rc);
    showStatus("Decode chyba");
    return false;
  }

  drawOverlay();
  return true;
}

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
    // Pokracujeme aj bez SPIFFS, portal bude fungovat
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
