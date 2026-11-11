/* sstv_demod.h -- SSTV (Scottie 1/2/DX, Martin 1/2, PD 50..290) receiver: instantaneous-
 * frequency tracking + VIS header detection + per-line RGB decode. Pure C,
 * no pico-sdk, host-testable (see test/host/test_sstv_demod.c).
 *
 * branch SSTV_SSDV: this replaces the abandoned IMGFSK/SSDV image link (see
 * decoder_config.h and git history). Unlike IMGFSK's binary two-level FSK
 * (a bit-cell PLL, sonde_demod.h's job), SSTV encodes each pixel as a
 * continuously-varying AUDIO TONE frequency (1500 Hz black .. 2300 Hz
 * white) -- the right tool here is an instantaneous-frequency estimator,
 * not a bit slicer. Method: DC-centre the signal (same adaptive hi/lo
 * envelope trick as sonde_demod.c/aprs_rx.c), find upward zero-crossings,
 * and linearly interpolate the fractional-sample crossing time between the
 * two straddling samples -- this gives sub-sample-period timing resolution
 * from plain integer/fixed-point math, no NCO/quadrature mixer or floats
 * needed in the per-sample path. One frequency estimate comes out roughly
 * once per audio cycle (~435-667 us in this frequency range), which is
 * comparable to Scottie 1's own 432 us/pixel dwell time -- adequate but not
 * generous margin, expect this to be the first thing worth revisiting from
 * real-air feedback (this project's established pattern: see sonde_demod.h,
 * imgfsk_sync.h's own "unverified on real air" warnings before their first
 * field tests).
 *
 * ⚠️ The period between two crossings is NOT simply the sample count
 * between them: each crossing's own fractional-sample offset (frac_prev_q10
 * below) must be carried over and subtracted, or the estimate has a
 * systematic multi-percent error (~75 Hz at 48 kHz/1900 Hz measured while
 * developing this against a synthetic host-test tone -- large enough to
 * make every VIS/luminance threshold in this file unusable). Fixed and
 * verified against a clean synthetic sine before ever touching real
 * hardware, the same discipline this project's other decoders follow.
 *
 * Timing (VIS header + Scottie 1 scan-line structure) is tracked mostly by
 * elapsed SAMPLE COUNT from two anchor points: the break->leader2 edge (for
 * the VIS header) and the VIS-decode-complete instant (for the image scan).
 * No active per-line re-synchronisation yet (a real decoder would re-lock
 * on each line's sync pulse to absorb clock drift over Scottie 1's ~110 s
 * frame) -- deferred until real hardware shows whether it is actually
 * needed (image slant would be the symptom). Protocol source: the SSTV
 * Handbook (Martin Bruchanov OK2MNM, sstv-handbook.com, chapter 4).
 *
 * ⚠️ (2026-10-03, retour terrain : image toujours bruitee par endroits --
 * petits eclats sombres disperses, pas alignes avec une forme coherente --
 * meme apres avoir corrige le moyennage par pixel ci-dessous) -- tentative
 * d'aller plus loin avec un vrai discriminateur FM en quadrature (melangeur
 * I/Q + filtre passe-bas + produit croise/atan2f, comme un discriminateur
 * FM logiciel classique type gqrx/SDR#) pour moyenner ~21 mesures par pixel
 * au lieu d'une seule -- ABANDONNEE apres validation synthetique : le
 * filtre passe-bas, necessairement lent pour rejeter l'image 2*f0 du
 * melangeur, met des DIZAINES de pixels a se stabiliser apres tout saut de
 * frequence important (un battement entre l'ancienne et la nouvelle
 * frequence, qui ne s'attenue qu'a l'echelle du temps de reponse du
 * filtre) -- confirme sur signal synthetique propre : ~44 colonnes de
 * corruption apres chaque transition franche (fin d'un canal de couleur,
 * debut du suivant), bien pire que le bruit disperse qu'il visait a
 * corriger, puisque CE projet decode des images avec de vrais contours
 * (texte, traits) a chaque ligne, pas seulement des aplats. Abandonnee
 * AVANT le terrain, exactement le but de valider contre un signal
 * synthetique propre d'abord -- cf plus haut. Reste sur le passage par
 * zero (moins de mesures par pixel, mais aucun regime transitoire de ce
 * genre) avec le moyennage par pixel ci-dessous ; une vraie amelioration
 * demanderait un filtre complexe correctement concu (ex. Hilbert + mixage,
 * pas un simple passe-bas reel en cascade), un chantier a part entiere. */
#ifndef SSTV_DEMOD_H
#define SSTV_DEMOD_H

#include <stdint.h>
#include <stdbool.h>

#define SSTV_SCOTTIE1_WIDTH  320
#define SSTV_SCOTTIE1_HEIGHT 256
#define SSTV_MAX_WIDTH       800     /* PD 290 */

/* Multimode (2026-10-31, UVLink fw 0.12) : le mode est choisi par le code VIS
 * dans une table (sstv_demod.c, SSTV_MODES). Trois familles de ligne :
 *   Scottie : sep, Vert, sep, Bleu, SYNCHRO, porche, Rouge (synchro au milieu)
 *   Martin  : SYNCHRO, porche, Vert, sep, Bleu, sep, Rouge, sep
 *   PD      : SYNCHRO, porche, Y pair, R-Y, B-Y, Y impair (deux lignes par balayage)
 * Identifiants de mode = ceux du protocole BLE (SSTV_INFO, nouvelle image). */
enum {
    SSTV_MODE_NONE = 0,
    SSTV_MODE_SCOTTIE1 = 1, SSTV_MODE_SCOTTIE2, SSTV_MODE_SCOTTIEDX,
    SSTV_MODE_MARTIN1, SSTV_MODE_MARTIN2,
    SSTV_MODE_PD50, SSTV_MODE_PD90, SSTV_MODE_PD120, SSTV_MODE_PD160, SSTV_MODE_PD180, SSTV_MODE_PD240, SSTV_MODE_PD290,
};
enum { SSTV_FAM_SCOTTIE = 0, SSTV_FAM_MARTIN, SSTV_FAM_PD };

typedef struct sstv_mode {
    uint8_t  id;        /* SSTV_MODE_* */
    uint8_t  vis;       /* code VIS (7 bits) */
    uint8_t  family;    /* SSTV_FAM_* */
    uint16_t width, height;
    uint32_t sync_us;   /* impulsion de synchro 1200 Hz */
    uint32_t porch_us;  /* porche 1500 Hz apres la synchro */
    uint32_t sep_us;    /* separateur 1500 Hz entre composantes (0 = aucun) */
    uint32_t chan_us;   /* duree d'une composante (toute la largeur) */
    uint32_t line_us;   /* duree d'un balayage complet (2 lignes en PD) */
    const char *name;
} sstv_mode_t;

const sstv_mode_t *sstv_mode_by_id(uint8_t id);

typedef struct {
    /* DC-centring (adaptive hi/lo envelope, same shape as sonde_demod_t) */
    int32_t lp, lp_hi, lp_lo;
    int32_t lp2;        /* deuxieme etage passe-bas (repli si pas de passe-bande) */
    const struct sstv_bpcoef *cf;   /* passe-bande 900..2400 Hz, NULL = repli */
    int64_t  bqs[2][4];             /* etats biquad : x1, x2, y1, y2 (HP puis LP) */
    bool    zc_armed;
    /* Detecteur de synchro spectral (Goertzel 1200 Hz, par blocs de 5 ms) :
     * independant de l'amplitude, comme les autres decodeurs. */
    bool     gz_on;       /* actif seulement a 48 kHz / 32 kHz (coefficients fixes) */
    int32_t  gz_c;        /* 2cos(2*pi*1200/fs), Q14 */
    uint32_t gz_blk;      /* taille du bloc, echantillons */
    uint32_t gz_n;
    int64_t  gz_s1, gz_s2;
    uint64_t gz_e;
    uint32_t blk_run;     /* blocs de synchro consecutifs (tolerance 1 manquant) */
    uint32_t blk_miss;   /* hysteresis : le signal est passe franchement sous zero */
    int32_t prev_centered;

    /* Zero-crossing frequency estimator */
    uint32_t sample_rate_hz;
    uint32_t samples_since_cross;
    int32_t  frac_prev_q10;    /* fractional-sample position (Q10) of the
                                 * last crossing within its own bracketing
                                 * sample pair -- MUST be carried over and
                                 * subtracted from the next period, or the
                                 * result has a systematic multi-percent
                                 * error (see this session's derivation) */
    uint32_t freq_mhz;         /* latest instantaneous frequency, milli-Hz */
    bool     have_freq;

    /* VIS header + image state machine */
    int      state;
    uint32_t state_samples;    /* elapsed samples since entering d->state */
    uint64_t hz_accum;         /* running sum of freq_mhz over the current
                                 * window, for boxcar-averaged classification
                                 * (VIS bits) */
    uint32_t hz_accum_n;

    uint8_t  vis_bits_val;
    int      vis_bit_idx;      /* 0..6 data bits, 7 parity, 8 stop */
    bool     vis_parity_ok;    /* set when bit_idx 7 (parity) completes,
                                 * read back when bit_idx 8 (stop) completes
                                 * -- MUST live in the struct, not a local:
                                 * these are two separate top-level calls */

    int      line;             /* index of the line currently being scanned */
    int      line_ready_idx;   /* valid only right after a true return: the
                                 * 0-based index of the just-completed line */
    int      lines_ready;      /* lignes pretes apres un retour vrai : 1, ou 2 en PD */
    int      pixel;            /* -1 between pixels, else last pixel index
                                 * written in the channel currently scanning */
    /* Mode courant (choisi par le VIS, conserve jusqu'au VIS suivant) et
     * sequence d'elements de la ligne (EL_* dans sstv_demod.c) */
    const sstv_mode_t *mode;
    const uint8_t *seq;
    uint8_t  seq_len, seq_pos, sync_pos;
    bool     image_new;        /* VIS accepte : nouvelle image (a consommer par l'appelant) */
    uint8_t  raw[4][SSTV_MAX_WIDTH];   /* composantes de la ligne en cours : V,B,R ou Y0,Cr,Cb,Y1 */
    uint8_t  rgb[SSTV_MAX_WIDTH * 3];  /* sortie de sstv_demod_rgb_line() */

    /* ⚠️ (2026-10-03, diagnostic de regression temporaire -- "pas de
     * decodage", lines=0 persistant malgre une vraie emission Scottie 1 et
     * un signal qui bouge (env/lp non nul et variable)) : compteurs
     * cumulatifs a CHAQUE etape du pipeline VIS, pour savoir ou ca cale --
     * meme principe que n_hdlc/n_ok pour l'APRS interne (voir ce projet,
     * historique). A retirer une fois la regression identifiee. */
    uint32_t n_leader;    /* ST_HUNT -> ST_WAIT_BREAK (leader 1900 Hz trouve) */
    uint32_t n_break;     /* ST_WAIT_BREAK -> ST_IN_BREAK (creux 1200 Hz trouve) */
    uint32_t n_leader2;   /* ST_IN_BREAK -> ST_LEADER2 (front montant valide) */
    uint32_t n_vis_done;  /* ST_VIS_WINDOW termine (9 fenetres), bon ou mauvais */
    uint32_t n_vis_ok;    /* VIS accepte : parite bonne ET code Scottie 1 (60) */

    /* Mesure de l'horloge de l'emetteur (2026-10-04) : une synchro de ligne
     * = 1200 Hz tenu au moins 6 ms. L'intervalle entre deux synchros est la
     * duree reelle d'une ligne (base de temps = quartz du RP2040). */
    uint32_t sample_idx;  /* index absolu de l'echantillon courant */
    uint32_t sync_run;    /* compteur tolerant a ~1200 Hz (monte vite, descend lentement) */
    bool     sync_armed;  /* une synchro est en cours (un seul evenement par impulsion) */
    uint32_t sync_last;   /* debut de la derniere synchro */
    bool     sync_have;
    uint32_t n_sync_iv;   /* intervalles retenus (400..460 ms) */
    uint64_t sum_sync_iv; /* somme de ces intervalles, en echantillons */
    uint32_t n_freq;      /* diagnostic : echantillons avec une estimation de frequence */
    uint32_t n_band;      /* diagnostic : echantillons dans la bande 1140..1260 Hz */
    uint32_t n_band_run;  /* diagnostic : plus longue serie consecutive dans la bande */
    uint32_t band_run;    /* diagnostic : serie courante */
    uint32_t fhist[8];    /* diagnostic : histogramme des frequences estimees */

    /* Redressement (2026-10-31, UVLink) : sans resynchronisation, les lignes
     * defilent en roue libre depuis la fin du VIS sur l'horloge de l'ADC ;
     * tout ecart entre l'horloge de l'emetteur et la notre (et l'ecart entre
     * la cadence nominale et la cadence REELLE de l'ADC : 48 077 Hz sur
     * l'ESP32-S3 pour 48 000 demandes, soit 1 600 ppm = 1,6 px par ligne,
     * toute l'image sur 256 lignes) penche l'image. Chaque ligne Scottie 1
     * porte une synchro 1200 Hz de 9 ms entre Bleu et Rouge : on detecte la
     * FIN de cette impulsion (premier cycle nettement plus court apres >= 5 ms
     * a 1200 Hz, precision ~un demi-cycle soit ~1 px) et, si elle tombe a
     * +/- 10 ms de l'instant attendu, on recale la base de temps de la ligne
     * dessus. Reste alors la derive A L'INTERIEUR d'une ligne (0,7 ms pour
     * 1 600 ppm, 0,02 ms pour 50 ppm), invisible. */
    bool     freq_new;        /* une estimation de frequence vient de tomber (ce pas) */
    uint32_t period_cur;      /* duree du dernier cycle mesure, echantillons */
    bool     in1200;          /* serie d'estimations dans la bande 1100..1300 Hz en cours */
    uint32_t run1200_start;   /* debut (sample_idx) de cette serie */
    uint32_t edge_pending;    /* front candidat (sample_idx), en attente d'une estimation >= 1400 Hz */
    uint8_t  edge_pending_n;  /* estimations restantes pour confirmer (0 = aucun candidat) */
    uint32_t favg[4];         /* 4 dernieres estimations (milli-Hz) : moyenne pour l'en-tete VIS */
    uint8_t  favg_i;
    uint32_t freq_avg_mhz;    /* moyenne glissante sur 4 cycles (~3 ms) : decisions leader / break */
    uint32_t sync_end_idx;    /* fin de synchro estimee (sample_idx) */
    bool     sync_end_new;    /* a consommer par la machine d'etats */
    uint32_t sync_enter_idx;  /* sample_idx a l'entree de ST_IMG_SYNC (synchro attendue) */
    uint32_t n_resync;        /* lignes recalees */
    uint32_t n_resync_miss;   /* synchros vues hors fenetre (ignorees) */
    int32_t  last_resync_err; /* dernier ecart constate, echantillons (+ = emetteur en retard) */
} sstv_demod_t;

void sstv_demod_init(sstv_demod_t *d, uint32_t sample_rate_hz);
/* Cadence REELLE de l'ADC (ex. 48 077 Hz pour 48 000 demandes sur l'ESP32-S3,
 * 32 051 pour 32 000) : la base de temps (durees des etats, pixels, mesure
 * de frequence) l'utilise ; les filtres restent ceux de la cadence nominale
 * donnee a sstv_demod_init (l'ecart de 0,2 % est sans effet sur eux). */
void sstv_demod_set_rate(sstv_demod_t *d, uint32_t actual_hz);

/* Ecart d'horloge de l'emetteur, en ppm (+ = emetteur plus lent que le
 * nominal Scottie 1 de 428,22 ms par ligne). Retourne false tant qu'il n'y a
 * pas au moins min_iv intervalles de synchro retenus. */
bool sstv_demod_clock_error_ppm(const sstv_demod_t *d, uint32_t min_iv, int32_t *ppm);

/* Feed one discriminator sample (same |x| <= ~32768 convention as
 * sonde_demod.h/audio_slicer.h). Returns true once a full line scan is
 * ready : d->lines_ready lignes (1, ou 2 en PD) a partir de la ligne
 * d->line_ready_idx (NOT d->line, which by then already points at the next
 * line to be scanned) ; les pixels se lisent par sstv_demod_rgb_line(). */
bool sstv_demod_sample(sstv_demod_t *d, int32_t sample);

/* Ligne prete numero which (0..lines_ready-1), en RGB entrelace (3 octets par
 * pixel, d->mode->width pixels). En PD la conversion YCrCb (BT.601, plage
 * studio comme MMSSTV) est faite ici. Valide jusqu'au prochain appel. */
const uint8_t *sstv_demod_rgb_line(sstv_demod_t *d, int which);

#endif /* SSTV_DEMOD_H */
