# SHMÚ Radar pre ESP32-C3 (Slovensko)

Jednoduchý meteorologický radar postavený na **ESP32-C3** a okrúhlom **240×240 LCD displeji (GC9A01)**. Nevyžaduje žiadne ďalšie súčiastky.
Zariadenie pravidelne sťahuje najnovší radarový snímok z otvorených dát **Slovenského hydrometeorologického ústavu (SHMÚ)** a zobrazuje okolie zadanej polohy v niekoľkých úrovniach priblíženia.

Pôvodný projekt (pre Česko): https://www.petanovo.cz/esp-meteoradar-od-letadel-k-meteo-radarovym-datum/

3D model: https://makerworld.com/cs/models/2872376-esp32-plane-radar-live-ads-b-on-a-round-display#profileId-3207083

<p align="center">
    <img src="data/chmi_radar_1.jpg" width="350">
</p>

---

## Funkcie

- 🌧️ Sťahovanie najnovších radarových snímok SHMÚ (CMAX kompozit)
- 📡 Automatická aktualizácia každých 2,5 minúty
- 📍 Nastavenie vlastnej polohy (zemepisná šírka a dĺžka na Slovensku)
- 🔍 Prepínanie zoomu:
  - 10 km
  - 25 km
  - 50 km
  - 100 km
- 📶 Konfigurácia WiFi pomocou **WiFiManageru**
- 💾 Uloženie nastavení do internej pamäte ESP32 (Preferences)
- 🕒 Nastaviteľný časový posun zobrazeného času radarového snímku (+1 hodina v zime, +2 hodiny v lete)
- 🔘 Ovládanie jedným tlačidlom
- 🖥️ Podpora okrúhleho displeja GC9A01 (240×240)

---

# Použitý hardware

- ESP32-C3
- Okrúhly TFT displej GC9A01 (240×240)
- Jedno tlačidlo (súčasťou ESP32-C3 Super Mini)

---

# Zapojenie

## Displej

| Displej | ESP32-C3 |
|----------|-----------|
| MOSI | GPIO3 |
| SCLK | GPIO4 |
| CS | GPIO1 |
| DC | GPIO10 |
| RESET | GPIO0 |

## Tlačidlo

| Tlačidlo | ESP32-C3 |
|-----------|-----------|
| Jeden kontakt | GPIO9 |
| Druhý kontakt | GND |

V programe je použitý interný **pull-up** rezistor. Toto tlačidlo je integrované priamo na doske ESP32-C3 Super Mini ako tlačidlo BOOT.

---

# Ovládanie

## Krátke stlačenie tlačidla

Prepína rozsah radaru:

```
10 km → 25 km → 50 km → 100 km
```

## Dlhé stlačenie (cca 3 sekundy)

Vymaže uložené nastavenia a znova spustí WiFiManager.

---

# Konfigurácia

Pri prvom spustení (alebo po dlhom podržaní tlačidla) sa vytvorí WiFi sieť:

```
ESP-MeteoRadar
```

Po pripojení je možné nastaviť:

- WiFi sieť
- heslo
- zemepisnú šírku (napr. 48.1486 pre Bratislavu, 48.6690 pre Banskú Bystricu)
- zemepisnú dĺžku (napr. 17.1077 pre Bratislavu, 19.6990 pre Banskú Bystricu)
- predvolený zoom
- časový posun zobrazeného času (+1 hodina v zime, +2 hodiny v lete)

Všetky nastavenia sú uložené v internej pamäti ESP32.

---

# Zdroj radarových dát

Projekt využíva otvorené dáta Slovenského hydrometeorologického ústavu (SHMÚ):

https://www.shmu.sk/sk/?page=1&id=meteo_radar
https://opendata.shmu.sk/

---

# Použité knižnice

- LovyanGFX
- PNGdec
- WiFiManager
- Preferences

---

# Možné budúce rozšírenia

- mapa Slovenskej republiky ako podklad
- zobrazenie svetových strán
- názvy väčších miest
- legenda intenzity zrážok
- OTA aktualizácia firmware
- animácia posledných radarových snímok

---

# Licencia

Projekt je uvoľnený pod licenciou **MIT**.
