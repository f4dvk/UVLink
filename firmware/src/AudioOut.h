#pragma once
#include <Arduino.h>
#include "soc/ledc_struct.h"
#include "config.h"

// ============================================================================
// Sortie audio PWM (LEDC) sur GPIO7 -> filtre RC -> micro du poste.
// Anneau d'échantillons 16 bits rempli par le flux BLE (ou la console),
// vidé par un timer matériel à la cadence audio : chaque tick écrit le
// rapport cyclique directement dans les registres LEDC (ISR en IRAM, pas
// d'appel au pilote). Sans donnée : repos au milieu de l'échelle (silence).
// ============================================================================
class AudioOut {
public:
    void begin(uint32_t rateHz) {
        ledcSetup(AUDIO_PWM_CH, AUDIO_PWM_FREQ_HZ, AUDIO_PWM_BITS);
        ledcAttachPin(AUDIO_PWM_GPIO, AUDIO_PWM_CH);
        ledcWrite(AUDIO_PWM_CH, 0);      // au repos : pas de porteuse (voir onTimer)
        self_ = this;
        timer_ = timerBegin(1, 5, true);                     // 80 MHz / 5 = 16 MHz
        timerAttachInterrupt(timer_, &AudioOut::onTimer, false);   // le S3 n'a que le mode LEVEL
        timerAlarmWrite(timer_, 16000000UL / rateHz, true);
        // L'alarme n'est armée que pendant un flux (setActive) : hors émission
        // l'ISR à 8 kHz ne sert à rien et son activité, verrouillée sur 8 000,0 Hz
        // exactement, se replierait sur l'ADC (8 012,8 Hz réels) en ~13 Hz.
    }

    // Depuis n'importe quelle tâche (un seul producteur à la fois).
    size_t write(const int16_t* s, size_t n) {
        size_t k = 0;
        while (k < n) {
            uint16_t nh = (head_ + 1) & kMask;
            if (nh == tail_) break;                          // plein
            buf_[head_] = s[k++];
            head_ = nh;
        }
        return k;
    }
    void   clear()           { tail_ = head_; primed_ = false; }
    size_t available() const { return (head_ - tail_) & kMask; }
    size_t freeSpace() const { return kMask - available(); }
    void   setVolume(uint8_t v) { vol_ = v; }                // 0..255 (255 = 1,0)
    uint8_t volume() const   { return vol_; }
    void   setActive(bool a) {
        if (a == active_) return;
        active_ = a;
        if (a) { clear(); timerAlarmEnable(timer_); }
        else   { timerAlarmDisable(timer_); clear(); ledcWrite(AUDIO_PWM_CH, 0); }   // broche basse, pas de porteuse
    }
    uint32_t underruns() const { return underruns_; }

private:
    static constexpr uint16_t kSize = AUDIO_OUT_RING, kMask = kSize - 1;
    static constexpr int32_t  kMid  = (1 << AUDIO_PWM_BITS) / 2;
    static_assert((kSize & kMask) == 0, "AUDIO_OUT_RING doit etre une puissance de 2");

    static void ARDUINO_ISR_ATTR onTimer() {
        AudioOut* a = self_;
        int32_t s = 0;
        uint16_t avail = (a->head_ - a->tail_) & kMask;
        // pré-tampon : on attend AUDIO_OUT_PRIME échantillons avant de jouer
        // (les paquets BLE arrivent par rafales) ; anneau vide -> on ré-amorce.
        if (!a->primed_ && avail >= AUDIO_OUT_PRIME) a->primed_ = true;
        if (a->primed_) {
            if (avail) { s = a->buf_[a->tail_]; a->tail_ = (a->tail_ + 1) & kMask; }
            else { a->primed_ = false; if (a->active_) a->underruns_++; }   // 1 par trou, pas par tick
        }
        // int16 -> 10 bits (>>6), volume (>>8), autour du milieu.
        // Hors flux (pas de PTT) : rapport cyclique 0 = broche basse, AUCUNE
        // porteuse 39 kHz : celle-ci se recouple sur l'entrée ADC (masse
        // commune) et, repliée à 8 kHz d'échantillonnage, donne un sifflement
        // vers 940 Hz que l'AGC remonte dès que l'audio du poste est faible.
        int32_t d = a->active_ ? kMid + ((s * (int32_t)a->vol_) >> (16 - AUDIO_PWM_BITS + 8)) : 0;
        if (d < 0) d = 0; else if (d > (1 << AUDIO_PWM_BITS) - 1) d = (1 << AUDIO_PWM_BITS) - 1;
        LEDC.channel_group[0].channel[AUDIO_PWM_CH].duty.duty = (uint32_t)d << 4;
        LEDC.channel_group[0].channel[AUDIO_PWM_CH].conf1.duty_start = 1;
        LEDC.channel_group[0].channel[AUDIO_PWM_CH].conf0.low_speed_update = 1;
    }

    static inline AudioOut* self_ = nullptr;
    hw_timer_t* timer_ = nullptr;
    int16_t  buf_[kSize];
    volatile uint16_t head_ = 0, tail_ = 0;
    volatile uint8_t  vol_ = 255;
    volatile bool     active_ = false, primed_ = false;
    volatile uint32_t underruns_ = 0;
};
