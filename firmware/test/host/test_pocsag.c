/* test_pocsag.c -- banc PC du décodeur POCSAG (lib/decoders/pocsag.c) : lit du
 * PCM 16 bits signé mono sur stdin (cadence -r, 32000 par défaut) et imprime
 * un message par ligne, comme le md_pocsag de Portsdown 4 :
 *
 *   gcc -O2 -I../../lib/decoders -o /tmp/test_pocsag test_pocsag.c ../../lib/decoders/pocsag.c -lm
 *   ../../../tools/gen_pocsag.py /tmp/p.pcm --baud 1200 --noise 0.3
 *   /tmp/test_pocsag -r 32000 < /tmp/p.pcm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pocsag.h"

static int n_msgs;

static void on_msg(const pocsag_msg_t* m, void* user)
{
    (void)user;
    n_msgs++;
    printf("POCSAG%u RIC %07lu F%u %s%s bch+%u bad=%u%s\n", m->baud, (unsigned long)m->address, m->function,
           m->tone_only ? "(bip seul)" : m->numeric ? "num: " : "txt: ", m->tone_only ? "" : m->text,
           m->corrected, m->bad, m->partial ? " [douteux]" : "");
}

int main(int argc, char** argv)
{
    int rate = 32000, i;
    bool partial = false;
    pocsag_t* p = malloc(sizeof *p);
    int16_t buf[4096];
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-r") && i + 1 < argc) rate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-p")) partial = true;
    }
    pocsag_init(p, (float)rate, 7, on_msg, NULL);
    pocsag_set_show_partial(p, partial);
    for (;;) {
        size_t r = fread(buf, 2, sizeof buf / 2, stdin);
        if (!r) break;
        for (i = 0; i < (int)r; i++) pocsag_sample(p, (float)buf[i]);
    }
    pocsag_flush(p);
    fprintf(stderr, "%d message(s), %lu synchro(s), %lu mot(s) incorrigible(s), %lu douteux cache(s), sizeof(pocsag_t)=%zu\n",
            n_msgs, (unsigned long)p->n_sync, (unsigned long)p->n_bad_words, (unsigned long)p->n_hidden, sizeof *p);
    free(p);
    return n_msgs ? 0 : 1;
}
