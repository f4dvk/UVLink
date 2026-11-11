/* pocsag.c -- decodeur POCSAG 512/1200/2400 bauds, voir pocsag.h.
 * Port en bibliotheque du md_pocsag.c de Portsdown 4 (f4dvk), issu du
 * Pocsag.java de HTMOD (Radtel Tools). Meme chaine, memes seuils ; seules
 * les sorties (JSON -> structure + rappel) et les tampons (statiques dans
 * pocsag_t) changent. Les constantes sont celles de la norme CCIR 584.
 */
#include "pocsag.h"
#include <math.h>
#include <string.h>

#define SYNC 0x7CD215D8u
#define IDLE 0x7A89C197u
#define GEN  0x769u                       /* x^10+x^9+x^8+x^6+x^5+x^3+1 */
#define NUMERIC_CHARS "0123456789*U -)("

static int popcount32(uint32_t v)
{
#if defined(__GNUC__)
    return __builtin_popcount(v);
#else
    int n = 0;
    while (v) { v &= v - 1; n++; }
    return n;
#endif
}

/* ---------------------------------------------------------------- BCH(31,21) */

static int16_t syndromes[1024];           /* syndrome -> masque d'erreur (1 ou 2 bits, sur 31 bits : stocke en index), -1 inconnu */
static uint32_t syn_mask[1024];
static bool bch_ready;

static uint32_t bch_rem(uint32_t v)
{
    uint32_t r = v;
    int i;
    for (i = 30; i >= 10; i--) {
        if ((r >> i) & 1) r ^= GEN << (i - 10);
    }
    return r;
}

static void bch_init(void)
{
    int i, j;
    if (bch_ready) return;
    for (i = 0; i < 1024; i++) { syndromes[i] = -1; syn_mask[i] = 0; }
    for (i = 0; i < 31; i++) {
        uint32_t s1 = bch_rem(1u << i);
        syndromes[s1] = 1; syn_mask[s1] = 1u << i;
        for (j = i + 1; j < 31; j++) {
            uint32_t m = (1u << i) | (1u << j), s = bch_rem(m);
            if (syndromes[s] == -1) { syndromes[s] = 2; syn_mask[s] = m; }
        }
    }
    bch_ready = true;
}

/* Mot corrige dans *out, nombre de bits corriges, ou -1 si plus de 2 erreurs */
static int bch_correct(uint32_t cw, uint32_t* out)
{
    uint32_t c31 = cw >> 1, s = bch_rem(c31), fixed, parity, mask;
    int errors;
    if (s == 0) mask = 0;
    else if (syndromes[s] == -1) return -1;
    else mask = syn_mask[s];
    fixed = c31 ^ mask;
    parity = (uint32_t)(popcount32(fixed) & 1);
    errors = popcount32(mask) + ((parity != (cw & 1)) ? 1 : 0);
    if (errors > 2) return -1;
    *out = (fixed << 1) | parity;
    return errors;
}

/* ---------------------------------------------------------------- messages */

static uint32_t reverse_bits(uint32_t v, int n)
{
    uint32_t r = 0;
    int i;
    for (i = 0; i < n; i++) {
        if ((v >> i) & 1) r |= 1u << (n - 1 - i);
    }
    return r;
}

static void rtrim(char* s)
{
    int n = (int)strlen(s);
    while ((n > 0) && ((s[n - 1] == ' ') || (s[n - 1] == '\n') || (s[n - 1] == '\t') || (s[n - 1] == '\r'))) s[--n] = 0;
}

static void decode_numeric(const uint32_t* payload, int n, char* out, int size)
{
    int i, k, p = 0;
    for (i = 0; i < n; i++) {
        for (k = 4; k >= 0; k--) {
            if (p < size - 1) out[p++] = NUMERIC_CHARS[reverse_bits((payload[i] >> (k * 4)) & 0xF, 4)];
        }
    }
    out[p] = 0;
    rtrim(out);
    p = 0;                                /* espaces de tete, comme Java trim() */
    while (out[p] == ' ') p++;
    if (p) memmove(out, out + p, strlen(out + p) + 1);
}

/* Texte 7 bits (poids faible en tete) ; caracteres de commande inattendus
 * remplaces par 0x7f (comptes comme douteux, rendus '?') */
static void decode_alpha(const uint32_t* payload, int n, char* out, int size)
{
    uint32_t acc = 0;
    int nbits = 0, i, b, p = 0;
    for (i = 0; i < n; i++) {
        for (b = 19; b >= 0; b--) {
            acc = (acc << 1) | ((payload[i] >> b) & 1);
            if (++nbits == 7) {
                uint32_t c = reverse_bits(acc, 7);
                acc = 0;
                nbits = 0;
                if (p >= size - 1) continue;
                if (c == 10) out[p++] = '\n';
                else if ((c >= 32) && (c <= 126)) out[p++] = (char)c;
                else if ((c != 0) && (c != 3) && (c != 4) && (c != 0x17) && (c != 13)) out[p++] = 0x7f;
            }
        }
    }
    out[p] = 0;
    rtrim(out);
}

/* Choix numerique / texte au contenu (comme HTMOD et le mode auto de multimon-ng) */
static bool guess_numeric(int function, const char* num, const char* alpha)
{
    int odd = 0, bad = 0, ln = (int)strlen(num), la = (int)strlen(alpha);
    const char* p;
    float num_odd, alpha_odd;
    if (!la) return true;
    for (p = num; *p; p++) if (!strchr("0123456789 -", *p)) odd++;
    for (p = alpha; *p; p++) if (*p == 0x7f) bad++;
    num_odd = odd / (float)(ln > 0 ? ln : 1);
    alpha_odd = bad / (float)la;
    if (num_odd < alpha_odd) return true;
    if (num_odd > alpha_odd) return false;
    return (function == 0) && (ln <= 16);
}

static void emit(pocsag_t* p, const pocsag_demod_t* d)
{
    pocsag_msg_t* m = &p->msg;
    int n = d->npayload;
    bool numeric = false, partial = false;
    const char* text = "";
    if (n > 0) {
        decode_numeric(d->payload, n, p->num, (int)sizeof p->num);
        decode_alpha(d->payload, n, p->alpha, (int)sizeof p->alpha);
        numeric = (p->force == 2) || ((p->force == 0) && guess_numeric(d->function, p->num, p->alpha));
        if ((d->bad > 0) && !numeric) {   /* residu de fin de rafale : texte illisible */
            int q = 0;
            const char* s;
            for (s = p->alpha; *s; s++) if (*s == 0x7f) q++;
            if (q * 4 >= (int)strlen(p->alpha)) partial = true;
        }
        text = numeric ? p->num : p->alpha;
    }
    if (partial && !p->show_partial) { p->n_hidden++; return; }
    m->baud = (uint16_t)d->baud;
    m->address = d->address;
    m->function = (uint8_t)d->function;
    m->numeric = numeric;
    m->tone_only = n == 0;
    m->partial = partial;
    m->corrected = (uint8_t)(d->corrected > 255 ? 255 : d->corrected);
    m->bad = (uint8_t)(d->bad > 255 ? 255 : d->bad);
    {
        size_t i, k = 0;
        for (i = 0; text[i] && k < POCSAG_TEXT_MAX - 1; i++) m->text[k++] = (text[i] == 0x7f) ? '?' : text[i];
        m->text[k] = 0;
    }
    p->n_msgs++;
    if (p->cb) p->cb(m, p->user);
}

/* ---------------------------------------------------------------- demodulateur (un par debit) */

static void demod_init(pocsag_demod_t* d, float rate, int baud)
{
    float spb = rate / (float)baud;
    memset(d, 0, sizeof *d);
    d->baud = baud;
    d->step = 1.0f / spb;
    d->ma_len = (int)(spb / 4 + 0.5f);
    if (d->ma_len < 1) d->ma_len = 1;
    if (d->ma_len > POCSAG_MA_MAX) d->ma_len = POCSAG_MA_MAX;
    d->peak_decay = expf(-1.0f / (spb * 32));
}

static void finish_message(pocsag_t* p, pocsag_demod_t* d)
{
    if (!d->building) return;
    d->building = false;
    emit(p, d);
}

static void lose_sync(pocsag_t* p, pocsag_demod_t* d)
{
    finish_message(p, d);
    d->synced = false;
    d->cw_index = 0;
    d->bits_in_cw = 0;
}

static void start_sync(pocsag_t* p, pocsag_demod_t* d, bool inv)
{
    d->synced = true;
    d->inverted = inv;
    d->bits_in_cw = 0;
    d->cw_index = 0;
    d->bad_in_batch = 0;
    p->n_sync++;
}

static void add_payload(pocsag_demod_t* d, uint32_t w)
{
    if (d->npayload < POCSAG_PAYLOAD_MAX) d->payload[d->npayload++] = w;
}

static void codeword(pocsag_t* p, pocsag_demod_t* d, uint32_t raw, int index)
{
    uint32_t cw;
    int n = bch_correct(raw, &cw);
    if (n < 0) {
        p->n_bad_words++;
        if (++d->bad_in_batch >= 6) {     /* trop d'erreurs : bruit */
            lose_sync(p, d);
            return;
        }
        if (d->building) {
            d->bad++;
            add_payload(d, (raw >> 11) & 0xFFFFF);
        }
        return;
    }
    if (cw == IDLE) {
        finish_message(p, d);
        return;
    }
    if (!(cw & 0x80000000u)) {            /* mot d'adresse */
        finish_message(p, d);
        d->building = true;
        d->address = (((cw >> 13) & 0x3FFFF) << 3) | (uint32_t)(index / 2);
        d->function = (int)((cw >> 11) & 3);
        d->npayload = 0;
        d->corrected = n;
        d->bad = 0;
    } else if (d->building) {
        d->corrected += n;
        add_payload(d, (cw >> 11) & 0xFFFFF);
    }
}

static void bit(pocsag_t* p, pocsag_demod_t* d, bool b)
{
    uint32_t cw;
    d->shift = (d->shift << 1) | (b ? 1u : 0u);
    if (!d->synced) {
        if (popcount32(d->shift ^ SYNC) <= 1) start_sync(p, d, false);
        else if (popcount32(d->shift ^ ~SYNC) <= 1) start_sync(p, d, true);
        return;
    }
    if (++d->bits_in_cw < 32) return;
    d->bits_in_cw = 0;
    cw = d->inverted ? ~d->shift : d->shift;
    if (d->cw_index == 16) {              /* entre deux lots : synchro, sinon fin de transmission */
        if (popcount32(cw ^ SYNC) <= 2) {
            d->cw_index = 0;
            d->bad_in_batch = 0;
        } else lose_sync(p, d);
        return;
    }
    codeword(p, d, cw, d->cw_index);
    d->cw_index++;
}

static void sample(pocsag_t* p, pocsag_demod_t* d, float x)
{
    float v, a, thr;
    bool ns;
    d->ma_sum += x - d->ring[d->ring_idx];
    d->ring[d->ring_idx] = x;
    if (++d->ring_idx >= d->ma_len) d->ring_idx = 0;
    v = d->ma_sum / (float)d->ma_len;
    a = fabsf(v);
    d->peak = (a > d->peak) ? a : d->peak * d->peak_decay;
    thr = d->peak * 0.35f;
    ns = d->state ? (v > -thr) : (v > thr);
    if (ns != d->state) {
        float k = d->synced ? 0.06f : 0.2f;   /* transition attendue en limite de bit */
        d->state = ns;
        d->phase += (d->phase < 0.5f) ? -d->phase * k : (1.0f - d->phase) * k;
    }
    d->integ += d->state ? 1 : -1;
    d->phase += d->step;
    if (d->phase >= 1.0f) {
        d->phase -= 1.0f;
        bit(p, d, d->integ > 0);
        d->integ = 0;
    }
}

/* ---------------------------------------------------------------- API */

void pocsag_init(pocsag_t* p, float rate_hz, uint8_t baud_mask, pocsag_cb_t cb, void* user)
{
    static const int bauds[3] = { 512, 1200, 2400 };
    int i;
    bch_init();
    memset(p, 0, sizeof *p);
    if (rate_hz < 8000.0f) rate_hz = 8000.0f;
    if (!(baud_mask & 7)) baud_mask = 7;
    for (i = 0; i < 3; i++) {
        if (baud_mask & (1u << i)) demod_init(&p->d[p->n++], rate_hz, bauds[i]);
    }
    p->cb = cb;
    p->user = user;
}

void pocsag_set_force(pocsag_t* p, int force) { p->force = force < 0 ? 0 : force > 2 ? 0 : force; }
void pocsag_set_show_partial(pocsag_t* p, bool on) { p->show_partial = on; }

void pocsag_sample(pocsag_t* p, float x)
{
    float y = x - p->dc_x + 0.999f * p->dc_y;     /* passe-haut ~5 Hz a 32 kHz */
    int k;
    p->dc_x = x;
    p->dc_y = y;
    for (k = 0; k < p->n; k++) sample(p, &p->d[k], y);
}

void pocsag_flush(pocsag_t* p)
{
    int k;
    for (k = 0; k < p->n; k++) lose_sync(p, &p->d[k]);
}

bool pocsag_synced(const pocsag_t* p)
{
    int k;
    for (k = 0; k < p->n; k++) if (p->d[k].synced) return true;
    return false;
}
