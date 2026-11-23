/* Minimal DEFLATE decoder (RFC 1951), after the structure of Mark Adler's "puff".
   inflate_zlib() skips the 2-byte zlib header and ignores the Adler-32 trailer.
   Returns bytes written, or -1 on error. */
#include <setjmp.h>
#include <string.h>

typedef struct {
    unsigned char* out; size_t outlen, outcnt;
    const unsigned char* in; size_t inlen, incnt;
    int bitbuf, bitcnt;
    jmp_buf env;
} inf_state;

#define MAXBITS 15
#define MAXLCODES 286
#define MAXDCODES 30
#define MAXCODES (MAXLCODES + MAXDCODES)
#define FIXLCODES 288

typedef struct { short* count; short* symbol; } huffman;

static int inf_bits(inf_state* s, int need) {
    long val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->incnt == s->inlen) longjmp(s->env, 1);
        val |= (long)(s->in[s->incnt++]) << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = (int)(val >> need);
    s->bitcnt -= need;
    return (int)(val & ((1L << need) - 1));
}
static int inf_stored(inf_state* s) {
    unsigned len;
    s->bitbuf = 0; s->bitcnt = 0;
    if (s->incnt + 4 > s->inlen) return 2;
    len = s->in[s->incnt++]; len |= s->in[s->incnt++] << 8;
    if (s->in[s->incnt++] != (~len & 0xff) || s->in[s->incnt++] != ((~len >> 8) & 0xff)) return -2;
    if (s->incnt + len > s->inlen) return 2;
    if (s->outcnt + len > s->outlen) return 1;
    memcpy(s->out + s->outcnt, s->in + s->incnt, len);
    s->outcnt += len; s->incnt += len;
    return 0;
}
static int inf_decode(inf_state* s, const huffman* h) {
    int code = 0, first = 0, index = 0, len, count;
    for (len = 1; len <= MAXBITS; len++) {
        code |= inf_bits(s, 1);
        count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return -10;
}
static int inf_construct(huffman* h, const short* length, int n) {
    int symbol, len, left; short offs[MAXBITS + 1];
    for (len = 0; len <= MAXBITS; len++) h->count[len] = 0;
    for (symbol = 0; symbol < n; symbol++) h->count[length[symbol]]++;
    if (h->count[0] == n) return 0;
    left = 1;
    for (len = 1; len <= MAXBITS; len++) { left <<= 1; left -= h->count[len]; if (left < 0) return left; }
    offs[1] = 0;
    for (len = 1; len < MAXBITS; len++) offs[len + 1] = offs[len] + h->count[len];
    for (symbol = 0; symbol < n; symbol++) if (length[symbol] != 0) h->symbol[offs[length[symbol]]++] = (short)symbol;
    return left;
}
static int inf_codes(inf_state* s, const huffman* lencode, const huffman* distcode) {
    static const short lens[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
    static const short lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
    static const short dists[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
    static const short dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
    int symbol, len; unsigned dist;
    do {
        symbol = inf_decode(s, lencode);
        if (symbol < 0) return symbol;
        if (symbol < 256) {
            if (s->outcnt == s->outlen) return 1;
            s->out[s->outcnt++] = (unsigned char)symbol;
        } else if (symbol > 256) {
            symbol -= 257;
            if (symbol >= 29) return -10;
            len = lens[symbol] + inf_bits(s, lext[symbol]);
            symbol = inf_decode(s, distcode);
            if (symbol < 0) return symbol;
            dist = dists[symbol] + inf_bits(s, dext[symbol]);
            if (dist > s->outcnt) return -11;
            if (s->outcnt + len > s->outlen) return 1;
            while (len--) { s->out[s->outcnt] = s->out[s->outcnt - dist]; s->outcnt++; }
        }
    } while (symbol != 256);
    return 0;
}
static int inf_fixed(inf_state* s) {
    static int virgin = 1;
    static short lencnt[MAXBITS + 1], lensym[FIXLCODES], distcnt[MAXBITS + 1], distsym[MAXDCODES];
    static huffman lencode, distcode;
    if (virgin) {
        int symbol; short lengths[FIXLCODES];
        lencode.count = lencnt; lencode.symbol = lensym; distcode.count = distcnt; distcode.symbol = distsym;
        for (symbol = 0; symbol < 144; symbol++) lengths[symbol] = 8;
        for (; symbol < 256; symbol++) lengths[symbol] = 9;
        for (; symbol < 280; symbol++) lengths[symbol] = 7;
        for (; symbol < FIXLCODES; symbol++) lengths[symbol] = 8;
        inf_construct(&lencode, lengths, FIXLCODES);
        for (symbol = 0; symbol < MAXDCODES; symbol++) lengths[symbol] = 5;
        inf_construct(&distcode, lengths, MAXDCODES);
        virgin = 0;
    }
    return inf_codes(s, &lencode, &distcode);
}
static int inf_dynamic(inf_state* s) {
    int nlen, ndist, ncode, index, err;
    short lengths[MAXCODES], lencnt[MAXBITS + 1], lensym[MAXLCODES], distcnt[MAXBITS + 1], distsym[MAXDCODES];
    huffman lencode = { lencnt, lensym }, distcode = { distcnt, distsym };
    static const short order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    nlen = inf_bits(s, 5) + 257; ndist = inf_bits(s, 5) + 1; ncode = inf_bits(s, 4) + 4;
    if (nlen > MAXLCODES || ndist > MAXDCODES) return -3;
    for (index = 0; index < ncode; index++) lengths[order[index]] = (short)inf_bits(s, 3);
    for (; index < 19; index++) lengths[order[index]] = 0;
    err = inf_construct(&lencode, lengths, 19);
    if (err != 0) return -4;
    index = 0;
    while (index < nlen + ndist) {
        int symbol, len;
        symbol = inf_decode(s, &lencode);
        if (symbol < 0) return symbol;
        if (symbol < 16) lengths[index++] = (short)symbol;
        else {
            len = 0;
            if (symbol == 16) { if (index == 0) return -5; len = lengths[index - 1]; symbol = 3 + inf_bits(s, 2); }
            else if (symbol == 17) symbol = 3 + inf_bits(s, 3);
            else symbol = 11 + inf_bits(s, 7);
            if (index + symbol > nlen + ndist) return -6;
            while (symbol--) lengths[index++] = (short)len;
        }
    }
    if (lengths[256] == 0) return -9;
    err = inf_construct(&lencode, lengths, nlen);
    if (err && (err < 0 || nlen != lencode.count[0] + lencode.count[1])) return -7;
    err = inf_construct(&distcode, lengths + nlen, ndist);
    if (err && (err < 0 || ndist != distcode.count[0] + distcode.count[1])) return -8;
    return inf_codes(s, &lencode, &distcode);
}
static long long inflate_raw(unsigned char* dest, size_t destlen, const unsigned char* src, size_t srclen) {
    inf_state s; int last, type, err;
    s.out = dest; s.outlen = destlen; s.outcnt = 0;
    s.in = src; s.inlen = srclen; s.incnt = 0;
    s.bitbuf = 0; s.bitcnt = 0;
    if (setjmp(s.env) != 0) err = 2;
    else {
        do {
            last = inf_bits(&s, 1);
            type = inf_bits(&s, 2);
            err = type == 0 ? inf_stored(&s) : (type == 1 ? inf_fixed(&s) : (type == 2 ? inf_dynamic(&s) : -1));
            if (err != 0) break;
        } while (!last);
    }
    return err == 0 ? (long long)s.outcnt : -1;
}
static long long inflate_zlib(unsigned char* dest, size_t destlen, const unsigned char* src, size_t srclen) {
    if (srclen < 2 || (src[0] & 0x0F) != 8 || ((src[0] << 8) | src[1]) % 31) return inflate_raw(dest, destlen, src, srclen); /* not zlib-wrapped: try raw */
    return inflate_raw(dest, destlen, src + 2, srclen - 2);
}
