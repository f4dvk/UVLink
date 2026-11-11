#pragma once
#include <Arduino.h>
#include "config.h"
#include "UvK5.h"
#include "AudioStream.h"
#include "Decoders.h"

// ============================================================================
// Commandes CONTROL (docs/protocol.md) et construction du STATUS.
// Les trames reçues par BLE sont mises en file par le callback NimBLE et
// traitées ici depuis loop() (les commandes série bloquent jusqu'à 500 ms :
// pas question de le faire dans la tâche de la pile BLE).
// ============================================================================
enum : uint8_t {
    CMD_PING      = 0x01,
    CMD_GET_INFO  = 0x02,
    CMD_SET_VFO   = 0x10,
    CMD_GET_VFO   = 0x11,
    CMD_SET_RADIO = 0x12,
    CMD_PTT       = 0x13,
    CMD_MONITOR   = 0x14,
    CMD_RECALL_CH = 0x15,
    CMD_SET_MODE  = 0x20,
    CMD_SET_AUDIO = 0x21,
    CMD_CAL_AUDIO = 0x22,     // calibration du gain RX : seconds:u8 ; fin signalée par un CONTROL non sollicité [0x22, 1, gain]
    CMD_SET_AF_GAIN = 0x23,   // gain audio RX du POSTE (BK4819 REG_48, hôte H29) : gain:u8 (0 = réglage du poste, 1..78)
    CMD_SET_RSSI_CORR = 0x24, // correction RSSI dBm par bande : corr[7]:i8 (bandes 50/108/137/174/350/400/470 MHz), mémorisée (NVS)
    CMD_GET_RSSI_CORR = 0x25, // -> corr[7]:i8
    CMD_GET_GPS   = 0x40,
    CMD_SET_POS   = 0x41,     // position manuelle de l'opérateur (distance / azimut APRS, sondes)
    CMD_UNKNOWN   = 0xFF,
};

#define FW_MAJOR 0
#define FW_MINOR 14

struct CtrlMsg {
    uint8_t len;
    uint8_t data[64];
};

static inline uint16_t rd16(const uint8_t* p) { return p[0] | ((uint16_t)p[1] << 8); }
static inline uint32_t rd32(const uint8_t* p) {
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void wr16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static inline void wr32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

class Control {
public:
    Control(UvK5& radio, AudioStream& audio, Decoders& dec) : radio_(radio), audio_(audio), dec_(dec) {}

    uint8_t mode() const { return dec_.mode(); }
    bool    pttWanted() const { return pttWanted_; }    // PTT demandé par l'appli (0x13)
    void    clearPtt() { pttWanted_ = false; }          // garde-fou RADIO_PTT_NO_AUDIO_MS (main.cpp)

    // Traite une commande ; écrit la réponse dans rsp, renvoie sa longueur.
    size_t handle(const uint8_t* in, size_t n, uint8_t* rsp) {
        if (n == 0) return 0;
        size_t k = 0;
        rsp[k++] = in[0];
        switch (in[0]) {
        case CMD_PING:
            rsp[k++] = UVL_PROTO_VERSION; rsp[k++] = FW_MAJOR; rsp[k++] = FW_MINOR;
            break;
        case CMD_GET_INFO: {
            rsp[k++] = radio_.present() ? 1 : 0;
            rsp[k++] = radio_.protoVer();
            const char* fw = radio_.firmware();
            size_t l = strlen(fw);
            memcpy(rsp + k, fw, l); k += l;
            break;
        }
        case CMD_SET_VFO: {
            if (n < 18) { rsp[k++] = 0; break; }
            UvK5::RfParams p;
            uint8_t vfo  = in[1];
            p.rxHz       = rd32(in + 2);
            p.txHz       = rd32(in + 6);
            p.modulation = in[10];
            p.narrow     = in[11];
            p.rxCtcss    = rd16(in + 12);
            p.txCtcss    = rd16(in + 14);
            p.power      = in[16];
            p.squelch    = in[17];
            p.flags      = n > 18 ? in[18] : 0;
            rsp[k++] = radio_.present() && radio_.applyVfo(vfo, p) ? 1 : 0;
            break;
        }
        case CMD_GET_VFO: {
            uint8_t vfo = n > 1 ? in[1] & 1 : 0;
            const UvK5::RfParams& p = radio_.vfo(vfo);
            rsp[k++] = vfo;
            wr32(rsp + k, p.rxHz); k += 4;
            wr32(rsp + k, p.txHz); k += 4;
            rsp[k++] = p.modulation; rsp[k++] = p.narrow;
            wr16(rsp + k, p.rxCtcss); k += 2;
            wr16(rsp + k, p.txCtcss); k += 2;
            rsp[k++] = p.power; rsp[k++] = p.squelch; rsp[k++] = p.flags;
            break;
        }
        case CMD_SET_RADIO:
            if (n < 4) { rsp[k++] = 0; break; }
            rsp[k++] = radio_.present() && radio_.setRadio(in[1], in[2]) ? 1 : 0;
            break;
        case CMD_PTT:
            if (n < 2) { rsp[k++] = 0; break; }
            pttWanted_ = in[1] != 0;
            // main.cpp combine pttWanted_ et le flux AUDIO_TX ; on applique
            // tout de suite la mise ON (latence), le OFF attend la fin du flux.
            rsp[k++] = radio_.present() && (!pttWanted_ || radio_.ptt(true)) ? 1 : 0;
            break;
        case CMD_MONITOR:
            rsp[k++] = n >= 2 && radio_.present() && radio_.monitor(in[1] != 0) ? 1 : 0;
            break;
        case CMD_RECALL_CH:
            rsp[k++] = n >= 4 && radio_.present() && radio_.recallChannel(in[1], rd16(in + 2)) ? 1 : 0;
            break;
        case CMD_SET_MODE:
            if (n >= 2 && in[1] <= Decoders::ISM) { dec_.setMode(in[1]); rsp[k++] = 1; }
            else rsp[k++] = 0;
            break;
        case CMD_SET_AUDIO:       // stream_rx (0 off, 1 toujours, 2 auto), gain max (0 = inchangé), agc (0 inchangé, 1 off, 2 on)
            if (n < 2) { rsp[k++] = 0; break; }
            audio_.setRxMode(in[1]);
            if (n > 2) audio_.setGain(in[2]);
            if (n > 3 && in[3]) audio_.setAgc(in[3] == 2);
            rsp[k++] = 1;
            break;
        case CMD_CAL_AUDIO: {     // seconds:u8 (0 = 3 s) : mesure de crête puis gain fixe, AGC coupé
            uint8_t s = n >= 2 && in[1] ? in[1] : 3;
            if (s > 30) s = 30;
            audio_.startCal((uint32_t)s * 1000);
            rsp[k++] = 1;
            break;
        }
        case CMD_SET_AF_GAIN:     // gain:u8 -> ok (1 si le poste l'a pris, 0 sinon ; mémorisé dans tous les cas)
            if (n < 2) { rsp[k++] = 0; break; }
            rsp[k++] = radio_.setAfGain(in[1]) ? 1 : 0;
            break;
        case CMD_SET_RSSI_CORR:
            if (n < 8) { rsp[k++] = 0; break; }
            radio_.setRssiCorr((const int8_t*)(in + 1)); rsp[k++] = 1;
            break;
        case CMD_GET_RSSI_CORR:
            memcpy(rsp + k, radio_.rssiCorr(), 7); k += 7;
            break;
        case CMD_GET_GPS:
            k += dec_.buildGps(rsp + k);
            break;
        case CMD_SET_POS:         // lat_e5:i32, lon_e5:i32 (0,0 = aucune)
            if (n < 9) { rsp[k++] = 0; break; }
            dec_.setMyPosition((int32_t)rd32(in + 1), (int32_t)rd32(in + 5));
            rsp[k++] = 1;
            break;
        default:
            rsp[0] = CMD_UNKNOWN; rsp[k++] = in[0];
        }
        return k;
    }

    // STATUS : ver, radio_present, flags, smeter, rssi_dbm:i16, batt_mv:u16,
    //          rx0:u32, tx0:u32, rx1:u32, tx1:u32, mode, gps_fix, adc_dc:u16,
    //          rx_gain:u8 (fixe, ou maximal avec AGC), agc:u8,
    //          af_gain:u8 (gain AF du poste demandé, 0 = réglage du poste), adc_peak:u16,
    //          adc_rms:u16 (LSB 12 bits, dernière seconde), sat_pct:u8  (36 o)
    size_t buildStatus(uint8_t* out, uint16_t adcDc) {
        const UvK5::Status& s = radio_.lastStatus();
        size_t k = 0;
        out[k++] = UVL_PROTO_VERSION;
        out[k++] = radio_.present() ? 1 : 0;
        uint8_t fl = (s.sig ? 1 : 0) | (s.tx ? 2 : 0) | (s.dualWatch ? 4 : 0)
                   | (radio_.monitorOn() ? 8 : 0) | (s.rxVfo ? 16 : 0)
                   | (radio_.txVfo() ? 32 : 0) | (radio_.pttRequested() ? 64 : 0)
                   | (audio_.saturated() ? 128 : 0);           // ADC en butée : baisser le volume du poste
        out[k++] = fl;
        out[k++] = s.sMeter;
        wr16(out + k, (uint16_t)s.rssiDbm); k += 2;
        wr16(out + k, s.batterymV); k += 2;
        for (uint8_t v = 0; v < 2; v++) {
            wr32(out + k, radio_.vfo(v).rxHz); k += 4;
            wr32(out + k, radio_.vfo(v).txHz); k += 4;
        }
        out[k++] = dec_.mode();
        out[k++] = (dec_.gpsFix().valid ? 1 : 0) | (dec_.gpsSeen() ? 2 : 0);
        wr16(out + k, adcDc); k += 2;
        out[k++] = audio_.gain();
        out[k++] = audio_.agc() ? 1 : 0;
        out[k++] = radio_.afGainSet();
        wr16(out + k, audio_.rawPeak()); k += 2;
        wr16(out + k, audio_.rawRms()); k += 2;
        out[k++] = audio_.satPercent();
        return k;
    }

private:
    UvK5&        radio_;
    AudioStream& audio_;
    Decoders&    dec_;
    bool         pttWanted_ = false;
};
