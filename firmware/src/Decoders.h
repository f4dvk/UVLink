#pragma once
#include <Arduino.h>
#include <string.h>
#include "config.h"
#include "AudioStream.h"

extern "C" {
#include "sarsat_decoder.h"
#include "country_codes.h"
#include "aprs_rx.h"
#include "aprs_parse.h"
#include "gps_nmea.h"
#include "sonde_demod.h"
#include "sonde_sync.h"
#include "sonde_m10.h"
#include "sstv_demod.h"
#include "pocsag.h"
#include "ookpulse.h"
}

// ============================================================================
// Décodeurs (lib/decoders, code C du projet RP2040 repris tel quel) :
//   - SARSAT 406 1G : capture continue en anneau, fenêtre de 1 s réanalysée
//     toutes les 250 ms, détection de salve relative au plancher de souffle,
//     AGC, trancheur + BCH -> EVENT SARSAT (struct) + lignes de texte.
//   - APRS : AFSK 1200 / AX.25 en continu -> EVENT APRS (struct RXINFO) +
//     trame brute sur DATA.
//   - Sondes : RS41 (4800 Bd) + M10/M20 (9615 Bd) en continu -> EVENT SONDE +
//     lignes de texte.
//   - SSTV Scottie 1 : chaque ligne décodée part en RGB565 (3 notifications).
//   - POCSAG 512/1200/2400 (mode POCSAG, ADC 32 kHz comme la phonie, audio
//     plat forcé) : port du md_pocsag.c de Portsdown 4 -> EVENT POCSAG
//   - ISM 433/868 (mode ISM, ADC 32 kHz, audio plat forcé) : détecteur
//     d'impulsions OOK sur le souffle du discriminateur (lib/decoders/ookpulse.c)
//     -> EVENT ISM (durées en µs) ; l'appli les donne à rtl_433 (format .ook)
//     (adresse, fonction, texte ; fragmenté si > 170 caractères).
//   - GPS NMEA (UART, depuis loop()) -> EVENT GPS, GET_GPS, STATUS.
//
// L'ADC (AudioIn, propriété d'AudioStream) tourne à la cadence du mode ; les
// blocs bruts 12 bits arrivent par le "tap" d'AudioStream (tâche ADC) et sont
// mis dans un anneau ; une tâche dédiée ("decode", cœur 1, priorité 4) fait
// le travail et envoie les EVENT. Le changement de mode est demandé depuis
// loop() (SET_MODE / console) et appliqué par la tâche elle-même.
// ============================================================================
class Decoders {
public:
    typedef bool (*Send)(const uint8_t* d, size_t n);
    enum Mode : uint8_t { PHONIE = 0, APRS = 1, SARSAT = 2, SONDE = 3, SSTV = 4, POCSAG = 5, ISM = 6 };
    enum EvType : uint8_t { EV_LEVEL = 1, EV_SARSAT = 2, EV_APRS = 3, EV_SONDE = 4, EV_SSTV = 5, EV_GPS = 6,
                            EV_TEXT = 7, EV_SSTV_INFO = 8, EV_POCSAG = 9, EV_ISM = 10 };
    enum TextSrc : uint8_t { TXT_SARSAT = 2, TXT_APRS = 3, TXT_SONDE = 4 };
    enum Verdict : uint8_t { LVL_OK = 0, LVL_CLIP = 1, LVL_BIAS = 2, LVL_HOT = 3, LVL_LOW = 4 };

    static constexpr uint32_t kRingMask = DEC_RING_SAMPLES - 1;
    static constexpr int      kWindow   = DEC_SARSAT_RATE_HZ * DEC_SARSAT_WINDOW_MS / 1000;   // 16000
    static constexpr int      kHop      = DEC_SARSAT_RATE_HZ * DEC_SARSAT_HOP_MS / 1000;      // 4000
    static_assert((DEC_RING_SAMPLES & kRingMask) == 0, "DEC_RING_SAMPLES doit etre une puissance de 2");
    static_assert((kWindow + kHop) * 2 <= DEC_RING_SAMPLES, "l'anneau (32 kHz) doit contenir une fenetre SARSAT + un saut (16 kHz x 2)");

    void begin(AudioStream& audio, Send event, Send data) {
        audio_ = &audio; evSend_ = event; dataSend_ = data;
        gps_init(&gps_);
        audio.setTap(&Decoders::tapEntry, this);
        xTaskCreatePinnedToCore(taskEntry, "decode", DEC_TASK_STACK, this, 4, &task_, 1);
    }

    // ---- depuis loop() ----
    // La bascule se fait ici, dans la tâche appelante (loopTask) : le pilote ADC
    // protège start/stop par un mutex FreeRTOS pris par la tâche qui a appelé
    // adc_digi_start() (setup), et un mutex ne se rend que depuis cette tâche.
    // La tâche "decode" se range pendant ce temps (switching_ / idle_).
    void setMode(uint8_t m) {
        if (m > ISM) return;
        if (m == SARSAT || m == SONDE) {             // modes retirés (2026-10-30) : décodage en phonie, bande 400-440 MHz
            Serial.printf("[dec] mode %s retire : decodage automatique en phonie\n", modeName(m));
            m = PHONIE;
        }
        switching_ = true;
        while (task_ && !idle_) delay(1);
        applyMode(m);
        switching_ = false;
        if (task_) xTaskNotifyGive(task_);
    }
    uint8_t mode() const       { return mode_; }
    // Décodeurs en phonie (DEC_PHONIE_DECODERS) : activés par main.cpp quand le
    // VFO reçu est dans la bande DEC_PHONIE_BAND (sondes M10/M20 + RS41, SARSAT).
    void setPhonieDecoders(bool on) {
        if (on == phonieDec_) return;
        phonieDec_ = on;
        if (mode_ == PHONIE) {
            if (on) {
                switching_ = true; while (task_ && !idle_) delay(1);
                initSondeChain(); initSarsat(2); winAt_ = widx_; ridx_ = widx_;
                switching_ = false; if (task_) xTaskNotifyGive(task_);
            }
            Serial.printf("[dec] decodeurs en phonie %s (ADC %.0f Hz)\n", on ? "ACTIFS (sondes + SARSAT, bande 400-440 MHz)" : "arretes", (double)audio_->in().actualRate());
        }
    }
    bool phonieDecoders() const { return phonieDec_; }
    // APRS en phonie (VFO écouté sur 144,800 MHz) : chaîne AFSK 1200 à 16 kHz
    // (anneau 32 kHz moyenné par 2), mêmes EVENT / DATA que le mode APRS.
    void setPhonieAprs(bool on) {
        if (on == phonieAprs_) return;
        phonieAprs_ = on;
        if (mode_ == PHONIE) {
            if (on) {
                switching_ = true; while (task_ && !idle_) delay(1);
                aprs_rx_init(&aprs_, DEC_SARSAT_RATE_HZ, &Decoders::aprsPacketEntry, this); aprsPair_ = -1;
                if (!phonieDec_ && !phonieSstv_) { winAt_ = widx_; ridx_ = widx_; }
                switching_ = false; if (task_) xTaskNotifyGive(task_);
            }
            Serial.printf("[dec] APRS en phonie %s (144,800 MHz, AFSK 1200 a 16 kHz)\n", on ? "ACTIF" : "arrete");
        }
    }
    bool phonieAprs() const { return phonieAprs_; }
    // SSTV en phonie (DEC_PHONIE_SSTV) : toujours actif, sur l'anneau 32 kHz tel quel.
    static constexpr bool phonieSstv_ = DEC_PHONIE_SSTV != 0;
    void initSstv(uint32_t nominalHz) {
        sstv_demod_init(&sstv_, nominalHz);
        sstv_demod_set_rate(&sstv_, (uint32_t)(audio_->in().actualRate() + 0.5f));   // cadence REELLE (48 077 / 32 051 Hz) : sinon l'image penche
        sstvLines_ = 0; lastSstvInfoMs_ = 0;
    }
    void initSarsat(uint8_t decim) {
        sarsatDecim_ = decim;
        noiseRms_ = 0; noiseSeeded_ = false; seedCount_ = 0; quietRun_ = 0; rearmAt_ = 0; pendingValid_ = false; lastId_[0] = 0; lastDecodeMs_ = 0;
    }
    void initSondeChain() {
        float fs = audio_->in().actualRate();           // cadence RÉELLE (80 645 ou 32 051 Hz)
        sonde_demod_init(&sdemod_, (int)(fs + 0.5f), 4800);
        sonde_sync_init(&ssync_);
        // capture dans un tampon dédié (32 kHz : 1616 symboles x 3,34 = 5 400 ech) ; win_ reste au SARSAT.
        sonde_m10_rx_init(&m10rx_, fs, m10buf_, (int)(sizeof m10buf_ / sizeof m10buf_[0]));
        nextM10PushMs_ = 0;
    }
    sonde_m10_rx_t& m10rx()    { return m10rx_; }   // diagnostic console (compteurs, dernière capture)
    // Anneau brut (12 bits) : accès pour le vidage console 'Z' (à faire en mode
    // PHONIE, le tap n'écrit plus : le contenu reste figé).
    uint16_t ringRaw(uint32_t i) const { return ring_[i & kRingMask]; }
    uint32_t ringWriteIndex() const    { return widx_; }
    static constexpr uint32_t ringSize() { return kRingMask + 1; }
    static uint32_t rateFor(uint8_t m) {
        switch (m) {
        case APRS:   return DEC_APRS_RATE_HZ;

        case SSTV:   return DEC_SSTV_RATE_HZ;
        default:     return AUDIO_ADC_RATE_HZ;      // phonie, POCSAG et ISM : suréchantillonnage, moyenné à 8 kHz par AudioStream
        }
    }
    static const char* modeName(uint8_t m) {
        static const char* const n[] = { "PHONIE", "APRS", "SARSAT", "SONDE", "SSTV", "POCSAG", "ISM" };
        return m <= ISM ? n[m] : "?";
    }

    void gpsFeed(char c) { if (gps_feed(&gps_, c)) gpsSeen_ = true; }
    const gps_fix_t& gpsFix() const { return gps_.fix; }
    bool  gpsSeen() const { return gpsSeen_; }
    void  setMyPosition(int32_t latE5, int32_t lonE5) { myLat_ = latE5; myLon_ = lonE5; }

    // GET_GPS / EVENT GPS : flags, lat_e5:i32, lon_e5:i32, speed_kmh:u16, course:u16, alt_m:i16, sats, h, m, s (20 o)
    size_t buildGps(uint8_t* o) const {
        const gps_fix_t& f = gps_.fix;
        size_t k = 0;
        o[k++] = (f.valid ? 1 : 0) | (gpsSeen_ ? 2 : 0);
        put32(o + k, (uint32_t)f.lat_e5); k += 4;
        put32(o + k, (uint32_t)f.lon_e5); k += 4;
        put16(o + k, f.speed_kmh); k += 2;
        put16(o + k, f.course_deg); k += 2;
        put16(o + k, (uint16_t)f.alt_m); k += 2;
        o[k++] = f.sats; o[k++] = f.utc_h; o[k++] = f.utc_m; o[k++] = f.utc_s;
        return k;
    }

    void poll() {                                   // périodique, depuis loop()
        uint32_t now = millis();
        if (gpsSeen_ && now - lastGpsMs_ >= GPS_PUSH_MS) {
            lastGpsMs_ = now;
            uint8_t b[24]; b[0] = EV_GPS;
            size_t n = buildGps(b + 1);
            if (evSend_) evSend_(b, n + 1);
        }
    }

    // Injection d'échantillons de test (console 'F') : PCM 16 bits signé à la
    // cadence du mode, à la place de l'ADC tant que inject est vrai. Au début,
    // le décodeur repart de zéro (sans poste, l'entrée ADC flottante lui a
    // peut-être fait "voir" une image / une trame en cours).
    void setInject(bool on) {
        if (on && !inject_) {
            switching_ = true;
            while (task_ && !idle_) delay(1);
            inject_ = true;
            resetDecoder(mode_);
            switching_ = false;
            if (task_) xTaskNotifyGive(task_);
        } else inject_ = on;
    }
    void injectPcm(const int16_t* s, size_t n) {
        uint16_t raw[256];
        while (n) {
            size_t k = n > 256 ? 256 : n;
            for (size_t i = 0; i < k; i++) { int v = 2048 + (s[i] >> 4); raw[i] = (uint16_t)(v < 0 ? 0 : v > 4095 ? 4095 : v); }
            push(raw, k);
            s += k; n -= k;
        }
    }
    uint32_t backlog() const {                       // échantillons pas encore traités
        return widx_ - ridx_;
    }
    uint32_t processed() const { return processed_; }   // échantillons passés au décodeur (modes continus)
    uint8_t  taskState() const { return taskState_; }    // 0 attente, 1 service, 2 envoi EVENT (diagnostic)

    // ---- statistiques (console) ----
    struct Stats { uint32_t sarsatWindows, sarsatBursts, sarsatDecoded, aprsPackets, aprsFcsBad, rs41Ok, rs41Bad,
                   m10Hits, m10Gps, sstvLines, pocsagMsgs, ismPackets, ismPulses, overruns, events; int floorRms, lastPeak, lastRms; };
    const Stats& stats() const { return st_; }
    const pocsag_t& pocsag() const { return pocsag_; }   // diagnostic console (synchros, mots incorrigibles, douteux cachés)

    // Analyse hors ligne d'une salve SARSAT (console) : X arme, la prochaine
    // fenêtre de crête >= DEC_SARSAT_PEAK_ON est gardée dans win_ ; Q la vide.
    void sarsatArm() { sarsatArm_ = true; sarsatHold_ = false; }
    bool sarsatHeld() const { return sarsatHold_; }
    void sarsatRelease() { sarsatHold_ = false; sarsatArm_ = false; }
    const int32_t* sarsatWin() const { return win_; }
    static constexpr int sarsatWinLen() { return kWindow; }

private:
    // ---------------- tap ADC (tâche ADC) ----------------
    static void tapEntry(const uint16_t* raw, size_t n, void* ctx) {
        Decoders* d = static_cast<Decoders*>(ctx);
        if (!d->inject_ && !d->switching_) d->push(raw, n);
    }
    void push(const uint16_t* raw, size_t n) {
        if (mode_ == PHONIE && !phonieDec_ && !phonieAprs_ && !phonieSstv_) return;
        for (size_t i = 0; i < n; i++) {
            int r = raw[i] & 0x0FFF;
            ring_[widx_ & kRingMask] = (uint16_t)r;
            widx_++;
            // niveau (pour EVENT LEVEL) : composante continue lente (~2 s), crête, carré, rails
            dcQ8_ += (((int32_t)r << 8) - dcQ8_) >> 15;
            int32_t s = ((int32_t)r - (dcQ8_ >> 8)) << DEC_GAIN_SHIFT;
            if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
            int32_t a = s < 0 ? -s : s;
            if (a > lv_.peak) lv_.peak = a;
            if (r < lv_.min) lv_.min = r;
            if (r > lv_.max) lv_.max = r;
            if (r == 0 || r == 4095) lv_.clipped++;
            lv_.sq += (uint64_t)((int64_t)s * s);
            lv_.n++;
        }
        if (task_) xTaskNotifyGive(task_);
    }

    // ---------------- tâche de décodage ----------------
    static void taskEntry(void* p) { static_cast<Decoders*>(p)->task(); }
    void task() {
        for (;;) {
            idle_ = true; taskState_ = 0;
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
            idle_ = false; taskState_ = 1;
            if (switching_) continue;
            switch (mode_) {
            case APRS:   streamService(); break;
            case PHONIE: if (phonieDec_ || phonieAprs_ || phonieSstv_) { streamService(); if (phonieDec_) sarsatService(); }
                         else { idle_ = true; ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100)); idle_ = false; } break;
            case SSTV:   streamService(); break;
            case POCSAG: streamService(); break;
            case ISM:    streamService(); break;
            default: break;
            }
            uint32_t now = millis();
            if (mode_ != PHONIE && now - lastLevelMs_ >= DEC_LEVEL_MS) { lastLevelMs_ = now; sendLevel(); }
        }
    }

    void applyMode(uint8_t m) {
        mode_ = PHONIE;                              // le tap n'écrit plus pendant la bascule
        audio_->setInputRate(rateFor(m));
        resetDecoder(m);
        mode_ = m;
        Serial.printf("[dec] mode %s (ADC %lu Hz)\n", modeName(m), (unsigned long)rateFor(m));
    }

    // État du décodeur du mode m remis à zéro (anneau compris). Tâche "decode" rangée.
    void resetDecoder(uint8_t m) {
        widx_ = ridx_ = winAt_ = 0;
        lv_ = Level();
        dcQ8_ = 2048 << 8;
        switch (m) {
        case APRS:
            aprs_rx_init(&aprs_, DEC_APRS_RATE_HZ, &Decoders::aprsPacketEntry, this);
            break;
        case PHONIE:
            if (phonieDec_) { initSondeChain(); initSarsat(2); }
            if (phonieAprs_) { aprs_rx_init(&aprs_, DEC_SARSAT_RATE_HZ, &Decoders::aprsPacketEntry, this); aprsPair_ = -1; }
            if (phonieSstv_) initSstv(AUDIO_ADC_RATE_HZ);
            break;
        case SSTV:
            initSstv(DEC_SSTV_RATE_HZ);
            break;
        case POCSAG:
            pocsag_init(&pocsag_, audio_->in().actualRate(), DEC_POCSAG_BAUDS, &Decoders::pocsagEntry, this);   // cadence RÉELLE (32 051 Hz)
            pocsag_set_show_partial(&pocsag_, DEC_POCSAG_SHOW_PARTIAL != 0);
            break;
        case ISM:
            ook_init(&ook_, audio_->in().actualRate(), &Decoders::ismEntry, this);
            break;
        default: break;
        }
    }

    // ---- modes en continu (APRS, sondes, SSTV) : on vide l'anneau échantillon par échantillon ----
    void streamService() {
        if (widx_ - ridx_ > DEC_RING_SAMPLES) {       // dépassement : on saute au plus récent
            st_.overruns++;
            ridx_ = widx_ - DEC_RING_SAMPLES / 2;
        }
        while (ridx_ != widx_) {
            uint16_t raw = ring_[ridx_ & kRingMask];
            ridx_++; processed_++;
            int32_t s = ((int32_t)raw - (dcQ8_ >> 8)) << DEC_GAIN_SHIFT;
            if (mode_ == APRS) {
                aprs_rx_sample(&aprs_, raw);
            } else if (mode_ == PHONIE) {
                if (phonieAprs_) {                    // 32 kHz -> 16 kHz : moyenne de 2 (comme les fenêtres SARSAT)
                    if (aprsPair_ < 0) aprsPair_ = raw;
                    else { aprs_rx_sample(&aprs_, (uint16_t)((aprsPair_ + raw) >> 1)); aprsPair_ = -1; }
                }
                if (phonieSstv_) sstvStep(s);
                if (!phonieDec_) { if (switching_) return; continue; }
                uint8_t bit;
                if (sonde_demod_sample(&sdemod_, s, &bit)) {
                    sonde_rs41_result_t r;
                    sonde_evt_t e = sonde_sync_feed(&ssync_, bit, &r);
                    if (e == SONDE_EVT_RS41) {
                        if (r.n_blocks > 0) { st_.rs41Ok++; pushRs41(r); } else st_.rs41Bad++;
                    } else if (e == SONDE_EVT_M10) {
                        st_.m10Hits++; pushM10Header();
                    }
                }
                m10_gps_t g;
                if (sonde_m10_rx_sample(&m10rx_, s, &g)) {
                    if (g.has_position) { st_.m10Gps++; pushM10Gps(g); }
                    else if (m10rx_.frame_len) Serial.printf("[sonde] trame M10/M20 %02X %02X valide sans position\n", m10rx_.frame[0], m10rx_.frame[1]);
                }
            } else if (mode_ == SSTV) {
                sstvStep(s);
            } else if (mode_ == POCSAG) {
                pocsag_sample(&pocsag_, (float)s);
            } else if (mode_ == ISM) {
                ook_sample(&ook_, (float)s);
            }
            if (switching_) return;                  // bascule demandée : on sort vite
        }
    }

    // ---- SARSAT : fenêtre glissante (copie de la boucle principale du RP2040) ----
    void sarsatService() {
        const uint32_t hopR = (uint32_t)kHop * sarsatDecim_, winR = (uint32_t)kWindow * sarsatDecim_;   // unités anneau
        if (widx_ - winAt_ > DEC_RING_SAMPLES - hopR) { st_.overruns++; winAt_ = widx_ - winR; }
        while (widx_ - winAt_ >= hopR) {
            winAt_ += hopR;
            if (winAt_ < winR) continue;               // pas encore une fenêtre entière
            sarsatWindow(winAt_);
            if (switching_) return;
        }
    }

    void sarsatWindow(uint32_t end) {
        if (sarsatHold_) return;                      // fenêtre gardée pour 'Q' (analyse hors ligne)
        st_.sarsatWindows++;
        // anneau -> fenêtre int32 centrée, gain ; statistiques
        const long dc = dcQ8_ >> 8;
        int peak = 0, rawMin = 4095, rawMax = 0, clipped = 0;
        uint64_t sq = 0;
        uint32_t start = end - (uint32_t)kWindow * sarsatDecim_;
        // Passe-haut 1er ordre (~40 Hz) AVANT le gain : en audio plat (AF 9, AFC
        // coupé) le discriminateur sort un échelon continu à la capture de la
        // porteuse 406 (décalage de fréquence balise/poste : mesuré ~+2000 LSB
        // sur la balise d'essai 434,2 MHz, 2026-10-30). Sans ce filtre, l'AGC
        // de fenêtre se normalisait sur cet échelon et écrêtait les données à
        // 32767 : aucune synchro. L'échelon devient un transitoire de ~5 ms,
        // absorbé dans les 160 ms de porteuse pure qui ouvrent la salve.
        long hpY = 0, hpXprev = 0;                   // y = x - x1 + a*y1, a = 0.984 (Q10 : 1008/1024)
        for (int i = 0; i < kWindow; i++) {
            int raw = sarsatDecim_ == 2
                    ? ((int)ring_[(start + 2 * i) & kRingMask] + (int)ring_[(start + 2 * i + 1) & kRingMask]) / 2   // 32 kHz -> 16 kHz
                    : ring_[(start + i) & kRingMask];
            if (raw < rawMin) rawMin = raw;
            if (raw > rawMax) rawMax = raw;
            if (raw == 0 || raw == 4095) clipped++;
            long x = ((long)raw - dc) << 8;          // Q8
            if (i == 0) hpXprev = x;
            hpY = x - hpXprev + ((hpY * 1008) >> 10);
            hpXprev = x;
            long s = (hpY >> 8) << DEC_GAIN_SHIFT;
            if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
            win_[i] = (int32_t)s;
            int a = s < 0 ? (int)-s : (int)s;
            if (a > peak) peak = a;
            sq += (uint64_t)((int64_t)s * s);
        }
        int rms = (int)isqrt64(sq / (unsigned)kWindow);
        int clipPct = clipped * 100 / kWindow;
        st_.lastPeak = peak; st_.lastRms = rms;

        // Plancher de souffle : amorcé à la 4e fenêtre (DC et ADC stabilisés ;
        // amorcé à la 1re, il restait figé à 3x le bruit réel et TOUTES les
        // fenêtres passaient pour des salves, 2026-10-30), suivi sur les fenêtres
        // de souffle, et recalé si « calme » persiste 16 fenêtres (4 s : ce n'est
        // pas une salve, c'est le nouveau plancher).
        if (!noiseSeeded_) {
            if (++seedCount_ >= 4 && rms > 200) { noiseRms_ = rms; noiseSeeded_ = true; }
            st_.floorRms = noiseRms_;
            return;
        }
        long quietPct = (long)rms * 100 / (noiseRms_ > 1 ? noiseRms_ : 1);
        bool quieted  = quietPct < DEC_SARSAT_QUIET_PCT;
        if (quieted) {
            if (++quietRun_ >= 16) { Serial.printf("[sarsat] plancher recale %d -> %d\n", noiseRms_, rms); noiseRms_ = rms; quietRun_ = 0; quieted = false; }
        } else quietRun_ = 0;
        bool isBurst  = clipPct == 0 && rms <= DEC_SARSAT_MAX_RMS && (quieted || peak >= DEC_SARSAT_PEAK_ON);
        if (!isBurst) {
            if (!quieted) noiseRms_ += (rms - noiseRms_) >> 5;   // suivi du plancher (~8 s)
            st_.floorRms = noiseRms_;
            return;
        }
        st_.sarsatBursts++;
        if (sarsatArm_ && peak >= DEC_SARSAT_PEAK_ON) { sarsatArm_ = false; sarsatHold_ = true; }   // console 'X' : on garde cette fenêtre
        if (end < rearmAt_) return;                   // réarmement après un décodage

#if DEC_SARSAT_AGC_TARGET
        if (rms > 100) {
            for (int i = 0; i < kWindow; i++) {
                long v = (long)win_[i] * DEC_SARSAT_AGC_TARGET / rms;
                win_[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : (int32_t)v;
            }
        }
#endif
        sarsat_result_t& r = res_;
        int rc = sarsat_decode_window(win_, kWindow, DEC_SARSAT_RATE_HZ, &r);
        if (rc == 0) return;                          // pas de synchro
        Serial.printf("[sarsat] crete=%d rms=%d (%ld%% du plancher %d) -> %d bits %s\n",
                      peak, rms, quietPct, noiseRms_, r.frame_bits, r.raw_hex);
        if (rc < 0) { Serial.println("[sarsat] BCH incorrigible, trame rejetee"); return; }

        bool allZero = true;
        for (const char* p = r.hex_id; *p; p++) if (*p != '0') { allZero = false; break; }
        if (allZero) { Serial.println("[sarsat] BCH OK mais ID nul : faux positif"); rearmAt_ = end + DEC_SARSAT_RATE_HZ / 2; return; }

        // trame corrigée par le BCH : exigée identique dans une 2e fenêtre
        if (r.info.bch1_fixed || r.info.bch2_fixed) {
            bool confirmed = pendingValid_ && strcmp(r.hex_id, pendingId_) == 0 &&
                             r.info.lat == pendingLat_ && r.info.lon == pendingLon_ &&
                             end - pendingAt_ < (uint32_t)kWindow;
            if (!confirmed) {
                snprintf(pendingId_, sizeof(pendingId_), "%s", r.hex_id);
                pendingLat_ = r.info.lat; pendingLon_ = r.info.lon; pendingAt_ = end; pendingValid_ = true;
                Serial.printf("[sarsat] BCH corrige %u+%u bit : attente d'une 2e fenetre\n", r.info.bch1_fixed, r.info.bch2_fixed);
                return;
            }
            pendingValid_ = false;
        }

        uint32_t now = millis();
        bool repeat = strcmp(r.hex_id, lastId_) == 0 && lastDecodeMs_ && now - lastDecodeMs_ < DEC_SARSAT_DEDUP_S * 1000UL;
        snprintf(lastId_, sizeof(lastId_), "%s", r.hex_id);
        lastDecodeMs_ = now;
        st_.sarsatDecoded++;
        Serial.printf("[sarsat] t=%lu BCH OK  ID=%s pays=%u (%s) proto=%u %s%s%s\n", (unsigned long)millis(), r.hex_id, r.info.country_code,
                      get_country_name(r.info.country_code), r.info.protocol_bits,
                      r.info.is_test_message ? "TEST " : "", r.info.has_position ? "" : "(sans position) ",
                      repeat ? "(repetition)" : "");
        pushSarsat(r, repeat);
        rearmAt_ = end + DEC_SARSAT_RATE_HZ * DEC_SARSAT_REARM_MS / 1000;
    }

    // EVENT SARSAT : struct SARSAT_BEACON du RP2040 (29 o) + repeat:u8 + bch_fixed:u8 + raw_hex (NUL)
    void pushSarsat(const sarsat_result_t& r, bool repeat) {
        const BeaconInfo1G& in = r.info;
        bool pos = in.has_position && !in.position_default;
        uint8_t p[80]; size_t k = 0;
        p[k++] = EV_SARSAT;
        p[k++] = (uint8_t)r.frame_bits;
        p[k++] = (uint8_t)in.protocol;
        p[k++] = in.is_test_message ? 1 : 0;
        p[k++] = pos ? 1 : 0;
        put16(p + k, in.country_code); k += 2;
        put32(p + k, (uint32_t)(int32_t)(pos ? in.lat * 100000.0 : 0)); k += 4;
        put32(p + k, (uint32_t)(int32_t)(pos ? in.lon * 100000.0 : 0)); k += 4;
        memset(p + k, 0, 15);
        for (int i = 0; i < 15 && r.hex_id[i]; i++) p[k + i] = (uint8_t)r.hex_id[i];
        k += 15;
        p[k++] = repeat ? 1 : 0;
        p[k++] = (uint8_t)(in.bch1_fixed + in.bch2_fixed);
        size_t hl = strnlen(r.raw_hex, sizeof(r.raw_hex) - 1);
        memcpy(p + k, r.raw_hex, hl); k += hl; p[k++] = 0;
        event(p, k);
        // En phonie, seul l'EVENT binaire part : les 15 lignes « écran » qui
        // suivaient (16 notifications en 640 ms, flux audio bloqué pendant ce
        // temps puis relâché d'un coup) faisaient décrocher le contrôleur du PC
        // à chaque trame reçue (2026-10-30, balise d'essai toutes les 10 s).
        if (mode_ == PHONIE) return;
        pushText(TXT_SARSAT, 0xFF, 0, "");
        for (int i = 0; i < r.n_lines; i++) pushText(TXT_SARSAT, (uint8_t)i, i == 0, r.lines[i]);
    }

    // ---- APRS ----
    static void aprsPacketEntry(const uint8_t* ax25, int len, void* user) { static_cast<Decoders*>(user)->onAprsPacket(ax25, len); }
    void onAprsPacket(const uint8_t* ax25, int len) {
        st_.aprsPackets++;
        // trame brute sur DATA (fragments : frag:u8, b7 = dernier, b0-6 = index)
        if (dataSend_) {
            uint8_t f[244]; int idx = 0;
            for (int off = 0; off < len; idx++) {
                int n = len - off; if (n > 243) n = 243;
                f[0] = (uint8_t)(idx | (off + n >= len ? 0x80 : 0));
                memcpy(f + 1, ax25 + off, n);
                dataSend_(f, n + 1);
                off += n;
            }
        }
        aprs_info_t ai;
        bool parsed = aprs_parse(ax25, len, &ai);
        if (parsed && (ai.has_pos || ai.is_adrasec || ai.kind == APRS_KIND_STATUS || ai.kind == APRS_KIND_MESSAGE)) {
            uint32_t distM = 0; uint16_t brg = 0; bool hasDist = false;
            int32_t myLat, myLon; myPosition(myLat, myLon);
            if ((ai.has_pos || ai.is_adrasec) && (myLat || myLon))
                hasDist = aprs_geo(myLat, myLon, ai.lat_e5, ai.lon_e5, &distM, &brg);
            char via[28]; ax25_via_str(ax25, len, via, sizeof via);
            uint8_t p[200]; size_t n = 0;
            p[n++] = EV_APRS;
            p[n++] = ai.kind;
            p[n++] = (uint8_t)((ai.has_pos ? 1 : 0) | (ai.has_course ? 2 : 0) | (ai.has_alt ? 4 : 0) | (hasDist ? 8 : 0) | (ai.is_adrasec ? 16 : 0));
            p[n++] = (uint8_t)ai.sym_table; p[n++] = (uint8_t)ai.sym_code;
            put32(p + n, (uint32_t)ai.lat_e5); n += 4;
            put32(p + n, (uint32_t)ai.lon_e5); n += 4;
            put16(p + n, (uint16_t)ai.course_deg); n += 2;
            put16(p + n, (uint16_t)ai.speed_kmh); n += 2;
            put16(p + n, (uint16_t)(int16_t)ai.alt_m); n += 2;
            put16(p + n, (uint16_t)(distM / 100 > 65535 ? 65535 : distM / 100)); n += 2;
            put16(p + n, brg); n += 2;
            n = putStr(p, n, ai.src, 100); n = putStr(p, n, ai.name, 112); n = putStr(p, n, via, 140); n = putStr(p, n, ai.text, BLE_NOTIFY_MAX);
            Serial.printf("[aprs] %s %s%s %s%s\n", ai.src, via[0] ? "via " : "direct", via, ai.has_pos ? "pos " : "", ai.text);
            event(p, n);
            return;
        }
        char lines[4][SARSAT_LINE_CHARS + 1];
        int nl = aprs_rx_format(ax25, len, (char*)lines, 4, SARSAT_LINE_CHARS - 1);
        Serial.printf("[aprs] %s\n", nl > 0 ? lines[0] : "?");
        pushText(TXT_APRS, 0xFF, 0, "");
        for (int i = 0; i < nl && i < 4; i++) pushText(TXT_APRS, (uint8_t)i, 0, lines[i]);
    }

    void myPosition(int32_t& lat, int32_t& lon) const {
        if (gps_.fix.valid) { lat = gps_.fix.lat_e5; lon = gps_.fix.lon_e5; }
        else { lat = myLat_; lon = myLon_; }
    }

    // ---- sondes ----  EVENT SONDE : kind:u8 (1 RS41, 2 en-tête M10/M20, 3 M10/M20 GPS), has_pos, lat_e5, lon_e5, alt_m:i32
    // + télémétrie optionnelle (fw 0.8) : type:u8, temp_c10:i16, rh:u8, vh_dms:u16, vv_dms:i16,
    //   heading:u16, sats:u8, hh:u8, mm:u8, ss:u8, sn[12]  (38 o au total)
    void pushSondeStruct(uint8_t kind, bool hasPos, int32_t lat, int32_t lon, int32_t alt, const m10_gps_t* g = nullptr) {
        uint8_t p[48]; size_t k = 0;                 // 3 + 12 + 25 (télémétrie) = 40 o
        p[k++] = EV_SONDE; p[k++] = kind; p[k++] = hasPos ? 1 : 0;
        put32(p + k, (uint32_t)lat); k += 4; put32(p + k, (uint32_t)lon); k += 4; put32(p + k, (uint32_t)alt); k += 4;
        if (g) {
            p[k++] = g->type;
            p[k++] = (uint8_t)g->temp_c10; p[k++] = (uint8_t)((uint16_t)g->temp_c10 >> 8);
            p[k++] = g->rh_pct;
            p[k++] = (uint8_t)g->vh_dms; p[k++] = (uint8_t)(g->vh_dms >> 8);
            p[k++] = (uint8_t)g->vv_dms; p[k++] = (uint8_t)((uint16_t)g->vv_dms >> 8);
            p[k++] = (uint8_t)g->heading_deg; p[k++] = (uint8_t)(g->heading_deg >> 8);
            p[k++] = g->sats; p[k++] = g->hh; p[k++] = g->mm; p[k++] = g->ss;
            for (int i = 0; i < 12; i++) p[k++] = (uint8_t)g->sn[i];
        }
        event(p, k);
    }
    void pushSondeLines(const char* head, bool hasPos, int32_t lat, int32_t lon, int32_t alt, bool withAlt) {
        char l[4][SARSAT_LINE_CHARS]; int n = 0;
        snprintf(l[n++], SARSAT_LINE_CHARS, "%s", head);
        if (!hasPos) snprintf(l[n++], SARSAT_LINE_CHARS, "position inconnue");
        else {
            snprintf(l[n++], SARSAT_LINE_CHARS, "%.4f,%.4f", lat / 100000.0, lon / 100000.0);
            if (withAlt) snprintf(l[n++], SARSAT_LINE_CHARS, "alt %ld m", (long)alt);
            int32_t myLat, myLon; myPosition(myLat, myLon);
            uint32_t distM; uint16_t brg;
            if ((myLat || myLon) && aprs_geo(myLat, myLon, lat, lon, &distM, &brg))
                snprintf(l[n++], SARSAT_LINE_CHARS, "%.1fkm  %03u deg", distM / 1000.0, brg);
        }
        pushText(TXT_SONDE, 0xFF, 0, "");
        for (int i = 0; i < n; i++) pushText(TXT_SONDE, (uint8_t)i, i == 0, l[i]);
    }
    void pushRs41(const sonde_rs41_result_t& r) {
        Serial.printf("[sonde] RS41 %s %.5f %.5f alt=%ld\n", r.has_position ? "pos" : "sans pos", r.lat_e5 / 1e5, r.lon_e5 / 1e5, (long)r.alt_m);
        pushSondeStruct(1, r.has_position, r.lat_e5, r.lon_e5, r.alt_m);
        pushSondeLines("RADIOSONDE RS41", r.has_position, r.lat_e5, r.lon_e5, r.alt_m, true);
    }
    void pushM10Header() {
        uint32_t now = millis();
        if (nextM10PushMs_ && now < nextM10PushMs_) return;
        nextM10PushMs_ = now + DEC_SONDE_M10_PUSH_MS;
        Serial.printf("[sonde] en-tete M10/M20 verrouille (#%lu)\n", (unsigned long)st_.m10Hits);
        if (mode_ == PHONIE) return;                 // pas de lignes texte ni d'EVENT « en-tête » en phonie (charge BLE)
        pushSondeStruct(2, false, 0, 0, 0);
        pushSondeLines("RADIOSONDE M10/M20", false, 0, 0, 0, false);
    }
    void pushM10Gps(const m10_gps_t& g) {
        Serial.printf("[sonde] %s GPS %.5f %.5f alt=%ld", g.type == 0x20 ? "M20" : "M10", g.lat_e5 / 1e5, g.lon_e5 / 1e5, (long)g.alt_m);
        if (g.temp_c10 != INT16_MIN) Serial.printf(" T=%.1fC", g.temp_c10 / 10.0);
        if (g.rh_pct != 255) Serial.printf(" RH=%u%%", g.rh_pct);
        if (g.vh_dms != 0xFFFF) Serial.printf(" HS=%.1f VS=%.1f m/s HDG=%u", g.vh_dms / 10.0, g.vv_dms / 10.0, g.heading_deg);
        if (g.hh != 255) Serial.printf(" %02u:%02u:%02u", g.hh, g.mm, g.ss);
        if (g.sn[0]) Serial.printf(" SN %s", g.sn);
        Serial.println();
        pushSondeStruct(3, true, g.lat_e5, g.lon_e5, g.alt_m, &g);
        // En PHONIE (sondes en tâche de fond) : l'EVENT seul. Les 7 notifications
        // de lignes texte par trame s'ajoutaient aux 25 paquets audio/s + STATUS
        // et le contrôleur BLE du PC (Intel 7265) décrochait toutes les 15 s
        // (2026-10-30 : « plantage au bout de quelques trames »). L'appli a tout
        // dans l'EVENT. En mode SONDE (pas d'audio) les lignes restent.
        if (mode_ == PHONIE) return;
        pushSondeLines(g.type == 0x20 ? "RADIOSONDE M20" : "RADIOSONDE M10", true, g.lat_e5, g.lon_e5, g.alt_m, true);
        // lignes de télémétrie (texte, même affichage que l'écran du poste)
        char l[SARSAT_LINE_CHARS]; int row = 4;
        if (g.vh_dms != 0xFFFF) { snprintf(l, sizeof l, "VS %+.1f  HS %.0f m/s  %03u deg", g.vv_dms / 10.0, g.vh_dms / 10.0, g.heading_deg); pushText(TXT_SONDE, (uint8_t)row++, false, l); }
        if (g.temp_c10 != INT16_MIN || g.rh_pct != 255) {
            int n = 0; l[0] = 0;
            if (g.temp_c10 != INT16_MIN) n += snprintf(l + n, sizeof l - n, "T %.1f C", g.temp_c10 / 10.0);
            if (g.rh_pct != 255) n += snprintf(l + n, sizeof l - n, "%sRH %u %%", n ? "  " : "", g.rh_pct);
            pushText(TXT_SONDE, (uint8_t)row++, false, l);
        }
        if (g.sn[0] || g.hh != 255) {
            int n = 0; l[0] = 0;
            if (g.sn[0]) n += snprintf(l + n, sizeof l - n, "SN %s", g.sn);
            if (g.hh != 255) n += snprintf(l + n, sizeof l - n, "%s%02u:%02u:%02u", n ? "  " : "", g.hh, g.mm, g.ss);
            if (g.sats != 255) n += snprintf(l + n, sizeof l - n, "  %u sat", g.sats);
            pushText(TXT_SONDE, (uint8_t)row++, false, l);
        }
    }

    // ---- SSTV (multimode fw 0.12 : Scottie 1/2/DX, Martin 1/2, PD 50..290, choisi par le VIS) ----
    void sstvStep(int32_t s) {
        bool ready = sstv_demod_sample(&sstv_, s);
        if (sstv_.image_new) { sstv_.image_new = false; pushSstvNewImage(); }
        if (ready) { for (int k = 0; k < sstv_.lines_ready; k++) { st_.sstvLines++; sstvLines_++; pushSstvLine(sstv_.line_ready_idx + k, sstv_demod_rgb_line(&sstv_, k)); } }
    }
    // Les modes plus larges que 320 px (PD 120/160/180/240/290) sont décodés en entier mais
    // transmis réduits de moitié (moyenne 2x2) : le lien BLE (~25 notifications/s) ne suivrait pas.
    int sstvDecim() const { return sstv_.mode && sstv_.mode->width > 320 ? 2 : 1; }
    // EVENT SSTV, nouvelle image : line = 0xFFFF, x0 = largeur transmise, npx = 0, puis (fw 0.12)
    // mode:u8, hauteur transmise:u16, largeur du mode:u16, hauteur du mode:u16
    void pushSstvNewImage() {
        const sstv_mode_t* m = sstv_.mode; int dec = sstvDecim();
        uint8_t p[16]; size_t k = 0;                 // 13 octets
        p[k++] = EV_SSTV; put16(p + k, 0xFFFF); k += 2; put16(p + k, (uint16_t)(m->width / dec)); k += 2; p[k++] = 0;
        p[k++] = m->id; put16(p + k, (uint16_t)(m->height / dec)); k += 2; put16(p + k, m->width); k += 2; put16(p + k, m->height); k += 2;
        event(p, k);
        sstvLines_ = 0; lastSstvInfoMs_ = millis();
        Serial.printf("[sstv] t=%lu nouvelle image %s %ux%u (VIS %u)%s%s\n", (unsigned long)millis(), m->name, m->width, m->height, m->vis,
                      dec > 1 ? ", transmise en demi-taille" : "", mode_ == PHONIE ? " en phonie" : "");
    }
    // EVENT SSTV : line:u16, x0:u16, npx:u8, rgb565[npx] — une ligne = largeur/80 notifications de 166 o
    void pushSstvLine(int line, const uint8_t* rgb) {
        const sstv_mode_t* m = sstv_.mode; if (!m) return;
        int dec = sstvDecim(), w = m->width / dec, h = m->height / dec;
        if (dec > 1) {
            // moyenne de 2 pixels de large ; les lignes impaires sont moyennées avec la ligne paire gardée
            if ((line & 1) == 0) { for (int x = 0; x < w; x++) for (int c = 0; c < 3; c++) sstvHalf_[x * 3 + c] = (uint8_t)((rgb[6 * x + c] + rgb[6 * x + 3 + c]) >> 1); return; }
            for (int x = 0; x < w; x++) for (int c = 0; c < 3; c++) sstvHalf_[x * 3 + c] = (uint8_t)((sstvHalf_[x * 3 + c] + ((rgb[6 * x + c] + rgb[6 * x + 3 + c]) >> 1)) >> 1);
            rgb = sstvHalf_; line >>= 1;
        }
        if (line == h - 1) Serial.printf("[sstv] t=%lu image complete : %u lignes recalees, %u synchros hors fenetre\n", (unsigned long)millis(), (unsigned)sstv_.n_resync, (unsigned)sstv_.n_resync_miss);
        static constexpr int kPx = 80;               // notifications de 166 o (<= BLE_NOTIFY_MAX)
        for (int x0 = 0; x0 < w; x0 += kPx) {
            int n = w - x0; if (n > kPx) n = kPx;
            uint8_t p[6 + kPx * 2]; size_t k = 0;
            p[k++] = EV_SSTV; put16(p + k, (uint16_t)line); k += 2; put16(p + k, (uint16_t)x0); k += 2; p[k++] = (uint8_t)n;
            for (int i = 0; i < n; i++) {
                uint8_t r = rgb[(x0 + i) * 3], g = rgb[(x0 + i) * 3 + 1], b = rgb[(x0 + i) * 3 + 2];
                uint16_t v = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
                put16(p + k, v); k += 2;
            }
            event(p, k);
        }
        uint32_t now = millis();
        if (line == h - 1 || now - lastSstvInfoMs_ >= 2000) { lastSstvInfoMs_ = now; pushSstvInfo(line == h - 1); }
    }
    // EVENT SSTV_INFO : mode:u8 (SSTV_MODE_* : 1 Scottie 1, 2 Scottie 2, 3 Scottie DX, 4 Martin 1, 5 Martin 2,
    //                   6 PD 50, 7 PD 90, 8 PD 120, 9 PD 160, 10 PD 180, 11 PD 240, 12 PD 290),
    //                   flags:u8 (b0 valide, b1 finale), lignes:u16 (décodées, taille du mode),
    //                   recalees:u16 (fw 0.11 : lignes recalées sur leur synchro ; avant : synchros vues), ecart_ppm:i32
    void pushSstvInfo(bool final) {
        int32_t ppm = 0; bool ok = sstv_demod_clock_error_ppm(&sstv_, 8, &ppm);
        uint8_t p[12]; size_t k = 0;
        p[k++] = EV_SSTV_INFO; p[k++] = sstv_.mode ? sstv_.mode->id : 0; p[k++] = (uint8_t)((ok ? 1 : 0) | (final ? 2 : 0));
        put16(p + k, (uint16_t)sstvLines_); k += 2; put16(p + k, (uint16_t)sstv_.n_resync); k += 2;
        put32(p + k, (uint32_t)ppm); k += 4;
        event(p, k);
    }

    // ---- POCSAG ----  EVENT POCSAG : frag:u8 (b7 dernier, b0-6 index), baud:u16, address:u32, function:u8,
    //      flags:u8 (b0 numérique, b1 bip seul, b2 douteux), corrected:u8, bad:u8, texte ASCII 7 bits (NUL)
    //      Le texte est découpé en fragments de 170 caractères (BLE_NOTIFY_MAX) ; l'appli recolle.
    static void pocsagEntry(const pocsag_msg_t* m, void* user) { static_cast<Decoders*>(user)->onPocsag(*m); }
    void onPocsag(const pocsag_msg_t& m) {
        st_.pocsagMsgs++;
        Serial.printf("[pocsag] %u Bd RIC %07lu F%u %s%s%s%s\n", m.baud, (unsigned long)m.address, m.function,
                      m.tone_only ? "(bip seul)" : m.numeric ? "num " : "txt ", m.tone_only ? "" : m.text,
                      m.bad ? " [mots incorrigibles]" : "", m.partial ? " [douteux]" : "");
        static constexpr size_t kHead = 11, kChunk = BLE_NOTIFY_MAX - 1 - kHead - 1;   // 170 caractères par notification
        size_t len = strlen(m.text), off = 0; int idx = 0;
        do {
            size_t n = len - off; if (n > kChunk) n = kChunk;
            uint8_t p[BLE_NOTIFY_MAX]; size_t k = 0;
            p[k++] = EV_POCSAG;
            p[k++] = (uint8_t)(idx | (off + n >= len ? 0x80 : 0));
            put16(p + k, m.baud); k += 2;
            put32(p + k, m.address); k += 4;
            p[k++] = m.function;
            p[k++] = (uint8_t)((m.numeric ? 1 : 0) | (m.tone_only ? 2 : 0) | (m.partial ? 4 : 0));
            p[k++] = m.corrected; p[k++] = m.bad;
            memcpy(p + k, m.text + off, n); k += n; p[k++] = 0;
            event(p, k);
            off += n; idx++;
        } while (off < len);
    }

    // ---- niveau audio ----  EVENT LEVEL : peak:u16, rms:u16, dc:u16, clip_pct:u8, verdict:u8, adc_min:u16, adc_max:u16, floor_rms:u16
    void sendLevel() {
        Level l = lv_; lv_ = Level();                 // prise + remise à zéro (course bénigne avec la tâche ADC)
        if (!l.n) return;
        int rms = (int)isqrt64(l.sq / l.n);
        int clipPct = (int)(l.clipped * 100 / l.n);
        int dc = dcQ8_ >> 8;
        uint8_t v = clipPct > 0 ? LVL_CLIP : (dc < 700 || dc > 3400) ? LVL_BIAS : (rms > 4500 || l.peak > 15000) ? LVL_HOT : rms < 800 ? LVL_LOW : LVL_OK;
        uint8_t p[16]; size_t k = 0;
        p[k++] = EV_LEVEL;
        put16(p + k, (uint16_t)(l.peak > 65535 ? 65535 : l.peak)); k += 2;
        put16(p + k, (uint16_t)(rms > 65535 ? 65535 : rms)); k += 2;
        put16(p + k, (uint16_t)dc); k += 2;
        p[k++] = (uint8_t)clipPct; p[k++] = v;
        put16(p + k, (uint16_t)l.min); k += 2; put16(p + k, (uint16_t)l.max); k += 2;
        put16(p + k, (uint16_t)(mode_ == SARSAT ? noiseRms_ : 0)); k += 2;
        event(p, k);
    }

    // ---- utilitaires ----
    void pushText(uint8_t src, uint8_t line, bool invert, const char* s) {
        uint8_t p[4 + SARSAT_LINE_CHARS]; size_t k = 0;
        p[k++] = EV_TEXT; p[k++] = src; p[k++] = line; p[k++] = invert ? 1 : 0;
        size_t sl = strnlen(s, SARSAT_LINE_CHARS - 1);
        memcpy(p + k, s, sl); k += sl;
        event(p, k);
    }
    bool eventsOn_ = true;                      // console 'E 0/1' : diagnostic (EVENT coupés)
public:
    void setEvents(bool on) { eventsOn_ = on; }
private:
    void event(const uint8_t* p, size_t n) {
        if (!eventsOn_) return;
        st_.events++;
        if (!evSend_) return;
        if (n > BLE_NOTIFY_MAX) n = BLE_NOTIFY_MAX;  // APRS : texte tronqué (chaînes NUL-terminées)
        taskState_ = 2;
        for (int essai = 0; essai < 4 && !evSend_(p, n); essai++) delay(5);
        taskState_ = 1;
    }
    static size_t putStr(uint8_t* p, size_t n, const char* s, size_t lim) {
        while (*s && n < lim - 1) p[n++] = (uint8_t)*s++;
        p[n++] = 0;
        return n;
    }
    static void put16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
    static void put32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
    static uint32_t isqrt64(uint64_t v) {
        uint64_t r = 0, bit = 1ULL << 62;
        while (bit > v) bit >>= 2;
        while (bit) { if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; } else r >>= 1; bit >>= 2; }
        return (uint32_t)r;
    }

    struct Level { int32_t peak = 0; int min = 4095, max = 0; uint32_t clipped = 0, n = 0; uint64_t sq = 0; };

    AudioStream* audio_ = nullptr;
    Send evSend_ = nullptr, dataSend_ = nullptr;
    TaskHandle_t task_ = nullptr;
    volatile uint8_t mode_ = PHONIE;
    volatile bool inject_ = false, switching_ = false, idle_ = false;

    uint16_t ring_[DEC_RING_SAMPLES];
    volatile uint32_t widx_ = 0, processed_ = 0;
    volatile uint8_t  taskState_ = 0;
    uint32_t ridx_ = 0, winAt_ = 0;
    int32_t  dcQ8_ = 2048 << 8;
    Level    lv_;
    uint32_t lastLevelMs_ = 0;
    Stats    st_ = {};

    // SARSAT
    int32_t  win_[kWindow];
    sarsat_result_t res_;
    int      noiseRms_ = 0; bool noiseSeeded_ = false;
    uint8_t  seedCount_ = 0, quietRun_ = 0;
    bool     sarsatArm_ = false, sarsatHold_ = false;   // console X / x : garder la prochaine fenêtre forte
    uint32_t rearmAt_ = 0;
    bool     pendingValid_ = false; char pendingId_[24] = {}; double pendingLat_ = 0, pendingLon_ = 0; uint32_t pendingAt_ = 0;
    char     lastId_[24] = {}; uint32_t lastDecodeMs_ = 0;

    // APRS
    aprs_rx_t aprs_;

    // sondes
    sonde_demod_t sdemod_; sonde_sync_t ssync_;
    sonde_m10_rx_t        m10rx_;
    bool                  phonieDec_ = false;
    bool                  phonieAprs_ = false;      // APRS en phonie (144,800 MHz)
    int                   aprsPair_ = -1;           // échantillon en attente pour la moyenne de 2
    // ---- ISM ----  EVENT ISM : frag:u8 (b7 dernier, b0-6 index), id:u8 (numéro de paquet), puis des paires
    //      pulse:u16 gap:u16 (µs, LE), 44 paires par notification. Un paquet rtl_433 = toutes les paires
    //      d'une rafale (silence > 30 ms). Console `O 1` : le paquet est aussi imprimé en lignes « .ook ».
    static void ismEntry(const ook_packet_t* p, void* user) { static_cast<Decoders*>(user)->onIsm(*p); }
    void onIsm(const ook_packet_t& p) {
        st_.ismPackets++; st_.ismPulses += (uint32_t)p.n;
        uint8_t id = ismId_++;
        static constexpr int kPairs = (BLE_NOTIFY_MAX - 3) / 4;      // 44
        int idx = 0;
        for (int off = 0; off < p.n; off += kPairs, idx++) {
            int n = p.n - off; if (n > kPairs) n = kPairs;
            uint8_t b[BLE_NOTIFY_MAX]; size_t k = 0;
            b[k++] = EV_ISM;
            b[k++] = (uint8_t)(idx | (off + n >= p.n ? 0x80 : 0));
            b[k++] = id;
            for (int i = 0; i < n; i++) { put16(b + k, p.pulse[off + i]); k += 2; put16(b + k, p.gap[off + i]); k += 2; }
            event(b, k);
        }
        if (ismDump_) {                                          // banc USB : lignes « pulse gap » comme un fichier .ook
            Serial.printf("[ook] paquet %u : %d impulsions, souffle %.0f, porteuse %.0f\n;ook\n", id, p.n, (double)ook_.noise, (double)ook_.quiet);
            for (int i = 0; i < p.n; i++) { Serial.printf("%u %u\n", p.pulse[i], p.gap[i]); if ((i & 63) == 63) delay(3); }
            Serial.println("[ook] fin");
        } else Serial.printf("[ism] paquet %u : %d impulsions (souffle %.0f, porteuse %.0f)\n", id, p.n, (double)ook_.noise, (double)ook_.quiet);
    }
    uint8_t ismId_ = 0;
    bool    ismDump_ = false;
public:
    void setIsmDump(bool on) { ismDump_ = on; }
    const ook_t& ook() const { return ook_; }
private:

    uint8_t               sarsatDecim_ = 2;          // 2 en phonie (anneau 32 kHz -> fenêtres 16 kHz)
    int16_t               m10buf_[6000];             // capture M10/M20 à 32 kHz (12 Ko)
    uint32_t nextM10PushMs_ = 0;

    // POCSAG
    pocsag_t pocsag_;
    // ISM
    ook_t ook_;

    // SSTV
    sstv_demod_t sstv_;
    uint32_t sstvLines_ = 0, lastSstvInfoMs_ = 0;
    uint8_t  sstvHalf_[SSTV_MAX_WIDTH / 2 * 3];      // ligne réduite de moitié (modes > 320 px)

    // GPS / position
    gps_t    gps_;
    bool     gpsSeen_ = false;
    uint32_t lastGpsMs_ = 0;
    int32_t  myLat_ = 0, myLon_ = 0;
};
