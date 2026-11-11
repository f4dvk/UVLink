#pragma once
#include <Arduino.h>
#include "config.h"
#include "Adpcm.h"
#include "AudioIn.h"
#include "AudioOut.h"

// ============================================================================
// Flux audio BLE (docs/protocol.md, AUDIO_RX / AUDIO_TX).
//
// Paquet : seq:u8, flags:u8, pred:i16, idx:u8, adpcm[...]
//   flags b0 = début de flux, b1 = fin de flux, b2-b3 = cadence (0 = 16 kHz,
//   1 = 8 kHz). pred/idx = état du codeur AVANT le 1er échantillon du paquet.
//
// Poste -> appli : la tâche ADC appelle onAdc() bloc par bloc ; suppression
// de la composante continue, gain, ADPCM, et notification dès qu'un paquet
// est plein (si l'appli est abonnée et que la porte est ouverte).
// Appli -> poste : onTxPacket() (callback NimBLE) décode dans l'anneau de
// la sortie PWM ; txStreaming() sert au PTT automatique (main.cpp).
// ============================================================================
class AudioStream {   // (fabsf / expf : <math.h> via Arduino.h)
public:
    typedef bool (*Send)(const uint8_t* pkt, size_t n);     // false = refusé par la pile BLE (tampons pleins)
    typedef void (*Tap)(const uint16_t* raw12, size_t n, void* ctx);   // second consommateur des blocs ADC (décodeurs)

    enum RxMode : uint8_t { RX_OFF = 0, RX_ALWAYS = 1, RX_AUTO = 2 };   // AUTO = squelch ouvert / monitor / sans poste
    static constexpr size_t kHdr = 5;

    struct Packet { uint8_t len; uint8_t data[kHdr + AUDIO_PKT_SAMPLES / 2]; };

    bool begin(Send send) {
        send_ = send;
        out_.begin(AUDIO_SAMPLE_RATE_HZ);
        // Les paquets passent par une file : la tâche ADC ne touche jamais la
        // pile BLE (notify peut attendre des tampons), et si la liaison ne
        // suit pas on jette le plus ancien (latence bornée) au lieu de bloquer.
        txq_ = xQueueCreate(AUDIO_TX_QUEUE, sizeof(Packet));
        xTaskCreatePinnedToCore(sendTaskEntry, "audiotx", 4096, this, 3, nullptr, 0);
        configureRate(AUDIO_ADC_RATE_HZ);
        return in_.begin(AUDIO_ADC_RATE_HZ, &AudioStream::adcSink, this);
    }

    AudioIn&  in()  { return in_; }
    AudioOut& out() { return out_; }

    void setTap(Tap t, void* ctx) { tapCtx_ = ctx; tap_ = t; }

    // Cadence de l'ADC (phonie : AUDIO_ADC_RATE_HZ ; les décodeurs ont la
    // leur) ; la phonie BLE reste à AUDIO_SAMPLE_RATE_HZ :
    //  - cadence multiple de 8 kHz : moyenne de N échantillons (zéros aux
    //    multiples de 8 kHz, voir config.h) ;
    //  - sinon (13,2 kHz APRS) : passe-bas un pôle (~3,4 kHz) puis décimation.
    // À appeler hors de la tâche ADC (attend sa pause).
    bool setInputRate(uint32_t hz) {
        if (!in_.restart(hz)) return false;
        configureRate(hz);
        return true;
    }
    uint8_t decimation() const { return decim_; }

    // ---- réglages / état fournis par main.cpp ----
    void setSubscribed(bool s)      { subscribed_ = s; }
    void setGate(bool open)         { gate_ = open; }         // squelch ouvert / monitor / pas de poste
    void setTxBlocked(bool b)       { txBlocked_ = b; }       // le poste émet : pas de capture
    void setRxMode(uint8_t m)       { rxMode_ = m > 2 ? RX_AUTO : (RxMode)m; }
    void setGain(uint8_t g)         { if (g) { gain_ = g; if (gainF_ > g) gainF_ = g; dirty_ = true; } }   // gain fixe, ou maximal avec l'AGC
    void setAgc(bool on)            { agc_ = on; if (!on) gainF_ = gain_; dirty_ = true; }
    bool settingsDirtyTake()        { bool d = dirty_; dirty_ = false; return d; }   // gain / AGC changés -> à mémoriser (NVS)
    // Calibration : crête brute de l'ADC mesurée pendant ms (parler à niveau
    // normal), puis gain FIXE = AUDIO_AGC_TARGET / crête et AGC coupé. Sans
    // AGC, le bruit résiduel du poste n'est plus remonté pendant les silences.
    void startCal(uint32_t ms)      { calPeak_ = 1.0f; calUntil_ = millis() + ms; calActive_ = true; }
    bool calActive() const          { return calActive_; }
    bool calFinishedTake()          { bool d = calDone_; calDone_ = false; return d; }
    uint8_t applyCal() {
        float g = (float)AUDIO_AGC_TARGET / calPeak_;
        uint8_t gi = g < 1.0f ? 1 : g > 64.0f ? 64 : (uint8_t)(g + 0.5f);
        setGain(gi); setAgc(false);
        return gi;
    }
    uint16_t calPeak() const        { return (uint16_t)calPeak_; }
    bool     agc() const     { return agc_; }
    uint8_t  agcGain() const { return (uint8_t)(gainF_ + 0.5f); }   // gain courant
    uint32_t satTake()       { uint32_t v = sat_; sat_ = 0; return v; }   // échantillons ADC en butée (0 / 4095)
    bool     saturated() const { return satSec_ > 200; }   // > 2,5 % des échantillons de la dernière seconde en butée
    // Mesures brutes de l'ADC sur la dernière seconde (LSB 12 bits, écart à la
    // composante continue) : pour régler le gain AF du poste depuis l'appli.
    uint16_t rawPeak() const   { return rawPeak_; }
    uint16_t rawRms() const    { return rawRms_; }
    uint8_t  satPercent() const{ uint32_t r = in_.rate(); uint32_t p = r ? satSec_ * 100 / r : 0; return (uint8_t)(p > 100 ? 100 : p); }
    // Capture brute de l'ADC (diagnostic console 'R') : n échantillons 12 bits dans buf.
    void startCapture(uint16_t* buf, size_t n) { capFill_ = 0; capN_ = n; capBuf_ = buf; }
    bool captureDone() const { return capBuf_ && capFill_ >= capN_; }
    void setMtu(uint16_t mtu) {
        int bytes = (int)mtu - 3 - kHdr;
        if (bytes > AUDIO_PKT_SAMPLES / 2) bytes = AUDIO_PKT_SAMPLES / 2;
        if (bytes < 2) bytes = 2;
        pktBytes_ = (uint16_t)(bytes & ~1);
    }
    uint8_t  rxMode() const  { return rxMode_; }
    uint8_t  gain() const    { return gain_; }
    uint16_t pktBytes() const{ return pktBytes_; }
    uint16_t dc() const      { return (uint16_t)dc_; }
    uint16_t peak()          { uint16_t p = peak_; peak_ = 0; return p; }
    uint32_t clips()         { uint32_t c = clip_; clip_ = 0; return c; }
    uint32_t sentTake()      { uint32_t v = sent_; sent_ = 0; return v; }
    uint32_t sentTotal() const { return sentTotal_; }
    uint32_t droppedTake()   { uint32_t v = dropped_; dropped_ = 0; return v; }   // jetés (file pleine / refus BLE)
    // Diagnostic liaison : n'envoie qu'un paquet sur `div`, tronqué à `len` octets (0 = normal)
    void setTest(uint8_t div, uint16_t len) { testDiv_ = div ? div : 1; testLen_ = len; }
    uint32_t recvTake()      { uint32_t v = recv_; recv_ = 0; return v; }
    bool     streamingRx() const { return subscribed_ && rxMode_ != RX_OFF && !txBlocked_ && (rxMode_ == RX_ALWAYS || gate_); }

    // ---- appli -> poste ----
    void onTxPacket(const uint8_t* d, size_t n) {
        if (n < kHdr) return;
        uint8_t flags = d[1];
        if (flags & 1) { out_.clear(); txSeq_ = d[0]; }
        else if ((uint8_t)(txSeq_ + 1) != d[0]) txLost_++;
        txSeq_ = d[0];
        dec_.pred = (int16_t)(d[2] | (d[3] << 8));
        dec_.idx  = (int8_t)(d[4] > 88 ? 88 : d[4]);
        int16_t pcm[AUDIO_PKT_SAMPLES];
        size_t nb = n - kHdr;
        if (nb > sizeof(pcm) / 4) nb = sizeof(pcm) / 4;
        size_t ns = dec_.decodeBlock(d + kHdr, nb, pcm);
        out_.setActive(true);
        if (out_.write(pcm, ns) < ns) txDropped_++;
        recv_++;
        lastTxMs_ = millis();
        txEnded_ = (flags & 2) != 0;
    }
    bool txStreaming() const {
        return !txEnded_ && (millis() - lastTxMs_) < AUDIO_TX_TIMEOUT_MS;
    }
    void txIdle() { out_.setActive(false); }                  // appelé par main quand le flux est fini
    uint32_t txLost() const    { return txLost_; }
    uint32_t txDropped() const { return txDropped_; }

private:
    static void adcSink(const uint16_t* raw, size_t n, void* ctx) { static_cast<AudioStream*>(ctx)->onAdc(raw, n); }

    void configureRate(uint32_t hz) {
        decim_    = (hz % AUDIO_SAMPLE_RATE_HZ == 0) ? (uint8_t)(hz / AUDIO_SAMPLE_RATE_HZ) : 1;
        invDecim_ = 1.0f / (float)decim_;
        acc_ = 0; accN_ = 0;
        uint32_t post = hz / decim_;                      // cadence après la moyenne
        resample_ = post != AUDIO_SAMPLE_RATE_HZ;
        rsStep_   = (float)AUDIO_SAMPLE_RATE_HZ / (float)post;
        rsAcc_    = 0; rsLp_ = 0;
        rsK_      = 1.0f - expf(-2.0f * (float)M_PI * 3400.0f / (float)post);
    }

    void onAdc(const uint16_t* raw, size_t n) {
        if (tap_) tap_(raw, n, tapCtx_);
        if (capBuf_ && capFill_ < capN_) {
            size_t k = capN_ - capFill_; if (k > n) k = n;
            for (size_t i = 0; i < k; i++) capBuf_[capFill_ + i] = raw[i] & 0x0FFF;
            capFill_ += k;
        }
        bool stream = streamingRx();
        const float dcK = resample_ ? 0.0015f * rsStep_ : 0.0015f;   // même constante de temps quelle que soit la cadence
        // AGC : crête brute du bloc -> gain tel que la crête reste sous la cible
        // (attaque immédiate = limiteur), relâchement lent vers gain_ (maximum).
        {
            uint32_t now = millis();
            if (now - satWinMs_ >= 1000) {
                satSec_ = satAcc_; satAcc_ = 0; satWinMs_ = now;
                rawPeak_ = (uint16_t)secPeak_; rawRms_ = secN_ ? (uint16_t)sqrtf((float)((double)secSq_ / (double)secN_)) : 0;
                secPeak_ = 0; secSq_ = 0; secN_ = 0;
            }
        }
        float dev = 1.0f;
        {
            uint64_t sq = 0; const int dci = (int)dc_;
            for (size_t i = 0; i < n; i++) {
                int r = raw[i] & 0x0FFF;
                if (r <= 4 || r >= 4091) { sat_++; satAcc_++; }
                int di = r - dci; sq += (uint64_t)(di * di);
                float d = fabsf((float)r - dc_);
                if (d > dev) dev = d;
            }
            if (dev > secPeak_) secPeak_ = dev;
            secSq_ += sq; secN_ += n;
        }
        if (calActive_) {
            if (dev > calPeak_) calPeak_ = dev;
            if ((int32_t)(millis() - calUntil_) >= 0) { calActive_ = false; calDone_ = true; }
        }
        if (agc_) {
            float gmax = (float)AUDIO_AGC_TARGET / dev;
            if (gmax > (float)gain_) gmax = (float)gain_;
            if (gmax < 1.0f) gmax = 1.0f;
            uint32_t now = millis();
            if (gmax < gainF_) { gainF_ = gmax; agcRelMs_ = now; }
            else if (now - agcRelMs_ > AUDIO_AGC_RELEASE_MS) {
                gainF_ *= 1.0f + 0.04f * (float)n / (0.04f * (float)in_.rate());   // +4 % par 40 ms quelle que soit la taille du bloc
                if (gainF_ > gmax) gainF_ = gmax;
            }
        }
        const float g = agc_ ? gainF_ : (float)gain_;
        for (size_t i = 0; i < n; i++) {
            float x = (float)(raw[i] & 0x0FFF);
            if (decim_ > 1) {                                 // moyenne de decim_ échantillons -> 8 kHz
                acc_ += (uint32_t)x;
                if (++accN_ < decim_) continue;
                x = (float)acc_ * invDecim_;
                acc_ = 0; accN_ = 0;
            }
            dc_ += (x - dc_) * dcK;                           // suivi lent de la composante continue (~80 ms)
            int32_t s = (int32_t)((x - dc_) * g);
            if (s > 32767) { s = 32767; clip_++; } else if (s < -32768) { s = -32768; clip_++; }
            int32_t a = s < 0 ? -s : s;
            if (a > peak_) peak_ = (uint16_t)a;
            if (resample_) {
                rsLp_ += ((float)s - rsLp_) * rsK_;
                rsAcc_ += rsStep_;
                if (rsAcc_ < 1.0f) continue;
                rsAcc_ -= 1.0f;
                s = (int32_t)rsLp_;
            }
            if (!stream) { pcmFill_ = 0; continue; }
            if (pcmFill_ == 0) { startPred_ = enc_.pred; startIdx_ = enc_.idx; }
            pcm_[pcmFill_++] = (int16_t)s;
            if (pcmFill_ >= (size_t)pktBytes_ * 2) flushRx();
        }
        if (!stream) first_ = true;
    }

    void flushRx() {
        Packet p;
        uint8_t* pkt = p.data;
        pkt[0] = seq_++;
        pkt[1] = (first_ ? 1 : 0) | (AUDIO_SAMPLE_RATE_HZ == 8000 ? 4 : 0);
        pkt[2] = (uint8_t)startPred_; pkt[3] = (uint8_t)((uint16_t)startPred_ >> 8);
        pkt[4] = (uint8_t)startIdx_;
        size_t nb = enc_.encodeBlock(pcm_, pcmFill_, pkt + kHdr);
        p.len = (uint8_t)(kHdr + nb);
        pcmFill_ = 0; first_ = false;
        if (xQueueSend(txq_, &p, 0) != pdTRUE) {             // file pleine : on jette le plus ancien
            Packet old;
            xQueueReceive(txq_, &old, 0);
            dropped_++;
            xQueueSend(txq_, &p, 0);
        }
    }

    static void sendTaskEntry(void* p) { static_cast<AudioStream*>(p)->sendTask(); }
    void sendTask() {
        Packet p;
        for (;;) {
            if (xQueueReceive(txq_, &p, portMAX_DELAY) != pdTRUE) continue;
            if ((++testCnt_ % testDiv_) != 0) continue;
            if (testLen_ && testLen_ < p.len) p.len = (uint8_t)testLen_;
            bool ok = false;
            for (int essai = 0; essai < 4 && !ok; essai++) {   // tampons BLE pleins : on laisse passer un peu de temps
                ok = send_ && send_(p.data, p.len);
                if (!ok) delay(5);
            }
            if (ok) { sent_++; sentTotal_++; } else dropped_++;
        }
    }

    AudioIn  in_;
    AudioOut out_;
    Send     send_ = nullptr;
    Tap      tap_  = nullptr;
    void*    tapCtx_ = nullptr;
    volatile bool resample_ = false;
    float    rsStep_ = 1.0f, rsAcc_ = 0, rsLp_ = 0, rsK_ = 1.0f;
    volatile uint8_t decim_ = 1;                              // moyenne de decim_ échantillons ADC par échantillon 8 kHz
    float    invDecim_ = 1.0f;
    uint32_t acc_ = 0; uint8_t accN_ = 0;
    QueueHandle_t txq_ = nullptr;
    volatile uint8_t  testDiv_ = 1;
    volatile uint16_t testLen_ = 0;
    uint32_t testCnt_ = 0;
    ImaAdpcm enc_, dec_;

    // poste -> appli
    volatile bool subscribed_ = false, gate_ = true, txBlocked_ = false;
    volatile RxMode rxMode_ = RX_AUTO;
    volatile uint8_t gain_ = AUDIO_RX_GAIN;
    volatile bool agc_ = AUDIO_AGC != 0;
    volatile bool dirty_ = false;
    float    gainF_ = AUDIO_RX_GAIN;
    volatile bool  calActive_ = false, calDone_ = false;
    float    calPeak_ = 1.0f;
    uint32_t calUntil_ = 0;
    uint32_t agcRelMs_ = 0;
    volatile uint32_t sat_ = 0, satAcc_ = 0, satSec_ = 0;
    uint32_t satWinMs_ = 0;
    float    secPeak_ = 0; uint64_t secSq_ = 0; uint32_t secN_ = 0;
    volatile uint16_t rawPeak_ = 0, rawRms_ = 0;
    uint16_t* volatile capBuf_ = nullptr;
    volatile size_t capN_ = 0, capFill_ = 0;
    volatile uint16_t pktBytes_ = AUDIO_PKT_SAMPLES / 2;
    float    dc_ = 2048.0f;
    volatile uint16_t peak_ = 0;
    volatile uint32_t clip_ = 0, sent_ = 0, sentTotal_ = 0, dropped_ = 0, recv_ = 0;
    int16_t  pcm_[AUDIO_PKT_SAMPLES];
    size_t   pcmFill_ = 0;
    int16_t  startPred_ = 0; int8_t startIdx_ = 0;
    uint8_t  seq_ = 0;
    bool     first_ = true;

    // appli -> poste
    volatile uint32_t lastTxMs_ = 0, txLost_ = 0, txDropped_ = 0;
    volatile bool     txEnded_ = true;
    uint8_t  txSeq_ = 0;
};
