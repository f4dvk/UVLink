# Protocole BLE UVLink (v1 — brouillon)

Un seul service GATT, six caractéristiques. Toutes les valeurs multi-octets
sont **little-endian**. ATT MTU fixé à **185** côté firmware (charge utile
182 o par notification) et **une notification au plus toutes les 40 ms**,
toutes caractéristiques confondues : mesuré sur le PC de test (Intel 7265,
BT 4.2 sans extension de longueur de données, intervalle 15 ms), une
notification de 220 o ou deux notifications pleines en moins de 40 ms
bloquent la liaison en quelques secondes (0 crédit HCI, le firmware coupe
alors après 3 s et l'appli se reconnecte). Le firmware demande un intervalle
de connexion de 7,5–15 ms.

| UUID (suffixe `-6f3a-4d2b-8e5c-3a1f2c4d5e60`) | Nom       | Propriétés        | Sens         | Rôle |
|---|---|---|---|---|
| `9b0e0001` | service   | —                 | —            | service UVLink (annoncé dans la publicité) |
| `9b0e0002` | CONTROL   | write, notify     | appli ⇄ ESP  | commandes / réponses (pilotage du poste, réglages) |
| `9b0e0003` | STATUS    | notify            | ESP → appli  | état du poste : fréquences, squelch, S-mètre, PTT, mode décodeur |
| `9b0e0004` | AUDIO_RX  | notify            | ESP → appli  | audio reçu du poste, IMA ADPCM 8 kHz |
| `9b0e0005` | AUDIO_TX  | write sans réponse| appli → ESP  | audio à émettre, IMA ADPCM 8 kHz (déclenche le PTT) |
| `9b0e0006` | EVENT     | notify            | ESP → appli  | résultats des décodeurs (SARSAT, sondes, SSTV, APRS) |
| `9b0e0007` | DATA      | write sans réponse, notify | appli ⇄ ESP | trames AX.25 brutes (KISS sans framing) |

## CONTROL

Premier octet = identifiant de commande ; la réponse reprend le même
identifiant. `0xFF <id>` = commande inconnue.

| id | Commande | Charge utile (appli → ESP) | Réponse |
|---|---|---|---|
| `0x01` | PING        | — | `proto_ver:u8, fw_major:u8, fw_minor:u8` |
| `0x02` | GET_INFO    | — | `radio_present:u8, host_proto:u8, radio_fw[ascii…]` |
| `0x10` | SET_VFO     | `vfo:u8, rx_hz:u32, tx_hz:u32, mod:u8 (0 FM, 1 AM, 2 USB, 3 BYP, 4 RAW — BYP/RAW : hôte H30), narrow:u8, ctcss_rx:u16 (0,1 Hz), ctcss_tx:u16, power:u8 (7 haute, 6 moy., 1–5 basse), squelch:u8 (0–9), flags:u8 (b0 audio plat)` — 19 o | `ok:u8` |
| `0x11` | GET_VFO     | `vfo:u8` | `vfo:u8` + les 18 o suivants de SET_VFO (dernier réglage envoyé) |
| `0x12` | SET_RADIO   | `tx_vfo:u8, dual_watch:u8, squelch:u8 (0–9)` | `ok:u8` |
| `0x13` | PTT         | `on:u8` | `ok:u8` |
| `0x14` | MONITOR     | `on:u8` | `ok:u8` |
| `0x15` | RECALL_CH   | `vfo:u8, ch:u16` | `ok:u8` |
| `0x20` | SET_MODE    | `mode:u8` (0 phonie, 1 APRS, 4 SSTV, 5 POCSAG depuis fw 0.13, 6 ISM depuis fw 0.14 ; **2 SARSAT et 3 sonde retirés depuis fw 0.9, acceptés mais = phonie**) | `ok:u8` — choisit la cadence ADC et le décodeur actif ; APRS, SSTV et POCSAG **forcent l'audio plat** côté poste. POCSAG garde l'ADC de la phonie (32 kHz, l'audio BLE continue) et décode 512, 1200 et 2400 Bd en parallèle (port du `md_pocsag.c` de Portsdown 4). En phonie, dès que le VFO reçu est dans la bande **400-440 MHz** (`DEC_PHONIE_BAND_*`), les décodeurs sondes (M10/M20 + RS41) et **SARSAT** tournent en tâche de fond sur l'ADC 32 kHz (fenêtres SARSAT 16 kHz par moyenne de 2), sans couper l'audio ; audio plat **manuel** (case de l'appli : nécessaire aux sondes 9600 Bd, pas à SARSAT) (bypass filtres AF / désaccentuation, indispensable à 9600 Bd), quelle que soit la case « Audio plat » |
| `0x21` | SET_AUDIO   | `stream_rx:u8 (0 coupé, 1 permanent, 2 auto = squelch ouvert / monitor / sans poste), gain:u8 (×1…×64, 0 = inchangé ; gain maximal quand l'AGC est actif), agc:u8 (0 inchangé, 1 off, 2 on ; actif par défaut)` | `ok:u8` |
| `0x30` | APRS_CONFIG | `call[6], ssid:u8, path:u8, sym_table:u8, sym_code:u8, digi_level:u8, flags:u8` | `ok:u8` |
| `0x31` | SSTV_TX     | `mode:u8, image:u8` | `ok:u8` |
| `0x22` | CAL_AUDIO   | `seconds:u8 (0 = 3)` — calibration du gain RX : l'ESP mesure la crête brute de l'ADC pendant `seconds` (parler à niveau normal), fixe `gain = 20000 / crête` (×1…×64), **coupe l'AGC** et mémorise (NVS). Fin signalée par un CONTROL non sollicité `[0x22, 1, gain]` puis un STATUS | `ok:u8` (immédiat) |
| `0x23` | SET_AF_GAIN | `gain:u8` (0 = réglage du poste, 1..78 = gain audio RX du BK4819, 78 le plus fort ; même échelle que l'écran SARSAT du projet Sarsat). Mémorisé par l'ESP (NVS), réappliqué à chaque détection du poste. Hôte H29 requis. **Sans effet en audio plat** (sélection AF 9 : le chemin brut contourne l'étage de gain REG_48) : n'agit qu'en phonie sans « audio plat » | `ok:u8` (1 si le poste l'a pris) |
| `0x24` | SET_RSSI_CORR | `corr[7]:i8` : correction (dB) ajoutée au RSSI brut/2 − 160 pour chaque bande du BK4819 (bornes basses 50 / 108 / 137 / 174 / 350 / 400 / 470 MHz), équivalent de la table `dBmCorrTable` (menu DBMCAL) du firmware v6. Mémorisée (NVS) | `ok:u8` |
| `0x25` | GET_RSSI_CORR | — | `corr[7]:i8` |
| `0x40` | GET_GPS     | — | `flags:u8 (b0 fix valide, b1 module vu), lat_e5:i32, lon_e5:i32, speed_kmh:u16, course:u16, alt_m:i16, sats:u8, utc_h/m/s:u8` (20 o) |
| `0x41` | SET_POS     | `lat_e5:i32, lon_e5:i32` — position manuelle de l'opérateur (0,0 = aucune), utilisée pour distance/azimut APRS et sondes quand le GPS n'a pas de fix | `ok:u8` |

Les commandes `0x10`–`0x15` sont relayées telles quelles au poste via le
protocole série hôte de `benshi-esp32-sim/firmware/uv-k1-k5v3`
(trames `AB CD … DC BA`, commandes `0x0630`–`0x0634`).

## STATUS (toutes les secondes, ou dès que signal / S-mètre change) — 36 o (30 o avant fw 0.7, 28 o avant 0.6)

```
ver:u8, radio_present:u8, flags:u8, smeter:u8 (0..15), rssi_dbm:i16,
batt_mv:u16, rx0_hz:u32, tx0_hz:u32, rx1_hz:u32, tx1_hz:u32,
mode:u8, gps_fix:u8, adc_dc:u16, rx_gain:u8 (gain fixe, ou maximal si AGC), agc:u8 (1 = actif),
af_gain:u8 (gain AF du poste demandé, 0 = réglage du poste), adc_peak:u16, adc_rms:u16 (LSB 12 bits,
écart à la composante continue, dernière seconde), sat_pct:u8 (% d'échantillons en butée)
```

Squelch : le niveau 1–9 de SET_VFO est appliqué **par l'ESP** d'après le RSSI,
au-dessus du plancher de bruit qu'il mesure (RSSI lissé, descente 3 s,
montée 60 s) : seuil = plancher + 6 dB + 3 dB par niveau, hystérésis 3 dB, maintien 700 ms
(`config.h` RADIO_SQ_*), le poste restant en squelch 0 (AF permanent) : son
squelch matériel en mode hôte ne s'ouvre que par à-coups. Niveau 0 = AF
permanent (données). `smeter` suit le RSSI même squelch fermé ; depuis fw 0.15 il suit la recommandation IARU R1 VHF/UHF (S9 = -93 dBm, 6 dB par point en dessous, 10 dB par segment au-dessus : 10..15 = S9+10..S9+60), l'appli refait ce calcul sur `rssi_dbm` (avant : (rssi+135)/6).

`flags` : b0 signal reçu (squelch ouvert), b1 émission en cours,
b2 double veille, b3 monitor, b4 VFO en réception (0/1), b5 VFO d'émission,
b6 PTT demandé par l'appli, b7 ADC saturé (entrée audio au-delà de 0–3,1 V :
baisser le volume du poste ; l'AGC n'y peut rien). Les fréquences sont celles
du dernier SET_VFO (le poste ne les renvoie pas ; l'ESP mémorise le dernier
réglage en NVS et le réapplique quand il retrouve le poste). `mode` = mode décodeur actif (SET_MODE),
`gps_fix` : b0 fix valide, b1 module GPS vu.

## AUDIO_RX / AUDIO_TX

Chaque paquet : `seq:u8, flags:u8, pred:i16, idx:u8, adpcm[...]`. IMA ADPCM
mono, 4 bits/échantillon, nibble bas = premier échantillon. `pred`/`idx` =
état du codeur **avant** le premier échantillon du paquet : chaque paquet se
décode seul, une notification perdue ne désynchronise rien. `flags` : b0 début
de flux, b1 fin de flux, b2-b3 cadence (0 = 16 kHz, 1 = 8 kHz).

Cadence phonie : **8 kHz**, 160 octets d'ADPCM = 320 échantillons = 40 ms →
**25 paquets/s, 33 kbit/s**. Mesuré sur le PC de test (Intel 7265, BlueZ) :
à 50 notifications/s (16 kHz / 20 ms) le contrôleur du PC cesse d'acquitter
après quelques secondes et la liaison se bloque (0 crédit HCI côté ESP) ; à
25/s, zéro perte sur de longues durées. La NBFM étant limitée à 3 kHz, 8 kHz
ne perd rien. Le firmware coupe de lui-même une liaison restée 3 s sans
crédit HCI (l'appli se reconnecte).

AUDIO_RX est envoyé seulement si l'appli est abonnée, selon `stream_rx`
(SET_AUDIO) : en mode auto, quand le squelch est ouvert, le monitor actif ou
sans poste raccordé, et seulement en mode phonie ou POCSAG (les autres
décodeurs changent la cadence de l'ADC ou saturent le BLE d'EVENT) ; jamais
pendant une émission.

AUDIO_TX : l'appli envoie `PTT 1` puis les paquets ; le firmware décode dans
l'anneau de la sortie PWM (pré-tampon 80 ms) et maintient le PTT tant que le
flux dure (il le pose lui-même si l'appli n'a pas envoyé `PTT 1`). Le PTT
retombe sur `flags` b1 = fin, ou 300 ms après le dernier paquet.

## EVENT (décodeurs)

`type:u8` puis une charge utile propre à chaque décodeur, little-endian. Les
décodeurs sont le code C du projet RP2040 repris tel quel
(`firmware/lib/decoders/`) ; un seul mode à la fois (SET_MODE), l'ADC passe
à la cadence du mode (16 kHz SARSAT, 13,2 kHz APRS, 80 kHz sondes, 48 kHz
SSTV) et la phonie BLE est rééchantillonnée à 8 kHz.

| type | Source | Charge utile |
|---|---|---|
| `0x01` | niveau audio | `peak:u16, rms:u16, dc:u16, clip_pct:u8, verdict:u8 (0 OK, 1 écrêtage, 2 polarisation ADC, 3 trop fort, 4 trop faible), adc_min:u16, adc_max:u16, floor_rms:u16` — toutes les secondes dans un mode décodeur (`CMD_SARSAT_LEVEL` du RP2040 + plancher de souffle) |
| `0x02` | SARSAT 406   | structure `SARSAT_BEACON` du RP2040 (29 o : `frame_bits:u8, protocol:u8, is_test:u8, has_position:u8, country:u16, lat_e5:i32, lon_e5:i32, hex_id[15]`) puis `repeat:u8` (même ID dans les 120 s), `bch_fixed:u8` (bits corrigés), `raw_hex` (trame tranchée, NUL). Suivi des lignes de texte (`0x07`, source 2). |
| `0x03` | APRS RX      | `APRS_RXINFO` du RP2040 : `kind:u8, flags:u8 (b0 pos, b1 cap/vitesse, b2 alt, b3 distance, b4 ADRASEC), sym_table, sym_code, lat_e5:i32, lon_e5:i32, course:u16, speed_kmh:u16, alt_m:i16, dist_hm:u16, bearing:u16, src\0, name\0, via\0, text\0`. Les paquets non parsables partent en lignes de texte (`0x07`, source 3). |
| `0x04` | radiosonde   | `kind:u8 (1 RS41, 2 en-tête M10/M20 verrouillé, 3 M10/M20 GPS), has_pos:u8, lat_e5:i32, lon_e5:i32, alt_m:i32` + lignes de texte (`0x07`, source 4) |
| `0x05` | SSTV ligne   | `line:u16, x0:u16, npx:u8, rgb565[npx]` — une ligne = largeur/80 notifications (4 pour 320 px) ; `line = 0xFFFF` : nouvelle image (effacer), avec depuis fw 0.12 `x0` = largeur transmise, `npx = 0`, puis `mode:u8, h_transmise:u16, w_mode:u16, h_mode:u16`. Les modes plus larges que 320 px (PD 120/160/180/240/290) sont décodés en entier sur l'ESP mais transmis réduits de moitié (moyenne 2×2) : le lien BLE ne suivrait pas |
| `0x06` | GPS          | identique à la réponse GET_GPS, toutes les 3 s dès qu'un module a été vu |
| `0x07` | texte        | `src:u8 (2 SARSAT, 3 APRS, 4 sonde), line:u8 (0xFF = effacer l'écran), invert:u8, ascii[≤ 18]` — les lignes "écran du poste" telles que `sarsat_format_lines()` les produit |
| `0x09` | POCSAG       | fw ≥ 0.13 : `frag:u8 (b7 = dernier, b0-6 = index), baud:u16 (512/1200/2400), address:u32 (RIC), function:u8 (0-3), flags:u8 (b0 numérique, b1 bip seul = sans message, b2 douteux : mots incorrigibles et texte illisible, caché par défaut `DEC_POCSAG_SHOW_PARTIAL`), corrected:u8 (bits corrigés par le BCH), bad:u8 (mots incorrigibles gardés tels quels), texte ASCII 7 bits (NUL)`. Un message de plus de ~170 caractères part en plusieurs notifications (même en-tête, `frag` 0, 1, 2…) que l'appli recolle. Les caractères nationaux (`$ @ [ \ ] ^ \` { \| } ~`) sont transmis tels quels : l'appli les traduit selon le jeu choisi (US / FR / DE / SE / SI, comme le « Charset » de Portsdown). Le choix texte / numérique est fait au contenu par le décodeur (les réseaux SDIS envoient du texte avec la fonction 0) |
| `0x0A` | ISM          | fw ≥ 0.14, mode ISM : `frag:u8 (b7 = dernier, b0-6 = index), id:u8 (numéro de rafale), puis des paires pulse:u16 gap:u16` en µs (44 par notification). Impulsions OOK lues sur le **souffle** du discriminateur (porteuse = silence, `lib/decoders/ookpulse.c`) : une rafale (silence > 30 ms) = un paquet au format `.ook` de rtl_433, que l'appli lui donne (`rtl_433 -r x.ook -F json`). Expérimental : porteuses assez fortes pour éteindre le souffle, impulsions ≥ ~300 µs selon la FI du poste. Console `O 1` : paquets imprimés en lignes `.ook` |
| `0x08` | SSTV info    | `mode:u8, flags:u8 (b0 écart d'horloge valide, b1 image complète), lines:u16 (lignes décodées, taille du mode), syncs:u16 (lignes recalées sur leur synchro), ppm:i32`. Modes : 1 Scottie 1, 2 Scottie 2, 3 Scottie DX, 4 Martin 1, 5 Martin 2, 6 PD 50, 7 PD 90, 8 PD 120, 9 PD 160, 10 PD 180, 11 PD 240, 12 PD 290 (choisi par le code VIS ; `sstv_demod.c`, `SSTV_MODES`) |

Distance et azimut (APRS, sondes) sont calculés depuis le fix GPS du module
s'il est valide, sinon depuis la position SET_POS.

## DATA (AX.25)

ESP → appli : chaque trame AX.25 décodée (sans FCS), fragmentée en
notifications `frag:u8` (b7 = dernier fragment, b0–b6 = index) + octets.
Appli → ESP (étape 5) : trame à émettre par le modem AFSK (PTT automatique),
même fragmentation.

## Test sans poste

`tools/uvlink_feed.py PORT fichier --mode N [--realtime] [--wait S]` injecte
un enregistrement PCM dans les décodeurs à la place de l'ADC (console USB,
commande `F`, blocs de 4 Ko acquittés ; `--realtime` respecte la cadence,
indispensable en SSTV où chaque ligne coûte 4 notifications × 40 ms ;
`--wait` laisse le temps de connecter l'appli, l'ouverture du port ayant
redémarré la carte). `tools/gen406.py`, `gen_aprs.py`, `gen_sstv.py`
et `gen_pocsag.py` (rafale POCSAG 512/1200/2400 à la cadence voulue, bruit,
polarité) fabriquent des signaux synthétiques ; `tools/uvlink_ble.py` affiche les
EVENT/DATA reçus (`mode N`, `gps`, `pos LAT LON`), aussi en script :
`uvlink_ble.py - "mode 2" "sleep 60" quit`.
