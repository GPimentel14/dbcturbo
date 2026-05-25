/* blast.c
 * Copyright (C) 2003, 2012 Mark Adler
 * For conditions of distribution and use, see copyright notice in blast.h
 * version 1.3, modernized
 *
 * blast.c decompresses data compressed by the PKWare Compression Library.
 * This function provides functionality similar to the explode() function of
 * the PKWare library, hence the name "blast".
 *
 * This decompressor is based on the excellent format description provided by
 * Ben Rudiak-Gould in comp.compression on August 13, 2001.
 *
 * Modifications for dbcturbo:
 * - Modernized integer types using stdint.h
 * - Removed setjmp/longjmp in favor of explicit error codes
 * - All state is stack-allocated for full thread safety
 */

#include <stdint.h>
#include "blast.h"

/* input and output state */
struct state {
    /* input state */
    blast_in infun;             /* input function provided by user */
    void *inhow;                /* opaque information passed to infun() */
    uint8_t *in;                /* next input location */
    uint32_t left;              /* available input at in */
    int bitbuf;                 /* bit buffer */
    int bitcnt;                 /* number of bits in bit buffer */
    int error;                  /* input error flag (1 for EOF) */

    /* output state */
    blast_out outfun;           /* output function provided by user */
    void *outhow;               /* opaque information passed to outfun() */
    uint32_t next;              /* index of next write location in out[] */
    int first;                  /* true to check distances (for first 4K) */
    uint8_t out[MAXWIN];        /* output buffer and sliding window */
};

/*
 * Return need bits from the input stream.  This always leaves less than
 * eight bits in the buffer.  bits() works properly for need == 0.
 *
 * Format notes:
 * - Bits are stored from the least significant bit to the most significant.
 *   Bits are dropped from the bottom of the bit buffer using shift right,
 *   and new bytes are appended to the top using shift left.
 */
static int bits(struct state *s, int need)
{
    int val;

    val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->left == 0) {
            s->left = s->infun(s->inhow, &(s->in));
            if (s->left == 0) {
                s->error = 1; /* out of input */
                return 0;
            }
        }
        val |= (int)(*(s->in)++) << s->bitcnt;
        s->left--;
        s->bitcnt += 8;
    }

    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return val & ((1 << need) - 1);
}

/*
 * Huffman code decoding tables.
 * count[1..MAXBITS] is the number of symbols of each length.
 * symbol[] are the symbol values in canonical order.
 */
struct huffman {
    int16_t *count;       /* number of symbols of each length */
    int16_t *symbol;      /* canonically ordered symbols */
};

/*
 * Decode a code from the stream s using huffman table h.
 * Return the symbol or a negative value if there is an error.
 */
static int decode(struct state *s, struct huffman *h)
{
    int len;
    int code;
    int first;
    int count;
    int index;
    int bitbuf;
    int left;
    int16_t *next;

    bitbuf = s->bitbuf;
    left = s->bitcnt;
    code = first = index = 0;
    len = 1;
    next = h->count + 1;
    while (1) {
        while (left--) {
            code |= (bitbuf & 1) ^ 1;   /* invert code */
            bitbuf >>= 1;
            count = *next++;
            if (code < first + count) {
                s->bitbuf = bitbuf;
                s->bitcnt = (s->bitcnt - len) & 7;
                return h->symbol[index + (code - first)];
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
            len++;
        }
        left = (MAXBITS + 1) - len;
        if (left == 0) break;
        if (s->left == 0) {
            s->left = s->infun(s->inhow, &(s->in));
            if (s->left == 0) {
                s->error = 1;
                return -9;
            }
        }
        bitbuf = *(s->in)++;
        s->left--;
        if (left > 8) left = 8;
    }
    return -9; /* ran out of codes */
}

/*
 * Build the Huffman decoding tables from the compact representation.
 * Returns 0 for complete code, positive for incomplete, negative for error.
 */
static int construct(struct huffman *h, const uint8_t *rep, int n)
{
    int symbol;
    int len;
    int left;
    int16_t offs[MAXBITS + 1];
    int16_t length[256];

    /* convert compact repeat counts into symbol bit length list */
    symbol = 0;
    do {
        len = *rep++;
        left = (len >> 4) + 1;
        len &= 15;
        do {
            length[symbol++] = len;
        } while (--left);
    } while (--n);
    n = symbol;

    /* count number of codes of each length */
    for (len = 0; len <= MAXBITS; len++)
        h->count[len] = 0;
    for (symbol = 0; symbol < n; symbol++)
        (h->count[length[symbol]])++;
    if (h->count[0] == n)
        return 0;

    /* check for over-subscribed or incomplete set of lengths */
    left = 1;
    for (len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;
    }

    /* generate offsets into symbol table for each length */
    offs[1] = 0;
    for (len = 1; len < MAXBITS; len++)
        offs[len + 1] = offs[len] + h->count[len];

    /* put symbols in table sorted by length */
    for (symbol = 0; symbol < n; symbol++)
        if (length[symbol] != 0)
            h->symbol[offs[length[symbol]]++] = symbol;

    return left;
}

/*
 * Decode PKWare Compression Library stream.
 *
 * Format notes:
 * - First byte is 0 if literals are uncoded or 1 if they are coded.
 *   Second byte is 4, 5, or 6 for the number of extra bits in the distance
 *   code (log2 of dictionary size minus six).
 * - Compressed data is literals and length/distance pairs terminated by
 *   an end code (length == 519).
 */
static int decomp(struct state *s)
{
    int lit;
    int dict;
    int symbol;
    int len;
    uint32_t dist;
    int copy;
    uint8_t *from, *to;

    /* Huffman tables — stack-allocated for thread safety */
    int16_t litcnt[MAXBITS + 1], litsym[256];
    int16_t lencnt[MAXBITS + 1], lensym[16];
    int16_t distcnt[MAXBITS + 1], distsym[64];
    struct huffman litcode  = {litcnt,  litsym};
    struct huffman lencode  = {lencnt,  lensym};
    struct huffman distcode = {distcnt, distsym};

    /* PKWare canonical Huffman tables (compact representation) */
    static const uint8_t litlen[] = {
        11, 124, 8, 7, 28, 7, 188, 13, 76, 4, 10, 8, 12, 10, 12, 10, 8, 23, 8,
        9, 7, 6, 7, 8, 7, 6, 55, 8, 23, 24, 12, 11, 7, 9, 11, 12, 6, 7, 22, 5,
        7, 24, 6, 11, 9, 6, 7, 22, 7, 11, 38, 7, 9, 8, 25, 11, 8, 11, 9, 12,
        8, 12, 5, 38, 5, 38, 5, 11, 7, 5, 6, 21, 6, 10, 53, 8, 7, 24, 10, 27,
        44, 253, 253, 253, 252, 252, 252, 13, 12, 45, 12, 45, 12, 61, 12, 45,
        44, 173};
    static const uint8_t lenlen[]  = {2, 35, 36, 53, 38, 23};
    static const uint8_t distlen[] = {2, 20, 53, 230, 247, 151, 248};
    static const int16_t base[16]  = {3, 2, 4, 5, 6, 7, 8, 9, 10, 12, 16, 24, 40, 72, 136, 264};
    static const char    extra[16] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8};

    /* Build decoding tables every call (stateless = thread-safe) */
    construct(&litcode,  litlen,  sizeof(litlen));
    construct(&lencode,  lenlen,  sizeof(lenlen));
    construct(&distcode, distlen, sizeof(distlen));

    /* Read two-byte header */
    lit = bits(s, 8);
    if (s->error) return 2;
    if (lit > 1) return -1;
    dict = bits(s, 8);
    if (s->error) return 2;
    if (dict < 4 || dict > 6) return -2;

    /* Main decode loop */
    do {
        if (bits(s, 1)) {
            if (s->error) return 2;
            /* decode length */
            symbol = decode(s, &lencode);
            if (s->error) return 2;
            if (symbol < 0) return symbol;
            len = base[symbol] + bits(s, extra[symbol]);
            if (s->error) return 2;
            if (len == 519) break; /* end-of-stream sentinel */

            /* decode distance */
            symbol = (len == 2) ? 2 : dict;
            int dist_sym = decode(s, &distcode);
            if (s->error) return 2;
            if (dist_sym < 0) return dist_sym;
            dist  = (uint32_t)dist_sym << symbol;
            dist += bits(s, symbol);
            if (s->error) return 2;
            dist++;

            if (dist > MAXWIN || (s->first && dist > s->next))
                return -3;

            /* copy back-reference bytes */
            do {
                to = s->out + s->next;
                if (s->next < dist) {
                    from = to + (MAXWIN - dist);
                    copy = dist;
                } else {
                    from = to - dist;
                    copy = MAXWIN;
                }
                copy -= s->next;
                if (copy > len) copy = len;
                len -= copy;
                s->next += copy;
                do {
                    *to++ = *from++;
                } while (--copy);
                if (s->next == MAXWIN) {
                    if (s->outfun(s->outhow, s->out, s->next)) return 1;
                    s->next = 0;
                    s->first = 0;
                }
            } while (len != 0);
        } else {
            if (s->error) return 2;
            /* literal byte */
            symbol = lit ? decode(s, &litcode) : bits(s, 8);
            if (s->error) return 2;
            s->out[s->next++] = (uint8_t)symbol;
            if (s->next == MAXWIN) {
                if (s->outfun(s->outhow, s->out, s->next)) return 1;
                s->next = 0;
                s->first = 0;
            }
        }
    } while (1);
    return 0;
}

/* Public entry point — see blast.h */
int blast(blast_in infun, void *inhow, blast_out outfun, void *outhow)
{
    struct state s;
    int err;

    s.infun  = infun;
    s.inhow  = inhow;
    s.left   = 0;
    s.bitbuf = 0;
    s.bitcnt = 0;
    s.error  = 0;

    s.outfun = outfun;
    s.outhow = outhow;
    s.next   = 0;
    s.first  = 1;

    err = decomp(&s);

    /* flush any remaining output */
    if (err != 1 && s.next && s.outfun(s.outhow, s.out, s.next) && err == 0)
        err = 1;
    return err;
}
