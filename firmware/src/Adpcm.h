#pragma once
#include <stdint.h>

// ============================================================================
// IMA ADPCM (DVI) : 4 bits par échantillon, 16 bits -> 4:1. Même codec des
// deux côtés (appli : Audio/ImaAdpcm.cs). L'état (prédicteur + index) voyage
// dans l'en-tête de chaque paquet BLE : un paquet perdu ne désynchronise pas.
// ============================================================================
struct ImaAdpcm {
    int16_t pred = 0;
    int8_t  idx  = 0;

    void reset() { pred = 0; idx = 0; }

    uint8_t encode(int16_t s) {
        int step = stepTab[idx];
        int diff = (int)s - pred;
        uint8_t nib = 0;
        if (diff < 0) { nib = 8; diff = -diff; }
        int vp = step >> 3;
        if (diff >= step) { nib |= 4; diff -= step; vp += step; }
        step >>= 1;
        if (diff >= step) { nib |= 2; diff -= step; vp += step; }
        step >>= 1;
        if (diff >= step) { nib |= 1; vp += step; }
        int p = (nib & 8) ? pred - vp : pred + vp;
        pred = clamp16(p);
        idx  = clampIdx(idx + idxTab[nib]);
        return nib;
    }

    int16_t decode(uint8_t nib) {
        int step = stepTab[idx];
        int vp = step >> 3;
        if (nib & 4) vp += step;
        if (nib & 2) vp += step >> 1;
        if (nib & 1) vp += step >> 2;
        int p = (nib & 8) ? pred - vp : pred + vp;
        pred = clamp16(p);
        idx  = clampIdx(idx + idxTab[nib & 15]);
        return pred;
    }

    // Blocs : nibble bas = 1er échantillon (ordre WAV / IMA).
    size_t encodeBlock(const int16_t* in, size_t n, uint8_t* out) {
        size_t k = 0;
        for (size_t i = 0; i + 1 < n; i += 2)
            out[k++] = encode(in[i]) | (encode(in[i + 1]) << 4);
        return k;
    }
    size_t decodeBlock(const uint8_t* in, size_t nbytes, int16_t* out) {
        size_t k = 0;
        for (size_t i = 0; i < nbytes; i++) {
            out[k++] = decode(in[i] & 15);
            out[k++] = decode(in[i] >> 4);
        }
        return k;
    }

private:
    static int16_t clamp16(int v) { return v > 32767 ? 32767 : (v < -32768 ? -32768 : (int16_t)v); }
    static int8_t  clampIdx(int v) { return v < 0 ? 0 : (v > 88 ? 88 : (int8_t)v); }
    static constexpr int8_t idxTab[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
    static constexpr int16_t stepTab[89] = {
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
        50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
        337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
        2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
        15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };
};
