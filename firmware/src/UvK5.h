#pragma once
#include <Arduino.h>
#include <string.h>
#include <Preferences.h>
#include "config.h"
#include "UvK5Link.h"

// ============================================================================
// Pilote d'un poste Quansheng UV-K1 / UV-K5 V3 en mode hôte (firmware
// benshi-esp32-sim/firmware/uv-k1-k5v3, commandes 0x0630..0x0636).
// Repris de benshi-esp32-sim/src/UvK5.h ; fréquences en Hz (u32) comme sur
// le protocole BLE.
// ============================================================================
class UvK5 {
public:
    struct RfParams {
        uint32_t rxHz = 0, txHz = 0;
        uint16_t rxCtcss = 0, txCtcss = 0;  // 0,1 Hz ; 0 = aucune
        uint8_t  modulation = 0;            // 0 FM, 1 AM, 2 USB
        uint8_t  narrow = 0;                // 0 = 25 kHz, 1 = 12,5 kHz
        uint8_t  power = 7;                 // 7 HIGH, 6 MID, 1..5 LOW
        uint8_t  squelch = 1;               // 0 = AF permanent (données), 1..9 phonie
        uint8_t  flags = 0;                 // b0 = audio plat (AFSK/APRS)
    };

    struct Status {
        uint8_t  func = 0;        // 0 FG, 1 TX, 2 MONITOR, 3 INCOMING, 4 RX
        uint16_t rssiRaw = 0;
        int16_t  rssiDbm = -140;
        uint8_t  sMeter = 0;      // 0..15
        bool     sig = false;
        bool     tx = false;
        uint8_t  rxVfo = 0;
        bool     dualWatch = false;
        uint16_t batterymV = 0;
        uint8_t  afGain = 0;      // H29 : gain audio RX courant du poste (0 = réglage du poste, 1..78)
        uint32_t stamp = 0;
    };

    bool begin(HardwareSerial* ser) {
        link_.begin(ser, RADIO_UART_RX_GPIO, RADIO_UART_TX_GPIO, RADIO_UART_BAUD);
        // Le poste ne renvoie pas ses VFO : on garde le dernier SET_VFO /
        // SET_RADIO en NVS pour le réappliquer (et l'afficher) dès qu'il est vu.
        prefs_.begin("uvlink", false);
        for (uint8_t v = 0; v < 2; v++) {
            char key[6] = { 'v', 'f', 'o', (char)('0' + v), 0 };
            if (prefs_.getBytesLength(key) == sizeof(RfParams)) prefs_.getBytes(key, &vfo_[v], sizeof(RfParams));
        }
        txVfo_     = prefs_.getUChar("txvfo", 0) & 1;
        dualWatch_ = prefs_.getUChar("dw", 0) != 0;
        { static const int8_t def[7] = RSSI_DBM_CORR_TABLE; memcpy(rssiCorr_, def, 7);
          if (prefs_.getBytesLength("rcorr") == 7) prefs_.getBytes("rcorr", rssiCorr_, 7); }
        afGain_    = prefs_.getUChar("afgain", RADIO_AF_GAIN_DEFAULT);
        delay(50);
        return probe();
    }

    // Sonde le poste et entre en mode hôte. Appelé au boot puis périodiquement
    // tant que le poste n'est pas vu.
    bool probe() {
        char fw[24] = {0};
        if (!link_.probe(fw, sizeof(fw))) return false;
        strlcpy(fw_, fw, sizeof(fw_));
        present_ = enterHost(true);
        Serial.printf("[uvk5] poste %s, mode hote %s\n", fw_, present_ ? "actif" : "REFUSE");
        fails_ = 0;
        if (present_) restore();
        return present_;
    }

    // Réapplique les réglages mémorisés (NVS) après l'entrée en mode hôte.
    void restore() {
        for (uint8_t v = 0; v < 2; v++)
            if (vfo_[v].rxHz) {
                RfParams p = vfo_[v];
                bool ok = applyVfo(v, p, false);
                Serial.printf("[uvk5] VFO %c memorise %lu Hz : %s\n", 'A' + v, (unsigned long)p.rxHz, ok ? "applique" : "refuse");
            }
        if (vfo_[0].rxHz || vfo_[1].rxHz) setRadio(txVfo_, dualWatch_ ? 1 : 0, 0, 0, false);
        if (afGain_) setAfGain(afGain_, false);
        syncFlat(1);   // hôte < H31 : B envoyé en dernier a pu écraser le profil audio de A (voir syncFlat)
    }

    // 0x0637 AF_GAIN (hôte H29) : gain audio RX du BK4819, 0 = réglage du poste,
    // 1..78 (78 = le plus fort), même échelle que l'écran SARSAT du projet Sarsat.
    // Mémorisé en NVS et réappliqué à chaque détection du poste.
    bool setAfGain(uint8_t g, bool save = true) {
        if (g > 78) g = 78;
        afGain_ = g;
        if (save) prefs_.putUChar("afgain", g);
        if (!present_) return false;
        if (proto_ < 29) { Serial.println("[uvk5] AF_GAIN : firmware hote < H29, ignore"); return false; }
        uint8_t b = g;
        return cmdOk(0x0637, &b, 1);
    }
    uint8_t afGainSet() const { return afGain_; }

    bool        present()  const { return present_; }
    const char* firmware() const { return fw_; }
    uint8_t     protoVer() const { return proto_; }
    const Status&   lastStatus() const { return last_; }
    const RfParams& vfo(uint8_t v) const { return vfo_[v & 1]; }

    // 0x0631 SET_VFO (save = mémoriser en NVS)
    bool applyVfo(uint8_t vfo, const RfParams& p, bool save = true) {
        uint8_t b[20];
        b[0] = vfo & 1;
        wr32(b + 1, p.rxHz / 10);
        wr32(b + 5, p.txHz / 10);
        b[9]  = p.modulation;
        b[10] = p.narrow ? 1 : 0;
        b[11] = p.power;
        b[12] = p.rxCtcss ? 1 : 0; b[13] = p.rxCtcss ? ctcssIdx(p.rxCtcss) : 0;
        b[14] = p.txCtcss ? 1 : 0; b[15] = p.txCtcss ? ctcssIdx(p.txCtcss) : 0;
        b[16] = 0; b[17] = 0;             // step (0 = inchangé)
        b[18] = RADIO_SOFT_SQUELCH ? 0 : p.squelch;   // squelch logiciel : AF permanent côté poste, seuil RSSI ici
        b[19] = (p.flags & 0x01) | (flatForce_ ? 1 : 0);   // audio plat : choix de l'appli OU forcé (mode décodeur)
#if RADIO_DEBUG
        Serial.printf("[uvk5] SET_VFO %u rx=%lu tx=%lu mod=%u nar=%u pwr=%u ct=%u/%u sq=%u fl=%u\n",
                      b[0], (unsigned long)p.rxHz, (unsigned long)p.txHz, b[9], b[10], b[11],
                      p.rxCtcss, p.txCtcss, b[18], b[19]);
#endif
        bool ok = cmdOk(0x0631, b, sizeof(b));
        if (ok) {
            vfo_[vfo & 1] = p;
            if (save) {
                char key[6] = { 'v', 'f', 'o', (char)('0' + (vfo & 1)), 0 };
                prefs_.putBytes(key, &p, sizeof(RfParams));
                syncFlat(vfo & 1);
            }
        }
        return ok;
    }

    // Hôtes < H31 : « audio plat » est un drapeau UNIQUE côté poste, pris du
    // dernier SET_VFO reçu. Si on vient de régler l'autre VFO que celui écouté
    // et que leurs drapeaux diffèrent, le poste a changé de profil audio sous
    // les pieds du VFO écouté (2026-10-31 : au démarrage, VFO A 434,2 plat puis
    // VFO B non plat -> plus de SARSAT tant que l'appli ne renvoyait pas A).
    // On renvoie alors le VFO écouté, sans toucher à la NVS.
    void syncFlat(uint8_t justSet) {
        if (proto_ >= 31 || !present_) return;
        uint8_t rx = (last_.stamp ? last_.rxVfo : txVfo_) & 1;
        if (rx == justSet || !vfo_[rx].rxHz) return;
        if ((vfo_[rx].flags & 1) == (vfo_[justSet].flags & 1)) return;
        Serial.printf("[uvk5] audio plat : VFO %c re-applique (hote H%u < H31, drapeau unique)\n", 'A' + rx, proto_);
        applyVfo(rx, vfo_[rx], false);
    }

    // 0x0632 SET_RADIO
    // Correction dBm de la bande du VFO écouté : table par défaut RSSI_DBM_CORR_TABLE
    // (config.h), modifiable par l'appli (SET_RSSI_CORR 0x24), mémorisée en NVS.
    int rssiBandCorr(uint32_t hz) const {
        static const uint32_t lower[7] = RSSI_BAND_LOWER_HZ;
        for (int b = 6; b >= 0; b--) if (hz >= lower[b]) return rssiCorr_[b];
        return rssiCorr_[0];
    }
    const int8_t* rssiCorr() const { return rssiCorr_; }
    void setRssiCorr(const int8_t* c) { memcpy(rssiCorr_, c, 7); prefs_.putBytes("rcorr", rssiCorr_, 7); }
    bool setRadio(uint8_t txVfo, uint8_t dualWatch, uint8_t crossBand = 0, uint8_t txLock = 0, bool save = true) {
        uint8_t b[4] = { (uint8_t)(txVfo & 1), dualWatch, crossBand, txLock };
        bool ok = cmdOk(0x0632, b, sizeof(b));
        if (ok) {
            txVfo_ = txVfo & 1; dualWatch_ = dualWatch != 0;
            if (save) { prefs_.putUChar("txvfo", txVfo_); prefs_.putUChar("dw", dualWatch); }
        }
        return ok;
    }
    uint8_t txVfo() const { return txVfo_; }

    // 0x0633 PTT — sans attendre l'ack (le keying peut perturber la série).
    bool ptt(bool on) {
        uint8_t b = on ? 1 : 0;
        pttReq_ = on;
        return link_.sendFrame(0x0633, &b, 1);
    }
    bool pttRequested() const { return pttReq_; }

    // 0x0635 MONITOR
    bool monitor(bool on) {
        uint8_t b = on ? 1 : 0;
        bool ok = cmdOk(0x0635, &b, 1);
        if (ok) monitor_ = on;
        return ok;
    }
    bool monitorOn() const { return monitor_; }
    // Audio plat forcé (modes décodeurs : SARSAT 400 Bd Manchester, APRS AFSK,
    // sondes 4800/9615 Bd, SSTV) : sans le bypass des filtres 300-3000 Hz et
    // de la désaccentuation du BK4819, rien ne passe à 9600 Bd. Re-applique
    // le VFO courant au poste quand l'état change (sans toucher à la NVS).
    bool setFlatForce(bool on) {
        if (on == flatForce_) return true;
        flatForce_ = on;
        if (!present_) return true;
        uint8_t v = last_.rxVfo & 1;
        if (!vfo_[v].rxHz) v ^= 1;
        if (!vfo_[v].rxHz) return true;
        Serial.printf("[uvk5] audio plat %s (mode decodeur)\n", on ? "FORCE" : "selon l'appli");
        return applyVfo(v, vfo_[v], false);
    }
    bool flatForced() const { return flatForce_; }

    // 0x0636 RECALL_CH
    bool recallChannel(uint8_t vfo, uint16_t ch) {
        uint8_t b[3] = { (uint8_t)(vfo & 1), (uint8_t)(ch & 0xFF), (uint8_t)(ch >> 8) };
        return cmdOk(0x0636, b, sizeof(b));
    }

    // 0x0634 GET_STATUS
    bool getStatus(Status& s) {
        uint16_t rid = 0;
        uint8_t  d[24];
        size_t   n = sizeof(d);
        if (!link_.command(0x0634, nullptr, 0, rid, d, n, 200) || rid != 0x0634 || n < 11)
            return false;
        s.func      = d[0];
        s.afGain    = n >= 15 ? d[14] : 0;
        s.rssiRaw   = d[1] | ((uint16_t)d[2] << 8);
        s.rssiDbm   = (int16_t)(d[5] | ((uint16_t)d[6] << 8));
        s.rssiDbm  += rssiBandCorr(vfo_[s.rxVfo & 1].rxHz);    // table par bande (config.h), comme le v6
        uint8_t flg = d[10];
        s.tx        = (flg & 2) != 0;
        s.sig       = (flg & 8) != 0;
        s.rxVfo     = (flg >> 4) & 1;
        s.dualWatch = (flg & 0x20) != 0;
        s.batterymV = d[8] | ((uint16_t)d[9] << 8);
#if RADIO_SOFT_SQUELCH
        // Plancher de bruit : RSSI lissé, descente en RADIO_SQ_FLOOR_FALL_S,
        // montée en RADIO_SQ_FLOOR_RISE_S (une porteuse brève ne l'entraîne
        // pas, le bruit ±2 dB est moyenné). Niveau 1..9 du VFO écouté -> seuil
        // au-dessus du plancher, avec hystérésis ; 0 = AF permanent (le poste
        // est en squelch 0, son flag `sig` vaut alors toujours 1).
        // Squelch ouvert : le plancher ne monte pas (il ne descend que). Sinon
        // une réception continue l'entraînait en ~1 min jusqu'à la porteuse, le
        // seuil passait au-dessus du signal et l'audio se coupait jusqu'à un
        // changement de VFO (constaté 2026-11-11) ; comme sur un poste, une
        // porteuse présente garde le squelch ouvert tant qu'elle est là.
        uint8_t lvl = vfo_[s.rxVfo & 1].squelch;
        {
            uint32_t now = millis();
            float dt = lastFloorMs_ ? (float)(now - lastFloorMs_) / 1000.0f : 0.0f;
            lastFloorMs_ = now;
            float r = (float)s.rssiDbm;
            if (floorDbm_ <= RADIO_SQ_FLOOR_MIN_DBM) floorDbm_ = r;     // première mesure
            bool open = lvl > 0 && last_.sig;
            if (r < floorDbm_ || !open) {
                float tau = r < floorDbm_ ? (float)RADIO_SQ_FLOOR_FALL_S : (float)RADIO_SQ_FLOOR_RISE_S;
                float k = dt / tau; if (k > 1.0f) k = 1.0f;
                floorDbm_ += (r - floorDbm_) * k;
            }
            if (floorDbm_ < RADIO_SQ_FLOOR_MIN_DBM) floorDbm_ = RADIO_SQ_FLOOR_MIN_DBM;
        }
        if (lvl > 0) {
            int thr = (int)floorDbm_ + RADIO_SQ_L1_DB + (int)(lvl > 9 ? 8 : lvl - 1) * RADIO_SQ_STEP_DB;
            if (last_.sig) thr -= RADIO_SQ_HYST_DB;
            s.sig = s.rssiDbm >= thr;
            sqThrDbm_ = (int16_t)thr;
        }
#endif
        s.sMeter    = sMeterOf(s.rssiDbm);   // S-mètre d'après le RSSI, squelch ouvert ou non
        s.stamp     = millis();
        return true;
    }
    uint8_t squelchLevel() const { return vfo_[last_.rxVfo & 1].squelch; }
    int16_t floorDbm() const     { return (int16_t)floorDbm_; }   // plancher de bruit estimé
    int16_t squelchThrDbm() const{ return sqThrDbm_; }            // seuil courant du squelch logiciel

    // Keepalive + statut, cadence adaptative. Renvoie true si le statut a été
    // rafraîchi.
    bool poll() {
        if (!present_) return false;
        // Pendant un PTT demandé, le poste ne répond plus à GET_STATUS (mesuré
        // 2026-10-30 : « GET_STATUS ECHEC » tout le long de l'émission) : le
        // dernier statut restait figé (tx=0, S-mètre bloqué) et 10 s d'émission
        // auraient déclaré le poste perdu. On n'interroge plus, et on publie un
        // statut synthétique : en émission, S0, pas de signal reçu.
        if (pttReq_) {
            lastPollMs_ = millis();
            if (!last_.tx) { last_.tx = true; last_.sig = false; last_.sMeter = 0; return true; }
            return false;
        }
        uint32_t now = millis();
        uint32_t iv = (last_.sig || last_.tx || now - lastSigMs_ < 3000) ? RADIO_POLL_MS
                                                                        : RADIO_IDLE_POLL_MS;
        if (now - lastPollMs_ < iv) return false;
        lastPollMs_ = now;
        Status s;
        if (getStatus(s)) {
            last_ = s; fails_ = 0;
            if (s.sig || s.tx) lastSigMs_ = now;
            return true;
        }
        if (++fails_ >= 40) {
            present_ = false;
            Serial.println("[uvk5] poste perdu (pas de reponse GET_STATUS)");
        }
        return false;
    }

    bool readBkReg(uint8_t reg, uint16_t& v) { return link_.readBkReg(reg, v); }
    bool writeBkReg(uint8_t reg, uint16_t v) { return link_.writeBkReg(reg, v); }   // diagnostic (console W)
    void end() { if (present_) enterHost(false); present_ = false; }

private:
    bool enterHost(bool on) {
        uint8_t b = on ? 1 : 0;
        uint16_t rid = 0; uint8_t d[24]; size_t n = sizeof(d);
        if (!link_.command(0x0630, &b, 1, rid, d, n, 500) || rid != 0x0630 || n < 2)
            return false;
        proto_ = d[1];
        return on ? (d[0] != 0) : true;
    }

    bool cmdOk(uint16_t id, const uint8_t* body, size_t n) {
        uint16_t rid = 0; uint8_t d[8]; size_t rn = sizeof(d);
        if (!link_.command(id, body, n, rid, d, rn, 200)) return false;
        return rid == id && rn >= 1 && d[0] != 0;
    }

    static void wr32(uint8_t* p, uint32_t v) {
        p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
    }
    // 0,1 Hz -> index CTCSS_Options du firmware (le plus proche).
    static uint8_t ctcssIdx(uint16_t v) {
        static const uint16_t t[50] = {
            670, 693, 719, 744, 770, 797, 825, 854, 885, 915,
            948, 974, 1000, 1035, 1072, 1109, 1148, 1188, 1230, 1273,
            1318, 1365, 1413, 1462, 1514, 1567, 1598, 1622, 1655, 1679,
            1713, 1738, 1773, 1799, 1835, 1862, 1899, 1928, 1966, 1995,
            2035, 2065, 2107, 2181, 2257, 2291, 2336, 2418, 2503, 2541
        };
        uint8_t best = 0; uint16_t err = 0xFFFF;
        for (uint8_t i = 0; i < 50; i++) {
            uint16_t e = (t[i] > v) ? (t[i] - v) : (v - t[i]);
            if (e < err) { err = e; best = i; }
        }
        return best;
    }

    UvK5Link link_;
    Preferences prefs_;
    Status   last_;
    RfParams vfo_[2];
    bool     flatForce_ = false;
    uint8_t  afGain_ = 0;
    char     fw_[24] = {0};
    uint8_t  proto_ = 0;
    uint8_t  txVfo_ = 0;
    bool     dualWatch_ = false;
    int8_t   rssiCorr_[7] = RSSI_DBM_CORR_TABLE;
    bool     monitor_ = false;
    bool     pttReq_ = false;
    bool     present_ = false;
    uint8_t  fails_ = 0;
    uint32_t lastPollMs_ = 0;
    uint32_t lastFloorMs_ = 0;
    float    floorDbm_ = RADIO_SQ_FLOOR_MIN_DBM;
    int16_t  sqThrDbm_ = 0;
    // S-mètre (recommandation IARU Région 1 R.1, VHF/UHF) : S9 = -93 dBm, 6 dB par point
    // en dessous (S1 = -141 dBm), puis +10, +20 ... +60 dB au-dessus de S9. Segments 0..15 :
    // 1..9 = S1..S9, 10..15 = S9+10 .. S9+60. L'appli refait le même calcul sur rssi_dbm.
    static uint8_t sMeterOf(int dbm)
    {
        int sm;
        if (dbm <= -93) sm = (dbm + 147) / 6;
        else            sm = 9 + (dbm + 93) / 10;
        if (sm < 0) sm = 0;
        if (sm > 15) sm = 15;
        return (uint8_t)sm;
    }

    uint32_t lastSigMs_ = 0;
};
