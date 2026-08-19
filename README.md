# 🛰️ ESP32-C3 MeteoRadar & ADS-B Plane Radar (Slovensko)

<p align="center">
  <img src="https://img.shields.io/badge/Platform-ESP32--C3%20SuperMini-blue?style=for-the-badge&logo=espressif" alt="ESP32-C3">
  <img src="https://img.shields.io/badge/Display-GC9A01%20240x240%20SPI-orange?style=for-the-badge" alt="GC9A01">
  <img src="https://img.shields.io/badge/Framework-PlatformIO%20%2F%20Arduino-brightgreen?style=for-the-badge&logo=platformio" alt="PlatformIO">
  <img src="https://img.shields.io/badge/License-MIT-green?style=for-the-badge" alt="MIT License">
  <img src="https://img.shields.io/badge/Region-Slovakia%20%28SHM%C3%9A%29-red?style=for-the-badge" alt="Region Slovakia">
</p>

<p align="center">
  <b>Kompaktný stolný radar pre okrúhly 1.28″ LCD displej spájajúci zrážkový meteoradar SHMÚ a živé sledovanie lietadiel (ADS-B).</b>
</p>

<p align="center">
  <img src="data/shmu_radar_1.jpg" width="380" alt="ESP-MeteoRadar v prevádzke" style="border-radius: 50%; box-shadow: 0 4px 20px rgba(0,0,0,0.5);">
</p>

---

## 📖 Obsah
- [Prehľad projektu](#-prehľad-projektu)
- [Hlavné funkcie](#-hlavné-funkcie)
- [Použitý hardvér](#-použitý-hardvér)
- [Schéma zapojenia](#-schéma-zapojenia)
- [Ovládanie a funkcie tlačidla](#-ovládanie-a-funkcie-tlačidla)
- [Prvé spustenie a konfigurácia (WiFiManager)](#-prvé-spustenie-a-konfigurácia-wifimanager)
- [Ako nahrať firmvér](#-ako-nahrať-firmvér)
  - [Metóda 1: Rýchly flash cez webový prehliadač (bez inštalácie)](#metóda-1-rýchly-flash-cez-webový-prehliadač-odporúčané)
  - [Metóda 2: Kompilácia cez PlatformIO](#metóda-2-kompilácia-cez-platformio)
- [Časté otázky a riešenie problémov (Troubleshooting)](#-časté-otázky-a-riešenie-problémov)
- [Pôvodné projekty, inšpirácia a poďakovanie](#-pôvodné-projekty-inšpirácia-a-poďakovanie)
- [Licencia](#-licencia)

---

## 🌟 Prehľad projektu

Tento DIY projekt transformuje miniatúrnu vývojovú dosku **ESP32-C3 SuperMini** a okrúhly **1.28-palcový TFT displej GC9A01 (240×240 px)** na plnohodnotný stolný radarový prístroj. 

Zariadenie v nastaviteľnom časovom intervale (Karusel) automaticky strieda:
1. **Zrážkový meteoradar:** Sťahuje a resampluje oficiálne radarové kompozity zrážok zo **Slovenského hydrometeorologického ústavu (SHMÚ)** na vektorovej mape SR.
2. **ADS-B Letecký radar:** V reálnom čase monitoruje leteckú prevádzku v okolí vašej polohy cez **ADS-B feed** vrátane vyhľadávania letových trás (**ODKIAĽ > KAM** napr. `VIE>AMS`, `BGY>WAW`).

---
[![ESP32-C3 MeteoRadar & ADSB Plane Radar Demo](https://img.youtube.com/vi/1NHL9VXtsXE/0.jpg)](https://www.youtube.com/shorts/1NHL9VXtsXE)


## 🚀 Hlavné funkcie

* 🔄 **Automatický Karusel:** Plynulé striedanie režimov počasia a lietadiel v nastaviteľnom intervale (napr. každých 30 sekúnd).
* 🌧️ **SHMÚ Meteoradar (Slovensko):**
  * Sťahovanie najnovšieho zrážkového PNG kompozitu (`cmax.kruh`) zo serverov SHMÚ.
  * Zobrazenie času zosnímania radaru s automatickým offsetom časového pásma.
  * Zameriavací kríž a diaľkové kružnice.
* ✈️ **Pokročilý Letecký radar (ADS-B):**
  * Živé sledovanie lietadiel v reálnom čase z otvoreného feedu `adsb.fi`.
  * **Automatické zisťovanie letových trás:** Prepojenie volacieho znaku s databázou letových plánov (VRS standing-data) zobrazuje trasu letu (`VIE>AMS`).
  * **Kruhová vyrovnávacia pamäť (Route Cache):** Ukladá trasy do RAM pre okamžité vykreslenie a minimálny dátový prenos.
  * **Čitateľné 3-riadkové štítky:**
    * *Riadok 1:* Trasa letísk (fialová) alebo Callsign (biela / červená pre vojenské).
    * *Riadok 2:* Typ lietadla ICAO (svetlomodrá, napr. `A21N`) a rýchlosť v km/h (svetlozelená).
    * *Riadok 3:* Nadmorská výška v metroch (žltá) a farebná šípka stúpania / klesania.
  * **Edge Dots:** Obvodové body na okraji displeja indikujúce lietadlá nachádzajúce sa tesne za hranicou aktuálneho priblíženia.
  * **Vojenské lety:** Automatické zvýraznenie vojenských transpondérov červenou farbou.
* 🗺️ **Vektorová mapa SR a mestá:** Detailný polygón hranice Slovenskej republiky a krajské/okresné mestá (BA, TT, NR, TN, ZA, BB, PO, KE, BJ, PP, MI, LC) s dynamickým filtrovaním podľa zoomu.
* 🔍 **Prepínanie mierky jedným tlačidlom:** 5 úrovní priblíženia (**10 km**, **25 km**, **50 km**, **100 km** a **250 km**).
* ⚙️ **Webový konfiguračný portál (WiFiManager):** Pohodlné nastavenie vlastných GPS súradníc, predvoleného zoomu a intervalu karuselu priamo z mobilu alebo PC bez nutnosti úpravy zdrojového kódu.

---

## 🛠️ Použitý hardvér

| Komponent | Popis | Odporúčanie |
| :--- | :--- | :--- |
| **ESP32-C3 SuperMini** | Riadiaci mikrokontrolér (RISC-V 160MHz, WiFi, BLE, USB-C) | Kompaktný rozmer, nízka spotreba |
| **GC9A01 1.28″ Round LCD** | Okrúhly IPS displej 240×240 px, SPI zbernica | Krásne pozorovacie uhly a živé farby |
| **Tlačidlo (BOOT / Externé)** | Tlačidlo pripojené medzi GPIO9 a GND | Prepínanie zoomu a reset |
| **3D Krabička** | Krabička prispôsobená pre okrúhly radar | [Dostupná na MakerWorld](https://makerworld.com/cs/models/2872376-esp32-plane-radar-live-ads-b-on-a-round-display) |

---

## 🔌 Schéma zapojenia

Pripojenie displeja **GC9A01** k doske **ESP32-C3 SuperMini**:

```
           ESP32-C3 SuperMini                GC9A01 LCD (240x240)
         +--------------------+             +--------------------+
         |               3.3V |------------>| VCC                |
         |                GND |------------>| GND                |
         |              GPIO4 |------------>| SCL / SCLK (Clock) |
         |              GPIO3 |------------>| SDA / MOSI (Data)  |
         |              GPIO0 |------------>| RES / RST (Reset)  |
         |             GPIO10 |------------>| DC (Data/Command)  |
         |              GPIO1 |------------>| CS (Chip Select)   |
         |               3.3V |------------>| BLK / BL (Podsv.)  |
         +--------------------+             +--------------------+
```

### Zapojenie tlačidla:
* **Tlačidlo BOOT na doske:** Už je od výroby zapojené medzi **GPIO9** a **GND**.
* **Externé tlačidlo (voliteľné):** Zapojte medzi pin **GPIO9** a **GND**. V softvéri je zapnutý interný `INPUT_PULLUP`, externý rezistor nie je potrebný.

---

## 🕹️ Ovládanie a funkcie tlačidla

Zariadenie využíva jedno univerzálne tlačidlo na **GPIO9** obsluhované hardvérovým prerušením (ISR):

* 🔘 **Krátke stlačenie:** Cyklické prepínanie mierky zobrazenia:
  $$\text{10 km} \longrightarrow \text{25 km} \longrightarrow \text{50 km} \longrightarrow \text{100 km} \longrightarrow \text{250 km}$$
  *(Zvolená mierka sa automaticky uloží do NVS pamäte a zostane zachovaná aj po reštarte).*
* 🔴 **Dlhé podržanie (≥ 3 sekundy):** Vykoná kompletný reset nastavení WiFi, vymaže NVS pamäť a reštartuje dosku do režimu konfiguračného portálu.

---

## ⚙️ Prvé spustenie a konfigurácia (WiFiManager)

1. Po prvom zapnutí zariadenie vytvorí vlastný Wi-Fi prístupový bod s názvom **`ESPMeteoRadar`** (bez hesla).
2. Pripojte sa k sieti z mobilu alebo počítača. Automaticky sa otvorí konfiguračná stránka (ak nie, otvorte prehliadač a zadajte adresu `http://192.168.4.1`).
3. Zvoľte **Configure WiFi** a vyplňte:
   * **Názov a heslo domácej Wi-Fi siete**
   * **Zemepisná šírka (Lat):** Súradnice vášho domova/miesta (napr. `48.1486` pre Bratislavu, `48.7363` pre Banskú Bystricu, `48.7164` pre Košice).
   * **Zemepisná dĺžka (Lon):** (napr. `17.1077` pre Bratislavu, `19.1462` pre Banskú Bystricu, `21.2611` pre Košice).
   * **Predvolený rozsah (km):** Štartovacia mierka radaru (napr. `50`).
   * **Časový offset (hodiny):** `2` pre letný čas (CEST), `1` pre zimný čas (CET).
   * **Interval karuselu (sekundy):** Ako často sa má striedať počasie a lietadlá (napr. `30`).
4. Kliknite na **Save**. Doska sa pripojí k vašej Wi-Fi a okamžite načíta živé dáta.

---

## 📦 Ako nahrať firmvér

### Metóda 1: Rýchly flash cez webový prehliadač (Odporúčané)
Nemusíte inštalovať žiadne programovacie prostredie, ovládače ani knižnice.

1. Otvorte stránku **[web.esphome.io](https://web.esphome.io/)** v prehliadači Google Chrome, MS Edge alebo Opera.
2. Pripojte dosku ESP32-C3 cez USB kábel k počítaču.
3. Kliknite na **CONNECT** a v zozname zvoľte sériový port vášho ESP32 (napr. `COMx` na Windows, `/dev/ttyUSBx` alebo `/dev/ttyACMx` na Mac/Linux).
   > *Tip: Ak prehliadač dosku nevidí, podržte na doske tlačidlo **BOOT**, stlačte tlačidlo **RST** a pustite BOOT.*
4. Zvoľte **Install** $\rightarrow$ vyberte súbor [`merged-firmware.bin`](merged-firmware.bin) z tohto repozitára.
5. Počkajte na dokončenie nahrávania na 100 % a stlačte tlačidlo **RST** na doske.

---

### Metóda 2: Kompilácia cez PlatformIO

1. Nainštalujte **[Visual Studio Code](https://code.visualstudio.com/)** a rozšírenie **PlatformIO IDE**.
2. Naklonujte tento repozitár:
   ```bash
   git clone https://github.com/hackra76/ESP-MeteoRadar.git
   ```
3. Otvorte priečinok v prostredí VS Code.
4. PlatformIO automaticky stiahne potrebné knižnice (`LovyanGFX`, `WiFiManager`, `ArduinoJson`, `PNGdec`).
5. Pripojte ESP32-C3 a kliknite na tlačidlo **Upload** (šípka $\rightarrow$) v spodnej lište.

Skript `merge_bin.py` po každej kompilácii automaticky vygeneruje aj pripravený spojený súbor `merged-firmware.bin`.

---

## ❓ Časté otázky a riešenie problémov

> [!TIP]
> **Doska sa po uploade nespustí automaticky:**
> ESP32-C3 SuperMini využíva natívny interný USB radič bez prídavných DTR/RTS tranzistorov. Po úspešnom nahraní firmvéru stačí **krátko stlačiť hardvérové tlačidlo RST** na doske (alebo odpojiť a znova pripojiť USB kábel).

> [!NOTE]
> **Ako znova vyvolať konfiguračnú stránku WiFi?**
> Podržte tlačidlo na GPIO9 aspoň na 3 sekundy. Na displeji sa zobrazí hlásenie *Reset nastavenia...* a zariadenie opäť vytvorí prístupový bod `ESPMeteoRadar`.

> [!IMPORTANT]
> **Prečo sa pri niektorých lietadlách zobrazuje kód (napr. WZZ488R) a pri iných trasa (napr. VIE>AMS)?**
> Bežné komerčné lety s letovým plánom v medzinárodnej databáze sa automaticky preložia na kód letísk. Menšie súkromné alebo vojenské lety letový plán nemajú, preto sa pri nich zobrazuje ich oficiálny volací znak (Callsign).

---

## 🤝 Pôvodné projekty, inšpirácia a poďakovanie

Tento projekt stavia na myšlienkach a práci open-source komunity:

* 🌦️ **[ESP-MeteoRadar (Petanovo.cz)](https://www.petanovo.cz/esp-meteoradar-od-letadel-k-meteo-radarovym-datum/):** Pôvodný koncept meteo radaru pre ČR a inšpirácia pre hardvérové prevedenie.
* ✈️ **[ESP32-Plane-Radar (MatixYo)](https://github.com/MatixYo/ESP32-Plane-Radar):** Základný projekt leteckého radaru pre okrúhly displej GC9A01.
* 📏 **[ESP32-Plane-Radar-metrers (synex-c21)](https://github.com/synex-c21/ESP32-Plane-Radar-metrers):** Implementácia metrických jednotiek a integrácia vyhľadávania letových trás cez VRS standing-data.
* 🛰️ **[OpenData SHMÚ](https://opendata.shmu.sk/):** Poskytovateľ otvorených meteorologických radarových snímok pre územie Slovenska.
* 📡 **[ADSB.fi](https://opendata.adsb.fi/):** Komunitný otvorený feed ADS-B leteckých dát.
* 🗺️ **[vrs-standing-data (adsb.lol)](https://vrs-standing-data.adsb.lol/):** Statická databáza letových trás pre Virtual Radar Server.
* 🖨️ **[3D Case Model (MakerWorld)](https://makerworld.com/cs/models/2872376-esp32-plane-radar-live-ads-b-on-a-round-display):** Skvelý 3D model krabičky.

---

## 📄 Licencia

Tento projekt je zverejnený pod slobodnou licenciou **[MIT License](LICENSE)**. Kód môžete voľne používať, upravovať a šíriť.
