/* ookpulse.c -- voir ookpulse.h */
#include "ookpulse.h"
#include <string.h>
#include <math.h>

void ook_init(ook_t* o, float rate_hz, ook_cb_t cb, void* user)
{
    memset(o, 0, sizeof *o);
    o->rate = rate_hz; o->us_per_sample = 1e6f / rate_hz;
    o->cb = cb; o->user = user;
    /* réglé sur l'UV-K5 (2026-11-10, télécommande PT2262 et capteur Prologue réels) :
     * souffle corrélé (bande 0,5-6 kHz), FI qui réagit en < 60 µs, porteuse forte
     * qui laisse 6-16 % de souffle résiduel */
    o->on_frac = 0.22f;                  /* porteuse : max|y| sous 22 % du souffle moyen (35 % au plus en rafale, voir ook_sample)... */
    o->on_frac_max = 0.40f;
    o->off_frac = 0.60f;                 /* ...fin de porteuse : au-dessus de 60 % */
    o->min_on  = (int)(rate_hz * 90e-6f + 0.5f);    /* 90 µs de calme en plus de la fenêtre (7 échantillons) */
    o->min_off = (int)(rate_hz * 60e-6f + 0.5f);    /* 60 µs de souffle pour sortir (les retours brefs sont refusionnés par min_gap) */
    if (o->min_on < 2) o->min_on = 2;
    if (o->min_off < 2) o->min_off = 2;
    o->max_gap = (uint32_t)(rate_hz * 0.030f);      /* 30 ms de silence : fin de paquet */
    o->min_pulse = (int)(rate_hz * 200e-6f + 0.5f); /* impulsion < 200 µs = creux du souffle */
    o->min_gap   = (int)(rate_hz * 200e-6f + 0.5f); /* trou < 200 µs = retour de souffle dans une porteuse (plus petit silence réel : CAME 320, PT2262 350) */
    o->min_pulses = 6;
    o->noise = 1.0f; o->quiet = 0.1f;
}

static void emit(ook_t* o)
{
    if (o->pkt.n >= o->min_pulses) {
        o->n_packets++; o->n_pulses += (uint32_t)o->pkt.n;
        if (o->cb) o->cb(&o->pkt, o->user);
    } else if (o->pkt.n > 0) o->n_short++;
    o->pkt.n = 0;
}

static uint16_t us16(const ook_t* o, uint32_t samples)
{
    float us = (float)samples * o->us_per_sample;
    return us >= 65535.0f ? 65535 : (uint16_t)(us + 0.5f);
}

/* début de porteuse confirmé : l'instant réel est le premier échantillon calme
 * (on_cnt confirmations + la fenêtre du maximum glissant) */
static void carrier_on(ook_t* o, uint32_t now)
{
    uint32_t t = now - (uint32_t)o->on_cnt - (OOK_MAXWIN - 1);
    /* trou plus court que min_gap depuis la dernière impulsion : retour de
     * souffle au milieu d'une porteuse, l'impulsion précédente continue */
    if (o->pkt.n > 0 && (int32_t)(t - o->t_end) < o->min_gap) { o->pkt.n--; o->t_pulse = o->t_last_start; o->t_end = o->t_end_prev; o->n_merged++; }
    else o->t_pulse = t;
    o->carrier = true; o->on_cnt = o->off_cnt = 0;
}
/* fin de porteuse : l'impulsion est validée si elle dépasse la durée minimale ;
 * sinon c'est un creux du souffle, le silence en cours continue */
static void carrier_off(ook_t* o, uint32_t now)
{
    uint32_t t_end = now - (uint32_t)o->off_cnt;
    uint32_t len = t_end - o->t_pulse;
    if (len >= (uint32_t)o->min_pulse) {
        if (o->pkt.n >= OOK_MAX_PULSES) emit(o);
        if (o->pkt.n > 0) o->pkt.gap[o->pkt.n - 1] = us16(o, o->t_pulse - o->t_end);
        o->pkt.pulse[o->pkt.n] = us16(o, len); o->pkt.gap[o->pkt.n] = 0; o->pkt.n++;
        o->t_end_prev = o->t_end; o->t_end = t_end; o->t_last_start = o->t_pulse;
    } else o->n_glitch++;
    o->carrier = false; o->on_cnt = o->off_cnt = 0;
}

void ook_sample(ook_t* o, float x)
{
    float y = fabsf(x - o->prev); o->prev = x;
    /* maximum glissant de |y| sur OOK_MAXWIN échantillons : un seul échantillon
     * fort suffit à prouver le souffle, c'est le bon critère pour son absence */
    o->ring[o->ridx] = y; if (++o->ridx >= OOK_MAXWIN) o->ridx = 0;
    float m = 0; for (int i = 0; i < OOK_MAXWIN; i++) if (o->ring[i] > m) m = o->ring[i];
    uint32_t now = ++o->warm;
    /* seuil d'entrée adaptatif : dans une rafale, le niveau de porteuse mesuré
     * (quiet) peut monter à 30 % du souffle (signal moyen) ; on suit avec un
     * seuil entre on_frac et on_frac_max, et autant d'échantillons calmes en
     * plus que le seuil est haut (fausses porteuses). Entre deux rafales, retour
     * au plus strict. */
    float q = o->quiet / o->noise;
    float thr = 2.2f * q;                 /* le maximum sur 4 échantillons du résidu vaut ~2 fois sa moyenne */
    if (thr < o->on_frac) thr = o->on_frac;
    if (thr > o->on_frac_max) thr = o->on_frac_max;
    int min_on = o->min_on + (int)((thr - o->on_frac) / (o->on_frac_max - o->on_frac + 1e-6f) * 4.0f + 0.5f);
    if (!o->carrier) {
        o->noise += (y - o->noise) * (now < 2000 ? 0.01f : 0.001f);      /* souffle, ~30 ms, hors porteuse */
        if (o->noise < 1e-3f) o->noise = 1e-3f;
        if (o->pkt.n == 0 && o->quiet > 0.1f * o->noise) o->quiet -= (o->quiet - 0.1f * o->noise) * 0.001f;   /* hors rafale : retour au strict */
        if (m < thr * o->noise) {
            if (++o->on_cnt >= min_on && now > 1000) carrier_on(o, now);
        } else o->on_cnt = 0;
        if (!o->carrier && o->pkt.n > 0 && now - o->t_end >= o->max_gap) { o->pkt.gap[o->pkt.n - 1] = us16(o, now - o->t_end); emit(o); o->quiet = 0.1f * o->noise; }
    } else {
        if (now - o->t_pulse > (uint32_t)OOK_MAXWIN) o->quiet += (y - o->quiet) * 0.02f;   /* niveau de porteuse dans la rafale */
        if (m > o->off_frac * o->noise) {
            if (++o->off_cnt >= o->min_off) carrier_off(o, now);
        } else o->off_cnt = 0;
    }
}

void ook_flush(ook_t* o)
{
    if (o->carrier) { o->off_cnt = 0; carrier_off(o, o->warm); }
    if (o->pkt.n > 0 && o->pkt.gap[o->pkt.n - 1] == 0) o->pkt.gap[o->pkt.n - 1] = us16(o, o->warm - o->t_end);
    emit(o);
}

bool ook_carrier(const ook_t* o) { return o->carrier; }
float ook_noise(const ook_t* o) { return o->noise; }
