/* ookpulse.h -- détecteur d'impulsions OOK sur l'audio discriminateur d'un
 * récepteur FM (UVLink, mode ISM 433/868) : pendant une porteuse, le souffle
 * disparaît ; sans porteuse, le souffle est là. On suit l'enveloppe du souffle
 * et on en déduit les impulsions (porteuse) et les intervalles (silence) en
 * microsecondes, le format d'entrée « .ook » de rtl_433, qui fait ensuite le
 * décodage des protocoles (capteurs météo, télécommandes...).
 *
 * Chaîne : différence (x[n] - x[n-1], retire le continu et privilégie le
 * haut du spectre où le souffle domine) -> |y| -> maximum glissant sur
 * OOK_MAXWIN échantillons -> comparaison à une fraction du niveau de souffle
 * suivi (seulement quand on est « sans porteuse ») avec hystérésis et durées
 * minimales -> durées en µs -> paquet clos par un long silence.
 *
 * C portable : banc PC (firmware/test/host) et ESP32.
 */
#ifndef OOKPULSE_H
#define OOKPULSE_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define OOK_MAX_PULSES   640     /* paires impulsion/intervalle par paquet (rtl_433 : 1200) */
#define OOK_MAXWIN       4       /* fenêtre du maximum glissant (125 µs à 32 kHz) */

typedef struct {
    uint16_t pulse[OOK_MAX_PULSES];   /* durées porteuse, µs (saturées à 65535) */
    uint16_t gap[OOK_MAX_PULSES];     /* silence qui suit chaque impulsion, µs */
    int      n;                       /* nombre de paires */
} ook_packet_t;

typedef void (*ook_cb_t)(const ook_packet_t* pkt, void* user);

typedef struct {
    float   rate;            /* Hz */
    float   us_per_sample;
    float   prev;            /* x[n-1] */
    float   ring[OOK_MAXWIN];
    int     ridx;
    float   noise;           /* niveau de souffle suivi (moyenne de |y| sans porteuse) */
    float   quiet;           /* niveau suivi pendant la porteuse (résidu de souffle) */
    float   on_frac, off_frac;   /* seuils relatifs au souffle : entrée / sortie de porteuse */
    float   on_frac_max;         /* seuil d'entrée le plus haut toléré (adaptatif au niveau de porteuse de la rafale) */
    int     min_on, min_off;     /* durées minimales (échantillons) avant de valider un changement */
    int     on_cnt, off_cnt;     /* compteurs d'hystérésis temporelle */
    bool    carrier;             /* état courant */
    uint32_t t_pulse, t_end;     /* début de l'impulsion en cours, fin de la dernière impulsion validée (échantillons) */
    uint32_t t_last_start, t_end_prev;   /* début de la dernière impulsion validée, fin de l'avant-dernière (fusion des trous courts) */
    int      min_pulse;          /* impulsion plus courte = creux du souffle, ignorée */
    int      min_gap;            /* trou plus court = retour de souffle dans une porteuse, fusionné */
    uint32_t max_gap;            /* silence (échantillons) qui clôt le paquet */
    uint32_t warm;               /* échantillons vus (le suivi du souffle a besoin d'un peu de temps) */
    ook_packet_t pkt;
    ook_cb_t cb; void* user;
    /* statistiques */
    uint32_t n_packets, n_pulses, n_short, n_glitch, n_merged;   /* paquets émis, impulsions émises, paquets trop courts, creux ignorés, trous fusionnés */
    int      min_pulses;                     /* paires minimales pour émettre un paquet */
} ook_t;

void ook_init(ook_t* o, float rate_hz, ook_cb_t cb, void* user);
void ook_sample(ook_t* o, float x);
void ook_flush(ook_t* o);               /* clôt le paquet en cours (fin de flux) */
bool ook_carrier(const ook_t* o);
float ook_noise(const ook_t* o);

#endif
