#pragma once
#include <Arduino.h>
#include "driver/adc.h"
#include "config.h"

// ============================================================================
// Capture ADC continue (DMA) sur AUDIO_ADC_GPIO (GPIO4 = ADC1_CH3), cadence
// réglable (32 kHz phonie, moyenné à 8 kHz par AudioStream ; les décodeurs
// demandent la leur via restart()). Cadence réelle du S3 = 2,5 MHz / entier
// (8 000 -> 8 012,8 Hz, 32 000 -> 32 051 Hz).
// Une tâche lit le DMA et passe les blocs d'échantillons bruts 12 bits au
// consommateur (AudioStream aujourd'hui, décodeurs demain).
// ============================================================================
class AudioIn {
public:
    typedef void (*Sink)(const uint16_t* raw12, size_t n, void* ctx);

    bool begin(uint32_t rateHz, Sink sink, void* ctx) {
        sink_ = sink; ctx_ = ctx;
        if (!start(rateHz)) return false;
        run_ = true;
        xTaskCreatePinnedToCore(taskEntry, "adc", 4096, this, 5, &task_, 1);
        return true;
    }

    // Change la cadence (décodeurs). Le consommateur reste.
    bool restart(uint32_t rateHz) {
        if (rateHz == rate_) return true;
        pause_ = true;
        while (!paused_) delay(1);
        adc_digi_stop();
        adc_digi_deinitialize();
        bool ok = start(rateHz);
        if (!ok) {                      // pilote non réinitialisé (mémoire ?) : on NE relâche PAS la tâche,
            Serial.println("[audio] ADC : redemarrage ECHEC, capture suspendue (memoire ?)");
            rate_ = 0;                  // sinon adc_digi_read_bytes plante (LoadProhibited, 2026-10-30)
            return false;
        }
        pause_ = false;
        return ok;
    }

    uint32_t rate() const { return rate_; }
    // Cadence réellement produite par l'ADC du S3 : 2,5 MHz / entier (80 000
    // demandé -> 80 645 Hz, 32 000 -> 32 051). Indispensable aux décodeurs
    // qui intègrent à cadence fixe sans récupération d'horloge (M10/M20).
    float actualRate() const { uint32_t d = rate_ ? 2500000UL / rate_ : 1; return 2500000.0f / (float)(d ? d : 1); }
    uint32_t samplesTake() { uint32_t v = samples_; samples_ = 0; return v; }
    uint32_t overflows() const { return overflows_; }
    uint32_t restarts()  const { return restarts_; }     // redémarrages du pilote après débordement DMA

private:
    bool start(uint32_t rateHz) {
        adc_digi_init_config_t ic = {};
        ic.max_store_buf_size = AUDIO_IN_BLOCK * SOC_ADC_DIGI_RESULT_BYTES * 4;   // 10 Ko de tas (x8 = 20 Ko : trop avec BLE)
        ic.conv_num_each_intr = AUDIO_IN_BLOCK * SOC_ADC_DIGI_RESULT_BYTES;
        ic.adc1_chan_mask     = BIT(AUDIO_ADC_CHANNEL);
        ic.adc2_chan_mask     = 0;
        if (adc_digi_initialize(&ic) != ESP_OK) { Serial.println("[audio] adc_digi_initialize ECHEC"); return false; }

        adc_digi_pattern_config_t pat = {};
        pat.atten     = ADC_ATTEN_DB_11;
        pat.channel   = AUDIO_ADC_CHANNEL;
        pat.unit      = 0;
        pat.bit_width = 12;
        adc_digi_configuration_t cfg = {};
        cfg.conv_limit_en  = false;
        cfg.conv_limit_num = 250;
        cfg.pattern_num    = 1;
        cfg.adc_pattern    = &pat;
        cfg.sample_freq_hz = rateHz;
        cfg.conv_mode      = ADC_CONV_SINGLE_UNIT_1;
        cfg.format         = ADC_DIGI_OUTPUT_FORMAT_TYPE2;
        if (adc_digi_controller_configure(&cfg) != ESP_OK) { Serial.println("[audio] adc_digi_controller_configure ECHEC"); return false; }
        if (adc_digi_start() != ESP_OK) { Serial.println("[audio] adc_digi_start ECHEC"); return false; }
#if AUDIO_ADC_PULLUP
        // Après adc_digi_initialize (qui réinitialise la pastille et coupe les
        // pulls) : polarisation de secours du nœud audio, voir config.h.
        gpio_pullup_en((gpio_num_t)AUDIO_ADC_GPIO);
#endif
        rate_ = rateHz;
        return true;
    }

    static void taskEntry(void* p) { static_cast<AudioIn*>(p)->task(); }

    void task() {
        static uint8_t  dma[AUDIO_IN_BLOCK * SOC_ADC_DIGI_RESULT_BYTES];
        static uint16_t raw[AUDIO_IN_BLOCK];
        while (run_) {
            if (pause_) { paused_ = true; delay(5); continue; }
            paused_ = false;
            uint32_t got = 0;
            esp_err_t r = adc_digi_read_bytes(dma, sizeof(dma), &got, 100);
            if (r == ESP_ERR_INVALID_STATE) {
                // Anneau DMA débordé (tâche bloquée > 80 ms : sortie console massive,
                // pile BLE...) : le pilote reste en erreur tant qu'on ne le redémarre
                // pas, et l'audio était perdu pour de bon (constaté 2026-11-10 avec
                // la capture `R 8000`). Après quelques erreurs de suite : stop / start.
                overflows_++;
                if (++errRun_ >= 10) {
                    adc_digi_stop(); delay(2); adc_digi_start();
                    restarts_++; errRun_ = 0;
                    Serial.printf("[audio] ADC : anneau DMA deborde, redemarrage n° %lu\n", (unsigned long)restarts_);
                }
                delay(1);
                continue;
            }
            errRun_ = 0;
            if (r != ESP_OK || got == 0) continue;
            size_t n = 0;
            for (uint32_t i = 0; i + SOC_ADC_DIGI_RESULT_BYTES <= got; i += SOC_ADC_DIGI_RESULT_BYTES) {
                const adc_digi_output_data_t* d = (const adc_digi_output_data_t*)(dma + i);
                if (d->type2.channel == AUDIO_ADC_CHANNEL) raw[n++] = d->type2.data;
            }
            samples_ += n;
            if (n && sink_) sink_(raw, n, ctx_);
        }
        vTaskDelete(nullptr);
    }

    Sink     sink_ = nullptr;
    void*    ctx_  = nullptr;
    TaskHandle_t task_ = nullptr;
    volatile bool run_ = false, pause_ = false, paused_ = false;
    uint32_t rate_ = 0;
    volatile uint32_t samples_ = 0, overflows_ = 0, restarts_ = 0;
    uint32_t errRun_ = 0;
};
