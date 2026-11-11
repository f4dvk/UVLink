// UVLink - firmware ESP32-S3 mini : service BLE + liaison série poste (mode
// hôte) + audio bidirectionnel (ADC GPIO1 -> ADPCM -> BLE, BLE -> PWM GPIO7)
// + décodeurs (SARSAT, APRS, sondes, SSTV, POCSAG, ISM, GPS -> EVENT / DATA) + console USB.
#include <Arduino.h>
#include <NimBLEDevice.h>          // inclut host/ble_gap.h (ble_gap_set_data_len)
#include "config.h"
#include "UvK5.h"
#include "AudioStream.h"
#include "Decoders.h"
#include "Control.h"
#include "Console.h"
#include "Ws2812.h"
#include <Preferences.h>

static UvK5        g_radio;
static AudioStream g_audio;
static Decoders    g_dec;
static Control     g_ctrl(g_radio, g_audio, g_dec);
static Console     g_console(g_radio, g_audio, g_dec);

// Réglages audio RX mémorisés (NVS "audio") : gain fixe / maximal et AGC.
static Preferences g_audioPrefs;
static uint8_t  g_ledLevel = STATUS_LED_LEVEL;   // LED d'état 0..64, voir config.h
static uint32_t g_ledLast  = 0xFFFFFFFF;
static void audioPrefsLoad() {
    g_audioPrefs.begin("audio", false);
    uint8_t g = g_audioPrefs.getUChar("gain", 0);
    if (g) g_audio.setGain(g);
    if (g_audioPrefs.isKey("agc")) g_audio.setAgc(g_audioPrefs.getUChar("agc", 1) != 0);
    g_audio.settingsDirtyTake();
    Serial.printf("[audio] gain RX x%u, AGC %s%s\n", g_audio.gain(), g_audio.agc() ? "actif" : "off", g ? " (NVS)" : " (defaut)");
    if (g_audioPrefs.isKey("led")) { g_ledLevel = g_audioPrefs.getUChar("led", STATUS_LED_LEVEL); if (g_ledLevel > 64) g_ledLevel = 64; }
    Serial.printf("[led] luminosite %u/64%s\n", g_ledLevel, g_ledLevel ? "" : " (eteinte)");
}
static void audioPrefsSave() {
    g_audioPrefs.putUChar("gain", g_audio.gain());
    g_audioPrefs.putUChar("agc", g_audio.agc() ? 1 : 0);
}
void ledLevelSave() { g_audioPrefs.putUChar("led", g_ledLevel); }

static NimBLECharacteristic* g_control = nullptr;
static NimBLECharacteristic* g_status  = nullptr;
static NimBLECharacteristic* g_audioRx = nullptr;
static NimBLECharacteristic* g_audioTx = nullptr;
static NimBLECharacteristic* g_event   = nullptr;
static NimBLECharacteristic* g_data    = nullptr;
static bool g_connected = false;
static uint32_t g_connectMs = 0, g_lastCtrlMs = 0;   // liaison fantôme, voir config.h
static QueueHandle_t g_ctrlQueue = nullptr;

static HardwareSerial& radioSerial = Serial1;
#if GPS_ENABLE
static HardwareSerial& gpsSerial = Serial2;
#endif

static void led(uint8_t r, uint8_t g, uint8_t b) {
    r = (uint8_t)(r * g_ledLevel / 64); g = (uint8_t)(g * g_ledLevel / 64); b = (uint8_t)(b * g_ledLevel / 64);
    uint32_t v = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    if (v == g_ledLast) return;
    g_ledLast = v;
    ws2812Write(STATUS_LED_GPIO, r, g, b);
}
// Console 'e N' : luminosité 0..64 (0 = LED éteinte), mémorisée (NVS "audio"/"led").
void ledSetLevel(uint8_t lvl) {
    g_ledLevel = lvl > 64 ? 64 : lvl;
    g_ledLast = 0xFFFFFFFF;                          // force la réécriture au prochain ledUpdate()
    extern void ledLevelSave();
    ledLevelSave();
}
uint8_t ledLevel() { return g_ledLevel; }

static void ledUpdate() {
    const UvK5::Status& s = g_radio.lastStatus();
    if (!g_radio.present())      led(64, 0, g_connected ? 64 : 0);   // rouge : pas de poste (magenta si appli connectée)
    else if (s.tx)               led(64, 32, 0);         // orange : émission
    else if (s.sig)              led(0, 64, 0);          // vert : signal reçu
    else if (g_connected)        led(0, 0, 64);          // bleu : appli connectée
    else                         led(8, 8, 8);           // blanc faible : en attente
}

class ServerCb : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* s, NimBLEConnInfo& ci) override {
        g_connected = true;
        g_connectMs = millis(); g_lastCtrlMs = 0;
        g_audio.setMtu(ci.getMTU());
#if BLE_TX_OCTETS
        // Pas de DLE en émission : certains contrôleurs de PC (Intel 7265, BT 4.2)
        // cessent d'acquitter sous un flux soutenu de PDU longs -> plus aucun
        // crédit HCI, liaison zombie. Avec des PDU de 27 o le lien tient.
        int rc = ble_gap_set_data_len(ci.getConnHandle(), BLE_TX_OCTETS, BLE_TX_TIME_US);
        Serial.printf("[ble] PDU LL emis limites a %u o (rc=%d)\n", BLE_TX_OCTETS, rc);
#endif
        s->updateConnParams(ci.getConnHandle(), 6, 12, 0, 400);
        Serial.printf("[ble] connecte %s, intervalle initial %.2f ms\n", ci.getAddress().toString().c_str(),
                      ci.getConnInterval() * 1.25f);
    }
    void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
        g_connected = false;
        g_audio.setSubscribed(false);
        Serial.printf("[ble] deconnecte (%d), pub reprise\n", reason);
        NimBLEDevice::startAdvertising();
    }
    void onMTUChange(uint16_t mtu, NimBLEConnInfo&) override {
        g_audio.setMtu(mtu);
        Serial.printf("[ble] MTU %u -> paquets audio de %u o\n", mtu, g_audio.pktBytes());
    }
    void onConnParamsUpdate(NimBLEConnInfo& ci) override {
        Serial.printf("[ble] intervalle %.2f ms, latence %u, timeout %u ms\n",
                      ci.getConnInterval() * 1.25f, ci.getConnLatency(), ci.getConnTimeout() * 10);
    }
};

// Tâche BLE : on ne fait que mettre en file, la commande série se joue dans loop().
class ControlCb : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
        g_lastCtrlMs = millis();
        NimBLEAttValue v = c->getValue();
        CtrlMsg m;
        m.len = v.size() > sizeof(m.data) ? sizeof(m.data) : v.size();
        memcpy(m.data, v.data(), m.len);
        xQueueSend(g_ctrlQueue, &m, 0);
    }
};

// Appli -> poste : décodage direct dans l'anneau PWM (quelques µs, pas de
// blocage) ; le PTT est géré dans loop() d'après txStreaming().
class AudioTxCb : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
        g_lastCtrlMs = millis();                 // un flux TX = activité (liaison non muette)
        NimBLEAttValue v = c->getValue();
        g_audio.onTxPacket(v.data(), v.size());
    }
};

extern "C" uint16_t ble_hs_hci_avail_pkts;               // crédits ACL hôte -> contrôleur (diagnostic)

// Toutes les notifications passent par ici : une seule toutes les
// BLE_NOTIFY_GAP_MS (voir config.h), quelle que soit la tâche appelante
// (audiotx, decode, loop). Jamais depuis la tâche NimBLE.
static SemaphoreHandle_t g_notifyLock = nullptr;
static uint32_t g_lastNotifyMs = 0;
static bool notifyPaced(NimBLECharacteristic* c, const uint8_t* p, size_t n,
                        uint32_t gapMs = BLE_NOTIFY_MIN_GAP_MS) {
    if (!g_connected) return true;                       // personne : on jette sans bruit
    xSemaphoreTake(g_notifyLock, portMAX_DELAY);
    int32_t wait = (int32_t)(g_lastNotifyMs + gapMs - millis());
    if (wait > 0) delay(wait);
    bool ok = c->notify(p, n);
    g_lastNotifyMs = millis();
    xSemaphoreGive(g_notifyLock);
    return ok;
}

static volatile uint32_t g_sendFail = 0;
static bool audioSend(const uint8_t* pkt, size_t n) {
    if (notifyPaced(g_audioRx, pkt, n)) return true;
    if ((g_sendFail++ % 500) == 0)
        Serial.printf("[audio] notify refuse (t=%lu, refus=%lu, credits HCI=%u)\n",
                      (unsigned long)millis(), (unsigned long)g_sendFail, ble_hs_hci_avail_pkts);
    return false;
}

// Décodeurs -> appli (tâche "decode") : EVENT et DATA.
static bool eventSend(const uint8_t* p, size_t n) { return notifyPaced(g_event, p, n, BLE_NOTIFY_GAP_MS); }
static bool dataSend(const uint8_t* p, size_t n)  { return notifyPaced(g_data, p, n, BLE_NOTIFY_GAP_MS); }

class AudioRxCb : public NimBLECharacteristicCallbacks {
    void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo&, uint16_t subValue) override {
        g_lastCtrlMs = millis();                 // un abonnement = activité (liaison non fantôme)
        g_audio.setSubscribed((subValue & 1) != 0);
        Serial.printf("[ble] t=%lu AUDIO_RX %s (envoyes=%lu, refus=%lu)\n", (unsigned long)millis(),
                      (subValue & 1) ? "abonne" : "desabonne", (unsigned long)g_audio.sentTotal(), (unsigned long)g_sendFail);
    }
};


static void bleBegin() {
    NimBLEDevice::init(BLE_DEVICE_NAME);
    NimBLEDevice::setMTU(BLE_MTU);
    NimBLEDevice::setPower(BLE_TX_POWER_DBM);          // voir config.h (pics de courant sur le rail du poste)
#if BLE_TX_OCTETS
    ble_gap_write_sugg_def_data_len(BLE_TX_OCTETS, BLE_TX_TIME_US);
#endif

    NimBLEServer* srv = NimBLEDevice::createServer();
    srv->setCallbacks(new ServerCb());

    NimBLEService* svc = srv->createService(UVL_SVC_UUID);
    g_control = svc->createCharacteristic(UVL_CHR_CONTROL_UUID,
                    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
    g_control->setCallbacks(new ControlCb());
    g_status  = svc->createCharacteristic(UVL_CHR_STATUS_UUID,   NIMBLE_PROPERTY::NOTIFY);
    g_audioRx = svc->createCharacteristic(UVL_CHR_AUDIO_RX_UUID, NIMBLE_PROPERTY::NOTIFY);
    g_audioRx->setCallbacks(new AudioRxCb());
    g_audioTx = svc->createCharacteristic(UVL_CHR_AUDIO_TX_UUID, NIMBLE_PROPERTY::WRITE_NR);
    g_audioTx->setCallbacks(new AudioTxCb());
    g_event   = svc->createCharacteristic(UVL_CHR_EVENT_UUID,    NIMBLE_PROPERTY::NOTIFY);
    g_data    = svc->createCharacteristic(UVL_CHR_DATA_UUID,
                    NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY);
    srv->start();

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->setName(BLE_DEVICE_NAME);
    adv->addServiceUUID(UVL_SVC_UUID);
    adv->start();
}

// Console 'T' : PTT comme l'appli (CMD_PTT via Control::handle), pour les essais sans appli.
bool consolePttInject(bool on) {
    uint8_t cmd[2] = { CMD_PTT, (uint8_t)(on ? 1 : 0) }, rsp[8];
    size_t n = g_ctrl.handle(cmd, 2, rsp);
    return n > 1 && rsp[1] != 0;
}

static void sendStatus() {
    uint8_t st[40];
    size_t n = g_ctrl.buildStatus(st, g_audio.dc());
    notifyPaced(g_status, st, n);
}

// PTT = demandé par l'appli (0x13) OU flux AUDIO_TX en cours ; relâché quand
// les deux retombent (fin de flux signalée, ou AUDIO_TX_TIMEOUT_MS sans paquet).
static void pttUpdate() {
    static bool wasStreaming = false;
    bool streaming = g_audio.txStreaming();
    if (wasStreaming && !streaming) g_audio.txIdle();
    wasStreaming = streaming;
    // Garde-fou : PTT demandé mais plus aucun flux audio depuis RADIO_PTT_NO_AUDIO_MS
    static uint32_t lastStreamMs = 0, pttSinceMs = 0;
    static bool wasWanted = false;
    if (streaming) lastStreamMs = millis();
    if (g_ctrl.pttWanted() && !wasWanted) pttSinceMs = millis();
    wasWanted = g_ctrl.pttWanted();
    if (g_ctrl.pttWanted() && !streaming) {
        uint32_t ref = (int32_t)(lastStreamMs - pttSinceMs) > 0 ? lastStreamMs : pttSinceMs;
        if (millis() - ref > RADIO_PTT_NO_AUDIO_MS) {
            Serial.println("[audio] PTT demande sans flux audio depuis 10 s : relache (garde-fou)");
            g_ctrl.clearPtt();
        }
    }
    bool want = g_ctrl.pttWanted() || streaming;
    static uint32_t lastPttOnMs = 0;
    if (g_radio.present() && want != g_radio.pttRequested()) {
        Serial.printf("[audio] PTT %s (%s)\n", want ? "ON" : "OFF", streaming ? "flux audio" : "commande");
        g_radio.ptt(want);
        lastPttOnMs = millis();
        sendStatus();
    } else if (g_radio.present() && want && millis() - lastPttOnMs >= RADIO_PTT_KEEPALIVE_MS) {
        g_radio.ptt(true);                 // rafraîchit le garde-fou 8 s du firmware hôte (voir config.h)
        lastPttOnMs = millis();
    }
}

void setup() {
    Serial.setRxBufferSize(16384);       // injection de test ('F') : l'ISR USB CDC jette ce qui ne tient pas dans la file
    Serial.begin(115200);
    delay(300);
    setCpuFrequencyMhz(CPU_FREQ_MHZ);
    Serial.printf("\n[uvlink] fw %u.%u - ESP32-S3 mini / C-Board, CPU %u MHz, BLE %d dBm\n",
                  FW_MAJOR, FW_MINOR, (unsigned)getCpuFrequencyMhz(), (int)BLE_TX_POWER_DBM);
    {
        static const char* const why[] = { "inconnu", "mise sous tension", "externe (broche EN)", "logiciel (ESP.restart)",
                                           "panique", "chien de garde interruption", "chien de garde tache", "autre chien de garde",
                                           "sommeil profond", "baisse de tension (brownout)", "SDIO", "USB", "JTAG" };
        int r = (int)esp_reset_reason();
        Serial.printf("[uvlink] reset : %s (%d)\n", r >= 0 && r < (int)(sizeof(why) / sizeof(why[0])) ? why[r] : "?", r);
    }

    g_ctrlQueue = xQueueCreate(8, sizeof(CtrlMsg));
    g_notifyLock = xSemaphoreCreateMutex();
#if GPS_ENABLE
    gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_UART_RX_GPIO, -1);
#endif
    if (!g_audio.begin(audioSend)) Serial.println("[uvlink] audio : ADC continu indisponible");
    audioPrefsLoad();
    g_dec.begin(g_audio, eventSend, dataSend);
    bleBegin();
    Serial.println("[uvlink] BLE en publicite ; '?' pour la console");

    if (!g_radio.begin(&radioSerial))
        Serial.println("[uvlink] pas de poste sur l'UART (nouvel essai toutes les 5 s)");
    ledUpdate();
}

void loop() {
    static uint32_t lastStatusMs = 0, lastProbeMs = 0;
    static uint8_t  lastFlags = 0xFF, lastSm = 0xFF;

    g_console.poll();
#if GPS_ENABLE
    while (gpsSerial.available()) g_dec.gpsFeed((char)gpsSerial.read());
#endif
    g_dec.poll();

    CtrlMsg m;
    while (xQueueReceive(g_ctrlQueue, &m, 0) == pdTRUE) {
        uint8_t rsp[64];
        size_t n = g_ctrl.handle(m.data, m.len, rsp);
        if (n) notifyPaced(g_control, rsp, n);
        if (m.data[0] == CMD_SET_VFO || m.data[0] == CMD_SET_RADIO || m.data[0] == CMD_SET_MODE) sendStatus();
    }
    // Mode décodeur actif -> audio plat forcé côté poste (voir UvK5::setFlatForce).
    g_radio.setFlatForce(g_dec.mode() != Decoders::PHONIE);
#if DEC_PHONIE_DECODERS
    {   // sondes + SARSAT en phonie : VFO reçu dans la bande 400-440 MHz (voir config.h)
        uint8_t v = g_radio.lastStatus().rxVfo & 1;
        uint32_t f = g_radio.vfo(v).rxHz;
        g_dec.setPhonieDecoders(g_radio.present() && f >= DEC_PHONIE_BAND_LO_HZ && f <= DEC_PHONIE_BAND_HI_HZ);
        g_dec.setPhonieAprs(g_radio.present() && f + DEC_PHONIE_APRS_TOL_HZ >= DEC_PHONIE_APRS_HZ && f <= DEC_PHONIE_APRS_HZ + DEC_PHONIE_APRS_TOL_HZ);
    }
#endif
    // Fin de calibration (console 'c' ou CONTROL 0x22) : gain fixe, AGC coupé,
    // mémorisé, signalé à l'appli par un CONTROL non sollicité puis un STATUS.
    if (g_audio.calFinishedTake()) {
        uint16_t pk = g_audio.calPeak();
        uint8_t g = g_audio.applyCal();
        Serial.printf("[audio] calibration : crete ADC %u LSB -> gain fixe x%u, AGC off\n", pk, g);
        uint8_t rsp[3] = { CMD_CAL_AUDIO, 1, g };
        notifyPaced(g_control, rsp, sizeof(rsp));
        sendStatus();
    }
    if (g_audio.settingsDirtyTake()) audioPrefsSave();

    uint32_t now = millis();
    if (!g_radio.present()) {
        if (now - lastProbeMs >= 5000) { lastProbeMs = now; if (g_radio.probe()) sendStatus(); }
    } else if (g_radio.poll()) {
        const UvK5::Status& s = g_radio.lastStatus();
        uint8_t fl = (s.sig ? 1 : 0) | (s.tx ? 2 : 0);
        if (fl != lastFlags || s.sMeter != lastSm) {       // changement -> tout de suite
            lastFlags = fl; lastSm = s.sMeter;
            sendStatus(); lastStatusMs = now;
        }
    }
    if (now - lastStatusMs >= 1000) { lastStatusMs = now; sendStatus(); }

    pttUpdate();

    // Liaison zombie (plus aucun crédit HCI pendant BLE_STALL_MS : le PC
    // n'acquitte plus) : on coupe, l'appli se reconnecte.
    {
        static uint32_t stallSince = 0;
        if (g_connected && ble_hs_hci_avail_pkts == 0) {
            if (!stallSince) stallSince = now;
            else if (now - stallSince > BLE_STALL_MS) {
                Serial.println("[ble] liaison bloquee (0 credit HCI) : deconnexion forcee");
                NimBLEDevice::getServer()->disconnect(NimBLEDevice::getServer()->getPeerInfo(0).getConnHandle());
                stallSince = 0;
            }
        } else stallSince = 0;
        // Liaison fantôme : connecté mais ni abonnement ni écriture depuis la
        // connexion (voir config.h). Arithmétique SIGNÉE (la connexion, tâche
        // NimBLE, peut survenir après la prise de `now` en tête de boucle).
        if (g_connected && !g_lastCtrlMs) {
            int32_t  idle = (int32_t)(millis() - g_connectMs);
            int32_t  lim  = (int32_t)BLE_FIRST_ACTIVITY_MS;
            if (idle > lim) {
                Serial.printf("[ble] liaison fantome (aucune commande depuis %lu s) : deconnexion, publicite reprise\n", (unsigned long)(idle / 1000));
                NimBLEDevice::getServer()->disconnect(NimBLEDevice::getServer()->getPeerInfo(0).getConnHandle());
                g_connectMs = now; g_lastCtrlMs = 0;
            }
        }
        // Liaison muette : connecté mais plus aucune écriture (PING de l'appli
        // toutes les 60 s, flux TX) depuis BLE_IDLE_LINK_MS -> on coupe, la
        // publicité reprend (voir config.h).
        if (g_connected && g_lastCtrlMs) {
            int32_t idle = (int32_t)(millis() - g_lastCtrlMs);
            if (idle > (int32_t)BLE_IDLE_LINK_MS) {
                Serial.printf("[ble] liaison muette (aucune ecriture depuis %lu s) : deconnexion, publicite reprise\n", (unsigned long)(idle / 1000));
                NimBLEDevice::getServer()->disconnect(NimBLEDevice::getServer()->getPeerInfo(0).getConnHandle());
                g_lastCtrlMs = millis();
            }
        }
        // Chien de garde de la publicité : pas connecté et pas en publicité ->
        // relance ; si elle refuse BLE_ADV_RETRY_MAX fois de suite, on redémarre.
        static uint32_t lastAdvCheckMs = 0;
        static uint8_t  advFails = 0;
        if (now - lastAdvCheckMs >= 5000) {
            lastAdvCheckMs = now;
            if (!g_connected && NimBLEDevice::getServer()->getConnectedCount() == 0
                && !NimBLEDevice::getAdvertising()->isAdvertising()) {
                bool ok = NimBLEDevice::startAdvertising();
                Serial.printf("[ble] publicite arretee sans connexion : relance %s (%u)\n", ok ? "OK" : "REFUSEE", advFails + 1);
                if (ok) advFails = 0;
                else if (++advFails >= BLE_ADV_RETRY_MAX) {
                    Serial.println("[ble] publicite impossible : redemarrage de la carte");
                    Serial.flush(); delay(100);
                    ESP.restart();
                }
            } else advFails = 0;
        }
    }
    {
        const UvK5::Status& s = g_radio.lastStatus();
        bool present = g_radio.present();
        // Phonie en mode "auto" seulement hors décodeur : les EVENT (SSTV : 3
        // notifications par ligne) s'ajoutent aux 25 paquets audio/s et le
        // contrôleur du PC de test décroche vers 40-50 notifications/s.
        // Exception : POCSAG (ADC à 32 kHz comme la phonie, quelques EVENT par
        // minute) garde l'écoute, et le décodeur de l'appli reçoit l'audio.
        // "Permanent" (SET_AUDIO 1) force quand même le flux.
        // Maintien AUDIO_GATE_HOLD_MS après la chute du squelch : le flag `sig`
        // n'est lu que toutes les 250 ms et bat au seuil, chaque coupure
        // redémarre le flux (drapeau début) et vide le tampon de l'appli.
        static uint32_t sigHoldUntil = 0;
        if (s.sig) sigHoldUntil = now + AUDIO_GATE_HOLD_MS;
        bool sigHeld = (int32_t)(sigHoldUntil - now) > 0;
        g_audio.setGate((!present || sigHeld || g_radio.monitorOn()) && (g_dec.mode() == Decoders::PHONIE || g_dec.mode() == Decoders::POCSAG));
        g_audio.setTxBlocked(present && (s.tx || g_radio.pttRequested()));
    }
    ledUpdate();
    delay(2);
}
