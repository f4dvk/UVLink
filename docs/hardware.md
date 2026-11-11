# Matériel — ESP32-S3 mini sur la C-Board KD8CEC

> **BROUILLON — à vérifier sur la carte réelle** (sérigraphie de la Heemol
> "ESP32-S3 Mini", amazon.fr B0G2XKK6P6) avant de souder.

UVLink reprend le câblage de la C-Board tel que modifié par le projet
[Sarsat_UV-K1-5_RP2040](https://github.com/f4dvk/Sarsat_UV-K1-5_RP2040)
(`docs/hardware.md`), en remplaçant le RP2040-Zero par un ESP32-S3 mini, et
en **ajoutant une sortie audio PWM (GPIO7) vers le micro du poste** pour
l'émission (phonie et APRS/AFSK depuis l'appli).

## Correspondance des signaux

| Signal                              | C-Board (RP2040-Zero)  | ESP32-S3 mini | Côté poste                       |
|-------------------------------------|------------------------|---------------|----------------------------------|
| Série ESP → poste                   | GP0 (UART0 TX)         | **GPIO43** (sérigraphie TX) | RX série, jack 2,5 mm |
| Série poste → ESP                   | GP1 (UART0 RX)         | **GPIO44** (sérigraphie RX) | TX série, jack 3,5 mm (bague) |
| GPS NMEA (TX du module → ESP)       | GP5 (UART1 RX)         | **GPIO10**    | —                                |
| Audio HP du poste → ESP (ADC)       | GP26 + GP27 + GP28 (les 3 pads reliés) | **GPIO3 + 4 + 5** ; le firmware lit **GPIO4** (ADC1_CH3), 3 et 5 restent en entrée haute impédance | sortie HP / casque, jack 3,5 mm |
| Audio ESP → micro du poste (PWM)    | — (PWM sur la C-Board d'origine, retiré) | **GPIO7** (LEDC) | entrée micro, jack 2,5 mm |
| LED d'état                          | —                      | **GPIO21** (WS2812 ; mesuré sur la Heemol, pas GPIO48 ; luminosité 0..64 par la console `e N`, 0 = éteinte) | —    |
| Alimentation                        | pad 3V3 / GND          | pad 3V3 / GND | 3,3 V du poste (mod. résistances internes, voir projet Sarsat) |

PTT, squelch, S-mètre, fréquences : **par le port série** (mode hôte du
firmware `benshi-esp32-sim/firmware/uv-k1-k5v3`), aucun fil supplémentaire.

Le nœud ADC garde l'étage de la C-Board modifiée : `C1 = 1 µF` en série,
pont `2 × 22 kΩ` (3V3 / GND) pour polariser à ~1,65 V, deux `1N4148` en
écrêtage. L'ADC de l'ESP32-S3 (12 bits, atténuation 11 dB ≈ 0–3,1 V) se
comporte comme celui du RP2040 pour ce montage.

### Sortie PWM GPIO7 → micro

```
  GPIO7 ──[ R 4,7 kΩ ]──┬──[ C 10 nF ]── GND        (passe-bas ~3,4 kHz)
                        │
                       [ R 100 kΩ ]──┤├── entrée micro du poste (jack 2,5 mm)
                                   C 1 µF
```

LEDC 10 bits à 39 kHz, mis à jour à 16 kHz. Le pont 4,7 k / 100 k ramène les
3,3 V crête-à-crête au niveau micro (quelques dizaines de mV) ; ajuster
100 kΩ selon la déviation obtenue.

## Points à vérifier sur la carte

- **Broches de strapping ESP32-S3 : GPIO0, GPIO3, GPIO45, GPIO46** — ne pas
  les utiliser comme entrées tirées au repos. Les GPIO choisis ci-dessus
  (1, 5, 7, 43, 44) n'en font pas partie.
- GPIO43/44 sont l'UART0 d'usine : le firmware passe la console sur l'USB-C
  natif (`ARDUINO_USB_CDC_ON_BOOT=1`), ils sont donc libres pour le poste.
- La sérigraphie varie d'un fabricant à l'autre ("S3 Super Mini" n'est pas
  un standard Espressif) : vérifier TX/RX = 43/44 et la LED = 48.
- Flash / PSRAM : `platformio.ini` suppose 4 Mo / 2 Mo (ESP32-S3FH4R2).
  `esptool.py flash_id` pour confirmer.
- Le module GPS doit être en 3,3 V (GPIO10 non tolérant 5 V).
- **Pont de polarisation** : sur la carte réelle (mesure du 2026-10-27), le
  22 kΩ « haut » du pont n'est pas relié au 3,3 V de l'ESP (il allait au pad
  3V3 du RP2040-Zero) : le nœud reste à ~90 mV. Le firmware active le
  pull-up interne de GPIO4 (`AUDIO_ADC_PULLUP`, repos 1,16 V) en attendant ;
  relier ce 22 kΩ au pad 3V3 de l'ESP rend le montage conforme (repos ≈ 2 V
  avec le pull-up, 1,65 V sans).
- Ne jamais configurer un UART ou un pull sur GPIO3/GPIO5 : ils sont sur le
  nœud audio. Diagnostic du nœud par la console : `U 1` (pull-up interne,
  dc ≈ 2500 attendu avec le pont 22k/22k), `R [N] [Hz]` (capture brute,
  cadence temporaire au choix : `R 8192 80000` montre le spectre jusqu'à
  40 kHz), `P 0` (PWM détaché). Broche en l'air = dc ≈ 0 sans pull-up,
  ≈ 3900 avec.
- **Pas de filtre anti-repliement** sur le nœud : l'ADC phonie tourne à
  32 kHz et le firmware moyenne 4 échantillons (zéros à 8, 16, 24 kHz), ce qui
  neutralise un résidu d'horloge audio du poste qui, échantillonné à 8 kHz
  direct, se repliait en battement de quelques hertz (voir README,
  2026-10-28). Un RC 10 kΩ / 4,7 nF (~3,4 kHz) en série avant C1 ferait le
  reste si une raie HF restait visible dans `R`.

## Consommation

ESP32-S3 avec BLE actif : ~60–90 mA en moyenne (pics à ~300 mA en
publicité/TX radio). C'est plus que le RP2040 ; la modification du rail 3,3 V
du poste décrite dans le projet Sarsat (résistance série pontée) est
**indispensable**, et un condensateur de 100 µF au plus près du pad 3V3 est
recommandé.

**Mesuré le 2026-10-28** : alimenté par le rail 3,3 V du poste, l'audio reçu
(signal modulé) est découpé en synchronisme avec les envois BLE : dans chaque
paquet de 40 ms, ~16 ms d'audio puis ~24 ms atténués d'un facteur 4, soit un
battement audible (période composite 120 ms = 8 intervalles de connexion de
15 ms = 3 paquets). Alimenté par l'USB du PC, le même flux est parfaitement
plat ; sur le souffle seul (pas de porteuse) il est plat aussi : ce n'est pas
l'ADC mais le **récepteur du poste** (BK4819 sur le même rail 3,3 V) qui
décroche à chaque pic de courant de l'ESP. Parades, par ordre :

1. **Condensateur 470 µF** (10 V, électrolytique ordinaire) entre les pads
   3V3 et GND de l'ESP, au plus court. **Vérifié le 2026-10-28** : rapport de
   niveau début/fin de paquet (médiane sur ~200 paquets de parole) 4,6 sans,
   **1,05 avec** (référence USB 0,99). C'est la parade retenue.
2. Firmware : BLE à 0 dBm et CPU à 160 MHz ont été essayés avant le
   condensateur : **aucun effet mesurable** (4,4). `BLE_TX_POWER_DBM` reste
   donc à +9 pour la portée ; `CPU_FREQ_MHZ` 160 est conservé (consommation).
3. Si un résidu réapparaissait (batterie faible, rail plus mou) :
   **alimentation séparée**, petit abaisseur (MP1584 ou équivalent) de la
   batterie 7,4 V du poste vers le pad **5V** de l'ESP (son LDO interne fait
   le 3,3 V ; jamais 7,4 V sur ce pad, LDO limité à ~6 V).

### Atténuateur d'entrée pour travailler potentiomètre à fond (2026-10-30)

En audio plat (sondes, décodeurs), le gain REG_48 du poste n'agit pas : seul
le potentiomètre règle le niveau au jack. Pour le laisser **à fond** et garder
un niveau correct, **résistance série R_s entre le jack et C1** (C1 bloque le
continu, la polarisation du nœud ne bouge pas, dc ≈ 1 630 vérifié) :

```
jack HP ──[ R_s ]──┤├── nœud ADC ── GPIO4
                   C1 1 µF   │
                       22 kΩ vers GND, 2 × 1N4148, pull-up interne ~45 kΩ
```

Mesuré sur la carte (sonde M10 reçue, potentiomètre à fond) :

| R_s | Audio plat : crête de salve / RMS | Chemin FM gain 30 | Décodage M10 |
|---|---|---|---|
| aucune | écrêté (> 2 500, saturation ~20 %) | 1 780 / 338 | aléatoire |
| 270 kΩ | 300 / 115 | 105 / 19 (atténuation 1/17 : nœud ≈ 16,9 kΩ) | 39/39 |
| **47 kΩ (retenue)** | **1 270 / 520, saturation 0 %** | 500 / 92 ; gain 78 : 1 265 / 486 | **33/33, score 0,92** |

Règle : A = R_nœud / (R_s + R_nœud) avec R_nœud ≈ 16,9 kΩ (22 kΩ ‖ pull-up
interne, mesuré). Cible en audio plat : crête 1 200-1 500 LSB sur 2 048,
saturation 0 % (ligne « ADC » de l'appli en mode SONDE). Si le 22 kΩ du haut
est un jour relié au 3V3, R_nœud tombe vers 8-11 kΩ : reprendre R_s ≈ 27 kΩ.
En phonie, le gain poste (REG_48) et le gain numérique de l'ESP compensent.

Vérification : enregistrer le flux BLE (`tools/uvlink_ble.py … rec`) pendant
une émission modulée et comparer, paquet par paquet, le niveau des 16
premières ms à celui des 20 dernières (voir le journal du README).
