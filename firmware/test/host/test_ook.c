/* test_ook.c -- banc PC du détecteur d'impulsions OOK (lib/decoders/ookpulse.c) :
 * lit du PCM 16 bits mono sur stdin (cadence -r, 32000 par défaut) et écrit sur
 * stdout un fichier d'impulsions « .ook » pour rtl_433 :
 *   gcc -O2 -I../../lib/decoders -o /tmp/test_ook test_ook.c ../../lib/decoders/ookpulse.c -lm
 *   ../../../tools/gen_ook.py nexus.ook /tmp/n.pcm --noise-rms 540
 *   /tmp/test_ook < /tmp/n.pcm > /tmp/out.ook && rtl_433 -r /tmp/out.ook -F json
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ookpulse.h"

static int n_pkt;
static void on_pkt(const ook_packet_t* p, void* user)
{
    (void)user; n_pkt++;
    printf(";ook\n");
    for (int i = 0; i < p->n; i++) printf("%u %u\n", p->pulse[i], p->gap[i]);
}
int main(int argc, char** argv)
{
    int rate = 32000, i;
    float on = -1, off = -1; int mon = -1, moff = -1, mpulse = -1, mgap = -1;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-r") && i + 1 < argc) rate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-on") && i + 1 < argc) on = (float)atof(argv[++i]);       /* seuil d'entrée (fraction du souffle) */
        else if (!strcmp(argv[i], "-off") && i + 1 < argc) off = (float)atof(argv[++i]);     /* seuil de sortie */
        else if (!strcmp(argv[i], "-mon") && i + 1 < argc) mon = atoi(argv[++i]);            /* échantillons calmes requis */
        else if (!strcmp(argv[i], "-moff") && i + 1 < argc) moff = atoi(argv[++i]);          /* échantillons forts requis */
        else if (!strcmp(argv[i], "-mpulse") && i + 1 < argc) mpulse = atoi(argv[++i]);      /* impulsion minimale (échantillons) */
        else if (!strcmp(argv[i], "-mgap") && i + 1 < argc) mgap = atoi(argv[++i]);          /* trou minimal fusionné (échantillons, 0 = pas de fusion) */
    }
    ook_t* o = malloc(sizeof *o);
    ook_init(o, (float)rate, on_pkt, NULL);
    if (on >= 0) o->on_frac = on;
    if (off >= 0) o->off_frac = off;
    if (mon >= 0) o->min_on = mon;
    if (moff >= 0) o->min_off = moff;
    if (mpulse >= 0) o->min_pulse = mpulse;
    if (mgap >= 0) o->min_gap = mgap;
    printf(";pulse data\n;version 1\n;timescale 1us\n;samplerate %d\n;freq1 433920000\n", rate);
    int16_t buf[4096];
    for (;;) { size_t r = fread(buf, 2, 4096, stdin); if (!r) break; for (i = 0; i < (int)r; i++) ook_sample(o, (float)buf[i]); }
    ook_flush(o);
    printf(";end\n");
    fprintf(stderr, "%d paquet(s), %lu impulsions, %lu paquets trop courts, souffle %.0f, sizeof(ook_t)=%zu\n",
            n_pkt, (unsigned long)o->n_pulses, (unsigned long)o->n_short, (double)o->noise, sizeof *o);
    free(o);
    return n_pkt ? 0 : 1;
}
