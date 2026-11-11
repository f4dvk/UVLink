# UVLink

Pont **Bluetooth LE** entre un poste **Quansheng UV-K1 / UV-K5 V3** et une
application **Ubuntu / Windows / Android** : pilotage du poste, audio dans les
deux sens, et décodeurs embarqués (SARSAT 406 MHz, APRS, radiosondes, SSTV,
POCSAG).

Matériel : un **ESP32-S3 mini** monté sur la **C-Board KD8CEC** (à la place du
RP2040-Zero), raccordée aux jacks du poste. Voir [docs/hardware.md](docs/hardware.md).

```
   poste UV-K1/K5 V3 ──série 38400──┐
   (firmware hôte)   ──audio HP ──► │  ESP32-S3 mini   ◄──BLE──►  appli UVLink
                     ◄─audio PWM ── │  (C-Board)                  (Avalonia .NET)
   GPS NMEA ────────────────────────┘
```

## Pourquoi un nouveau projet

- [benshi-esp32-sim](https://github.com/f4dvk/benshi-esp32-sim) émule une
  radio Benshi en **Bluetooth Classic** pour HTCommander. L'ESP32-S3 n'a que
  du BLE : HTCommander ne peut pas le voir (il détecte les radios par un
  enregistrement SDP Classic). UVLink définit donc **son propre protocole
  BLE** ([docs/protocol.md](docs/protocol.md)) et **sa propre appli**.
- [Sarsat_UV-K1-5_RP2040](https://github.com/f4dvk/Sarsat_UV-K1-5_RP2040)
  fait tourner les décodeurs sur la C-Board et affiche sur l'écran du poste.
  UVLink réutilise ces décodeurs (C portable, seul `main.c` dépend du
  pico-sdk) sur l'ESP32-S3, mais les résultats vont à l'appli.

Ce qui est repris, **par fonction et non par code** :

| Fonction | Origine | Dans UVLink |
|---|---|---|
| Pilotage du poste (VFO, mémoires, PTT, squelch, S-mètre) | benshi-esp32-sim `UvK5.h` / `UvK5Link.h` + firmware hôte `firmware/uv-k1-k5v3` | firmware ESP32-S3, relayé par CONTROL/STATUS |
| Audio phonie | benshi-esp32-sim `AudioBridge.h` (SBC, Bluetooth Classic) | IMA ADPCM 8 kHz sur BLE (25 paquets/s) ; ADC GPIO4, PWM GPIO7 |
| TNC AFSK 1200 / APRS / balise | benshi-esp32-sim `TncModem`, `AprsBeacon` | firmware ESP32-S3, trames via DATA/EVENT |
| Décodeur SARSAT 406 | Sarsat RP2040 `dec406*`, `sarsat_decoder`, `audio_slicer` | firmware ESP32-S3 |
| APRS RX / digipeater | Sarsat RP2040 `aprs_rx`, `aprs_parse`, `aprs_digi` | firmware ESP32-S3 |
| Radiosondes RS41 / M10-M20 | Sarsat RP2040 `sonde_*` | firmware ESP32-S3 (ADC 96 kHz) |
| SSTV RX (+ TX par le poste) | Sarsat RP2040 `sstv_demod`, streaming d'image | firmware ESP32-S3, image affichée dans l'appli |
| POCSAG 512/1200/2400 | Portsdown 4 `multidec/md_pocsag.c` (port C du Pocsag.java de HTMOD) | firmware ESP32-S3 (mode POCSAG) **et** appli (sur l'audio BLE), liste des messages dans l'appli |
| ISM 433/868 (capteurs, télécommandes) | rtl_433 sur le PC, comme le mode ISM / Remotes de Portsdown 4 ; détecteur d'impulsions OOK `lib/decoders/ookpulse.c` (silence du souffle) | firmware ESP32-S3 (mode ISM : impulsions en µs) + appli bureau (rtl_433, format `.ook`), onglet ISM. Expérimental |
| GPS NMEA | Sarsat RP2040 `gps_nmea` | firmware ESP32-S3, GPIO10 |
| Interface utilisateur | HTCommander (idées, pas le code) | appli Avalonia (`app/`) |

## Organisation

```
firmware/   PlatformIO, ESP32-S3 (Arduino + NimBLE)      -> pio run -e esp32s3mini
app/        .NET 8 + Avalonia 11.3 (comme PCT_Report)
  UVLink/           code partagé (vues, modèles, client BLE, codec, décodage affichage)
  UVLink.Desktop/   Ubuntu / Windows
  UVLink.Android/   Android (workload `dotnet workload install android`)
docs/       hardware.md (brochage), protocol.md (GATT)
```

Le poste doit tourner le firmware **mode hôte** de
`benshi-esp32-sim/firmware/uv-k1-k5v3/` (le même qu'avec l'ESP32 classique).

## État

2026-11-10 : **POCSAG** (fw 0.13), « comme Portsdown 4 ». Le décodeur
`md_pocsag.c` du décodeur multiple de Portsdown (port C du Pocsag.java de
HTMOD, qui décode sur signal faible là où multimon-ng ne sort que des
adresses) devient une bibliothèque portable `firmware/lib/decoders/pocsag.c`
(même chaîne : anti-continu, moyenne glissante d'un quart de bit, bascule à
hystérésis 35 %, DPLL, synchro 0x7CD215D8 directe ou inversée, BCH(31,21)
jusqu'à 2 bits, choix texte / numérique au contenu, messages douteux cachés) :
**mode POCSAG** (`SET_MODE 5`, console `M 5`), ADC laissé à 32 kHz comme en
phonie (l'écoute continue), audio plat forcé (nécessaire à 1200 et 2400 Bd),
les trois débits décodés en parallèle, 4,7 Ko de RAM. **EVENT 0x09** :
débit, RIC, fonction, drapeaux, texte ASCII 7 bits (fragmenté au-delà de
170 caractères, l'appli recolle). Appli : **onglet POCSAG** (liste façon
Portsdown : `POCSAG1200   RIC 0028328   F0   Alpha` puis le message, bascule
« MODE POCSAG (ESP) », jeu de caractères nationaux US / FR / DE / SE / SI
appliqué à l'affichage, case « masquer les bips seuls », réglages mémorisés)
et **second décodeur dans l'appli** (`Pocsag/PocsagRx.cs`, portage C# à
l'identique) sur l'audio Bluetooth 8 kHz, comme pour l'APRS : chacun rattrape
ce que l'autre a manqué, un message vu par les deux n'est affiché qu'une fois
(« ESP+appli »). Banc PC (`firmware/test/host/test_pocsag.c`,
`tools/gen_pocsag.py` : rafales synthétiques NRZ filtrées, bruit, polarité) :
à 32 kHz les trois débits passent avec un bruit de 0,6 fois le signal (BCH
+1/+2 bits à 2400) ; à 8 kHz (appli) 512 et 1200 Bd passent à 0,6, **2400 Bd
tient à 0,3 mais plus à 0,6** (3,3 échantillons par bit) ; sortie identique à
celle du `md_pocsag` d'origine sur les mêmes fichiers à 22 050 Hz ; polarité
inverse, bip seul, message de 220 caractères (2 fragments) et caractères
nationaux vérifiés ; même résultat avec le portage C#. `uvlink_feed.py
--mode 5` (32 kHz) injecte un fichier dans l'ESP ; `uvlink_ble.py` affiche les
EVENT POCSAG. Soir : carte flashée, injection de rafales 512 et 1200 Bd par
`uvlink_feed.py --mode 5` décodées sur l'ESP à l'identique du banc, aucun
débordement de l'ADC. La porte audio « auto » (main.cpp), réservée à la
phonie, s'ouvre aussi en mode POCSAG : l'écoute continue (audio plat) et le
décodeur de l'appli reçoit le flux. **Pas encore essayé sur l'air** (réseau
SDIS 173,7 MHz, e*Message 466 MHz, DAPNET 439,9875 MHz) : squelch à 0. À
titre de comparaison, un enregistrement du RT-660 via HTMod (audio
haut-parleur Bluetooth, coupure raide sous 300 Hz) ne laisse rien décoder en
512 Bd : l'audio plat de l'UV-K5 est indispensable. Sur téléphone, l'onglet
POCSAG devient une page plein écran comme CARTE, RÉGLAGES et SSTV
(`FullPageTabs` = 3, 4, 6, 7 ; RETOUR = onglet d'avant). Onglet DÉCODEUR :
la barre du haut (mode, voyant et verdict de niveau, barre RMS, EFFACER)
devient repliable et les niveaux ADC passent sur leur propre ligne ; sur
Android, EFFACER sortait de l'écran à droite.

2026-11-10 (nuit, suite) : **ISM 433/868 avec rtl_433** (fw 0.14, mode
`SET_MODE 6`, console `M 6`), « comme le mode ISM / Remotes du multidec de
Portsdown 4 », mais sur l'audio du poste et non sur une clé SDR. Principe :
dans l'audio discriminateur, une porteuse OOK (ou FSK dont une tonalité
tombe dans le canal) se voit par le **silence du souffle** ; l'ESP suit
l'enveloppe du souffle (différence, |y|, maximum glissant sur 4
échantillons, seuils 30 / 60 % du souffle suivi, 7 échantillons calmes pour
entrer, antidatage, creux < 120 µs ignorés : `lib/decoders/ookpulse.c`) et
envoie les durées porteuse / silence en µs (**EVENT 0x0A**, 44 paires par
notification, une rafale = un paquet). L'appli (`Ism/Rtl433Runner.cs`)
recolle, écrit le format `.ook` de rtl_433, journalise tout dans
`Documents/UVLink/ism/AAAAMMJJ.ook`, et lance `rtl_433 -r … -F json`
(installé sur le PC : 23.11 ici, 244 décodeurs) chaque seconde sur les
nouvelles rafales ; onglet **ISM** : bascule « MODE ISM (ESP) », choix
Capteurs (tous les décodeurs) / Télécommandes (protocoles dont le nom évoque
une télécommande, relus dans `rtl_433 -R help`, plus les codes fixes CAME
320/640, Nice FLO 700/1400, Holtek HT12 430/870 en décodeurs flex comme
Portsdown), liste des décodages (JSON complet en infobulle), page plein
écran sur téléphone (rtl_433 embarqué dans l'APK depuis le 2026-11-11, voir
plus bas). Outils : `tools/gen_ook.py` fabrique l'audio
« souffle / silence » d'un fichier `.ook`, `test/host/test_ook.c` est le banc
du détecteur, `uvlink_feed.py --mode 6` injecte dans l'ESP, `uvlink_ble.py`
affiche l'EVENT. **Banc** (Nexus-TH 500 µs PPM et CAME 320/640 PWM
synthétiques, souffle RMS 540) : tout décode si la porteuse éteint le
souffle à 95 % et que la FI répond en ≤ 80 µs (CAME : ≤ 40 µs) ; un signal
faible (souffle résiduel 15 %) ou une FI lente (≥ 120 µs) perd les
impulsions. Le détecteur a été réécrit deux fois
au banc : la moyenne glissante (au lieu du maximum) multipliait les fausses
porteuses, car un seul échantillon fort suffit à prouver le souffle.
**Sur l'air** (433,920 MHz, FM 25 kHz, squelch 0, soirée du 10) : le souffle
réel du BK4819 est corrélé (bande 0,5-6 kHz, autocorrélation 0,75 à 31 µs),
d'où bien plus de creux que le modèle blanc : seuil d'entrée ramené à 22 %
du souffle, 7 échantillons calmes, impulsion minimale 200 µs -> 0 fausse
impulsion sur 250 ms de souffle pur. La FI répond en moins de 60 µs (front
mesuré sur l'enveloppe), meilleure que prévu. Reçus : un capteur Prologue /
Rubicson du voisinage (impulsions ~450 µs, intervalles 2000 / 4000, synchro
9200, lignes de 36 bits répétées, identifiées à la main) et la télécommande
PT2262 / EV1527 de l'opérateur (unité 350 µs, 24 bits, synchro 31 unités ;
rtl_433 la décode en « Smoke-GS558 » sur une trame idéale). Aucun n'est
encore décodé par rtl_433 : chaque trame porte 2-6 erreurs, impulsions
coupées par de brefs retours de souffle (fusion des trous < 200 µs ajoutée)
et impulsions courtes manquées quand la porteuse ne laisse que 30 % de
souffle résiduel au lieu de 10-15 % (niveau qui varie au fil de la rafale :
seuil rendu adaptatif, 2,2 × le niveau de porteuse mesuré dans la rafale,
borné 22-40 %, remis au strict entre deux rafales). Physique de la chose :
détecter l'absence de souffle demande ~250 µs de confirmation, donc les
impulsions de 350 µs ne passent qu'avec un signal fort ; les protocoles à
500 µs et plus (Nexus, Prologue, Oregon) sont le vrai terrain de jeu. Audio
brut des rafales réelles conservé pour continuer le réglage hors ligne
(console `Z` juste après un paquet : la dernière seconde de l'anneau).
Reste à faire : flasher cette version du détecteur (la carte a été
débranchée avant), ré-essayer la télécommande à courte distance, puis
attendre les capteurs météo.

2026-11-11 : **rtl_433 embarqué dans l'APK Android.** Le téléphone affichait
« rtl_433 introuvable » : l'appli ne cherchait le programme que sur PC.
`tools/build_rtl433_android.sh` compile rtl_433 23.11 (même version que le
paquet Ubuntu) avec le NDK en exécutable statique sans pilote SDR, dans
`app/UVLink.Android/native/<abi>/librtl_433.so` ; le projet Android
l'empaquette dans `lib/<abi>/` (manifeste `extractNativeLibs="true"`) et
`MainActivity` donne au lanceur son chemin dans `nativeLibraryDir`, seul
dossier d'où Android ≥ 10 autorise l'exécution d'un fichier. Sans ces
binaires le message devient « rtl_433 absent de cet APK ». Première
compilation : threads désactivés (bionic n'a pas `pthread_cancel`, utilisé
par le serveur rtl_tcp) et API 24 pour le 32 bits (`fseeko`) ; APK
regénérée avec le binaire arm64 (1,1 Mo).

2026-11-11 : **pavé numérique et choix du décodeur à l'accueil.** Sur
téléphone, le clavier Android que montre Avalonia 11.3 pour un champ
« nombre » n'a pas de point : la saisie de fréquence ouvre maintenant un
pavé de l'appli (chiffres, point, ⌫, C, TX pour la fréquence d'émission,
OK) sous les champs, qui ne prennent plus le focus (donc pas de clavier
système) ; la première touche remplace l'ancienne valeur et le poste n'est
réglé qu'à OK (`VfoCard.Key`, `VfoCardView.Keypad`, activé en mise en page
étroite ou sur Android). Le décodeur de l'ESP se choisit aussi sur la
pastille « Décodeur : … » du bloc ÉTAT de l'accueil et du bandeau plein
écran (menu Phonie / APRS / SSTV / POCSAG / ISM), en plus de RÉGLAGES.
`--demo-saisie` ouvre la saisie du VFO A pour les captures.

2026-11-10 (nuit) : **fiche station sur la carte**, façon aprs.fi. Un clic
(ou un toucher) sur une épingle ouvre un panneau en bas de la carte :
indicatif, dernière trame (heure, « il y a », source ESP / appli),
première trame et nombre de trames, position avec distance et relèvement
depuis l'observateur, cap / vitesse / altitude, commentaire, dernier statut
reçu (les trames de statut sans position sont rattachées à la station),
chemin (direct / via), trame TNC2 brute (sélectionnable), ✕ pour fermer
(le symbole dans le titre et un bouton aprs.fi ont été retirés à la demande
de l'opérateur). Sondes et
balises 406 ont une fiche simplifiée (réceptions, position, détail,
trajectoire). Mécanique : les entités Mapsui portent `key` / `layer`,
`MapControl.Info` -> `GetMapInfo` sur les trois couches -> `MapSelect` du
modèle ; `AprsEvent.Raw` porte la trame TNC2 (décodeur de l'appli) et
`_rawBySrc` retrouve la dernière trame brute par indicatif pour les EVENT de
l'ESP. Vérifié hors écran : `UVLINK_SNAPSHOT_TAP=lat,lon` simule un clic
avant la capture ; F1ABC, F4DVK-9, la sonde et la balise du `--demo-carte`
ouvrent chacun leur fiche, en bureau et en 400×800. Au passage, la fenêtre
du bureau s'ouvre **maximisée** par défaut (sauf `--window=` et captures).

2026-11-10 (soir, suite) : **l'ADC pouvait mourir pour de bon.** Symptôme :
niveaux figés dans l'appli (crête 1254 / RMS 1084 immuables), console `a` à
« 0 ech lus, debordements » qui grimpe. Cause : quand l'anneau DMA de
`adc_digi` déborde (tâche ADC bloquée > 80 ms), `adc_digi_read_bytes` renvoie
`ESP_ERR_INVALID_STATE` à jamais et la tâche se contentait de compter. Le
déclencheur trouvé au banc : la capture console `R 8000`, dont les 24 Ko
sortaient d'un bloc sur l'USB (R 2000, `Z`, `?` ne le font pas ; les modes
APRS / SSTV / POCSAG seuls tiennent sans débordement). Correctifs : sortie
de `R` cadencée comme `Y`, et dans `AudioIn` dix erreurs de suite ->
`adc_digi_stop` / `adc_digi_start` (compteur « redemarrages » dans `a`).
Vérifié : cinq `R 8000` de suite, 0 débordement, captures vivantes ; le
chemin de redémarrage n'a pas pu être exercé faute de déclencheur restant.
Au passage, forme d'onde réelle en audio plat, squelch 0, sans porteuse :
**onde carrée 250 / 2635 LSB** (0,19 / 2,0 V), paliers de 60 à 650 µs,
facteur de crête 1,15 : l'étage audio du poste est en butée sur le souffle,
le drapeau SATURATION de l'appli vient de là. À régler au potentiomètre
(le gain AF du poste n'agit pas en audio plat) : le point de fonctionnement
retenu est **volume maximal moins deux crans** (à fond, même avec le 47 kΩ,
le plat écrête ; à -3 crans RMS 300, à -4 le niveau s'effondre à 20 LSB).
Mesuré ensuite à ce volume : audio normal à gain AF 30 = crête 122 / RMS 35, à 78 (VOLUME_GAIN
63 + DAC 15, le maximum) = 1272 / 545, soit le niveau de l'audio plat
(1217 / 529). Le gain AF part donc à **78 par défaut**
(`RADIO_AF_GAIN_DEFAULT`, NVS `afgain`) : audio normal et plat au même
niveau sans réglage ; `f N` reste là pour l'ajuster.

2026-10-26 : le firmware publie le service BLE, pilote le poste en mode hôte
(SET_VFO, SET_RADIO, PTT, MONITOR, RECALL_CH, GET_STATUS → STATUS BLE à 1 Hz
ou sur changement), et offre une **console de test sur l'USB-C** (`?` dans le
moniteur série : `p` sonde, `v 0 145.5`, `t 1`, `s`…). `tools/uvlink_ble.py`
(bleak, Ubuntu) ou nRF Connect permettent de tester le BLE sans l'appli.
**Testé sur la carte** (Heemol ESP32-S3 mini, sans poste) : flash USB-C,
console, BLE depuis le PC, LED (GPIO21 sur cette carte).

L'appli Desktop (Ubuntu) se connecte en BLE via BlueZ (`BlueZTransport`,
Linux.Bluetooth), reçoit le STATUS, envoie les commandes ; écran "cockpit"
sombre (fréquences A/B, S-mètre à segments, voyants, PTT, réglage VFO,
mode décodeur, journal). Windows passe par InTheHand.BluetoothLE (WinRT,
cible `net8.0-windows10.0.19041.0`, compile mais pas encore testé) ;
Android : `UVLink.Android`, APK dans `dist/android/fr.f4dvk.uvlink-Signed.apk`
(signé avec la clé de debug, installable par `adb install` ou copie sur le
téléphone) :

```
cd app
ANDROID_HOME=$HOME/Android/Sdk JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64 \
dotnet publish UVLink.Android/UVLink.Android.csproj -c Release -f net8.0-android \
  -p:AndroidSdkDirectory=$HOME/Android/Sdk -p:JavaSdkDirectory=/usr/lib/jvm/java-21-openjdk-amd64 \
  -p:AndroidKeyStore=false -o ../dist/android
```

(charge de travail `dotnet workload install android` installée le 2026-10-30 ;
SDK Android dans ~/Android/Sdk, plateformes 33/34/36.) Première APK générée le
2026-10-30, connexion BLE et audio RX validés sur téléphone le soir même : BLE via InTheHand (permissions
BLUETOOTH_SCAN / CONNECT demandées au lancement), audio du téléphone par
AudioTrack (haut-parleur, réserve 240 ms) et AudioRecord (micro, source
« communication vocale » avec annulation d'écho, permission RECORD_AUDIO
demandée au lancement) dans `UVLink.Android/AndroidAudio.cs`, même contrat
que l'ALSA du bureau ; carte et décodeurs identiques au bureau.
Lancer : `dotnet run --project app/UVLink.Desktop -f net8.0 -- --autoconnect`.

2026-10-27 : **audio dans les deux sens** (étape 3). Firmware : ADC continu
(DMA) sur GPIO4 (GPIO1 à l'origine) → suppression de la composante continue, gain → IMA ADPCM →
AUDIO_RX ; AUDIO_TX → ADPCM → anneau → timer 8 kHz → PWM LEDC GPIO7 (registres
écrits dans l'ISR), PTT automatique tant que le flux dure. Appli : moteur
audio partagé (`Audio/`), ALSA en P/Invoke sur Linux, NAudio (WinMM) sur
Windows (compile, pas testé), AudioTrack/AudioRecord sur Android (compile, pas testé) ; écoute avec
volume, vumètres RX/TX, PTT = micro. Testé sur Ubuntu : 25 paquets/s sans
perte dans les deux sens (bleak et appli). Le flux est à **8 kHz** : à
16 kHz (50 paquets/s) le contrôleur Bluetooth du PC de test (Intel 7265)
bloquait la liaison — voir [docs/protocol.md](docs/protocol.md). Outils :
`tools/uvlink_ble.py` (`rec`, `play`, `tone`, `audio`), console USB (`a`
statistiques, `o` tonalité sur GPIO7, `g`/`A`/`V` réglages).

Attention : ouvrir le port USB-C (moniteur série) **redémarre la carte**
(USB CDC natif), donc coupe la liaison BLE en cours. Si `pio run -t upload`
échoue avec « No serial data received » après « Chip is ESP32-S3 », flasher
sans le stub :
`esptool.py --chip esp32s3 --port /dev/ttyACM0 --no-stub write_flash 0x10000 .pio/build/esp32s3mini/firmware.bin`.

Sans poste raccordé, l'entrée ADC flotte : en mode SSTV le décodeur « voit »
de fausses images (lignes noires/blanches dans l'appli), et le niveau audio
affiché n'a pas de sens. C'est normal au banc ; `uvlink_feed.py` remet le
décodeur à zéro au début de chaque injection.

Côté PC, si BlueZ reste bloqué après une liaison rompue (scan « en cours »
indéfiniment, `connect` qui expire) : `rfkill block bluetooth ; rfkill
unblock bluetooth` remet l'adaptateur d'aplomb (pas besoin de sudo). L'appli
purge d'elle-même le périphérique du cache BlueZ si la découverte GATT
n'aboutit pas, puis réessaie.

2026-10-27 (suite) : **décodeurs** (étape 4). Les sources C du projet RP2040
sont reprises **telles quelles** dans `firmware/lib/decoders/` (SARSAT 1G,
APRS AFSK/AX.25, RS41 + M10/M20, SSTV Scottie/Martin/PD, GPS NMEA) ; `Decoders.h`
remplace `main.c` du RP2040 : anneau alimenté par l'ADC continu, tâche
« decode » dédiée, EVENT BLE à la place des trames vers l'écran du poste
(voir [docs/protocol.md](docs/protocol.md), section EVENT). Un mode à la
fois (SET_MODE depuis l'appli ou `M n` sur la console) ; l'ADC passe à la
cadence du mode et la phonie BLE reste disponible (rééchantillonnage 8 kHz).
Appli : onglets JOURNAL / DÉCODEUR (niveau audio, écran texte, historique
des balises) / APRS (paquets parsés + moniteur AX.25) / SSTV (image en
direct), position de l'opérateur (SET_POS) pour distance/azimut, GPS
affiché. **Testé sur la carte sans poste** par injection de signaux
synthétiques (`tools/uvlink_feed.py` + `gen406.py` / `gen_aprs.py` /
`gen_sstv.py`) : salve SARSAT décodée (ID, pays, position), 3/3 paquets
APRS, image SSTV reçue dans l'appli. Sondes : compile et tourne (mode,
niveau), pas de signal de test — à valider sur l'air comme sur le RP2040.
Licence : avec les décodeurs M10/M20 (radiosonde_auto_rx, GPL-3) le projet
est de fait **GPL-3**.

Étapes 1 à 4 de la feuille de route faites (hors Android).

2026-10-27 (soir) : **premier essai avec le poste** (UV-K1, firmware hôte
H27), fw 0.5. Corrigé : l'appli exigeait un STATUS de 29 o alors qu'il en
fait 28, donc aucun STATUS n'était lu (ni fréquence, ni S-mètre) ; l'audio
était haché parce que le pacing BLE à 40 ms laissait exactement un créneau
par paquet audio et chaque STATUS en volait un (file pleine, paquets jetés) :
le 40 ms ne s'applique plus qu'aux EVENT/DATA des décodeurs
(`BLE_NOTIFY_GAP_MS`), audio/STATUS/CONTROL ont 5 ms (`BLE_NOTIFY_MIN_GAP_MS`).
Ajouté : **squelch logiciel** (le squelch matériel du poste en mode hôte
s'ouvre par à-coups ; le poste reste en squelch 0 et l'ESP décide d'après le
RSSI par rapport au plancher de bruit mesuré, `RADIO_SQ_*`), S-mètre qui suit
le RSSI, **AGC** numérique sur l'audio RX (`AUDIO_AGC_*`) et drapeau
« ADC saturé » (STATUS b7, bandeau rouge dans l'appli : baisser le volume du
poste), dernier SET_VFO/SET_RADIO mémorisé en NVS et réappliqué au démarrage,
l'appli pousse son VFO si le poste n'en a aucun et se reconnecte seule 3 s
après une coupure. Exécutable autonome : `dotnet publish` (voir ci-dessous),
sortie dans `dist/linux-x64/` (ignoré par git).

```
cd app/UVLink.Desktop
dotnet publish -c Release -f net8.0 -r linux-x64 --self-contained true \
  -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true \
  -p:DebugType=none -o ../../dist/linux-x64
```

2026-10-28 : **battement à 6–7 Hz sur l'audio reçu** (signalé avec un
signal présent ; les enregistrements du souffle seul n'en montrent aucun).
Rien de périodique à cette cadence dans la chaîne (sondage du poste 4 Hz,
paquets 25 Hz, tampon PC 25 Hz) : piste retenue = **repliement**, l'ADC
n'ayant aucun filtre anti-repliement, tout résidu proche d'un multiple de
8 kHz (horloge audio du BK4819, ou l'interruption PWM de l'ESP qui tournait à
8 000,0 Hz exactement même au repos face à un ADC à 8 012,8 Hz réels) se
replie en quelques hertz. Firmware (non testé, module débranché) : ADC phonie
à **32 kHz moyenné par 4** (`AUDIO_ADC_RATE_HZ`, zéros exactement aux
multiples de 8 kHz), interruption PWM armée seulement pendant un flux
appli→poste, console `R [N] [Hz]` (capture brute à cadence choisie, jusqu'à
8192 échantillons). À faire au retour du module : flasher, écouter, et
`R 8192 80000` sur un signal pour chercher une raie près de 8/16/24 kHz.
Autre suspect si le battement persiste avec un signal très fort : pompage de
l'AGC RF du BK4819 (écouter le poste au casque, sans la carte, même signal).

2026-10-28 (suite, poste branché) : mesures avec un moniteur BLE pendant les
émissions de l'utilisateur : 25 paquets/s et 0 perte, porteuse sur la
fréquence du poste ou à côté, avec le nouveau firmware. Les 10–14 paquets/s
« avec signal » d'hier et les blocages « 0 crédit HCI » venaient du
contrôleur Bluetooth du PC (Intel 7265) resté coincé après une liaison
rompue : `Powered off` BlueZ échoue alors, seul `rfkill block/unblock` le
relance. Deux corrections dans l'appli : (1) **gestionnaires de notifications
empilés** à chaque reconnexion (Linux.Bluetooth garde ses objets : 77 puis
103 paquets/s dans le journal = chaque paquet 3 à 4 fois, lecture hachée) ->
retrait des gestionnaires et libération des objets à chaque coupure ;
(2) après 3 échecs de connexion consécutifs, l'appli **réinitialise
l'adaptateur** (power off/on, sinon rfkill, accessible sans root via l'ACL
de `/dev/rfkill`). Raie permanente à **2643 Hz** (-53 dBFS, dérive ±2 Hz)
dans l'audio reçu, même sans modulation : disparaît quand l'USB du PC est
débranché (boucle de masse / alimentation PC), pas le rétroéclairage.

2026-10-28 (soir) : **le battement trouvé**. Enregistrement en mode autonome
(ESP alimenté par le poste, USB débranché) : replié sur la période des
paquets BLE (40 ms), chaque paquet contient 16 ms d'audio puis 24 ms atténués
×4, pattern composite de 120 ms (8 intervalles de connexion). Sur USB : plat.
Sur souffle seul : plat. Les pics de courant BLE de l'ESP font décrocher le
récepteur du poste, alimenté par le même rail 3,3 V (voir docs/hardware.md,
§ Consommation : condensateur 470–1000 µF ou alimentation séparée depuis la
batterie). Essai firmware BLE 0 dBm + CPU 160 MHz : sans effet (rapport
début/fin de paquet 4,4 contre 4,6). **Condensateur 470 µF sur le pad 3V3 :
rapport 1,05, comme sur USB (0,99)** — résolu. `BLE_TX_POWER_DBM` remis à
+9 (reflashé), `CPU_FREQ_MHZ` 160 conservé.

2026-10-28 (nuit) : **calibration du gain RX** (fw 0.6). L'AGC remontait le
bruit résiduel du poste pendant les silences ; demande de l'utilisateur : un
gain fixe. Le protocole hôte n'a aucun réglage de gain AF (REG_48 du BK4819)
et le potentiomètre du poste reste de toute façon dans la chaîne : la
calibration est donc côté ESP. CONTROL `0x22` / console `k [s]` : crête
brute mesurée pendant 3 s (parler à niveau normal) -> gain fixe
`20000 / crête`, AGC coupé, mémorisé en NVS (namespace « audio », relu au
démarrage). STATUS passe à 30 o (gain courant, AGC). Appli : case « AGC »,
bouton « CALIBRER », champs mis à jour depuis le STATUS. Mode d'emploi :
volume du poste juste sous le bandeau SATURATION, puis CALIBRER pendant une
émission parlée. Ajouté aussi : curseur **Micro** (10–400 %, gain numérique
du micro du PC avant l'ADPCM, barre « Niveau TX » pour le régler) et retour
à la ligne automatique des réglages audio (panneau droit trop étroit).
**PTT coupé après ~7 s** : garde-fou anti-émission-bloquée du firmware hôte
(`HOST_TX_MAX_500MS` = 16 → 8 s sans nouvelle trame PTT ON). L'ESP renvoie
maintenant PTT ON toutes les 6 s tant que l'émission est demandée
(`RADIO_PTT_KEEPALIVE_MS`) ; le poste rejoue sa séquence de keying à chaque
renvoi (bref creux). Mieux côté hôte, si on y touche un jour : ne
rafraîchir que `s_txGuard` quand PTT ON arrive déjà en émission.

2026-10-30 : **décodeur M10/M20 réparé** (jamais validé jusque-là, ni ici
ni sur le RP2040). Méthode : décodeur de référence rs1729/RS lu et réécrit
en Python, appliqué à l'enregistrement réel du 11 septembre (32 kHz,
position connue) -> 47 trames valides ; la même chaîne du firmware, passée
sur l'hôte via un petit harnais C, sortait `E4 9A 20 00` au lieu de
`64 9F 20 00` (premier bit forcé à 1, chips qui glissent). Nouveau
`sonde_m10_rx_t` (lib/decoders/sonde_m10.c) : corrélateur d'en-tête au
niveau échantillon (somme glissante par symbole, 32 accumulations par
échantillon, tout en entier), capture brute après le pic, intégration à
cadence FIXE (cadence ADC **réelle** : `AudioIn::actualRate()`, 80 645 Hz
et non 80 000), 25 décalages × 2 débits (9615 / 9600) tranchés par la somme
de contrôle, champs M10 et M20. Hôte : 47 trames / 42 positions (identique
à la référence). Sur cible : capture dans le tampon SARSAT `win_` (le tampon
statique ajouté d'abord épuisait le tas : « BLE_INIT: Malloc failed », puis
plantage de l'ADC) ; redémarrage ADC durci (échec = capture suspendue, plus
de LoadProhibited) ; tampon DMA ADC ramené à 10 Ko. Même correctif porté
dans **Sarsat_UV-K1-5_RP2040** (`rp2040/src/sonde_m10.[ch]`, main.c ;
46 trames à 96 kHz sur l'hôte ; images build-pico / build-pico2 refaites,
non flashées). Premier essai sur l'air sans décodage : l'audio « plat »
n'était appliqué que si la case de l'appli était cochée -> désormais
**forcé dès qu'un mode décodeur est actif** (`UvK5::setFlatForce`, SET_VFO
re-appliqué au poste, drapeau b0), rendu au choix de l'appli en phonie.
La case était en fait déjà cochée : **diagnostic sur l'air** (console `d`,
`Y`, nouvelle commande `Z` = vidage de l'anneau brut, 406 ms à 80 kHz) avec
une vraie sonde à -31 dBm : salves de ~170 ms toutes les secondes (= une
trame M10 de 168 ms), 222 pics de corrélateur mais score max 0,61 (le seul
préambule alternant donne 0,5-0,6 ; une vraie trame donne 0,8), zéro
trame. Comparaison avec le SDR du 11/09 (trames valides) : séquences d'un
symbole 430/515 au SDR contre 69/458 au poste ; énergie 3-5 kHz -7 dB au
SDR contre -14 dB au poste, 5-8 kHz -13 contre -27 dB. **Le poste coupe
l'audio à ~3 kHz malgré le drapeau « plat »** (filtre raide, probablement
le LPF 3 kHz du DSP du BK4819 non contourné par H27) : à 9615 Bd biphasé,
les runs d'un symbole (4,8 kHz) disparaissent et aucune égalisation
numérique ne les récupère (essayé : inverse 1er/2e ordre 2,5-4 kHz, gain
69 -> ~100 runs seulement). L'ESP est hors de cause (même chaîne : 47/47
trames sur l'audio SDR). Cause trouvée dans l'appli « Sonde M10 » du firmware
amont (armel/uv-k1-k5v3-firmware-custom, App/apps/sondem10, validée sur le
terrain) : elle écrit **REG_47 = 0x6942** (sélection AF 9, « bypasses the
speech filter, too narrow for 9615 chips/s »), REG_73 |= 0x10 (AFC coupé) et
REG_43 = 0x7E2C (FI large), en plus des bits de REG_2B. Firmware hôte
**H28** (benshi-esp32-sim/firmware/uv-k1-k5v3) : HOST_ApplyAudioFilters()
reproduit ces trois réglages en audio plat. **Le poste doit être reflashé en
H28** pour que les décodeurs 9600 Bd fonctionnent ; UVLink n'a rien d'autre
à changer (il force déjà l'audio plat en mode décodeur).
**Poste reflashé en H28, sonde M10 sur l'air (-75 dBm, S10)** : bande
revenue (3-5 kHz à -6 dB, séquences d'un symbole 369/249 comme au SDR),
score du corrélateur 0,82, premières trames décodées. Puis réglage du
corrélateur : seuil 0,50 -> **0,62** et relance de capture seulement sur un
pic nettement meilleur (un faux pic du préambule, jusqu'à 0,61, relançait la
capture et jetait la vraie trame) : **50 trames valides sur 51 pics en 45 s**,
position 49,65972 / 3,31384, 99 m, affichée dans l'appli (EVENT SONDE).
Même réglage reporté dans le RP2040 (images refaites).

2026-10-30 (après-midi) : **gain audio du poste depuis l'appli** (fw 0.7,
hôte **H29**). Comme la touche 5 de l'écran SARSAT du projet Sarsat
(`afgain.c`) : commande hôte `0x0637 AF_GAIN {u8}` = gain audio RX du BK4819
(REG_48, VOLUME_GAIN / DAC_GAIN), 0 = réglage d'origine, 1..78 ; maintenu
toutes les 500 ms côté poste, rendu à la sortie du mode hôte, renvoyé dans
le statut hôte. ESP : CONTROL `0x23`, console `f N`, mémorisé en NVS et
réappliqué à chaque détection du poste (refusé proprement si hôte < H29).
STATUS 36 o : gain AF demandé + **mesures brutes de l'ADC** sur la dernière
seconde (crête, RMS en LSB 12 bits, % d'échantillons en butée). Appli :
champ « Gain AF poste » + bouton GAIN POSTE, ligne « ADC : dc / crête / RMS /
saturation » sous les réglages audio. Les mêmes mesures qu'à l'écran du
poste, pour régler le niveau sans toucher au potentiomètre.
Mesuré ensuite : **le gain n'agit pas en audio plat** (AF select 9 contourne
l'étage de gain de REG_48 ; audio plat coupé : crête 160 LSB à 10, 2479 à
60 ; audio plat actif : saturée quelle que soit la valeur). Donc : en phonie,
« Audio plat » décoché ; en mode décodeur (sondes), seul le potentiomètre
règle le niveau, et le décodeur s'en moque (décisions par signe). L'appli
prévient quand le réglage est sans effet.

2026-10-30 (soir) : **purge automatique des liaisons fantômes.** Symptôme
récurrent : BlueZ garde UVLink « connecté » (lien radio vivant, pile morte),
l'ESP se croit connecté et ne publie plus, l'appli ne le trouve plus ; seul un
redémarrage de l'ESP ou un rfkill débloquait. Trois verrous : (1) l'appli
envoie un **PING toutes les 10 s** ; sans réponse, elle coupe, purge et se
reconnecte, et le 2e échec déclenche la réinitialisation de l'adaptateur ;
(2) avant tout scan, l'appli **déconnecte un lien BlueZ résiduel** vers
UVLink ; (3) l'ESP coupe lui-même une liaison **sans aucune écriture CONTROL**
depuis 15 s après connexion ou 60 s ensuite (`BLE_FIRST_WRITE_MS`,
`BLE_ZOMBIE_MS`) et reprend la publicité. Les outils de test doivent donc
écrire au moins toutes les 60 s (`ble_monitor.py` envoie un PING toutes les 20 s).
Corrigé dans la foulée : le calcul de silence non signé coupait chaque
connexion dès la première boucle (4 294 967 s) -> arithmétique signée.

2026-10-30 (soir, 17 h) : **l'appli ne trouvait plus de Bluetooth** (quatre
minutes de « aucun UVLink trouvé », adaptateur réinitialisé sans effet). Cause
reconstituée : l'instance précédente avait été tuée (publication) sans se
déconnecter ; **bluetoothd garde alors le lien radio ouvert** (vérifié :
`bluetoothctl devices Connected` liste encore UVLink après la mort de
l'appli), l'ESP est resté « connecté » deux heures, et à la relance de
l'appli il n'a plus jamais republié (seul un reset de la carte l'a rendu
visible ; la raison exacte n'a pas pu être lue, la carte ayant été réinitialisée
par l'ouverture de la console). Garde-fous ajoutés au firmware : (1) l'appli
envoie un **PING toutes les 60 s** (résultat ignoré, la surveillance passive
par STATUS reste seule juge) et l'ESP coupe une **liaison muette** sans aucune
écriture depuis 3 min (`BLE_IDLE_LINK_MS`) puis republie ; (2) **chien de garde
de la publicité** toutes les 5 s : pas connecté et plus en publicité ->
relance, trois refus -> redémarrage de la carte (`BLE_ADV_RETRY_MAX`) ; (3) la
raison du reset est affichée au démarrage et `d` donne l'uptime et le tas
libre. Les outils de test (`ble_monitor.py`) doivent de nouveau écrire au
moins toutes les 3 min. Supprimé le même soir à la demande de l'opérateur : le
réglage **« Gain AF poste »** de l'appli (le niveau est aligné sur l'audio plat,
poste au volume maximal + atténuateur 47 kΩ ; la commande SET_AF_GAIN 0x23 et
la console `f N` restent dans le firmware).

2026-10-30 (soir, 17 h 30) : **SARSAT en phonie enfin validé sur l'air**
(balise d'essai 434,2 MHz, trame toutes les 10 s : 3/3 décodées, ID
1C72091A2B3FDFF ELT-DT France, affichée dans l'appli). Deux défauts : (1) le
**plancher de souffle** était amorcé à la toute première fenêtre (DC/ADC pas
stabilisés) et restait figé à 3x le bruit réel, donc toutes les fenêtres
passaient pour des salves ; il est désormais amorcé à la 4e fenêtre et recalé
quand « calme » persiste 4 s ; (2) en **audio plat** (AF 9, AFC coupé) le
discriminateur sort un **échelon continu** à la capture de la porteuse 406
(décalage balise/poste, ~+2000 LSB mesuré) : l'AGC de fenêtre se normalisait
dessus et écrêtait les données à 32767, aucune synchro. Un **passe-haut 1er
ordre (~40 Hz)** avant le gain règle le cas (l'échelon devient un transitoire
de 5 ms dans les 160 ms de porteuse pure). Console : `X` arme la capture de
la prochaine fenêtre forte, `Q` la vide (16 kHz, int16 hex) pour le banc PC
`Sarsat_UV-K1-5_RP2040/rp2040/test/host/test_slicer fichier.wav`. Rappel de
l'opérateur : **l'AFC actif perturbe le décodage** (constaté sur le firmware
RP2040) ; le profil « audio plat » du poste (H28) le coupe, les décodeurs
phonie n'ont été validés qu'avec la case « Audio plat » cochée.

2026-10-30 (soir, 17 h 30) : **plus de bouton de validation dans l'appli** :
chaque réglage part dès qu'il change (squelch, largeur, puissance, audio plat,
double veille, mode décodeur, audio RX/gain/AGC ; fréquences et position :
Entrée ou sortie du champ). Au changement A/B l'appli **relit le VFO dans
l'ESP** (GET_VFO) pour remplir le formulaire sans l'envoyer, puis le VFO
sélectionné devient celui d'émission (SET_RADIO). À la connexion le formulaire
est chargé depuis l'ESP avant tout envoi automatique. La case « Double veille »
suit l'état réel du poste (STATUS). C'est ce bouton manquant qui faisait
croire que la double veille ne se désactivait pas (le poste, lui, répondait
bien dw=0).

2026-10-30 (soir, 18 h) : **onglet CARTE** (Mapsui 5.1 + OpenStreetMap, même
pile que PCT_Report). Trois couches alimentées par les EVENT : stations APRS
(symbole APRS réel, planches hessu/aprs-symbols), radiosondes (icône ballon,
étiquette type / numéro de série / altitude / température / vitesse verticale,
trace du vol), balises SARSAT (icône avion en détresse, halo rouge, ambre pour
une balise de test), plus la position de l'opérateur (GPS de l'ESP ou position
manuelle). **Effacement réglable** (« Effacer après » : 15 min, 30 min, 1 h par
défaut, 3 h, 12 h, jamais) : un marqueur sans nouvelle position depuis ce délai
disparaît (contrôle toutes les 30 s). Cases pour masquer chaque couche, boutons
+ / - / TOUT (cadrer tous les marqueurs), poignée entre les panneaux pour
agrandir le bandeau du bas. Vérification sans poste : `UVLink.Desktop
--demo-carte` (marqueurs fictifs autour de Laon) et `UVLINK_SNAPSHOT=/chemin.png`
(capture de la vue 8 s après l'ouverture puis fermeture). Les tuiles viennent
d'Internet ; sans réseau les marqueurs s'affichent sur fond vide. Crédits :
icône avion UXWing (libre, sans attribution), symboles APRS hessu/aprs-symbols
(CC BY-SA), ballon dessiné pour le projet.

2026-10-30 (soir, 18 h) : **pertes de liaison à chaque trame SARSAT** (signalé
après un rechargement du pilote btusb). En phonie, une trame SARSAT décodée
faisait partir l'EVENT binaire PUIS 15 lignes « écran » : 16 notifications en
640 ms, flux audio bloqué pendant ce temps (verrou de cadence partagé) puis
relâché d'un coup, et le contrôleur Intel 7265 décrochait (« 0 crédit HCI »).
Comme pour les sondes, **seul l'EVENT binaire part en phonie**. Console : `E 0`
/ `E 1` coupe / rétablit toutes les notifications EVENT (diagnostic), et les
décodages SARSAT sont horodatés (`t=` ms). Mesure après correction : 120 s
sans EVENT puis 100 s avec, 14 trames décodées, aucune coupure.

2026-10-30 (soir, 18 h 20) : **RSSI en dBm corrigé par bande** comme la table
`dBmCorrTable` du firmware v6 (menu caché DBMCAL) : l'ESP ajoute à brut/2 − 160
la correction de la bande du VFO écouté (`RSSI_DBM_CORR_TABLE`, config.h) :
VHF 137-174 MHz −15 dB, UHF 400-470 MHz −16 dB (valeurs de l'opérateur), autres
bandes = défauts du v6. Le S-mètre et le squelch logiciel suivent. Vérifié :
434,2 MHz, brut 85 -> −118 avant, −134 après. Dans la foulée, **réglable
depuis l'appli** : bloc « RSSI dBm : correction par bande » (panneau de droite,
7 valeurs −40..+40 dB), lu dans l'ESP à la connexion (GET_RSSI_CORR 0x25),
envoyé dès modification (SET_RSSI_CORR 0x24), mémorisé dans l'ESP (NVS) ; la
table de config.h ne sert plus que de valeur de départ.

2026-10-30 (soir, 19 h 50) : **premier essai Android** (APK installée sur le
téléphone de l'opérateur) : « operation canceled » à la connexion. Cause :
InTheHand passe `AndroidActivity.CurrentActivity` comme contexte à
`BluetoothGatt.connect()` ; nous ne passons pas par son sélecteur
`RequestDevice` qui le renseigne, le contexte était nul et la bibliothèque
lève OperationCanceledException. Corrigé dans `MainActivity.OnCreate`
(`InTheHand.AndroidActivity.CurrentActivity = this`). Dans la foulée :
connexion GATT bornée à 20 s avec message explicite, et **demande de MTU 185**
après connexion (Android négocie 23 o par défaut : 8 notifications par paquet
audio, intenable). Et **mise en page adaptative** (`MainView.axaml.cs`,
ApplyLayout) : sous 700 px de large, en-tête sur deux lignes, blocs
RÉCEPTION / VFO empilés dans une page qui défile, bandeau du bas à hauteur
fixe ; au-dessus, la mise en page bureau inchangée. Essai sur le bureau :
`UVLINK_SNAPSHOT_SIZE=412x1100 UVLINK_SNAPSHOT=/chemin.png` rend la vue à
cette taille, et `--window=LxH` force la taille de la fenêtre. Rappel : le PC
ne garde la liaison avec l'ESP que si l'appli de bureau tourne (ou jusqu'à 3
min après l'avoir tuée) ; l'ESP n'accepte qu'un seul appareil à la fois.

2026-10-30 (soir, 20 h) : **Android fonctionne** (téléphone de l'opérateur) :
connexion BLE et audio RX validés. Il a fallu, en plus du contexte d'activité
InTheHand : (1) **tolérer le « annulé » de la bibliothèque** : `connectGatt()`
lance déjà une connexion en arrière-plan et le second `connect()` répond faux
pendant qu'elle est en cours (cas rapporté sur Samsung / Android 16) ; l'appli
attend jusqu'à 6 s l'état connecté, puis réessaie jusqu'à 3 fois ; (2)
découverte des services bornée à 15 s (32feet #467) ; (3) vérification des
permissions à chaque connexion avec message dans le journal. Reste à valider
sur téléphone : PTT / micro, carte, décodeurs, et la tenue dans la durée.

2026-10-30 (soir, 20 h 45) : **PTT depuis le téléphone**. Au doigt, la page
qui défile volait le pointeur au bouton (relâcher immédiat) : le PTT tactile
est une **bascule** (toucher = émettre, retoucher = arrêter, bouton allumé),
le maintien reste à la souris. Puis « PTT ok mais pas d'audio, jauge TX
presque nulle » : mesuré sur l'ESP, **252 paquets reçus, 0 perdu, 0 jeté**, le
chemin est bon, c'est le niveau du micro Android (source « communication
vocale ») qui est très faible. Micro brut en premier (repli sur
VoiceCommunication) et **AGC d'émission** dans le moteur audio (crête visée
60 %, gain jusqu'à x24, montée lente / descente immédiate), case « AGC » à
côté du curseur Micro, active par défaut sur toutes les plateformes.

2026-10-30 (soir, 21 h) : **poste resté en émission après le PTT** (téléphone).
Android n'accepte qu'une opération GATT à la fois : le PTT off (écriture avec
réponse) partait derrière le flux audio (écritures sans réponse) et se perdait,
la bibliothèque attendant un rappel qui ne vient jamais. Trois verrous : (1)
toutes les écritures BLE du transport InTheHand passent par un verrou et sont
bornées à 3 s ; (2) le relâcher borne la fermeture du micro à 2 s puis envoie
le PTT off jusqu'à 3 fois ; (3) **garde-fou dans l'ESP** : PTT demandé mais
aucun flux audio depuis 10 s -> relâché d'office (`RADIO_PTT_NO_AUDIO_MS`),
l'appli envoyant toujours le micro avec le PTT.

2026-10-30 (soir, 21 h 15) : **pas de voyant TX ni de S-mètre pendant
l'émission** (téléphone). Mesuré avec la nouvelle commande console `T 1|0`
(PTT injecté comme l'appli, via CMD_PTT) : **le poste ne répond plus à
GET_STATUS tant qu'il émet** (« GET_STATUS ECHEC » tout du long ; il traite
bien les commandes sans réponse comme le PTT, sinon le garde-fou de 8 s
couperait). L'ESP gardait donc le dernier statut (tx=0, S-mètre figé) et 10 s
d'émission l'auraient fait déclarer « poste perdu ». Désormais, PTT demandé =
plus d'interrogation du poste et **statut synthétique** (TX, S0, pas de
signal) ; l'interrogation reprend au relâcher. Validé sur téléphone dans la foulée : connexion, audio RX, PTT, audio TX, voyant TX.

2026-10-30 (soir, 21 h 30) : **onglet SPECTRE** : spectre de l'audio reçu
(PCM 8 kHz décodé, FFT 1024 points fenêtre de Hann, 512 raies de 0 à 4 kHz en
dB sous la pleine échelle, 20 images/s) et chute d'eau (160 lignes), crête
affichée. Alimenté par les paquets reçus, écoute active ou non ; sans flux, le
dernier spectre reste affiché avec « pas d'audio ». `Audio/AudioSpectrum.cs`
(FFT maison, sans bibliothèque), `Views/SpectrumView.cs` (dessin direct). Essai
sans poste : `--demo-spectre` (porteuse 1 kHz + balayage + souffle). Dans la foulée (demande de
l'opérateur, téléphone) : les blocs AUDIO, DÉCODEUR, position et correction
RSSI quittent le panneau de droite (qui ne garde que VFO et POSTE) pour un
**onglet RÉGLAGES**. Onglets du bas : 0 journal, 1 décodeur, 2 APRS, 3 carte,
4 réglages, 5 spectre, 6 SSTV. Puis **menu ≡** (bouton dans la barre du haut, à gauche du titre, sur PC
comme sur téléphone) listant les onglets ; en mode étroit la rangée d'entêtes
est masquée (style `TabControl.menuonly`) et remplacée par ≡ + nom de l'onglet.

2026-10-30 (soir, 22 h) : **refonte du cadran** (demande de l'opérateur :
« mutualiser l'affichage de la fréquence et le réglage »). Les blocs RÉCEPTION
et VFO sont remplacés par **deux cartes VFO** (`ViewModels/VfoCard.cs`,
`Views/VfoCardView.axaml`) : fréquence en grand (toucher = saisie en place,
RX et décalage TX, Entrée ou sortie du champ = envoi), badges RX / TX ▶, bouton
« émettre ici » sur l'autre carte, S-mètre et dBm sur la carte écoutée, puis
les **pastilles** mode (FM / AM / USB / BYP / RAW), squelch (menu 0..9),
excursion 12,5 / 25 kHz, audio plat, puissance : chaque toucher envoie
aussitôt le SET_VFO de la carte. Le panneau de droite (largeur fixe) garde
ÉMISSION (PTT, MONITOR, double veille) et un bloc ÉTAT (mode décodeur, chemin
audio, batterie, flux, GPS, firmware, **dernier décodage** : toucher = onglet).
Large : A | B | émission ; étroit : empilés. Les modes **BYP / RAW exigent le
firmware hôte H30** (`benshi-esp32-sim/firmware/uv-k1-k5v3`, build avec
`ENABLE_BYP_RAW_DEMODULATORS`) ; en H29 l'appli prévient et le poste reste en FM.

2026-10-30 (soir, 22 h 15) : **jauge batterie** dans la barre du haut (PC et
téléphone) : barre + pourcentage + tension, rouge sous 15 %. Pourcentage
d'après la tension du STATUS, courbe 2S Li-ion par paliers (8,40 V = 100 %,
7,50 V = 45 %, 6,60 V = 0 %, `MainViewModel.BatteryPercentOf`). Le dBm est
passé à côté de la fréquence sur la carte VFO écoutée. `--demo-carte` injecte
aussi un STATUS factice (poste présent, 7,37 V) pour les captures.

2026-10-31 (matin) : **SARSAT muet au démarrage avec « audio plat »** (fw 0.10,
hôte **H31**). Côté poste, le drapeau « audio plat » était unique, pris du
dernier `SET_VFO` reçu : l'ESP réappliquant VFO A (434,2 MHz, plat) puis VFO B
(non plat) à la mise sous tension, le poste restait en audio filtré sur A ->
aucune trame tant que l'appli ne renvoyait pas un `SET_VFO A` (bascule plat
off/on). H31 mémorise le drapeau **par VFO** et applique celui du VFO écouté
(suit la double veille) ou du VFO d'émission pendant le PTT. Pour les hôtes
< H31, `UvK5::syncFlat()` réémet le VFO écouté quand les deux drapeaux
diffèrent (au démarrage et après tout `SET_VFO` de l'autre VFO). Bin hôte :
`benshi-esp32-sim/firmware/uv-k1-k5v3/bin/…H31-435192e.bin` (111 824 o).

2026-10-31 (matin) : **APRS en phonie, deux décodeurs** (fw 0.10). Jusqu'ici
l'AFSK 1200 ne tournait qu'en mode APRS explicite (ADC 13,2 kHz, audio plat
forcé). Maintenant, dès que le VFO écouté est sur **144,800 MHz ± 15 kHz**
(`DEC_PHONIE_APRS_HZ`), l'ESP fait tourner la chaîne `aprs_rx` à 16 kHz sur
l'anneau 32 kHz de la phonie (moyenne de 2, comme SARSAT), sans couper
l'audio ni forcer l'audio plat. La chaîne C est devenue indépendante de la
cadence (`aprs_rx_init(fs)`, tables FIR 13,2 k et 16 k). En parallèle,
**l'appli décode aussi** l'audio BLE (`Aprs/AprsRx.cs`, portage C# de la même
chaîne, `Aprs/AprsParse.cs` pour le champ info) : case « Décodeur APRS de
l'appli » dans RÉGLAGES, actif par défaut, demande le flux audio (Permanent
ou Auto). Le flux 8 kHz est interpolé ×2 avant la chaîne : à 8 kHz
(6,7 échantillons par bit) le trancheur se trompe sur ~11 % des échantillons
et rien ne passe, mesuré sur des trames synthétiques (5/5 après interpolation).
Une trame vue par les deux décodeurs n'est affichée qu'une fois (source
« ESP+appli » dans l'onglet APRS, colonne après l'heure) ; le moniteur AX.25
marque [ESP] / [appli]. Essais : fichiers AFSK synthétiques (position, statut,
message, Mic-E, objet) propres et bruités, C et C# donnent les mêmes résultats.
Validé sur l'air le 31/10 (07 h 15) : balise Mic-E F4DVK-7 décodée à la fois
par l'ESP (console `[aprs]`) et par le décodeur de l'appli sur 4 min d'audio
BLE enregistrées depuis le PC (`tools`-like `ble_monitor.py`, 8 kHz, 0 paquet
perdu) ; une injection console `F` de 5 trames synthétiques à 32 kHz ressort
5/5 en phonie.

2026-10-31 (après-midi) : **carte plein écran sur téléphone**. En largeur
étroite (< 700 px), choisir CARTE dans le menu ≡ ne montre plus la carte dans
le bandeau de 480 px sous les cartes VFO : la page entière est remplacée par
une tuile carte (`MapPage` dans `MainView.axaml`), avec en haut un bandeau de
rappel (voyant signal, VFO écouté et sa fréquence `RxFreqText`, mode, RSSI /
S-mètre, voyants liaison / poste / TX) et les boutons ≡ et RETOUR (retour à
l'onglet d'avant). Le contrôle Mapsui est déplacé d'un conteneur à l'autre
(`UpdateMapPage`), pas recréé : marqueurs et cadrage sont conservés. Les
boutons + / − / TOUT passent en bas à gauche en mode étroit (le panneau des
filtres couvre le haut). Mise en page bureau inchangée. Vérifié par capture
hors écran 400×800 (`UVLINK_SNAPSHOT_SIZE`, `--demo-carte`).

2026-10-31 (après-midi) : **« SSTV : l'état reste en phonie »**. L'ESP passe
bien en SSTV sur SET_MODE (console `M 4` et depuis le téléphone : ADC 48 kHz,
lignes et événements). Le piège : la liste RÉGLAGES → DÉCODEUR de l'appli ne
suivait pas le mode réel ; après un redémarrage de l'ESP (flash, coupure)
elle affichait encore « SSTV » alors que l'ESP était revenu en phonie, et
re-choisir la même entrée n'envoie rien. Maintenant la liste reflète le mode
du STATUS (hors 2 s après notre propre SET_MODE, comme la double veille), et
l'onglet SSTV a un bouton « DÉCODEUR SSTV » qui bascule le mode sur place,
avec le mode courant et le verdict de niveau à côté.

2026-10-31 (après-midi) : **SSTV : image redressée et détection automatique
en phonie** (fw 0.11). Deux causes à l'image penchée : le démodulateur était
initialisé à 48 000 Hz alors que l'ADC du S3 tourne réellement à 48 077 Hz
(2,5 MHz / 52 : 1 600 ppm = 1,6 px par ligne, toute l'image sur 256 lignes),
et après l'en-tête VIS les lignes défilaient en roue libre : l'écart d'horloge
était mesuré (ppm affichés) mais jamais corrigé. Maintenant
`sstv_demod_set_rate()` reçoit la cadence réelle de l'ADC, et chaque ligne
est recalée sur la **fin de l'impulsion de synchro 1200 Hz** (9 ms entre
Bleu et Rouge) : détectée au premier cycle nettement plus court après ≥ 5 ms
à 1200 Hz (précision ±0,4 ms ≈ ±1 px), acceptée à ±4 ms de l'instant attendu.
L'en-tête VIS se décide sur une moyenne de 4 cycles (bruit). Banc PC
(`sstv_host.c`, même chaîne d'entrée que l'ESP, image `gen_sstv.py`) : image
droite dans 11 cas sur 11 (ADC 48 077, émetteur −3 000 ppm, phonie 32 051 Hz
filtrée 300-3000 + désaccentuation, bruit blanc jusqu'à −12 dB), 256 lignes
recalées sur 256. Et le démodulateur tourne désormais **en permanence en
phonie** (`DEC_PHONIE_SSTV`, anneau 32 kHz, filtres 32 kHz déjà présents,
toute fréquence) : l'en-tête VIS déclenche l'image, l'appli saute sur
l'onglet SSTV ; plus besoin de choisir le mode (le mode SSTV dédié reste là,
bouton « MODE SSTV DÉDIÉ »). Vérifié sur l'ESP : 24 lignes injectées à
32 kHz en phonie → VIS + 24 lignes, retard 0 échantillon. Simulation du
trajet BLE (8 kHz, IMA ADPCM, ×4) : décodable aussi, image plus bruitée
(frange sur les contours) ; un second décodeur dans l'appli serait possible,
comme pour l'APRS.

2026-10-31 (15 h) : **première image sur l'air en phonie** (émetteur UV-K5 +
RP2040, projet Sarsat_UV-K1-5_RP2040) : droite, mais décalée d'un bloc
constant (le début de ligne réapparaissait à droite, ~25 px ≈ 10 ms). Deux
causes : cet émetteur n'envoie **pas** la synchro 9 ms supplémentaire avant
la ligne 0 (choix délibéré de `sstv_tx.c`, compatibilité HTCommander) alors
que le décodeur l'attendait, et le recalage par ligne n'accrochait pas sur
l'air : les filtres du poste adoucissent la transition 1200 → 1500 Hz
(plusieurs cycles à 1300-1400 Hz) et le détecteur exigeait 1400 Hz en deux
estimations. Corrigé : ligne 0 calée sur le premier saut au-dessus de
1380 Hz après le VIS (avec ou sans synchro initiale), fin de synchro
confirmée à 1380 Hz sur 4 estimations, fenêtre de recalage ±10 ms,
hystérésis du détecteur de passages par zéro ramenée à span/8 et enveloppe
relâchée en 8 ms (sinon les premiers cycles d'un ton plus faible après la
synchro 1200 Hz, plus forte avec la désaccentuation, étaient ratés : 14 px de
décalage sur la ligne 0). L'onglet SSTV affiche désormais « N recalées sur
leur synchro » (champ `synchros` du SSTV_INFO). Banc : 256/256 lignes
recalées sur les 5 fichiers (avec/sans synchro initiale, bruit, ADPCM,
48 077 Hz), bord gauche à 3 px près sur toutes les lignes.

2026-11-01 : **GPS du téléphone en repli** (appli). Case « GPS du téléphone
si le module GPS de l'ESP n'a pas de fix » dans RÉGLAGES (Android seulement,
grisée sur PC ; mémorisée dans `settings.txt` à côté du journal, nouveau
`Services/Settings.cs`). `ILocationService` (`Services/`) + `AndroidLocation`
(LocationManager, fournisseurs GPS et réseau, 5 s / 10 m, premier plan
seulement, permissions FINE + COARSE sur toutes les versions dans le
manifeste, demandées au moment où l'on coche). Logique dans
`MainViewModel` : tant que l'ESP signale « pas de fix » ou « aucun module »,
la position du téléphone remplit « Ma position », part en SET_POS (déjà
utilisé par l'ESP pour distance / azimut quand son GPS n'a pas de fix ;
renvoyée seulement si déplacement > 30 m, et à chaque connexion car l'ESP a
pu redémarrer) et place le marqueur sur la carte ; dès que le module a un fix
il reprend la main, bascules notées au journal. La ligne GPS de l'état
affiche les deux sources (« GPS : pas de fix … — téléphone : lat lon
(±précision) »). Rien à changer côté firmware. Vérifié sur le téléphone le
1er novembre : position reçue et fonctionnelle.

2026-10-31 (soir) : **SSTV multimode** (fw 0.12). Le démodulateur ne
connaissait que Scottie 1 ; il décode maintenant Scottie 1/2/DX, Martin 1/2
et PD 50/90/120/160/180/240/290, le mode étant choisi par le code VIS dans
une table (`sstv_demod.c`, `SSTV_MODES` : taille, durées de synchro, porche,
séparateur et composante). La ligne est décrite par une séquence d'éléments
par famille (Scottie : sep V sep B **synchro** porche R ; Martin : **synchro**
porche V sep B sep R sep ; PD : **synchro** porche Y-pair R-Y B-Y Y-impair,
deux lignes par balayage, conversion YCrCb BT.601 plage studio comme MMSSTV).
Le recalage par ligne (fw 0.11) suit la synchro de chaque mode : durée
admise de la série 1200 Hz de ½ à 1,5 fois la synchro du mode, +30 ms pour la
première de Martin/PD qui prolonge le bit de stop du VIS ; si le front
arrive alors qu'on est encore dans l'élément précédent, celui-ci est tronqué
(et la ligne livrée pour Martin/PD). Les modes plus larges que 320 px sont
décodés en entier mais **transmis réduits de moitié** (moyenne 2×2, PD 120 en
320×248, PD 290 en 400×308) : le lien BLE ne suivrait pas sinon. L'événement
« nouvelle image » porte le mode et les tailles ; l'appli redimensionne son
bitmap et affiche le nom du mode. `tools/gen_sstv.py --mode …` génère tous
les modes (numpy) ; banc PC (`sstv_host.c`) : les 12 modes décodés
intégralement, toutes synchros dans la fenêtre, à 48 077 Hz comme à
32 051 Hz filtré 300–3000 Hz avec bruit blanc 0,08. L'écart d'horloge n'est
plus donné en Martin (synchro de 4,9 ms trop courte pour les blocs Goertzel
de 5 ms). Vérifié sur l'ESP (fw 0.12 flashé le 1er novembre) par injection
en phonie de fichiers `--rate 32051` : Martin 2 (48 lignes), PD 120
(32 lignes, « transmise en demi-taille ») et Scottie 1 sans synchro de
départ, VIS reconnu et toutes les lignes comptées. Premier essai : plantage
« stack smashing » à la première image, l'événement « nouvelle image » de
13 octets était écrit dans un tampon de 12 (corrigé). Reste à voir sur l'air
avec un autre émetteur que le UV-K5 (qui n'émet qu'en Scottie 1).

2026-10-31 (soir) : **SSTV aussi en page plein écran sur téléphone**
(`FullPageTabs` = CARTE, RÉGLAGES, SSTV). En largeur étroite l'onglet SSTV
passe en une colonne : image dans une `Viewbox` pleine largeur (échelle
320:256 conservée, ~375×300 px sur un écran de 400 px), texte d'état
dessous ; en largeur bureau l'image reste à 320×256 avec le texte à droite
(`ApplyLayout`). Une image reçue en phonie bascule donc directement sur la
page plein écran. Capture 400×800 vérifiée.

2026-10-31 (après-midi) : **RÉGLAGES aussi en page plein écran sur
téléphone**. La mécanique de la page CARTE (`FullPage` / `FullHost` dans
`MainView.axaml`, `UpdateMapPage`) est générale : en largeur étroite, les
onglets listés dans `FullPageTabs` (CARTE, RÉGLAGES) quittent le bandeau de
480 px pour la page entière, sous le même bandeau de rappel (VFO écouté,
mode, RSSI, voyants) avec ≡, RETOUR et le nom de l'onglet. Le contenu est
déplacé, pas recréé. Capture 400×800 vérifiée pour les deux.

2026-10-30 (soir) : **atténuateur d'entrée** pour le potentiomètre à fond en
audio plat : 47 kΩ en série avant C1 (docs/hardware.md, tableau mesuré) ->
salve M10 à 1 270 LSB, 0 % de saturation, 33 trames sur 33, score 0,92.

2026-10-30 (soir) : **sondes en phonie + télémétrie** (fw 0.8, « pour
essai »). Activation choisie : automatique, dès que le VFO reçu est dans la
bande 400-406 MHz (`DEC_SONDE_IN_PHONIE`, `DEC_SONDE_BAND_*`), le récepteur
M10/M20 et la chaîne RS41 tournent en tâche de fond sur l'ADC de la phonie
(32 051 Hz réels), sans couper l'audio BLE ni forcer l'audio plat (case de
l'appli, manuelle ; sans elle, pas de décodage à 9600 Bd). Sur l'air :
43 trames sur 45 pics en 35 s. Télémétrie portée de rs1729 (température
Steinhart-Hart, humidité 555, vitesses sol/verticale, cap, satellites, heure
GPS, numéro de série) : EVENT SONDE 40 o, lignes texte supplémentaires,
journal de l'appli. Vérifié sur l'enregistrement du 11/09 (24,6 °C, 46 %,
12 sat, 15:59 UTC = 17:59 locales, SN 106-2-15288) et sur l'air (26 °C,
40 %). Plantage corrigé au passage (tampon EVENT trop court : « Stack
smashing »). Console : `v V RX [TX] [SQ] [NAR] [PLAT]`.
« Plantage au bout de quelques trames » ensuite : en phonie, les 7
notifications de lignes texte par trame s'ajoutaient aux 25 paquets audio/s
et aux STATUS, et le contrôleur BLE du PC décrochait toutes les 15 s
(journal : 8 paquets/s, puis « liaison BLE morte », ATT 0x0e). En phonie, le
décodeur n'envoie plus que l'EVENT (40 o, tout y est) ; les lignes texte
restent en mode SONDE. Appli : **journal permanent**
`~/.local/state/uvlink/journal.log` (même contenu que le panneau + exceptions
non gérées de tous les threads, tronqué à 2 Mo) pour ne plus perdre la
trace d'un plantage.

2026-10-30 (fin d'après-midi) : **modes SONDE et SARSAT retirés** (fw 0.9),
décodage **en phonie** dès que le VFO reçu est dans la bande **400-440 MHz**
(élargie à la demande : balise d'essai 434 MHz, essais sondes en 430-440).
SARSAT en phonie : fenêtres 16 kHz obtenues par moyenne de 2 sur l'anneau
32 kHz, fenêtre ramenée de 1 s à 750 ms (une salve dure 520 ms) pour tenir
dans l'anneau avec le saut de 250 ms ; capture M10 dans un tampon dédié de
12 Ko (win_ reste au SARSAT). Sur l'air : 56 trames M10 sur 56 pics en 45 s,
203 fenêtres SARSAT sans débordement (les salves M10 déclenchent l'analyse
SARSAT, 51 tentatives, 0 fausse trame). Appli : liste des modes réduite à
Phonie / APRS / SSTV (`ModeIds`), STATUS 2/3 affichés « Phonie (… auto) ».
**Reste à valider par l'utilisateur : une vraie salve 406 (balise d'essai
434 MHz) en phonie.** Chaîne BLE : après rechargement du pilote `btusb` du
PC, 100 s à 25 paquets/s sans perte, à 0 comme à +9 dBm ; +9 dBm conservé.
**Appli figée** (14:59) : le chien de garde (16 s sans STATUS, contrôleur du PC
de nouveau en vrac : « Opcode 0x200c failed: -16 » dans le noyau) appelait
`DisconnectAsync` puis `Dispose()` des objets Linux.Bluetooth **sur le thread
de l'interface**, qui attendent leurs veilleurs D-Bus -> interblocage.
Désormais : gestionnaires retirés tout de suite, `Dispose()` sur le pool de
threads sans attente, `DisconnectAsync` BlueZ borné à 3 s, découverte et
liste des périphériques bornées à 5 s, purge du chien de garde bornée à 5 s.

2026-10-30 : **LED d'état réglable / éteignable** (`STATUS_LED_LEVEL`,
console `e N` 0..64, mémorisé en NVS). Gain : quelques mA (≈ 5 mA par
couleur à 64, ≈ 2 mA en blanc faible) sur ~60–90 mA de moyenne ; le repos
du WS2812 (~1 mA) reste tant qu'il est alimenté par le 3,3 V de la carte.

## Feuille de route

1. **Liaison poste** : porter `UvK5Link.h` / `UvK5.h`, commandes CONTROL
   `0x10`–`0x15`, STATUS réel (squelch, S-mètre, fréquences).
2. **Appli — client BLE** : découverte/connexion (Windows : WinRT ;
   Linux : BlueZ D-Bus ; Android : `BluetoothLeScanner`), abstraction
   `IBleTransport` dans `UVLink/`, écran de pilotage (VFO, PTT, squelch).
3. **Audio** : ADC continu 16 kHz → ADPCM → AUDIO_RX ; AUDIO_TX → ADPCM →
   PWM GPIO7 + PTT. Lecture/capture côté appli (NAudio sur Windows, ALSA
   sur Linux, AudioTrack/AudioRecord sur Android).
4. **Décodeurs** : copier les `.c` du RP2040 dans `firmware/lib/decoders/`,
   remplacer l'anneau ADC+DMA par le pilote ADC continu de l'ESP32-S3,
   un mode à la fois (SARSAT, puis APRS, sondes, SSTV). *Fait.*
5. **TNC / APRS TX** : reprendre `TncModem` (esp32-afsk) et la balise,
   digipeater WIDEn-N (`aprs_digi.c` du RP2040, non copié pour l'instant).

## Licence

GPL-3 (les décodeurs radiosonde M10/M20 et DFM de `firmware/lib/decoders/`
viennent de `radiosonde_auto_rx`, GPL-3 ; voir les en-têtes des fichiers et
`CREDITS.md` du projet Sarsat_UV-K1-5_RP2040 pour les autres composants :
dec406 MIT, pico_tnc BSD-3). Le TNC AFSK de benshi-esp32-sim
(`dkaukov/esp32-afsk`, GPL-3) est donc compatible.
