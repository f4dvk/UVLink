#pragma once
#include <Arduino.h>
#include <stdlib.h>
#include <math.h>
#include "UvK5.h"
#include "AudioStream.h"
#include "Decoders.h"
#include "Ws2812.h"

extern "C" uint16_t ble_hs_hci_avail_pkts;   // crédits ACL hôte -> contrôleur (diagnostic)

// ============================================================================
// Mini-console sur l'USB-C (moniteur série) pour tester la liaison poste
// sans l'appli. Tape "?" pour l'aide.
// ============================================================================
class Console {
public:
    Console(UvK5& radio, AudioStream& audio, Decoders& dec) : radio_(radio), audio_(audio), dec_(dec) {}

    void poll() {
        while (Serial.available()) {
            char c = (char)Serial.read();
            if (c == '\r') continue;
            if (c == '\n') { line_[len_] = 0; run(); len_ = 0; continue; }
            if (len_ < sizeof(line_) - 1) line_[len_++] = c;
        }
    }

private:
    void run() {
        char* argv[8];
        int argc = 0;
        char* s = line_;
        while (*s && argc < 8) {
            while (*s == ' ') s++;
            if (!*s) break;
            argv[argc++] = s;
            while (*s && *s != ' ') s++;
            if (*s) *s++ = 0;
        }
        if (argc == 0) return;
        switch (argv[0][0]) {
        case '?': help(); break;
        case 'p':
            Serial.printf("[con] sonde... %s\n", radio_.probe() ? "OK" : "pas de poste");
            break;
        case 'v': {   // v <vfo> <rxMHz> [txMHz] [squelch] [narrow]
            if (argc < 3) { help(); break; }
            UvK5::RfParams p;
            p.rxHz    = (uint32_t)llround(atof(argv[2]) * 1e6);
            p.txHz    = argc > 3 ? (uint32_t)llround(atof(argv[3]) * 1e6) : p.rxHz;
            p.squelch = argc > 4 ? (uint8_t)atoi(argv[4]) : 1;
            p.narrow  = argc > 5 ? (uint8_t)atoi(argv[5]) : 0;
            p.flags   = argc > 6 ? (uint8_t)(atoi(argv[6]) & 1) : 0;    // b0 = audio plat
            Serial.printf("[con] SET_VFO -> %s\n", radio_.applyVfo(atoi(argv[1]), p) ? "ok" : "ECHEC");
            break;
        }
        case 'w':     // w <txVfo> <dualWatch>
            if (argc < 3) { help(); break; }
            Serial.printf("[con] SET_RADIO -> %s\n", radio_.setRadio(atoi(argv[1]), atoi(argv[2])) ? "ok" : "ECHEC");
            break;
        case 'T': {   // T 1|0 : PTT comme l'appli (commande CONTROL 0x13 injectée, tenue par le garde-fou 10 s)
            if (argc < 2) { help(); break; }
            extern bool consolePttInject(bool on);   // main.cpp : passe par Control::handle (CMD_PTT)
            Serial.printf("[con] CMD_PTT %s -> %s\n", argv[1], consolePttInject(atoi(argv[1]) != 0) ? "ok" : "refuse");
            break;
        }
        case 't':     // t 1|0
            if (argc < 2) { help(); break; }
            Serial.printf("[con] PTT %s -> %s\n", argv[1], radio_.ptt(atoi(argv[1]) != 0) ? "envoye" : "ECHEC");
            break;
        case 'm':     // m 1|0
            if (argc < 2) { help(); break; }
            Serial.printf("[con] MONITOR -> %s\n", radio_.monitor(atoi(argv[1]) != 0) ? "ok" : "ECHEC");
            break;
        case 'c':     // c <vfo> <ch>
            if (argc < 3) { help(); break; }
            Serial.printf("[con] RECALL_CH -> %s\n", radio_.recallChannel(atoi(argv[1]), atoi(argv[2])) ? "ok" : "ECHEC");
            break;
        case 'r': {   // r <reg hex>
            if (argc < 2) { help(); break; }
            uint16_t v = 0;
            uint8_t reg = (uint8_t)strtol(argv[1], nullptr, 16);
            if (radio_.readBkReg(reg, v)) Serial.printf("[con] BK4819[0x%02X] = 0x%04X\n", reg, v);
            else Serial.println("[con] lecture registre ECHEC (firmware sans ENABLE_UART_RW_BK_REGS ?)");
            break;
        }
        case 's': {
            UvK5::Status st;
            if (!radio_.present()) { Serial.println("[con] pas de poste"); break; }
            if (!radio_.getStatus(st)) { Serial.println("[con] GET_STATUS ECHEC"); break; }
            Serial.printf("[con] func=%u rssi=%d dBm (raw %u) S=%u sig=%u (squelch %u : plancher %d, seuil %d dBm) tx=%u rxVfo=%u dw=%u batt=%u mV\n",
                          st.func, st.rssiDbm, st.rssiRaw, st.sMeter, st.sig, radio_.squelchLevel(),
                          radio_.floorDbm(), radio_.squelchThrDbm(), st.tx, st.rxVfo, st.dualWatch, st.batterymV);
            break;
        }
        case 'x':
            radio_.end();
            Serial.println("[con] mode hote quitte");
            break;
        case 'l': {   // l PIN [R G B] : WS2812 sur PIN (recherche de la LED)
            if (argc < 2) { help(); break; }
            uint8_t r = argc > 2 ? atoi(argv[2]) : 64, g = argc > 3 ? atoi(argv[3]) : 0, b = argc > 4 ? atoi(argv[4]) : 0;
            ws2812Write((uint8_t)atoi(argv[1]), r, g, b);
            Serial.printf("[con] WS2812 GPIO%s = %u,%u,%u\n", argv[1], r, g, b);
            break;
        }
        case 'a': {   // statistiques audio (compteurs remis à zéro à chaque appel)
            Serial.printf("[con] ADC %lu Hz : %lu ech lus, dc=%u crete=%u ecretage=%lu debordements=%lu redemarrages=%lu\n",
                          (unsigned long)audio_.in().rate(), (unsigned long)audio_.in().samplesTake(),
                          audio_.dc(), audio_.peak(), (unsigned long)audio_.clips(), (unsigned long)audio_.in().overflows(),
                          (unsigned long)audio_.in().restarts());
            {
                uint32_t sat = audio_.satTake();
                Serial.printf("[con] ADC brut (1 s) : crete=%u rms=%u LSB, saturation %u %% ; gain AF poste=%u\n",
                              audio_.rawPeak(), audio_.rawRms(), audio_.satPercent(), radio_.afGainSet());
                Serial.printf("[con] AGC %s gain=x%u (max x%u), saturation ADC=%lu%s\n",
                              audio_.agc() ? "actif" : "off", audio_.agcGain(), audio_.gain(), (unsigned long)sat,
                              sat > 50 ? " -> BAISSER LE VOLUME DU POSTE" : "");
            }
            Serial.printf("[con] RX->appli : mode=%u gain=x%u paquet=%u o, envoyes=%lu jetes=%lu, flux %s, credits HCI=%u\n",
                          audio_.rxMode(), audio_.agcGain(), audio_.pktBytes(), (unsigned long)audio_.sentTake(),
                          (unsigned long)audio_.droppedTake(), audio_.streamingRx() ? "ACTIF" : "arrete",
                          ble_hs_hci_avail_pkts);
            Serial.printf("[con] appli->PWM : recus=%lu perdus=%lu jetes=%lu sous-alim=%lu anneau=%u vol=%u %s\n",
                          (unsigned long)audio_.recvTake(), (unsigned long)audio_.txLost(), (unsigned long)audio_.txDropped(),
                          (unsigned long)audio_.out().underruns(), audio_.out().available(), audio_.out().volume(),
                          audio_.txStreaming() ? "FLUX" : "repos");
            break;
        }
        case 'e': {   // e N : luminosité de la LED d'état 0..64 (0 = éteinte), mémorisé
            if (argc < 2) { help(); break; }
            extern void ledSetLevel(uint8_t); extern uint8_t ledLevel();
            ledSetLevel((uint8_t)atoi(argv[1]));
            Serial.printf("[con] LED d'etat : %u/64%s\n", ledLevel(), ledLevel() ? "" : " (eteinte)");
            break;
        }
        case 'Y': {   // Y : dernière capture M10/M20 (échantillons 16 bits signés, hex, 32 par ligne) pour analyse hors ligne
            sonde_m10_rx_t& rx = dec_.m10rx();
            if (!rx.buf || rx.n <= 0) { Serial.println("[con] pas de capture M10/M20"); break; }
            Serial.printf("[con] capture M10 %d ech a %.0f Hz (pics=%lu, valides=%lu)\n", rx.n, (double)rx.fs, (unsigned long)rx.locks, (unsigned long)rx.frames_ok);
            for (int i = 0; i < rx.n; i++) { Serial.printf("%04X", (uint16_t)rx.buf[i]); if ((i & 31) == 31) { Serial.println(); if ((i & 1023) == 1023) delay(30); } }
            Serial.println("\n[con] fin capture M10");
            break;
        }
        case 'E':     // E 0|1 : notifications EVENT coupées / actives (diagnostic BLE)
            if (argc < 2) { help(); break; }
            dec_.setEvents(atoi(argv[1]) != 0); Serial.printf("[con] EVENT %s\n", atoi(argv[1]) ? "actifs" : "COUPES"); break;
        case 'X':     // X : armer la capture de la prochaine fenêtre SARSAT forte (gardée pour 'Q')
            dec_.sarsatArm(); Serial.println("[con] capture SARSAT armee (prochaine fenetre de crete >= 3000)"); break;
        case 'Q': {   // Q : vidage de la fenêtre SARSAT gardée (16 kHz, int16 hex, 32 par ligne) puis reprise
            if (!dec_.sarsatHeld()) { Serial.println("[con] pas de fenetre SARSAT gardee (X pour armer, Q pour vider)"); break; }
            const int32_t* w = dec_.sarsatWin(); int n = Decoders::sarsatWinLen();
            Serial.printf("[con] fenetre SARSAT %d ech a %d Hz\n", n, DEC_SARSAT_RATE_HZ);
            for (int i = 0; i < n; i++) {
                int32_t v = w[i]; if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
                Serial.printf("%04X", (uint16_t)(int16_t)v);
                if ((i & 31) == 31) { Serial.println(); if ((i & 1023) == 1023) delay(30); }
            }
            Serial.println("\n[con] fin fenetre SARSAT");
            dec_.sarsatRelease();
            break;
        }
        case 'Z': {   // Z : vidage de l'anneau brut du décodeur (32768 ech 12 bits, 406 ms à 80 kHz) pour analyse hors ligne
            uint8_t m = dec_.mode();
            if (m == Decoders::PHONIE) { Serial.println("[con] Z : choisir d'abord un mode decodeur (M 1..4)"); break; }
            float fs = audio_.in().actualRate();
            dec_.setMode(Decoders::PHONIE);            // fige l'anneau (le tap n'ecrit plus)
            uint32_t n = Decoders::ringSize(), w = dec_.ringWriteIndex();
            Serial.printf("[con] anneau %lu ech a %.0f Hz (mode %s)\n", (unsigned long)n, (double)fs, Decoders::modeName(m));
            for (uint32_t i = 0; i < n; i++) {
                Serial.printf("%03X", dec_.ringRaw(w + i) & 0x0FFF);
                if ((i & 31) == 31) { Serial.println(); if ((i & 1023) == 1023) delay(30); }
            }
            Serial.println("[con] fin anneau");
            dec_.setMode(m);
            break;
        }
        case 'W': {   // W <reg hex> <val hex> : écriture d'un registre BK4819 (diagnostic, sans réponse)
            if (argc < 3) { help(); break; }
            uint8_t reg = (uint8_t)strtol(argv[1], nullptr, 16); uint16_t val = (uint16_t)strtol(argv[2], nullptr, 16);
            Serial.printf("[con] BK4819[0x%02X] <- 0x%04X : %s\n", reg, val, radio_.writeBkReg(reg, val) ? "envoye" : "ECHEC");
            break;
        }
        case 'f': {   // f N : gain audio RX du POSTE (0 = réglage du poste, 1..78), hôte H29, mémorisé
            if (argc < 2) { help(); break; }
            bool ok = radio_.setAfGain((uint8_t)atoi(argv[1]));
            Serial.printf("[con] gain AF poste = %u (%s)\n", radio_.afGainSet(), ok ? "applique" : "memorise, poste absent ou hote < H29");
            break;
        }
        case 'k': {   // k [s] : calibration du gain RX (parler à niveau normal pendant s secondes)
            uint32_t s = argc > 1 ? atoi(argv[1]) : 3;
            if (s < 1) s = 1; if (s > 30) s = 30;
            audio_.startCal(s * 1000);
            Serial.printf("[con] calibration : parlez a niveau normal pendant %lu s...\n", (unsigned long)s);
            break;
        }
        case 'g':     // g GAIN : gain RX max (x1..x64) ; g a 0|1 : AGC off/on
            if (argc < 2) { help(); break; }
            if (argv[1][0] == 'a') { audio_.setAgc(argc > 2 && atoi(argv[2]) != 0); Serial.printf("[con] AGC %s\n", audio_.agc() ? "actif" : "off"); break; }
            audio_.setGain((uint8_t)atoi(argv[1]));
            Serial.printf("[con] gain RX = x%u\n", audio_.gain());
            break;
        case 'A':     // A 0|1|2 : flux RX off / toujours / auto
            if (argc < 2) { help(); break; }
            audio_.setRxMode((uint8_t)atoi(argv[1]));
            Serial.printf("[con] flux RX mode %u\n", audio_.rxMode());
            break;
        case 'N':     // N DIV LEN : test liaison (1 paquet sur DIV, tronque a LEN octets ; N 1 0 = normal)
            if (argc < 3) { help(); break; }
            audio_.setTest((uint8_t)atoi(argv[1]), (uint16_t)atoi(argv[2]));
            Serial.printf("[con] test liaison : 1 paquet sur %s, %s octets\n", argv[1], argv[2]);
            break;
        case 'R': {   // R [N] [Hz] : capture brute de l'ADC (N échantillons 12 bits, hex, 32 par ligne)
            static uint16_t cap[8192];                      // à 32 kHz : 256 ms, résolution 4 Hz en FFT
            size_t n = argc > 1 ? (size_t)atoi(argv[1]) : 4096;
            if (n > 8192) n = 8192;
            uint32_t hz = argc > 2 ? (uint32_t)atoi(argv[2]) : 0;   // cadence temporaire (ex. 80000 : voir jusqu'à 40 kHz)
            if (hz) audio_.setInputRate(hz);
            audio_.startCapture(cap, n);
            uint32_t t0 = millis();
            while (!audio_.captureDone() && millis() - t0 < 3000) delay(5);
            Serial.printf("[con] capture ADC %u ech a %lu Hz%s\n", (unsigned)n, (unsigned long)audio_.in().rate(),
                          audio_.captureDone() ? "" : " (INCOMPLETE)");
            // sortie cadencée comme 'Y' : 24 Ko d'un bloc bloquaient la tâche ADC (anneau DMA débordé)
            for (size_t i = 0; i < n; i++) { Serial.printf("%03X", cap[i]); if ((i & 31) == 31) { Serial.println(); if ((i & 1023) == 1023) delay(30); } }
            Serial.println("[con] fin capture");
            if (hz) audio_.setInputRate(Decoders::rateFor(dec_.mode()));
            break;
        }
        case 'P': {   // P 0|1 : sortie PWM GPIO7 détachée (haute impédance) / rattachée — diagnostic nœud ADC
            bool on = argc > 1 && atoi(argv[1]) != 0;
            if (on) ledcAttachPin(AUDIO_PWM_GPIO, AUDIO_PWM_CH);
            else { ledcDetachPin(AUDIO_PWM_GPIO); pinMode(AUDIO_PWM_GPIO, INPUT); }
            delay(300);
            Serial.printf("[con] PWM GPIO%u %s : dc ADC=%u (si dc remonte vers ~2048 quand le PWM est detache, le noeud ADC est relie a GPIO%u)\n",
                          AUDIO_PWM_GPIO, on ? "rattache" : "DETACHE (haute impedance)", audio_.dc(), AUDIO_PWM_GPIO);
            break;
        }
        case 'U': {   // U 0|1 : pull-up interne (~45 k) sur l'entrée ADC (diagnostic de la polarisation du nœud)
            bool on = argc > 1 && atoi(argv[1]) != 0;
            if (on) gpio_pullup_en((gpio_num_t)AUDIO_ADC_GPIO); else gpio_pullup_dis((gpio_num_t)AUDIO_ADC_GPIO);
            delay(300);
            Serial.printf("[con] pull-up GPIO%u %s : dc=%u (attendu ~2048 si pont 22k/22k present, ~1340 si seul le 22k vers GND, ~0 si court-circuit)\n",
                          AUDIO_ADC_GPIO, on ? "ON" : "OFF", audio_.dc());
            break;
        }
        case 'V':     // V 0-255 : volume PWM
            if (argc < 2) { help(); break; }
            audio_.out().setVolume((uint8_t)atoi(argv[1]));
            Serial.printf("[con] volume PWM = %u\n", audio_.out().volume());
            break;
        case 'o': {   // o [ms] [Hz] : tonalité de test sur le PWM GPIO7
            uint32_t ms = argc > 1 ? atoi(argv[1]) : 2000, hz = argc > 2 ? atoi(argv[2]) : 1000;
            Serial.printf("[con] tonalite %lu Hz pendant %lu ms sur GPIO%u\n", (unsigned long)hz, (unsigned long)ms, AUDIO_PWM_GPIO);
            AudioOut& o = audio_.out();
            o.clear(); o.setActive(true);
            float ph = 0, dph = 2.0f * (float)M_PI * hz / AUDIO_SAMPLE_RATE_HZ;
            uint32_t t0 = millis();
            while (millis() - t0 < ms) {
                while (o.freeSpace() > 0) { int16_t s = (int16_t)(sinf(ph) * 20000.0f); o.write(&s, 1); ph += dph; if (ph > 2 * M_PI) ph -= 2 * M_PI; }
                delay(5);
            }
            while (o.available()) delay(1);
            delay(AUDIO_OUT_PRIME * 1000 / AUDIO_SAMPLE_RATE_HZ);
            o.setActive(false);
            break;
        }
        case 'O':     // O 0|1 : impression des paquets ISM en lignes « .ook » (banc USB, mode ISM)
            dec_.setIsmDump(argc > 1 && atoi(argv[1]) != 0);
            Serial.printf("[con] impression des paquets ISM %s\n", argc > 1 && atoi(argv[1]) != 0 ? "ON" : "OFF");
            break;
        case 'M':     // M 0-6 : mode décodeur (0 phonie, 1 APRS, 2 SARSAT, 3 sonde, 4 SSTV, 5 POCSAG, 6 ISM)
            if (argc < 2) { help(); break; }
            dec_.setMode((uint8_t)atoi(argv[1]));
            Serial.printf("[con] mode %s demande\n", Decoders::modeName((uint8_t)atoi(argv[1])));
            break;
        case 'd': {   // statistiques décodeurs
            const Decoders::Stats& s = dec_.stats();
            const gps_fix_t& g = dec_.gpsFix();
            Serial.printf("[con] uptime %lu s, tas libre %lu o (min %lu o)\n", (unsigned long)(millis() / 1000),
                          (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap());
            Serial.printf("[con] mode %s, ADC %lu Hz, retard=%lu ech, traites=%lu, debordements=%lu, events=%lu\n",
                          Decoders::modeName(dec_.mode()), (unsigned long)audio_.in().rate(), (unsigned long)dec_.backlog(),
                          (unsigned long)dec_.processed(), (unsigned long)s.overruns, (unsigned long)s.events);
            Serial.printf("[con] SARSAT fenetres=%lu salves=%lu decodees=%lu plancher=%d crete=%d rms=%d\n",
                          (unsigned long)s.sarsatWindows, (unsigned long)s.sarsatBursts, (unsigned long)s.sarsatDecoded,
                          s.floorRms, s.lastPeak, s.lastRms);
            Serial.printf("[con] APRS paquets=%lu | RS41 ok=%lu bad=%lu | M10 entetes=%lu gps=%lu | SSTV lignes=%lu\n",
                          (unsigned long)s.aprsPackets, (unsigned long)s.rs41Ok, (unsigned long)s.rs41Bad,
                          (unsigned long)s.m10Hits, (unsigned long)s.m10Gps, (unsigned long)s.sstvLines);
            {
                const pocsag_t& pg = dec_.pocsag();
                Serial.printf("[con] POCSAG messages=%lu synchros=%lu mots incorrigibles=%lu douteux caches=%lu%s\n",
                              (unsigned long)s.pocsagMsgs, (unsigned long)pg.n_sync, (unsigned long)pg.n_bad_words,
                              (unsigned long)pg.n_hidden, pocsag_synced(&pg) ? " (cale)" : "");
                const ook_t& ok = dec_.ook();
                Serial.printf("[con] ISM paquets=%lu impulsions=%lu trop courts=%lu creux=%lu souffle=%.0f porteuse=%.0f%s\n",
                              (unsigned long)s.ismPackets, (unsigned long)s.ismPulses, (unsigned long)ok.n_short, (unsigned long)ok.n_glitch,
                              (double)ok.noise, (double)ok.quiet, ook_carrier(&ok) ? " (porteuse)" : "");
            }
            {
                sonde_m10_rx_t& rx = dec_.m10rx();
                Serial.printf("[con] M10/M20 rx : fs=%.0f Hz, pics=%lu, trames valides=%lu, meilleur score=%.2f (seuil %.2f), capture=%d ech%s\n",
                              (double)rx.fs, (unsigned long)rx.locks, (unsigned long)rx.frames_ok, (double)rx.max_score, (double)M10_RX_CORR_THR,
                              rx.n, rx.capturing ? " (en cours)" : "");
                rx.max_score = 0;
            }
            Serial.printf("[con] GPS %s fix=%u %.5f %.5f alt=%d sats=%u %02u:%02u:%02u\n", dec_.gpsSeen() ? "vu" : "absent",
                          g.valid, g.lat_e5 / 1e5, g.lon_e5 / 1e5, g.alt_m, g.sats, g.utc_h, g.utc_m, g.utc_s);
            break;
        }
        case 'F': {   // F N : injecte N echantillons PCM 16 bits LE (cadence du mode) lus en binaire sur l'USB,
                      // par blocs de 2048 echantillons (4096 o) acquittes par un octet 0x06 (l'hote attend l'ACK)
            if (argc < 2) { help(); break; }
            uint32_t total = (uint32_t)atol(argv[1]), done = 0;
            static int16_t buf[2048];
            Serial.printf("[con] injection de %lu echantillons a %lu Hz : envoie les donnees\n", (unsigned long)total,
                          (unsigned long)audio_.in().rate());
            dec_.setInject(true);
            Serial.setTimeout(8000);
            uint32_t partial = 0;
            while (done < total) {
                size_t want = total - done > 2048 ? 2048 : total - done;
                size_t gotB = Serial.readBytes((char*)buf, want * 2);
                size_t got = gotB / 2;
                if (!got) { Serial.printf("[con] t=%lu injection : plus rien apres %lu echantillons (delai 8 s)\n", (unsigned long)millis(), (unsigned long)done); break; }
                if (gotB != want * 2) { partial++; Serial.printf("[con] lecture partielle %u/%u o\n", (unsigned)gotB, (unsigned)(want * 2)); }
                dec_.injectPcm(buf, got);
                done += got;
                uint32_t w0 = millis(); bool said = false;
                while (dec_.backlog() > DEC_RING_SAMPLES / 2) {         // on laisse la tache decoder
                    delay(1);
                    if (!said && millis() - w0 > 1000) {
                        said = true;
                        Serial.printf("[con] t=%lu decodeur en retard : retard=%lu ech, traites=%lu, etat tache=%u\n",
                                      (unsigned long)millis(), (unsigned long)dec_.backlog(), (unsigned long)dec_.processed(), dec_.taskState());
                    }
                }
                if (said) Serial.printf("[con] t=%lu decodeur rattrape apres %lu ms\n", (unsigned long)millis(), (unsigned long)(millis() - w0));
                // ACK : l'USB CDC natif peut garder un octet isole dans son FIFO, et write()
                // rend 0 si son verrou est pris (100 ms) -> on insiste et on force l'envoi.
                for (int k = 0; k < 20 && Serial.write((uint8_t)0x06) == 0; k++) delay(5);
                Serial.flush();
            }
            if (partial) Serial.printf("[con] injection : %lu lectures partielles\n", (unsigned long)partial);
            // dernière fenêtre / fin de trame : un peu de silence
            memset(buf, 0, sizeof(buf));
            for (int i = 0; i < 20; i++) { dec_.injectPcm(buf, 1024); while (dec_.backlog() > DEC_RING_SAMPLES / 2) delay(1); }
            while (dec_.backlog() > 0 && dec_.mode() != Decoders::SARSAT) delay(1);
            delay(50);
            dec_.setInject(false);
            if (done < total) {                 // interrompue : on jette le reste pour ne pas l'interpreter en commandes
                uint32_t t0 = millis();
                while (millis() - t0 < 1500) { while (Serial.available()) Serial.read(); delay(10); }
            }
            Serial.printf("[con] injection terminee : %lu/%lu echantillons\n", (unsigned long)done, (unsigned long)total);
            break;
        }
        case 'L': {   // test automatique : 48 rouge, 47 vert, 38 bleu, 21 blanc
            static const uint8_t pins[4] = { 48, 47, 38, 21 };
            static const uint8_t col[4][3] = { {128,0,0}, {0,128,0}, {0,0,128}, {96,96,96} };
            for (int rep = 0; rep < 3; rep++)
                for (int i = 0; i < 4; i++) {
                    Serial.printf("[con] GPIO%u %s\n", pins[i], i == 0 ? "ROUGE" : i == 1 ? "VERT" : i == 2 ? "BLEU" : "BLANC");
                    ws2812Write(pins[i], col[i][0], col[i][1], col[i][2]);
                    delay(2500);
                    ws2812Write(pins[i], 0, 0, 0);
                }
            break;
        }
        default: help();
        }
    }

    void help() {
        Serial.println("[con] ?            aide\n"
                       "[con] p            sonder le poste / entrer en mode hote\n"
                       "[con] v V RX [TX] [SQ] [NAR] [PLAT]   VFO V (0/1), MHz, squelch 0-9, 1 = 12,5 kHz, 1 = audio plat\n"
                       "[con] w TXVFO DW   VFO d'emission, double veille 0/1\n"
                       "[con] t 1|0        PTT\n"
                       "[con] m 1|0        monitor (AF force)\n"
                       "[con] c V CH       rappel memoire CH sur le VFO V\n"
                       "[con] r HH         lire le registre BK4819 0xHH\n"
                       "[con] s            GET_STATUS\n"
                       "[con] x            quitter le mode hote\n"
                       "[con] a            statistiques audio (ADC, flux BLE, PWM)\n"
                       "[con] g N          gain RX max x1..x64 ; g a 0|1 AGC ; A 0|1|2 flux RX off/toujours/auto ; V 0-255 volume PWM\n"
                       "[con] k [s]        calibration : crete mesuree pendant s secondes (parler) -> gain fixe, AGC off, memorise\n"
                       "[con] f N          gain audio RX du poste 0..78 (0 = reglage du poste ; hote H29), memorise\n"
                       "[con] r REG | W REG VAL   lecture / ecriture d'un registre BK4819 (hex, diagnostic)\n"
                       "[con] e N          luminosite de la LED d'etat 0..64 (0 = eteinte), memorise\n"
                       "[con] Y            derniere capture M10/M20 brute (hex) ; 'd' donne pics / trames / meilleur score\n"
                       "[con] Z            vidage de l'anneau brut du decodeur (406 ms a 80 kHz) pour analyse hors ligne\n"
                       "[con] R [N] [Hz]   capture brute de l'ADC (N echantillons hex, cadence temporaire) pour analyse spectrale\n"
                       "[con] U 0|1        pull-up interne sur l'entree ADC ; P 0|1 PWM GPIO7 detache/rattache (diagnostic noeud ADC)\n"
                       "[con] o [ms] [Hz]  tonalite de test sur le PWM GPIO7\n"
                       "[con] M 0-6        mode : 0 phonie, 1 APRS, 2 SARSAT, 3 sonde, 4 SSTV, 5 POCSAG, 6 ISM ; d = statistiques decodeurs\n"
                       "[con] O 0|1        mode ISM : imprimer les paquets d'impulsions (lignes .ook pour rtl_433)\n"
                       "[con] F N          injecte N echantillons PCM16 (binaire, cadence du mode) a la place de l'ADC\n"
                       "[con] l PIN [R G B]  WS2812 sur PIN ; L = test auto 48/47/38/21");
    }

    UvK5&        radio_;
    AudioStream& audio_;
    Decoders&    dec_;
    char   line_[96];
    size_t len_ = 0;
};
