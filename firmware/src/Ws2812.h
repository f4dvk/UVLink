#pragma once
#include <Arduino.h>
#include <esp32-hal-rmt.h>

// WS2812 par RMT, un canal par broche (neopixelWrite() du core ne sait
// piloter que la première broche qu'on lui donne).
inline void ws2812Write(uint8_t pin, uint8_t r, uint8_t g, uint8_t b) {
    static rmt_obj_t* ch[4] = {nullptr, nullptr, nullptr, nullptr};
    static uint8_t    chPin[4] = {255, 255, 255, 255};
    int slot = -1;
    for (int i = 0; i < 4; i++) if (chPin[i] == pin) { slot = i; break; }
    if (slot < 0) {
        for (int i = 0; i < 4; i++) if (!ch[i]) { slot = i; break; }
        if (slot < 0) return;
        ch[slot] = rmtInit(pin, RMT_TX_MODE, RMT_MEM_64);
        if (!ch[slot]) return;
        rmtSetTick(ch[slot], 100);
        chPin[slot] = pin;
    }
    rmt_data_t d[24];
    const uint8_t col[3] = { g, r, b };
    int i = 0;
    for (int c = 0; c < 3; c++)
        for (int bit = 7; bit >= 0; bit--, i++) {
            bool one = col[c] & (1 << bit);
            d[i].level0 = 1; d[i].duration0 = one ? 8 : 4;
            d[i].level1 = 0; d[i].duration1 = one ? 4 : 8;
        }
    rmtWriteBlocking(ch[slot], d, 24);
}
