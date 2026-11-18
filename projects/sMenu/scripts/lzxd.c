/* Minimal LZX decoder (verbatim, aligned, uncompressed blocks; no reset; no delta).
   usage: lzxd WINDOW_BITS OUT_SIZE < in > out */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
static uint8_t *in; static size_t in_len, bitpos;   /* bit position */
static uint32_t peek(int n) {                      /* n <= 17 */
    uint32_t v = 0; size_t p = bitpos;
    for (int i = 0; i < n; i++, p++) {
        size_t w = (p >> 4) * 2; int b = 15 - (int)(p & 15);
        uint16_t word = (w + 1 < in_len) ? (uint16_t)(in[w] | in[w + 1] << 8) : 0;
        v = (v << 1) | ((word >> b) & 1);
    }
    return v;
}
static uint32_t bits(int n) { if (!n) return 0; uint32_t v = n > 16 ? (peek(16) << (n - 16)) : 0;
    if (n > 16) { bitpos += 16; v |= peek(n - 16); bitpos += n - 16; return v; }
    v = peek(n); bitpos += n; return v; }
typedef struct { int n; uint8_t len[700]; uint16_t sym[1 << 16]; } Tree;  /* 16-bit lookup */
static void build(Tree *t) {
    int code = 0; memset(t->sym, 0xff, sizeof(t->sym));
    for (int l = 1; l <= 16; l++) {
        for (int s = 0; s < t->n; s++) if (t->len[s] == l) {
            int lo = code << (16 - l), hi = (code + 1) << (16 - l);
            for (int i = lo; i < hi && i < 65536; i++) t->sym[i] = (uint16_t)s;
            code++;
        }
        code <<= 1;
    }
}
static int dec(Tree *t) { uint32_t p = peek(16); int s = t->sym[p];
    if (s == 0xffff) { fprintf(stderr, "bad code at bit %zu\n", bitpos); exit(2); }
    bitpos += t->len[s]; return s; }
static void readlens(uint8_t *lens, int first, int last) {
    static Tree pre; pre.n = 20;
    for (int i = 0; i < 20; i++) pre.len[i] = (uint8_t)bits(4);
    build(&pre);
    for (int x = first; x < last;) {
        int z = dec(&pre);
        if (z == 17) { int r = 4 + bits(4); while (r-- && x < last) lens[x++] = 0; }
        else if (z == 18) { int r = 20 + bits(5); while (r-- && x < last) lens[x++] = 0; }
        else if (z == 19) { int r = 4 + bits(1); int y = dec(&pre); int v = (lens[x] + 17 - y) % 17;
            while (r-- && x < last) lens[x++] = (uint8_t)v; }
        else { lens[x] = (uint8_t)((lens[x] + 17 - z) % 17); x++; }
    }
}
int main(int argc, char **argv) {
    int wb = atoi(argv[1]); size_t out_size = strtoul(argv[2], 0, 0);
    size_t cap = 1 << 20; in = malloc(cap); in_len = 0; size_t r;
    while ((r = fread(in + in_len, 1, cap - in_len, stdin)) > 0) { in_len += r; if (in_len == cap) in = realloc(in, cap *= 2); }
    uint8_t *out = calloc(out_size + 65536, 1); size_t op = 0;
    static const int slots_for[] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,30,32,34,36,38,42,50,66,98,162};
    int nslots = slots_for[wb], main_n = 256 + 8 * nslots;
    int ebits[300]; uint32_t base[300];
    for (int i = 0, b = 0; i < 300; i++) { ebits[i] = i < 4 ? 0 : (i / 2 - 1 > 17 ? 17 : i / 2 - 1); base[i] = (uint32_t)b; b += 1 << ebits[i]; }
    static Tree mainT, lenT, alT; mainT.n = main_n; lenT.n = 249; alT.n = 8;
    memset(mainT.len, 0, sizeof mainT.len); memset(lenT.len, 0, sizeof lenT.len);
    uint32_t R0 = 1, R1 = 1, R2 = 1;
    int e8 = bits(1); if (e8) { bits(16); bits(16); }
    size_t frame_end = 32768;
    while (op < out_size) {
        int type = bits(3); uint32_t hi = bits(16), lo = bits(8); uint32_t remaining = (hi << 8) | lo;
        if (type == 2) { for (int i = 0; i < 8; i++) alT.len[i] = (uint8_t)bits(3); build(&alT); }
        if (type == 1 || type == 2) {
            readlens(mainT.len, 0, 256); readlens(mainT.len, 256, main_n); build(&mainT);
            readlens(lenT.len, 0, 249); build(&lenT);
        } else if (type == 3) {
            bitpos = ((bitpos + 16) >> 4) << 4;    /* 1-16 bits of padding */
            size_t bp = bitpos >> 3;
            R0 = in[bp] | in[bp+1]<<8 | in[bp+2]<<16 | (uint32_t)in[bp+3]<<24; bp += 4;
            R1 = in[bp] | in[bp+1]<<8 | in[bp+2]<<16 | (uint32_t)in[bp+3]<<24; bp += 4;
            R2 = in[bp] | in[bp+1]<<8 | in[bp+2]<<16 | (uint32_t)in[bp+3]<<24; bp += 4;
            memcpy(out + op, in + bp, remaining); op += remaining; bp += remaining; if (remaining & 1) bp++;
            bitpos = bp << 3;
            while (op >= frame_end) frame_end += 32768;
            continue;
        } else { fprintf(stderr, "bad block type %d at out %zu\n", type, op); fwrite(out, 1, op, stdout); return 3; }
        while (remaining > 0 && op < out_size) {
            int s = dec(&mainT);
            if (s < 256) { out[op++] = (uint8_t)s; remaining--; }
            else {
                s -= 256; int ml = s & 7; if (ml == 7) ml += dec(&lenT); ml += 2;
                int slot = s >> 3; uint32_t off;
                if (slot == 0) off = R0;
                else if (slot == 1) { off = R1; R1 = R0; R0 = off; }
                else if (slot == 2) { off = R2; R2 = R0; R0 = off; }
                else {
                    int e = ebits[slot];
                    if (type == 2 && e >= 3) { uint32_t v = bits(e - 3) << 3; v += (uint32_t)dec(&alT); off = base[slot] - 2 + v; }
                    else off = base[slot] - 2 + bits(e);
                    R2 = R1; R1 = R0; R0 = off;
                }
                for (int i = 0; i < ml; i++, op++) out[op] = op >= off ? out[op - off] : 0;
                remaining -= (uint32_t)ml;
            }
            if (op >= frame_end) {                 /* realign at each 32K frame */
                if (bitpos & 15) bitpos = ((bitpos >> 4) + 1) << 4;
                frame_end += 32768;
            }
        }
    }
    fwrite(out, 1, out_size, stdout);
    return 0;
}
