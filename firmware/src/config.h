#pragma once
#include <Arduino.h>

// ============================================================================
// UVLink - configuration matérielle et protocole. TOUT ce qui est "en dur"
// est ici.
//
// Carte : ESP32-S3 mini ("Super Mini", 18 broches en bordure + pastilles
// au dos) montée sur la C-Board KD8CEC à la place du RP2040-Zero.
// Correspondance avec le brochage C-Board (docs/hardware.md) :
//
//   signal C-Board            RP2040   ESP32-S3   rôle
//   série -> poste (RX poste)  GP0      GPIO43 TX  UART, jack 2,5 mm
//   série <- poste (TX poste)  GP1      GPIO44 RX  UART, jack 3,5 mm bague
//   GPS NMEA (TX du module)    GP5      GPIO10     UART RX 9600
//   audio HP du poste          GP26/27/28  GPIO3/4/5  on lit GPIO4 = ADC1_CH3 (pont 22k/22k + 1 µF + clamp)
//   audio -> micro du poste    (PWM)    GPIO7      PWM LEDC + filtre RC (ajout UVLink)
//   LED d'état                 -        GPIO21     WS2812 de la carte (Heemol)
//   3,3 V / masse              3V3/GND  3V3/GND    alimentés par le poste
// ============================================================================

// ----------------------------------------------------------------------
// 1) Identité BLE
// ----------------------------------------------------------------------
#define BLE_DEVICE_NAME          "UVLink"
// Service UVLink + caractéristiques (voir docs/protocol.md)
#define UVL_SVC_UUID             "9b0e0001-6f3a-4d2b-8e5c-3a1f2c4d5e60"
#define UVL_CHR_CONTROL_UUID     "9b0e0002-6f3a-4d2b-8e5c-3a1f2c4d5e60"  // write + notify : commandes / réponses
#define UVL_CHR_STATUS_UUID      "9b0e0003-6f3a-4d2b-8e5c-3a1f2c4d5e60"  // notify : état du poste (1 Hz / sur changement)
#define UVL_CHR_AUDIO_RX_UUID    "9b0e0004-6f3a-4d2b-8e5c-3a1f2c4d5e60"  // notify : audio poste -> appli (ADPCM)
#define UVL_CHR_AUDIO_TX_UUID    "9b0e0005-6f3a-4d2b-8e5c-3a1f2c4d5e60"  // write sans réponse : audio appli -> poste
#define UVL_CHR_EVENT_UUID       "9b0e0006-6f3a-4d2b-8e5c-3a1f2c4d5e60"  // notify : résultats décodeurs (SARSAT, sonde, SSTV, APRS)
#define UVL_CHR_DATA_UUID        "9b0e0007-6f3a-4d2b-8e5c-3a1f2c4d5e60"  // write + notify : trames AX.25 (KISS)
// ATT MTU : 185 (charge utile 182 o = 7 PDU LL de 27 o). Mesuré sur le PC de
// test (Intel 7265, sans DLE) : une notification de 220 o (9 PDU) bloque la
// liaison en quelques secondes, 165 o (7 PDU) passe indéfiniment. Les paquets
// audio font 165 o ; les lignes SSTV sont découpées en conséquence.
#define BLE_MTU                  185
// Puissance d'émission BLE et fréquence CPU : réduites pour limiter les pics
// de courant de l'ESP. Mesuré 2026-10-28 : alimenté par le rail 3,3 V du
// poste (mode autonome), l'audio reçu était découpé en synchronisme avec les
// envois BLE (16 ms d'audio puis 24 ms atténués par paquet de 40 ms) alors
// qu'il était propre sur USB : les pics de courant (+9 dBm ≈ 300 mA) font
// décrocher brièvement le récepteur du poste (BK4819 sur le même rail).
// Mesuré ensuite : 0 dBm + 160 MHz ne changent rien (rapport début/fin de
// paquet 4,4 au lieu de 4,6) ; un condensateur de 470 µF sur le pad 3V3
// ramène à 1,05 (USB : 0,99). La parade est donc matérielle
// (docs/hardware.md) et la puissance BLE reste au maximum pour la portée.
#define BLE_TX_POWER_DBM         9        // -12 .. +9 (les decrochages du 2026-10-30 venaient du pilote btusb du PC, pas de la puissance)
#define CPU_FREQ_MHZ             160      // 80 / 160 / 240 (APB reste à 80 MHz)
// Taille des PDU LL émis (DLE) : 0 = laisser négocier (251 si le pair le
// supporte), 27 = forcer sans extension (test ; sans effet sur le blocage à
// 50 paquets/s, qui tient au nombre de notifications, pas à leur taille).
#define BLE_TX_OCTETS            0
#define BLE_TX_TIME_US           328
#define BLE_STALL_MS             3000     // 0 crédit HCI pendant ce temps -> déconnexion forcée
// Liaison fantôme (2026-10-30) : le contrôleur du PC garde le lien radio vivant
// alors que sa pile est morte -> l'ESP se croit connecté, ne publie plus, et
// l'appli ne le trouve plus. Signal de vie = les écritures CONTROL (l'appli
// envoie un PING toutes les 10 s). Sans écriture dans ces délais : déconnexion,
// la publicité reprend et l'appli peut se reconnecter.
// Mesuré ensuite (2026-10-30 après-midi) : une écriture avec réponse (PING)
// pendant le flux audio fait décrocher le contrôleur Intel 7265 en ~15 s
// (« 0 crédit HCI ») ; sans PING, 4 min sans faute. Plus AUCUN signal de vie
// par écriture : seul critère, un abonnement (ou une écriture) dans les
// BLE_FIRST_ACTIVITY_MS après la connexion ; ensuite le lien est jugé par les
// crédits HCI (BLE_STALL_MS) et, côté appli, par l'arrivée des STATUS.
#define BLE_FIRST_ACTIVITY_MS    15000
// Liaison muette (2026-10-30 soir) : après qu'une instance de l'appli a été
// tuée sans se déconnecter, l'ESP est resté « connecté » à bluetoothd pendant
// 2 h, puis n'a plus jamais publié : l'appli relancée ne le trouvait plus
// (seul un reset de la carte l'a rendu visible). Désormais l'appli envoie un
// PING toutes les 60 s ; sans AUCUNE écriture pendant BLE_IDLE_LINK_MS, l'ESP
// coupe la liaison et republie. Et quand il n'est pas connecté, loop()
// vérifie toutes les 5 s que la publicité tourne (sinon relance ; après
// BLE_ADV_RETRY_MAX échecs, redémarrage de la carte).
#define BLE_IDLE_LINK_MS         180000
#define BLE_ADV_RETRY_MAX        3
// Espacement minimal entre deux notifications (toutes caractéristiques
// confondues). Mesuré sur le PC de test (Intel 7265, pas de DLE, intervalle
// 15 ms) : la liaison se bloque dès que le contrôleur accumule plus d'une
// notification "pleine" (7 PDU de 27 o) : 50/s × 85 o, ou 166 o toutes les
// 20 ms (lignes SSTV) -> blocage en 10-30 s ; 165 o toutes les 40 ms (audio)
// -> stable pendant des heures. Le PC ne vide donc pas 7 PDU par événement :
// il lui faut 2-3 événements par notification. 40 ms = le débit de l'audio.
// Ce 40 ms s'applique aux rafales des décodeurs (EVENT / DATA). L'audio est
// déjà cadencé à 40 ms par l'ADC, STATUS à 1 Hz et CONTROL au coup par coup :
// eux n'ont qu'un espacement de politesse, sinon l'audio (25/s = tout le
// budget) perd un créneau à chaque STATUS, sa file ne se vide plus et jette.
#define BLE_NOTIFY_GAP_MS        40       // EVENT / DATA (décodeurs)
#define BLE_NOTIFY_MIN_GAP_MS    5        // AUDIO_RX / STATUS / CONTROL
#define BLE_NOTIFY_MAX           (BLE_MTU - 3)    // charge utile max d'une notification (EVENT tronqués au-delà)

// ----------------------------------------------------------------------
// 2) Liaison série avec le poste (firmware hôte benshi-esp32-sim/firmware/uv-k1-k5v3)
// ----------------------------------------------------------------------
#define RADIO_UART_TX_GPIO       43
#define RADIO_UART_RX_GPIO       44
#define RADIO_UART_BAUD          38400
#define RADIO_PTT_KEEPALIVE_MS   6000     // le firmware hôte coupe l'émission après 8 s sans
// Garde-fou : PTT demandé par l'appli (0x13) mais aucun flux AUDIO_TX depuis
// ce délai -> relâché d'office (2026-10-30, téléphone : le PTT off s'est perdu
// et le poste est resté en émission). L'appli envoie toujours le micro avec le PTT.
#define RADIO_PTT_NO_AUDIO_MS    10000
                                          // nouvelle trame PTT ON (HOST_TX_MAX_500MS = 16, filet
                                          // anti-émission-bloquée) : on la renvoie régulièrement.
                                          // Le poste rejoue sa séquence de keying (~20 ms) à chaque
                                          // renvoi : bref creux audible toutes les 6 s en émission.
#define RADIO_POLL_MS            250      // GET_STATUS quand un signal est présent
#define RADIO_IDLE_POLL_MS       400      // au repos (compromis rayonnement HF de la
                                          // liaison série / latence d'ouverture du squelch logiciel)
#define RADIO_DEBUG              1        // trace SET_VFO sur la console USB
// Squelch logiciel. Le squelch matériel du poste en mode hôte (niveaux 1-9,
// suivi par fronts d'interruption dans host.c) ne s'ouvre que par à-coups ;
// on laisse donc le poste en squelch 0 (AF permanent) et c'est l'ESP qui
// décide d'après le RSSI lu à chaque GET_STATUS, par rapport au plancher de
// bruit qu'il mesure lui-même (moyenne lissée du RSSI : descend en
// ~RADIO_SQ_FLOOR_FALL_S, remonte en ~RADIO_SQ_FLOOR_RISE_S seulement squelch
// fermé : une porteuse, brève ou permanente, ne l'entraîne pas), comme le
// squelch d'un poste :
//   seuil(niveau) = plancher + RADIO_SQ_L1_DB + (niveau - 1) × RADIO_SQ_STEP_DB
//   (1 = +6 dB … 9 = +30 dB), hystérésis RADIO_SQ_HYST_DB à la fermeture,
//   puis maintien AUDIO_GATE_HOLD_MS. Niveau 0 = AF permanent (données).
// Mesuré sur le UV-K1 : bruit -115 dBm ±2, d'où +6 dB au niveau 1.
// Le S-mètre suit le RSSI même squelch fermé. Le gating CTCSS/CDCSS du poste
// n'est actif qu'avec son squelch matériel (RADIO_SOFT_SQUELCH 0).
#define RADIO_SOFT_SQUELCH       1
#define RADIO_SQ_L1_DB           6
#define RADIO_SQ_STEP_DB         3
#define RADIO_SQ_HYST_DB         3
#define RADIO_SQ_FLOOR_RISE_S    60       // temps pour que le plancher rejoigne une porteuse permanente
#define RADIO_SQ_FLOOR_FALL_S    3        // temps pour redescendre sur le bruit après une porteuse
#define RADIO_SQ_FLOOR_MIN_DBM   (-130)   // borne basse
// Correction du RSSI en dBm par bande, comme la table dBmCorrTable du firmware
// v6 du projet Sarsat (menu caché DBMCAL) : le poste renvoie brut/2 - 160, on
// ajoute la correction de la bande du VFO écouté. Bandes du BK4819 (bornes
// basses, Hz) : 50 / 108 / 137 / 174 / 350 / 400 / 470 MHz. Valeurs de
// l'opérateur (2026-10-30) : VHF 144,8 MHz -15 dB, UHF 430 MHz -16 dB ; les
// autres bandes gardent les valeurs par défaut du v6.
#define RSSI_DBM_CORR_TABLE      { -15, -16, -15, -4, -7, -16, -1 }
#define RSSI_BAND_LOWER_HZ       { 50000000UL, 108000000UL, 137000000UL, 174000000UL, 350000000UL, 400000000UL, 470000000UL }

// ----------------------------------------------------------------------
// 3) Audio
// ----------------------------------------------------------------------
// Le nœud audio de la C-Board (3 pads GP26/27/28 du RP2040) arrive sur les
// broches 3, 4 et 5 de l'ESP ; on lit GPIO4 (ADC1_CH3), les deux autres
// restent en entrée haute impédance (ne rien y configurer : un UART y
// mettrait un pull-up sur le nœud).
#define AUDIO_ADC_GPIO           4        // ADC1_CH3
#define AUDIO_ADC_CHANNEL        3        // canal ADC1 correspondant (GPIOn = CH(n-1) pour GPIO1..10)
// Polarisation de secours : pull-up interne (~45 kΩ) sur l'entrée ADC. Mesuré
// 2026-10-27 : le 22 kΩ « haut » du pont de la C-Board n'est pas relié au 3,3 V
// (il allait au pad 3V3 du RP2040-Zero) ; seul le 22 kΩ vers la masse est là,
// le nœud reste à ~90 mV et la demi-onde négative est écrasée. Avec le pull-up :
// 1,16 V de repos (dc ≈ 1530), ±1,1 V de dynamique. Avec le pont complet en
// plus, repos ≈ 1,97 V (dc ≈ 2600), acceptable aussi.
#define AUDIO_ADC_PULLUP         1
#define AUDIO_PWM_GPIO           7
#define AUDIO_PWM_CH             0        // canal LEDC
#define AUDIO_PWM_FREQ_HZ        39062    // LEDC 10 bits (78 kHz refusé par le LEDC du S3)
#define AUDIO_PWM_BITS           10
#define AUDIO_SAMPLE_RATE_HZ     8000     // débit de l'audio BLE (phonie) : la NBFM
                                          // est limitée à 3 kHz, 8 kHz suffit. Les
                                          // décodeurs échantillonnent à leur propre
                                          // cadence (16 k SARSAT, 13,2 k APRS, 48 k SSTV,
                                          // 83 k max sur le S3) via l'ADC continu.
// Cadence de l'ADC en phonie : multiple de AUDIO_SAMPLE_RATE_HZ, ramené à 8 kHz
// par une moyenne glissante de AUDIO_ADC_RATE_HZ/8000 échantillons (AudioStream).
// Pourquoi : l'ADC n'a aucun filtre anti-repliement. Échantillonné à 8 kHz,
// tout résidu proche d'un multiple de 8 kHz dans l'audio du poste (horloge
// audio du BK4819, interruption PWM de l'ESP à 8 kHz...) se replie en un
// battement de quelques hertz, audible dès qu'il y a de l'audio (2026-10-28 :
// « battement 6-7 fois par seconde »). La moyenne de N échantillons à N×8 kHz
// a des zéros EXACTEMENT à 8, 16, 24 kHz... (-55 dB à ±13 Hz de 8 kHz) et
// atténue le souffle au-dessus de 4 kHz. Affaiblissement à 3 kHz : -2 dB.
// Cadence réelle du S3 : 2,5 MHz / entier -> 32 051 Hz, soit 8 012,8 Hz après
// décimation (+0,16 %, comme avant : 8 000 demandé donnait déjà 8 012,8).
#define AUDIO_ADC_RATE_HZ        32000
#define AUDIO_CODEC_ADPCM        1        // IMA ADPCM 4:1 -> 32 kbit/s @ 8 kHz
#define AUDIO_PKT_SAMPLES        320      // 40 ms par paquet BLE (160 o ADPCM + 5 o d'en-tête)
                                          // -> 25 paquets/s. Mesuré : à 50 notifications/s
                                          // (16 kHz / 20 ms) le contrôleur BLE du PC de test
                                          // (Intel 7265) cesse d'acquitter et la liaison se
                                          // bloque ; à 25/s c'est stable (0 perte).
#define AUDIO_IN_BLOCK           640      // échantillons par lecture DMA (20 ms à 32 kHz)
#define AUDIO_OUT_RING           4096     // anneau de sortie PWM (512 ms @ 8 kHz), puissance de 2
#define AUDIO_OUT_PRIME          640      // échantillons à accumuler avant de jouer (80 ms : gigue BLE)
#define AUDIO_RX_GAIN            16       // 12 bits -> 16 bits : x16 = pleine échelle ; réglable (SET_AUDIO)
                                          // = gain maximal quand l'AGC est actif
// AGC numérique : la crête de chaque bloc ADC (20 ms) est ramenée sous
// AUDIO_AGC_TARGET (attaque immédiate, limiteur), puis le gain remonte de
// 4 % par 40 ms après AUDIO_AGC_RELEASE_MS sans attaque, jusqu'à AUDIO_RX_GAIN.
// Il ne peut rien contre une saturation de l'ADC lui-même (entrée au-delà de
// 0-3,1 V : compteur « saturation ADC » de la console 'a' -> baisser le volume
// du poste).
#define AUDIO_AGC                1
#define AUDIO_AGC_TARGET         20000    // crête visée (~60 % de la pleine échelle 16 bits)
#define AUDIO_AGC_RELEASE_MS     1500
#define AUDIO_TX_TIMEOUT_MS      300      // fin de flux appli->poste si plus de paquet (PTT relâché)
#define AUDIO_TX_QUEUE           6        // paquets en attente d'envoi BLE (120 ms) avant de jeter le plus ancien
#define AUDIO_GATE_HOLD_MS       700      // flux RX maintenu après la fermeture du squelch (anti-hachures au seuil)

// ----------------------------------------------------------------------
// 4) GPS (NMEA, module alimenté en 3,3 V)
// ----------------------------------------------------------------------
#define GPS_ENABLE               1
#define GPS_UART_RX_GPIO         10       // câblage de la carte (5 est pris par le nœud audio)
#define GPS_BAUD                 9600
#define GPS_PUSH_MS              3000     // EVENT GPS vers l'appli (une fois un module vu)

// ----------------------------------------------------------------------
// 5) Décodeurs (lib/decoders, portés de Sarsat_UV-K1-5_RP2040). Un mode à la
//    fois (SET_MODE) ; l'ADC continu passe à la cadence du mode, la phonie
//    BLE est rééchantillonnée à 8 kHz (AudioStream) et continue de marcher.
// ----------------------------------------------------------------------
#define DEC_SARSAT_RATE_HZ       16000    // 40 échantillons / bit à 400 bauds
#define DEC_APRS_RATE_HZ         13200    // 11 × 1200 (aprs_rx.h)
#define DEC_SONDE_RATE_HZ        80000    // RS41 4800 Bd + M10/M20 9615 Bd ; le S3 plafonne à 83 333 Hz
#define DEC_SSTV_RATE_HZ         48000    // Scottie 1 (estimateur de fréquence par passages par zéro)
#define DEC_RING_SAMPLES         32768    // anneau partagé (puissance de 2) : 2 s à 16 kHz, 410 ms à 80 kHz
#define DEC_TASK_STACK           16384
#define DEC_LEVEL_MS             1000     // EVENT niveau audio
#define DEC_GAIN_SHIFT           3        // (brut - dc) << 3 -> domaine des décodeurs (|x| <= 16 k)
// SARSAT : capture en anneau, fenêtre réanalysée à chaque saut (decoder_config.h du RP2040)
#define DEC_SARSAT_WINDOW_MS     750      // une salve 406 dure 520 ms ; 750 + saut 250 = 1 s, tient dans l'anneau à 32 kHz (2 × 16 000 < 32 768)
#define DEC_SARSAT_HOP_MS        250
#define DEC_SARSAT_QUIET_PCT     60       // salve = RMS sous 60 % du plancher de souffle
#define DEC_SARSAT_PEAK_ON       3000     // ou crête absolue (repli)
#define DEC_SARSAT_MAX_RMS       4000     // au-dessus : souffle / écrêtage, pas de porteuse
#define DEC_SARSAT_REARM_MS      2000     // après un décodage réussi
#define DEC_SARSAT_DEDUP_S       120      // même ID dans cet intervalle -> "repeat"
#define DEC_SARSAT_AGC_TARGET    6000     // RMS visé avant le trancheur (0 = pas d'AGC)
#define DEC_SONDE_M10_PUSH_MS    3000     // limitation des "en-tête M10/M20 verrouillé"
// Décodeurs sondes + SARSAT EN PHONIE (2026-10-30) : dès que le VFO reçu est
// dans la bande DEC_PHONIE_BAND (400-440 MHz : météo 400-406, balises 406, et
// les essais utilisateur en 430-440 MHz), le récepteur M10/M20, la chaîne RS41
// et le décodeur SARSAT (fenêtres 16 kHz obtenues par moyenne de 2 sur
// l'anneau 32 kHz) tournent en tâche de fond sur l'ADC de la phonie, sans
// couper l'audio BLE ni forcer l'audio plat (case de l'appli, manuelle ; les
// sondes 9600 Bd en ont besoin, SARSAT 400 Bd s'en passe). Les modes
// explicites SONDE et SARSAT sont retirés (SET_MODE 2/3 = phonie).
#define DEC_PHONIE_DECODERS      1
#define DEC_PHONIE_BAND_LO_HZ    400000000UL
#define DEC_PHONIE_BAND_HI_HZ    440000000UL
// APRS en phonie (2026-10-31) : VFO écouté à DEC_PHONIE_APRS_HZ ± DEC_PHONIE_APRS_TOL_HZ
// (144,800 MHz, Europe) -> chaîne AFSK 1200 / AX.25 (aprs_rx) à 16 kHz sur
// l'anneau 32 kHz moyenné par 2, en plus du décodeur de l'appli sur l'audio
// BLE (chacun rattrape les trames manquées par l'autre). L'audio plat n'est
// pas forcé : la case de la carte VFO reste manuelle (le mode APRS explicite,
// lui, force l'audio plat et l'ADC à 13,2 kHz).
#define DEC_PHONIE_APRS_HZ       144800000UL
#define DEC_PHONIE_APRS_TOL_HZ   15000UL
// SSTV en phonie (2026-10-31) : le démodulateur Scottie 1 (sstv_demod, filtres
// 32 kHz) tourne en permanence sur l'anneau 32 kHz de la phonie, quelle que
// soit la fréquence (144,500, 145,800 ISS, 433,x…) : l'en-tête VIS (1900 /
// 1200 Hz, parité, code 60) suffit à déclencher l'image, qui part alors en
// EVENT comme en mode SSTV. Plus besoin de choisir le mode dans RÉGLAGES ; le
// mode SSTV explicite (ADC 48 kHz, audio plat forcé) reste disponible. Validé
// au banc avec l'audio filtré 300-3000 Hz + désaccentuation + bruit blanc.
#define DEC_PHONIE_SSTV          1
// POCSAG (2026-11-10, comme le décodeur de Portsdown 4) : mode explicite
// SET_MODE 5. L'ADC reste à la cadence de la phonie (32 kHz, l'audio BLE
// continue), l'audio plat est forcé (1200 et 2400 Bd ne passent pas les
// filtres AF du poste ; 512 Bd si). Les trois débits sont décodés en
// parallèle (DEC_POCSAG_BAUDS : b0 512, b1 1200, b2 2400). Les messages
// douteux (mots incorrigibles et texte illisible : résidus de fin de rafale)
// sont cachés comme dans Portsdown, sauf DEC_POCSAG_SHOW_PARTIAL.
#define DEC_POCSAG_BAUDS         7
#define DEC_POCSAG_SHOW_PARTIAL  0
// ISM 433/868 (mode ISM, fw 0.14) : impulsions OOK lues sur le souffle du
// discriminateur (lib/decoders/ookpulse.c), envoyées en µs à l'appli qui les
// donne à rtl_433. Expérimental : dépend de la vitesse de la FI du poste.

// Gain audio RX du poste (hôte H29, commande 0x0637 AF_GAIN, 1..78, 0 = réglage du
// poste) appliqué par défaut tant que l'opérateur n'en a pas mémorisé un autre
// (console `f N`). 78 = VOLUME_GAIN 63 + DAC_GAIN 15 du BK4819, le maximum :
// mesuré le 2026-11-10, l'audio normal est alors au même niveau que l'audio plat
// (qui contourne cet étage), 24 dB au-dessus du réglage 30 d'usine de l'opérateur.
#define RADIO_AF_GAIN_DEFAULT    78

// ----------------------------------------------------------------------
// 6) Divers
// ----------------------------------------------------------------------
#define STATUS_LED_GPIO          21       // WS2812 (Heemol S3 mini : GPIO21, pas 48)
// Luminosité de la LED d'état, 0..64 (0 = éteinte). Réglable à chaud par la
// console (`e N`), mémorisé en NVS. Consommation mesurable mais modeste :
// ~5 mA par couleur à 64, ~2 mA en blanc faible, plus ~1 mA de repos du
// WS2812 lui-même, que seule une coupure de son alimentation supprimerait
// (il reste alimenté par le 3,3 V de la carte même éteint).
#define STATUS_LED_LEVEL         64
#define UVL_PROTO_VERSION        1
