# SHMÚ Radar pre ESP32-C3 (Slovensko)[cite: 2]

Kompaktný meteorologický radar postavený na **ESP32-C3** a okrúhlom **240×240 LCD displeji (GC9A01)**[cite: 2]. Zariadenie pravidelne sťahuje najnovšie radarové dáta zo Slovenského hydrometeorologického ústavu (**SHMÚ**)[cite: 2], dynamicky ich oreže a presne resampluje okolo zvolenej GPS polohy. 

Súčasťou zobrazenia je vektorová **mapa štátnej hranice Slovenska**, názvy a značky **väčších miest** s krížikmi, rozsahové kruhy a živá časová pečiatka.

Pôvodný projekt (pre Česko): https://www.petanovo.cz/esp-meteoradar-od-letadel-k-meteo-radarovym-datum/[cite: 2]

3D model krabičky: https://makerworld.com/cs/models/2872376-esp32-plane-radar-live-ads-b-on-a-round-display#profileId-3207083[cite: 2]

<p align="center">
    <img src="data/chmi_radar_1.jpg" width="350">
</p>

---

## 🚀 Hlavné funkcie

- 🌧️ **Živé dáta SHMÚ:** Automatické sťahovanie najnovších CMAX kompozitných radarových snímok z open-data portálu SHMÚ[cite: 2].
- 🗺️ **Vektorová mapa SR & Mesta:** Vykreslenie detailného obrysu štátnej hranice Slovenska a značiek kľúčových miest (Bratislava, Košice, Prešov, Bardejov, Žilina atď.) s dynamickým filtrovaním podľa zoomu.
- 🔍 **Plynulý zoom s resamplingom:** 5 úrovní priblíženia bez deformácií a vizuálnych medzier[cite: 2]:
  - **10 km**[cite: 2]
  - **25 km**[cite: 2]
  - **50 km**[cite: 2]
  - **100 km**[cite: 2]
  - **250 km (Celé Slovensko)**
- 🔄 **Auto-aktualizácia:** Pravidelná obnova snímok v pozadí (konfigurovateľné v `config.h`).
- 📶 **WiFiManager:** Jednoduché prvotné nastavenie WiFi siete a polohy cez webový portál bez nutnosti hardcoded údajov v kóde[cite: 2].
- 💾 **Trvalá pamäť (NVS):** Ukladanie GPS polohy, zvoleného zoomu a časového posunu do `Preferences`[cite: 2].
- 🕒 **Časová zóna:** Automatický prepočet času snímky (UTC na SEČ/SELČ pomocou voliteľného offsetu +1/+2 hodiny)[cite: 2].
- 🔘 **Jedno tlačidlo:** Ovládanie prepínania zoomu a hardvérový factory reset pri štarte[cite: 2].

---

# 🛠️ Použitý hardware

- **ESP32-C3 SuperMini** (príp. ekvivalentná doska s ESP32-C3)[cite: 2]
- Okrúhly TFT displej **GC9A01** (240×240 pixelov, SPI rozhranie)[cite: 2]
- Integrované tlačidlo (priamo na doske SuperMini ako BOOT tlačidlo)[cite: 2]

---

# 🔌 Zapojenie

## Displej (GC9A01)

| Displej | ESP32-C3 Pin | Popis |
| :--- | :--- | :--- |
| **MOSI** | GPIO3 | SPI Data Out |
| **SCLK** | GPIO4 | SPI Clock |
| **CS** | GPIO1 | Chip Select |
| **DC** | GPIO10 | Data / Command |
| **RESET** | GPIO0 | Hardware Reset |
| **BLN / VCC** | 3.3V | Napájanie displeja |
| **GND** | GND | Zostava zeme |

## Tlačidlo

| Tlačidlo | ESP32-C3 Pin |
| :--- | :--- |
| **Jeden kontakt** | GPIO9 (Tlačidlo BOOT) |
| **Druhý kontakt** | GND |

> *Poznámka: V kóde je softvérovo aktivovaný vnútorný `INPUT_PULLUP`, nie sú nutné externé rezistory.*

---

# 🕹️ Ovládanie

- **Krátke stlačenie tlačidla:** Prepína rozsah (radius) radaru v cykle[cite: 2]:
  $$\text{10 km} \rightarrow \text{25 km} \rightarrow \text{50 km} \rightarrow \text{100 km} \rightarrow \text{250 km}$$
- **Dlhé stlačenie (podržanie ≥ 3 sekundy pri zapnutí alebo chode):** Vymaže uložené dáta v pamäti, vyresetuje WiFiManager a restartuje zariadenie do režimu konfiguračného portálu[cite: 2].

---

# ⚙️ Konfigurácia a prvé spustenie

1. Po prvom zapnutí (alebo resete) zariadenie vytvorí otvorenú WiFi sieť s názvom **`ESPMeteoRadar`**[cite: 2].
2. Pripojte sa k tejto sieti mobilom alebo PC. Automaticky vyskočí konfiguračná stránka (príp. prejdite na `192.168.4.1`)[cite: 2].
3. Vyberte svoju WiFi sieť, zadajte heslo a doplňte voliteľné parametre[cite: 2]:
   - **Zemepisná šírka / Latitude** (napr. `49.2918` pre Bardejov, `48.1486` pre Bratislavu)[cite: 2]
   - **Zemepisná dĺžka / Longitude** (napr. `21.2727` pre Bardejov, `17.1077` pre Bratislavu)[cite: 2]
   - **Predvolený rozsah km** (`10`, `25`, `50`, `100` alebo `250`)[cite: 2]
   - **Časový offset** (`+1` pre zimu, `+2` pre letný čas)[cite: 2]
4. Zariadenie sa reštartuje, uloží parametre do flash pamäte a stiahne aktuálne radarové dáta[cite: 2].

---

# 📦 Inštalácia a kompilácia (PlatformIO)

Projekt je pripravený pre vývojové prostredie **VS Code + PlatformIO**.

1. Klonujte alebo stiahnite tento repozitár.
2. Otvorte priečinok projektu v aplikácii **Visual Studio Code**.
3. PlatformIO automaticky detekuje závislosti definované v `platformio.ini`:
   - `LovyanGFX`[cite: 2]
   - `PNGdec`[cite: 2]
   - `WiFiManager`[cite: 2]
   - `Preferences`[cite: 2]
4. Pripojte ESP32-C3 k PC cez USB kábel, kliknite na ikonu **Build** ($\checkmark$) a následne **Upload** ($\rightarrow$) v spodnej lište PlatformIO.

---

# 📚 Zdroj dát a poďakovanie

- **Meteorologické dáta:** Slovenský hydrometeorologický ústav (SHMÚ) – [Open Data SHMÚ](https://opendata.shmu.sk/)[cite: 2]
- **Inšpirácia a pôvodný koncept:** [Petanovo.cz](https://www.petanovo.cz/)[cite: 2]

---

# 📄 Licencia

Tento projekt je šírený pod licenciou **MIT**. Voľne upravené a prispôsobené pre podmienki Slovenskej republiky[cite: 2].