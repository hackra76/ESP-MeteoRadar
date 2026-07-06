#pragma once

/*
merged bin cfg
python3 -m esptool --chip esp32c3 merge_bin -o esp-meteoradar.bin --flash_mode dio --flash_freq 80m --flash_size 4MB 0x0000 .pio/build/esp32-c3-devkitm-1/bootloader.bin 0x8000 .pio/build/esp32-c3-devkitm-1/partitions.bin 0x10000 .pio/build/esp32-c3-devkitm-1/firmware.bin
*/

// ===== Vychozi poloha stredu zobrazeni =====
// Tyto hodnoty se pouziji pri prvnim startu. Potom se daji zmenit ve WiFiManager portalu.
#define DEFAULT_CENTER_LAT "50.0755"
#define DEFAULT_CENTER_LON "14.4378"

// Vychozi okruh zobrazeni po startu. Tlacitkem se prepina 10 / 25 / 50 / 100 km.
#define DEFAULT_RADIUS_KM_TEXT "50"

// Offset casu radaroveho snimku pouze pro zobrazeni na displeji.
// CHMU soubor je typicky v UTC: leto +2, zima +1.
#define DEFAULT_TIME_OFFSET_HOURS 2

// Tlacitko mezi GPIO9 a GND, pouziva se interni pull-up.
// Kratky stisk prepina zoom, dlouhe podrzeni resetuje WiFi/polohu a spusti portal.
#define ZOOM_BUTTON_PIN 9
static constexpr uint32_t RESET_HOLD_MS = 3000;

// Aktualizace radaru
static constexpr uint32_t UPDATE_INTERVAL_MS = 5UL * 30UL * 1000UL; //2,5 min

// ===== CHMU OpenData =====
static constexpr const char* CHMU_INDEX_URL = "https://opendata.chmi.cz/meteorology/weather/radar/composite/maxz/png/";
static constexpr const char* CHMU_BASE_URL  = "https://opendata.chmi.cz/meteorology/weather/radar/composite/maxz/png/";

// PNG radar kompozit ma aktualne rozmer 512 x 512 px.
// Kdyby CHMU format zmenil, uprav podle Serial vypisu PNG rozmeru.
static constexpr int RADAR_IMG_W = 512;
static constexpr int RADAR_IMG_H = 512;

// Kalibrace souradnic PNG. Toto je zjednoduseny linearni prevod pro CR.
// Pro presnejsi mapovani staci doladit okraje podle znamych bodu v obrazku.
static constexpr float LAT_TOP    = 52.7037f;   //ok
static constexpr float LAT_BOTTOM = 47.0937f;   //ok
static constexpr float LON_LEFT   = 10.0669f;   //ok
static constexpr float LON_RIGHT  = 18.85f;     //ok

// ===== Displej =====
static constexpr int TFT_W = 240;
static constexpr int TFT_H = 240;

// Piny pro běžný kulatý GC9A01 240x240 přes SPI.
// Uprav podle své desky/displeje.
#define TFT_MOSI 3
#define TFT_SCLK 4
#define TFT_CS   1
#define TFT_DC   10
#define TFT_RST  0
#define TFT_BL   -1 // -1 = nepouziva se, pokud je pin pod napetim, displej sviti
