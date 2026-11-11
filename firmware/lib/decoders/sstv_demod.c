/* sstv_demod.c -- see sstv_demod.h. Pure C, no pico-sdk, host-testable. */
#include "sstv_demod.h"

#include <string.h>

enum {
    ST_HUNT = 0,      /* looking for a sustained ~1900 Hz leader tone */
    ST_WAIT_BREAK,    /* leader seen, waiting for the dip to ~1200 Hz */
    ST_IN_BREAK,      /* in the ~1200 Hz break, waiting for the rise back */
    ST_LEADER2,       /* anchor set at the break->leader2 edge; riding out
                        * the remaining ~300 ms of the second leader tone */
    /* ⚠️ (2026-10-03, VRAIE cause de "lines=0" malgre VIS/parite trouves --
     * voir sstv_tx.c, sstv_send_vis(), "BUG PROTOCOLAIRE... un dixieme
     * element que nous n'avons jamais envoye" -- le TX radio ENVOIE bien un
     * bit de START (1200 Hz, 30 ms) entre le 2e leader et les 7 bits de
     * donnees, conforme a la norme SSTV reelle, mais ce decodeur RX n'a
     * jamais ete mis a jour pour s'attendre a cette 10e fenetre -- il
     * enchainait directement sur 7 fenetres de "donnees" des la fin du
     * leader2, lisant donc systematiquement la QUEUE du bit de start comme
     * bit0, chaque bit reel decale d'un cran, et le vrai bit6 perdu dans la
     * case "parite" (bits#, retour terrain : code VIS lu = 120 = 60*2 pour
     * Scottie 1, exactement un decalage d'un cran avec un 0 insere en bas).
     * Consomme maintenant cette fenetre de 30 ms separement, SANS la
     * compter comme un bit de donnees, avant d'entrer dans ST_VIS_WINDOW. */
    ST_VIS_START,     /* 30 ms start bit (1200 Hz), consumed but not stored */
    ST_VIS_WINDOW,    /* 9 x 30 ms windows: 7 data bits, parity, stop */
    ST_IMG_FIRSTSYNC, /* Scottie quirk: extra 9 ms sync before line 0 only */
    ST_IMG_STEP,      /* image : element d->seq[d->seq_pos] de la ligne en cours */
};

#define HZ(v)   ((uint32_t)(v) * 1000u)   /* Hz -> milli-Hz */

#define LEADER_HZ        HZ(1900)
#define BREAK_HZ         HZ(1200)
#define BAND_TOL         HZ(60)   /* +/- tolerance for leader/break bands */

#define SCOTTIE1_SYNC_HZ   HZ(1200)
#define SCOTTIE1_BLACK_HZ  HZ(1500)
#define SCOTTIE1_WHITE_HZ  HZ(2300)

#define SSTV_SCOTTIE1_LINE_US  428220u   /* 1 ligne Scottie 1, nominal (repli sans mode) */

/* Table des modes (SSTV Handbook OK2MNM ch. 4, MMSSTV). Durees en us. */
static const sstv_mode_t SSTV_MODES[] = {
    /* id                  vis fam               w    h   sync  porch sep   chan    ligne    nom */
    { SSTV_MODE_SCOTTIE1,  60, SSTV_FAM_SCOTTIE, 320, 256,  9000, 1500, 1500, 138240,  428220, "Scottie 1"  },
    { SSTV_MODE_SCOTTIE2,  56, SSTV_FAM_SCOTTIE, 320, 256,  9000, 1500, 1500,  88064,  277692, "Scottie 2"  },
    { SSTV_MODE_SCOTTIEDX, 76, SSTV_FAM_SCOTTIE, 320, 256,  9000, 1500, 1500, 345600, 1050300, "Scottie DX" },
    { SSTV_MODE_MARTIN1,   44, SSTV_FAM_MARTIN,  320, 256,  4862,  572,  572, 146432,  446446, "Martin 1"   },
    { SSTV_MODE_MARTIN2,   40, SSTV_FAM_MARTIN,  320, 256,  4862,  572,  572,  73216,  226798, "Martin 2"   },
    { SSTV_MODE_PD50,      93, SSTV_FAM_PD,      320, 256, 20000, 2080,    0,  91520,  388160, "PD 50"      },
    { SSTV_MODE_PD90,      99, SSTV_FAM_PD,      320, 256, 20000, 2080,    0, 170240,  703040, "PD 90"      },
    { SSTV_MODE_PD120,     95, SSTV_FAM_PD,      640, 496, 20000, 2080,    0, 121600,  508480, "PD 120"     },
    { SSTV_MODE_PD160,     98, SSTV_FAM_PD,      512, 400, 20000, 2080,    0, 195584,  804416, "PD 160"     },
    { SSTV_MODE_PD180,     96, SSTV_FAM_PD,      640, 496, 20000, 2080,    0, 183040,  754240, "PD 180"     },
    { SSTV_MODE_PD240,     97, SSTV_FAM_PD,      640, 496, 20000, 2080,    0, 244480, 1000000, "PD 240"     },
    { SSTV_MODE_PD290,     94, SSTV_FAM_PD,      800, 616, 20000, 2080,    0, 228800,  937280, "PD 290"     },
};
#define SSTV_N_MODES ((int)(sizeof SSTV_MODES / sizeof SSTV_MODES[0]))

const sstv_mode_t *sstv_mode_by_id(uint8_t id)
{
    for (int i = 0; i < SSTV_N_MODES; i++)
        if (SSTV_MODES[i].id == id) return &SSTV_MODES[i];
    return 0;
}
static const sstv_mode_t *sstv_mode_by_vis(uint8_t vis)
{
    for (int i = 0; i < SSTV_N_MODES; i++)
        if (SSTV_MODES[i].vis == vis) return &SSTV_MODES[i];
    return 0;
}

/* Elements d'une ligne, et sequence de chaque famille (EL_END = fin de balayage) */
enum { EL_SYNC = 0, EL_PORCH, EL_SEP, EL_CH0, EL_CH1, EL_CH2, EL_CH3, EL_END };
static const uint8_t SEQ_SCOTTIE[] = { EL_SEP, EL_CH0, EL_SEP, EL_CH1, EL_SYNC, EL_PORCH, EL_CH2, EL_END };
static const uint8_t SEQ_MARTIN[]  = { EL_SYNC, EL_PORCH, EL_CH0, EL_SEP, EL_CH1, EL_SEP, EL_CH2, EL_SEP, EL_END };
static const uint8_t SEQ_PD[]      = { EL_SYNC, EL_PORCH, EL_CH0, EL_CH1, EL_CH2, EL_CH3, EL_END };

static uint32_t us_to_samples(const sstv_demod_t *d, uint32_t us)
{
    return (uint32_t)(((uint64_t)d->sample_rate_hz * us) / 1000000u);
}

/* Passe-bande 900..2400 Hz = passe-haut 900 Hz + passe-bas 2400 Hz, chacun
 * du 2e ordre (Butterworth, Q 0.707, RBJ). Coefficients en Q28 :
 * { b0, b1, b2, a1, a2 } pour chaque etage. Calcules pour 48 kHz (RP2040)
 * et 32 kHz (enregistrements PC de test). */
typedef struct sstv_bpcoef {
    int32_t hp[5];
    int32_t lp[5];
} sstv_bpcoef_t;

static const sstv_bpcoef_t SSTV_BP_48K = {
    { 246978379, -493956758, 246978379, -492238862, 227239197 },
    {   5391087,   10782175,   5391087, -419032599, 172161492 },
};
static const sstv_bpcoef_t SSTV_BP_32K = {
    { 236897295, -473794589, 236897295, -470076332, 209077390 },
    {  11073912,   22147824,  11073912, -362110772, 137970964 },
};

/* Un pas de biquad (forme directe I), Q28. */
static int32_t sstv_bq_step(int64_t *s, const int32_t *c, int32_t x)
{
    int64_t y = ((int64_t)c[0] * x + (int64_t)c[1] * s[0] + (int64_t)c[2] * s[1]
                 - (int64_t)c[3] * s[2] - (int64_t)c[4] * s[3]) >> 28;
    s[1] = s[0];
    s[0] = x;
    s[3] = s[2];
    s[2] = y;
    return (int32_t)y;
}

void sstv_demod_init(sstv_demod_t *d, uint32_t sample_rate_hz)
{
    memset(d, 0, sizeof *d);
    d->sample_rate_hz = sample_rate_hz;
    if (sample_rate_hz == 48000u)      d->cf = &SSTV_BP_48K;
    else if (sample_rate_hz == 32000u) d->cf = &SSTV_BP_32K;
    /* Goertzel 1200 Hz : 2cos(2*pi*1200/fs) en Q14, bloc de 5 ms */
    if (sample_rate_hz == 48000u) {
        d->gz_on = true; d->gz_c = 32362; d->gz_blk = 240u;
    } else if (sample_rate_hz == 32000u) {
        d->gz_on = true; d->gz_c = 31864; d->gz_blk = 160u;
    }
    d->pixel = -1;
    d->lines_ready = 1;
    d->state = ST_HUNT;
}

void sstv_demod_set_rate(sstv_demod_t *d, uint32_t actual_hz)
{
    if (actual_hz) d->sample_rate_hz = actual_hz;
}

/* DC-centre + zero-crossing frequency estimate, same adaptive envelope
 * trick as sonde_demod.c, but reporting a continuous instantaneous
 * frequency (linearly-interpolated fractional-sample crossing time)
 * instead of slicing a bit. */
/* Synchro de ligne par Goertzel 1200 Hz sur blocs de 5 ms. Un bloc est
 * "synchro" si la puissance a 1200 Hz est une part notable de l'energie du
 * bloc (un sinus pur donne 0,5). Evenement = debut d'une serie de blocs. */
static void sstv_sync_block_end(sstv_demod_t *d)
{
    int64_t  N = d->gz_blk;
    int64_t  c = d->gz_c;
    int64_t  p = d->gz_s1 * d->gz_s1 + d->gz_s2 * d->gz_s2
               - (((c * d->gz_s1) >> 14) * d->gz_s2);
    int64_t  e = (int64_t)(d->gz_e > 0x7FFFFFFFFFFFu ? 0x7FFFFFFFFFFFu : d->gz_e);
    bool sync = (e > N * 100) && (p > 0) && (p * 10 > e * N * 3);   /* part >= 30 % (sinus pur = 50 %) */

    if (sync) {
        if (d->blk_run == 0) {
            /* debut de la synchro : meme convention qu'avant (instant du 1er bloc) */
            uint32_t start = d->sample_idx - (uint32_t)N;
            if (d->sync_have) {
                uint32_t iv = start - d->sync_last;
                uint32_t ln = (uint32_t)((uint64_t)d->sample_rate_hz * (d->mode ? d->mode->line_us : SSTV_SCOTTIE1_LINE_US) / 1000000u);
                uint32_t lo = ln - ln / 14u, hi = ln + ln / 14u;   /* +/- 7 % */
                if (iv >= lo && iv <= hi) {
                    d->n_sync_iv++;
                    d->sum_sync_iv += iv;
                    d->sync_last = start;
                } else if (iv > hi) {
                    d->sync_last = start;
                }
            } else {
                d->sync_last = start;
                d->sync_have = true;
            }
        }
        d->blk_run++;
        d->blk_miss = 0;
    } else if (d->blk_run) {
        if (++d->blk_miss > 1u) {
            d->blk_run = 0;
            d->blk_miss = 0;
        }
    }

    d->gz_s1 = d->gz_s2 = 0;
    d->gz_e  = 0;
    d->gz_n  = 0;
}

static void sstv_goertzel_step(sstv_demod_t *d, int32_t x)
{
    if (!d->gz_on)
        return;
    int64_t s0 = (int64_t)x + (((int64_t)d->gz_c * d->gz_s1) >> 14) - d->gz_s2;
    d->gz_s2 = d->gz_s1;
    d->gz_s1 = s0;
    d->gz_e += (uint64_t)((int64_t)x * x);
    if (++d->gz_n >= d->gz_blk)
        sstv_sync_block_end(d);
}

static void sstv_update_freq(sstv_demod_t *d, int32_t sample)
{
    int32_t sig;
    d->freq_new = false;
    if (d->cf) {
        /* passe-bande : coupe le bruit hors 900..2400 Hz, DC compris */
        int32_t h = sstv_bq_step(d->bqs[0], d->cf->hp, sample);
        sig = sstv_bq_step(d->bqs[1], d->cf->lp, h);
        d->lp  += (sig - d->lp) >> 2;      /* garde lp pour la compatibilite des tests */
        sstv_goertzel_step(d, sig);
    } else {
        d->lp  += (sample - d->lp) >> 2;
        d->lp2 += (d->lp - d->lp2) >> 2;   /* repli : 2 passes-bas */
        sig = d->lp2;
    }

#ifndef SSTV_ENV_DECAY_SHIFT
#define SSTV_ENV_DECAY_SHIFT 8    /* enveloppe : relache en ~8 ms a 32 kHz (2026-10-31 : >>10 laissait l hysteresis bloquee ~4 ms apres une synchro forte, ligne 0 decalee de 14 px) */
#endif
#ifndef SSTV_HYST_DIV
#define SSTV_HYST_DIV 8    /* hysteresis = span/8 (span/4 ratait les premiers cycles d un ton plus faible apres la desaccentuation) */
#endif
    if (sig > d->lp_hi) d->lp_hi += (sig - d->lp_hi) >> 2;
    else                d->lp_hi -= (d->lp_hi - d->lp_lo) >> SSTV_ENV_DECAY_SHIFT;
    if (sig < d->lp_lo) d->lp_lo += (sig - d->lp_lo) >> 2;
    else                d->lp_lo += (d->lp_hi - d->lp_lo) >> SSTV_ENV_DECAY_SHIFT;

    int32_t centered = sig - ((d->lp_hi + d->lp_lo) >> 1);

    d->samples_since_cross++;

    /* Hysteresis (declencheur de Schmitt) : un passage a zero n'est accepte
     * que si le signal est descendu franchement sous -H avant. Le bruit qui
     * oscille autour de zero ne peut plus compter comme passages. */
    int32_t span = d->lp_hi - d->lp_lo;
    int32_t hyst = (span > 0) ? span / SSTV_HYST_DIV : 0;
    if (centered < -hyst)
        d->zc_armed = true;

    if (d->prev_centered < 0 && centered >= 0 && d->zc_armed) {
        d->zc_armed = false;
        int32_t denom = centered - d->prev_centered;
        int32_t frac_q10 = 0;
        if (denom != 0) {
            frac_q10 = (int32_t)(((int64_t)(-d->prev_centered) * 1024) / denom);
            if (frac_q10 < 0) frac_q10 = 0;
            if (frac_q10 > 1024) frac_q10 = 1024;
        }
        /* true period = samples_since_cross + frac_cur - frac_prev -- see
         * sstv_demod.h's warning on why frac_prev cannot be dropped. */
        int32_t period_q10 = ((int32_t)d->samples_since_cross << 10)
                            + frac_q10 - d->frac_prev_q10;
        if (period_q10 > 0) {
            d->freq_mhz = (uint32_t)(((uint64_t)d->sample_rate_hz * 1000u * 1024u)
                                      / (uint32_t)period_q10);
            d->have_freq = true;
            d->freq_new = true;
            d->period_cur = (uint32_t)(period_q10 >> 10);
        }
        d->frac_prev_q10 = frac_q10;
        d->samples_since_cross = 0;
    }
    d->prev_centered = centered;
}

static void sstv_enter(sstv_demod_t *d, int state)
{
    d->state = state;
    d->state_samples = 0;
    d->hz_accum = 0;
    d->hz_accum_n = 0;
}

/* Entree dans l'element pos de la sequence du mode courant. */
static void sstv_enter_step(sstv_demod_t *d, int pos)
{
    sstv_enter(d, ST_IMG_STEP);
    d->seq_pos = (uint8_t)pos;
    d->pixel = -1;
    if (d->seq[pos] == EL_SYNC) d->sync_enter_idx = d->sample_idx;
}

static uint32_t sstv_el_us(const sstv_demod_t *d, uint8_t el)
{
    const sstv_mode_t *m = d->mode;
    switch (el) {
    case EL_SYNC:  return m->sync_us;
    case EL_PORCH: return m->porch_us;
    case EL_SEP:   return m->sep_us;
    default:       return m->chan_us;
    }
}

/* Fin d'impulsion de synchro (1200 Hz tenu la duree de la synchro du mode,
 * puis saut net au-dessus de 1380 Hz) : a appeler quand une estimation de
 * frequence vient de tomber. Le front est estime au debut du cycle court qui
 * vient de se terminer, moins un demi-cycle a 1200 Hz (le front est quelque
 * part dans le dernier cycle long) : erreur uniforme +/- 0,42 ms, ~1 px. */
static void sstv_sync_edge(sstv_demod_t *d)
{
    uint32_t f = d->freq_mhz / 1000u;
    /* moyenne glissante sur 4 cycles pour l'en-tete VIS (bruit : une
     * estimation par cycle est trop nerveuse pour une bande de +/- 60 Hz) */
    d->favg[d->favg_i & 3] = d->freq_mhz; d->favg_i++;
    d->freq_avg_mhz = (d->favg[0] + d->favg[1] + d->favg[2] + d->favg[3]) / 4u;

    if (d->edge_pending_n) {
        /* sortie de la bande 1200 vue : on attend (4 estimations, ~3 ms) que la
         * frequence atteigne 1380 Hz. Sur l'air la transition 1200 -> 1500 Hz
         * est adoucie par les filtres du poste (plusieurs cycles a 1300-1400) :
         * exiger 1400 Hz en 2 estimations la ratait (2026-10-31, image decalee
         * d'une synchro + porche, aucun recalage). */
        if (f >= 1380u) {
            d->sync_end_idx = d->edge_pending; d->sync_end_new = true; d->edge_pending_n = 0; d->in1200 = false;
            return;
        }
        if (f <= 1300u) { d->edge_pending_n = 0; return; }   /* creux de bruit : toujours en synchro */
        if (--d->edge_pending_n == 0) d->in1200 = false;
        return;
    }
    if (f >= 1100u && f <= 1300u) {
        if (!d->in1200) { d->in1200 = true; d->run1200_start = d->sample_idx; }
        return;
    }
    if (!d->in1200) return;
    if (f < 1100u) { d->in1200 = false; return; }
    uint32_t run = d->sample_idx - d->run1200_start;
    /* duree admise : de la moitie a 1,5 fois la synchro du mode (+ 2 ms) ;
     * Scottie 9 ms -> 4,5..15,5 ms, Martin 4,862 -> 2,4..9,3, PD 20 -> 10..32 */
    uint32_t sync_us = d->mode ? d->mode->sync_us : 9000u;
    uint32_t lo = us_to_samples(d, sync_us / 2u), hi = us_to_samples(d, sync_us * 3u / 2u + 2000u);
    /* Martin, PD : la synchro de la ligne 0 suit sans interruption le bit de
     * stop du VIS (1200 Hz, 30 ms) : la serie est plus longue de 30 ms */
    if (d->state == ST_IMG_STEP && d->line == 0 && d->seq_pos == d->sync_pos && d->sync_pos == 0)
        hi += us_to_samples(d, 30000u);
    if (run >= lo && run <= hi) {
        d->edge_pending = d->sample_idx - d->period_cur - d->sample_rate_hz * 417u / 1000000u;
        d->edge_pending_n = 4;
    } else d->in1200 = false;
}

static uint8_t sstv_luma_from_freq(uint32_t freq_mhz)
{
    int32_t v = ((int32_t)(freq_mhz / 1000u) - 1500) * 255;
    v /= (2300 - 1500);
    if (v < 0)   v = 0;
    if (v > 255) v = 255;
    return (uint8_t)v;
}

/* Ecrit le pixel en cours (moyenne des estimations tombees dans sa fenetre)
 * dans la composante ch. */
static void sstv_flush_pixel(sstv_demod_t *d, int ch)
{
    if (d->pixel >= 0 && d->pixel < (int)d->mode->width && d->hz_accum_n)
        d->raw[ch][d->pixel] = sstv_luma_from_freq((uint32_t)(d->hz_accum / d->hz_accum_n));
}

/* Fin d'un balayage : 1 ligne (RVB) ou 2 (PD) pretes dans raw[][]. */
static void sstv_line_done(sstv_demod_t *d)
{
    d->lines_ready = (d->mode->family == SSTV_FAM_PD) ? 2 : 1;
    d->line_ready_idx = d->line;
    d->line += d->lines_ready;
    d->pixel = -1;
    if (d->line >= (int)d->mode->height) {
        d->line = 0;
        sstv_enter(d, ST_HUNT);     /* image complete : on guette le VIS suivant */
    } else
        sstv_enter_step(d, 0);
}

/* Recalage de la ligne sur la synchro detectee : attendue sync_us apres
 * l'entree dans l'element SYNC, acceptee a +/- 10 ms. On reprend la sequence
 * (porche puis composante suivante) a partir du front, en tenant compte du
 * temps deja ecoule. Si le front arrive alors qu'on est encore dans l'element
 * PRECEDENT (base de temps en retard), celui-ci est tronque ; pour Martin et
 * PD c'est le dernier element de la ligne d'avant, qui est alors livree.
 * Retourne vrai si un balayage vient d'etre livre ainsi. */
static bool sstv_resync(sstv_demod_t *d)
{
    d->sync_end_new = false;
    if (d->state != ST_IMG_STEP) return false;
    const sstv_mode_t *m = d->mode;
    uint32_t sync_n = us_to_samples(d, m->sync_us);
    int32_t  tol = (int32_t)us_to_samples(d, 10000);  /* rien d'autre a 1200 Hz dans une ligne : large */
    int pos = d->seq_pos, sp = d->sync_pos;
    int prev = (sp + d->seq_len - 1) % d->seq_len;   /* element avant la synchro (fin de la ligne d'avant si sp == 0) */
    int32_t expect;
    bool early = false;
    if (pos == sp || pos == sp + 1 || (pos == sp + 2 && d->state_samples <= us_to_samples(d, 6000))) {
        expect = (int32_t)(d->sync_enter_idx + sync_n);
    } else if (pos == prev) {
        uint32_t len = us_to_samples(d, sstv_el_us(d, d->seq[pos]));
        uint32_t remaining = d->state_samples < len ? len - d->state_samples : 0;
        expect = (int32_t)(d->sample_idx + remaining + sync_n);
        early = true;
    } else {
        d->n_resync_miss++;
        return false;
    }
    int32_t err = (int32_t)d->sync_end_idx - expect;
    d->last_resync_err = err;
    if (err < -tol || err > tol) { d->n_resync_miss++; return false; }
    d->n_resync++;

    bool delivered = false;
    if (early) {
        uint8_t el = d->seq[pos];
        if (el >= EL_CH0 && el <= EL_CH3) sstv_flush_pixel(d, el - EL_CH0);
        d->pixel = -1;
        if (sp == 0) {
            sstv_line_done(d);
            delivered = true;
            if (d->state != ST_IMG_STEP) return true;   /* image complete */
        }
    }
    uint32_t elapsed = d->sample_idx - d->sync_end_idx;
    uint32_t porch = us_to_samples(d, m->porch_us);
    if (elapsed <= porch) { sstv_enter_step(d, sp + 1); d->state_samples = elapsed; }
    else { sstv_enter_step(d, sp + 2); d->state_samples = elapsed - porch; }
    return delivered;
}

/* One VIS window (30 ms): classify the boxcar-averaged frequency as a data
 * bit (1100 Hz = 1, 1300 Hz = 0, threshold at the 1200 Hz midpoint) --
 * bit_idx 7 is the parity bit, 8 is the stop bit (consumed for timing only,
 * its value is not checked). Returns true once the stop bit window closes,
 * with *vis_ok telling whether the parity check passed. */
static bool sstv_vis_window_done(sstv_demod_t *d)
{
    uint32_t avg = d->hz_accum_n ? (uint32_t)(d->hz_accum / d->hz_accum_n) : 0;
    int bit = (avg < HZ(1200)) ? 1 : 0;

    if (d->vis_bit_idx < 7) {
        if (bit) d->vis_bits_val = (uint8_t)(d->vis_bits_val | (1u << d->vis_bit_idx));
        d->vis_bit_idx++;
        return false;
    }
    if (d->vis_bit_idx == 7) {
        int ones = 0;
        for (int i = 0; i < 7; i++)
            if (d->vis_bits_val & (1u << i)) ones++;
        d->vis_parity_ok = ((ones + bit) & 1) == 0;   /* even parity overall */
        d->vis_bit_idx++;
        return false;
    }
    /* idx == 8: stop bit window just finished */
    return true;
}

bool sstv_demod_clock_error_ppm(const sstv_demod_t *d, uint32_t min_iv, int32_t *ppm)
{
    if (d->n_sync_iv < min_iv || d->sum_sync_iv == 0u)
        return false;
    if (d->mode && d->mode->sync_us < 6000u)   /* Martin (4,862 ms) : trop court pour les blocs Goertzel de 5 ms */
        return false;
    /* moyenne mesuree / nominal, en ppm (entiers, 64 bits) */
    uint64_t nominal = (uint64_t)d->sample_rate_hz * (d->mode ? d->mode->line_us : SSTV_SCOTTIE1_LINE_US) / 1000000u;
    uint64_t mean_x1e6 = d->sum_sync_iv * 1000000u / d->n_sync_iv;    /* moyenne * 1e6 */
    int64_t  ratio_ppm = (int64_t)(mean_x1e6 / nominal) - 1000000;      /* (moyenne/nominal - 1) * 1e6 */
    *ppm = (int32_t)ratio_ppm;
    return true;
}

static uint8_t sstv_clamp8(int32_t v)
{
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

const uint8_t *sstv_demod_rgb_line(sstv_demod_t *d, int which)
{
    const sstv_mode_t *m = d->mode;
    uint8_t *o = d->rgb;
    if (!m) return o;
    int w = m->width;
    if (m->family == SSTV_FAM_PD) {
        /* Y pair / impair, R-Y (Cr), B-Y (Cb) ; BT.601 plage studio (MMSSTV) :
         * R = 1,164 (Y-16) + 1,596 (Cr-128) ; G = 1,164 (Y-16) - 0,813 (Cr-128) - 0,391 (Cb-128) ;
         * B = 1,164 (Y-16) + 2,018 (Cb-128). En Q10. */
        const uint8_t *Y = d->raw[which ? 3 : 0], *Cr = d->raw[1], *Cb = d->raw[2];
        for (int x = 0; x < w; x++) {
            int32_t y = 1192 * ((int32_t)Y[x] - 16), cr = (int32_t)Cr[x] - 128, cb = (int32_t)Cb[x] - 128;
            o[3 * x]     = sstv_clamp8((y + 1634 * cr) >> 10);
            o[3 * x + 1] = sstv_clamp8((y - 832 * cr - 400 * cb) >> 10);
            o[3 * x + 2] = sstv_clamp8((y + 2066 * cb) >> 10);
        }
    } else {
        /* Scottie / Martin : composantes emises dans l'ordre Vert, Bleu, Rouge */
        for (int x = 0; x < w; x++) {
            o[3 * x] = d->raw[2][x]; o[3 * x + 1] = d->raw[0][x]; o[3 * x + 2] = d->raw[1][x];
        }
    }
    return o;
}

bool sstv_demod_sample(sstv_demod_t *d, int32_t sample)
{
    sstv_update_freq(d, sample);
    d->state_samples++;

    /* Synchros de ligne : 1200 Hz tenu >= 6 ms. Intervalle entre deux synchros
     * retenu seulement s'il est proche d'une ligne (+/- 7 %). */
    d->sample_idx++;
    if (d->freq_new) sstv_sync_edge(d);
    if (d->have_freq) {
        d->n_freq++;
        uint32_t f = d->freq_mhz / 1000u;          /* Hz */
        uint32_t b = (f < 900u) ? 0u : (f < 1100u) ? 1u : (f < 1300u) ? 2u : (f < 1450u) ? 3u
                   : (f < 1650u) ? 4u : (f < 1900u) ? 5u : (f < 2150u) ? 6u : 7u;
        d->fhist[b]++;
    }

    if (d->have_freq) {
        d->hz_accum += d->freq_mhz;
        d->hz_accum_n++;
    }
    if (d->sync_end_new && sstv_resync(d)) return true;

    switch (d->state) {

    case ST_HUNT:
        if (!d->have_freq) return false;
        if (d->freq_avg_mhz > LEADER_HZ - BAND_TOL && d->freq_avg_mhz < LEADER_HZ + BAND_TOL) {
            if (d->state_samples >= us_to_samples(d, 100000)) {  /* 100 ms seen */
                d->n_leader++;
                sstv_enter(d, ST_WAIT_BREAK);
            }
        } else {
            d->state_samples = 0;   /* band lost -- restart the run-length count */
        }
        return false;

    case ST_WAIT_BREAK:
        if (d->have_freq && d->freq_avg_mhz > BREAK_HZ - BAND_TOL && d->freq_avg_mhz < BREAK_HZ + BAND_TOL) {
            d->n_break++;
            sstv_enter(d, ST_IN_BREAK);
        } else if (d->state_samples > us_to_samples(d, 400000))   /* gave up */
            sstv_enter(d, ST_HUNT);
        return false;

    case ST_IN_BREAK:
        if (d->have_freq && d->freq_avg_mhz > LEADER_HZ - BAND_TOL) {
            /* rising edge back towards the leader band: only a real VIS
             * break if it lasted a plausible 3-20 ms */
            uint32_t us = (uint32_t)(((uint64_t)d->state_samples * 1000000u) / d->sample_rate_hz);
            if (us >= 3000 && us <= 20000) {
                d->n_leader2++;
                sstv_enter(d, ST_LEADER2);
            } else
                sstv_enter(d, ST_HUNT);
        } else if (d->state_samples > us_to_samples(d, 30000)) {
            sstv_enter(d, ST_HUNT);   /* stayed low too long -- not a break */
        }
        return false;

    case ST_LEADER2:
        if (d->state_samples >= us_to_samples(d, 300000))
            sstv_enter(d, ST_VIS_START);
        return false;

    case ST_VIS_START:
        if (d->state_samples >= us_to_samples(d, 30000)) {
            d->vis_bits_val = 0;
            d->vis_bit_idx  = 0;
            sstv_enter(d, ST_VIS_WINDOW);
        }
        return false;

    case ST_VIS_WINDOW:
        if (d->state_samples >= us_to_samples(d, 30000)) {
            bool done = sstv_vis_window_done(d);
            if (done) {
                d->n_vis_done++;
                const sstv_mode_t *m = sstv_mode_by_vis(d->vis_bits_val);
                if (d->vis_parity_ok && m) {
                    d->n_vis_ok++;
                    d->mode = m;
                    d->seq = (m->family == SSTV_FAM_SCOTTIE) ? SEQ_SCOTTIE : (m->family == SSTV_FAM_MARTIN) ? SEQ_MARTIN : SEQ_PD;
                    d->seq_len = 0; d->sync_pos = 0;
                    while (d->seq[d->seq_len] != EL_END) { if (d->seq[d->seq_len] == EL_SYNC) d->sync_pos = d->seq_len; d->seq_len++; }
                    d->line = 0;
                    d->image_new = true;
                    d->n_resync = d->n_resync_miss = 0; d->last_resync_err = 0;
                    d->n_sync_iv = 0; d->sum_sync_iv = 0; d->sync_have = false;
                    memset(d->raw, 0, sizeof d->raw);
                    if (m->family == SSTV_FAM_SCOTTIE) sstv_enter(d, ST_IMG_FIRSTSYNC);
                    else sstv_enter_step(d, 0);        /* Martin, PD : la ligne 0 commence par sa synchro */
                } else {
                    sstv_enter(d, ST_HUNT);
                }
                return false;
            }
            d->state_samples = 0;
            d->hz_accum = 0;
            d->hz_accum_n = 0;
        }
        return false;

    case ST_IMG_FIRSTSYNC:
        /* Scottie : synchro 9 ms supplementaire avant la ligne 0 selon le
         * SSTV Handbook / CEC -- mais HTCommander et l'emetteur UV-K5 +
         * RP2040 (sstv_tx.c) ne l'emettent PAS (2026-10-31 : image recue
         * decalee d'une synchro). Dans les deux cas le porche puis le Vert
         * commencent au premier saut au-dessus de 1380 Hz : on s'y cale
         * (ce qui efface aussi le retard de detection du VIS) ; 9 ms sans
         * saut = repli sur le cadencement nominal. */
        if ((d->freq_new && d->freq_mhz > HZ(1380)) || d->state_samples >= us_to_samples(d, 9000))
            sstv_enter_step(d, 0);
        return false;

    case ST_IMG_STEP: {
        const sstv_mode_t *m = d->mode;
        uint8_t el = d->seq[d->seq_pos];
        if (el < EL_CH0 || el > EL_CH3) {
            /* synchro, porche, separateur : duree nominale (la synchro est
             * recalee par sstv_resync des que son front de fin est vu) */
            if (d->state_samples >= us_to_samples(d, sstv_el_us(d, el))) {
                if (d->seq[d->seq_pos + 1] == EL_END) { sstv_line_done(d); return true; }   /* Martin : sep final */
                sstv_enter_step(d, d->seq_pos + 1);
            }
            return false;
        }
        /* composante : index de pixel par le ratio EXACT (64 bits) -- voir
         * l'historique dans sstv_demod.h : arrondir une "duree par pixel"
         * (432 us = 20,736 ech. a 48 kHz) faisait pencher l'image. Chaque
         * pixel = moyenne des estimations tombees dans sa fenetre. */
        int ch = el - EL_CH0;
        uint32_t px_u = (uint32_t)(((uint64_t)d->state_samples * 1000000ULL * m->width) /
                                    ((uint64_t)m->chan_us * d->sample_rate_hz));
        int px = (px_u > (uint32_t)m->width) ? (int)m->width : (int)px_u;
        if (px != d->pixel) {
            sstv_flush_pixel(d, ch);
            d->pixel = px;
            d->hz_accum = 0;
            d->hz_accum_n = 0;
        }
        if (px_u >= (uint32_t)m->width) {
            sstv_flush_pixel(d, ch);
            d->pixel = -1;
            if (d->seq[d->seq_pos + 1] == EL_END) { sstv_line_done(d); return true; }
            sstv_enter_step(d, d->seq_pos + 1);
        }
        return false;
    }

    default:
        sstv_enter(d, ST_HUNT);
        return false;
    }
}
