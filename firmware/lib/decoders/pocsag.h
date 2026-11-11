/* pocsag.h -- decodeur POCSAG (512, 1200 et 2400 bauds) sur l'audio FM
 * demodule, C portable (pas de pico-sdk ni d'Arduino), testable sur PC.
 *
 * Port en bibliotheque du md_pocsag.c du decodeur multiple de Portsdown 4
 * (lui-meme porte du Pocsag.java de HTMOD / Radtel Tools) : sur un signal
 * faible il decode la ou multimon-ng ne sort que des adresses (essai du
 * 9/11/2026, reseau SDIS 173,7 MHz, rafale a 1 dB au-dessus du souffle :
 * 7 messages corrects contre 0).
 *
 * Chaine, par debit (les trois tournent en parallele sur le meme signal) :
 * anti-continu -> moyenne glissante (~1/4 de bit) -> bascule a hysteresis
 * (35 % de la crete suivie) -> DPLL calee sur les transitions -> integration
 * sur le bit -> registre 32 bits -> synchro 0x7CD215D8 (ou son inverse : la
 * polarite du discriminateur est inconnue) -> lots de 16 mots ->
 * correction BCH(31,21) jusqu'a 2 bits -> messages.
 *
 * Entree : un echantillon a la fois (float, echelle quelconque : la bascule
 * suit la crete), a la cadence donnee a pocsag_init(). L'audio doit etre
 * « plat » (sortie discriminateur, sans filtre de desaccentuation) pour 1200
 * et 2400 bauds ; 512 bauds passe aussi par le chemin audio normal.
 *
 * Sortie : un rappel par message (pocsag_msg_t). Le choix texte / numerique
 * se fait au contenu (les reseaux SDIS envoient du texte avec la fonction
 * 0), sauf si pocsag_set_force() l'impose. Le texte est en ASCII 7 bits tel
 * que transmis (les caracteres nationaux $ @ [ \ ] ` { | } ~ sont a traduire
 * par l'affichage selon le pays : voir le jeu « Charset » de Portsdown).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define POCSAG_PAYLOAD_MAX 160      /* mots de 20 bits par message : 457 caracteres de texte, 800 chiffres */
#define POCSAG_TEXT_MAX    480      /* texte rendu (NUL compris) */
#define POCSAG_MA_MAX      64       /* longueur maximale de la moyenne glissante (1/4 de bit : 16 a 32 kHz / 512 Bd) */

typedef struct {
    uint16_t baud;                  /* 512, 1200 ou 2400 */
    uint32_t address;               /* RIC (21 bits) */
    uint8_t  function;              /* 0..3 */
    bool     numeric;               /* texte rendu en numerique (sinon alphanumerique) */
    bool     tone_only;             /* appel sans message (bip seul) : text vide */
    bool     partial;               /* mots incorrigibles ET texte illisible (residu de fin de rafale) : cache sauf show_partial */
    uint8_t  corrected;             /* bits corriges par le BCH sur le message */
    uint8_t  bad;                   /* mots que le BCH n'a pas pu corriger (gardes tels quels) */
    char     text[POCSAG_TEXT_MAX]; /* ASCII 7 bits, '?' pour un caractere de commande inattendu */
} pocsag_msg_t;

typedef void (*pocsag_cb_t)(const pocsag_msg_t* m, void* user);

typedef struct {
    int      baud;
    float    step;                  /* bits par echantillon */
    int      ma_len;
    float    ring[POCSAG_MA_MAX];
    int      ring_idx;
    float    ma_sum;
    float    peak_decay, peak;
    bool     state;
    float    phase;
    int      integ;
    uint32_t shift;
    bool     synced, inverted;
    int      bits_in_cw, cw_index;  /* cw_index 16 : synchro attendue */
    int      bad_in_batch;
    /* message en cours */
    bool     building;
    uint32_t address;
    int      function;
    uint32_t payload[POCSAG_PAYLOAD_MAX];
    int      npayload, corrected, bad;
} pocsag_demod_t;

typedef struct {
    pocsag_demod_t d[3];
    int            n;               /* debits actifs */
    float          dc_x, dc_y;      /* passe-haut d'entree */
    int            force;           /* 0 auto, 1 texte, 2 numerique */
    bool           show_partial;    /* sortir aussi les messages douteux */
    pocsag_cb_t    cb;
    void*          user;
    /* statistiques */
    uint32_t       n_sync;          /* synchros acquises */
    uint32_t       n_msgs;          /* messages livres (bips compris) */
    uint32_t       n_bad_words;     /* mots incorrigibles rencontres */
    uint32_t       n_hidden;        /* messages douteux caches */
    /* tampons de rendu (evite 1,3 Ko de pile dans la tache) */
    pocsag_msg_t   msg;
    char           num[POCSAG_PAYLOAD_MAX * 5 + 1];
    char           alpha[POCSAG_PAYLOAD_MAX * 3 + 1];
} pocsag_t;

/* rate_hz : cadence reelle des echantillons (>= 8000). baud_mask : b0 512, b1 1200, b2 2400 (0 = les trois). */
void pocsag_init(pocsag_t* p, float rate_hz, uint8_t baud_mask, pocsag_cb_t cb, void* user);
void pocsag_set_force(pocsag_t* p, int force);            /* 0 auto, 1 texte, 2 numerique */
void pocsag_set_show_partial(pocsag_t* p, bool on);
void pocsag_sample(pocsag_t* p, float x);
void pocsag_flush(pocsag_t* p);                           /* fin de reception : livre les messages en cours */
bool pocsag_synced(const pocsag_t* p);                    /* un des debits est calé sur une rafale */

#ifdef __cplusplus
}
#endif
