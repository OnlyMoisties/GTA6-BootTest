/* RDNA2 (gfx10.3) compute interpreter, wave32. Single-header; included by d3d12_xs.c.
   Executes a shader blob for a dispatch against the game's real memory. Unsupported instructions are reported by name. */
#pragma once
#include <math.h>
#include "rdna2_ops.h"

typedef struct {
    uint32_t s[128];            /* SGPRs (s0..s105) + aliases stored at their encoding index (vcc 106/107, m0 124, exec 126/127) */
    uint32_t v[256][32];        /* VGPRs, 32 lanes */
    int scc;
    const uint8_t* code; size_t codesize; size_t pc;
    int done; char err[200];
    uint64_t lit;               /* current instruction's literal */
    uint64_t steps;
} Wave;
#define EXEC(w) ((w)->s[126])
#define VCC(w) ((w)->s[106])

/* ---- GPU address translation: 32-bit pointers the shader builds with hi=0 map to registered regions ---- */
typedef struct { uint64_t lo, full, size; } Alias;
static Alias g_alias[64]; static volatile LONG g_nalias;
static void emu_alias(uint64_t full, uint64_t size) {
    uint64_t lo = full & 0xFFFFFFFFull;
    for (LONG i = 0; i < g_nalias && i < 64; i++) if (g_alias[i].full == full) return;
    LONG i = InterlockedIncrement(&g_nalias) - 1; if (i < 64) { g_alias[i].lo = lo; g_alias[i].full = full; g_alias[i].size = size; }
}
static uint8_t* emu_ptr(uint64_t a, size_t n) {
    if (!(a >> 32)) for (LONG i = 0; i < g_nalias && i < 64; i++) if (a >= g_alias[i].lo && a < g_alias[i].lo + g_alias[i].size) { a = g_alias[i].full + (a - g_alias[i].lo); break; }
    if (!readable(a, n)) return NULL;
    return (uint8_t*)a;
}

static float f32(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t u32f(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static void werr(Wave* w, const char* what, const char* nm) { if (!w->done) { snprintf(w->err, sizeof w->err, "%s %s at pc 0x%zX", what, nm ? nm : "?", w->pc); w->done = 2; } }

/* scalar source operand (9-bit encoding space, values < 256 for scalar fields) */
static uint32_t ssrc(Wave* w, uint32_t c) {
    if (c <= 105) return w->s[c];
    switch (c) {
    case 106: case 107: case 124: case 126: case 127: return w->s[c];
    case 125: return 0;
    case 251: return VCC(w) == 0; case 252: return EXEC(w) == 0; case 253: return (uint32_t)w->scc;
    case 255: return (uint32_t)w->lit;
    case 240: return u32f(0.5f); case 241: return u32f(-0.5f); case 242: return u32f(1.0f); case 243: return u32f(-1.0f);
    case 244: return u32f(2.0f); case 245: return u32f(-2.0f); case 246: return u32f(4.0f); case 247: return u32f(-4.0f);
    case 248: return 0x3E22F983;   /* 1/(2*pi) */
    }
    if (c >= 128 && c <= 192) return c - 128;
    if (c >= 193 && c <= 208) return (uint32_t)-(int32_t)(c - 192);
    return 0;
}
static void sdst(Wave* w, uint32_t c, uint32_t v) { if (c <= 107 || c == 124 || c == 126) w->s[c] = v; else if (c == 127) w->s[127] = 0; }
/* vector source: VGPR or scalar broadcast */
static uint32_t vsrc(Wave* w, uint32_t c, int lane) { return c >= 256 ? w->v[c - 256][lane] : ssrc(w, c); }

/* ---- buffer descriptor (V#, 4 dwords) ---- */
typedef struct { uint64_t base; uint32_t stride, num, w3; } VBuf;
static VBuf vbuf(const uint32_t* d) { VBuf b; b.base = d[0] | ((uint64_t)(d[1] & 0xFFFF) << 32); b.stride = (d[1] >> 16) & 0x3FFF; b.num = d[2]; b.w3 = d[3]; return b; }

/* ---- images: gfx10 T# decode (base, dims, type, swizzle) + DXGI format from the resource registry ---- */
typedef struct { uint8_t* base; uint32_t w, h, d, fmt, bpp, pitch, slice; } ImgInfo;
static float half2f(uint16_t h) { uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023, u;
    if (!e) { if (!m) u = s << 31; else { e = 113; while (!(m & 1024)) { m <<= 1; e--; } m &= 1023; u = (s << 31) | (e << 23) | (m << 13); } }
    else if (e == 31) u = (s << 31) | 0x7F800000 | (m << 13); else u = (s << 31) | ((e + 112) << 23) | (m << 13);
    return f32(u); }
static uint16_t f2half(float f) { uint32_t u = u32f(f), s = (u >> 16) & 0x8000; int e = (int)((u >> 23) & 255) - 112; uint32_t m = u & 0x7FFFFF;
    if (((u >> 23) & 255) == 255) return (uint16_t)(s | 0x7C00 | (m ? 0x200 : 0));
    if (e <= 0) return (uint16_t)s; if (e >= 31) return (uint16_t)(s | 0x7C00);
    return (uint16_t)(s | (e << 10) | ((m + 0x1000) >> 13)); }
/* unsigned small floats (R11G11B10): 5-bit exponent, 6/5-bit mantissa */
static float uf2f(uint32_t v, int mbits) { uint32_t e = v >> mbits, m = v & ((1u << mbits) - 1);
    if (!e) return ldexpf((float)m, -14 - mbits); if (e == 31) return m ? NAN : INFINITY; return ldexpf(1.0f + (float)m / (1 << mbits), (int)e - 15); }
static uint32_t f2uf(float f, int mbits) { if (!(f > 0)) return 0; int e; float m = frexpf(f, &e); int ee = e - 1 + 15; if (ee >= 31) return (31u << mbits);
    if (ee <= 0) return (uint32_t)(f * ldexpf(1.0f, 14 + mbits)); uint32_t mm = (uint32_t)((m * 2 - 1) * (1 << mbits) + 0.5f); if (mm >> mbits) { mm = 0; ee++; } return ((uint32_t)ee << mbits) | mm; }
static int fmt_bytes(uint32_t f) {
    switch (f) { case 2: case 1: case 3: case 4: return 16; case 10: case 11: case 12: case 13: case 14: case 16: return 8;
    case 24: case 25: case 26: case 27: case 28: case 29: case 30: case 31: case 32: case 34: case 35: case 39: case 40: case 41: case 42: case 43: case 87: case 88: case 91: return 4;
    case 49: case 54: case 56: case 57: case 58: case 59: return 2; case 61: case 62: case 63: case 64: case 65: return 1; }
    return 0; }
static int img_decode(const uint32_t* t, ImgInfo* im, char* why, size_t whyn) {
    uint64_t base = ((uint64_t)t[0] | ((uint64_t)(t[1] & 0xFF) << 32)) << 8;
    uint32_t type = t[3] >> 28, sw = (t[3] >> 20) & 31;
    memset(im, 0, sizeof *im);
    im->w = (t[2] & 0x3FFF) + 1; im->h = ((t[2] >> 14) & 0x3FFF) + 1; im->d = type == 10 ? (t[4] & 0x1FFF) + 1 : 1;
    if (sw) { snprintf(why, whyn, "tiled image (swizzle mode %u) not supported yet:", sw); return 0; }
    const ResReg* rr = emu_res_lookup(base);
    if (!rr) { snprintf(why, whyn, "image base 0x%llX not a known resource:", base); return 0; }
    im->fmt = rr->fmt; im->bpp = fmt_bytes(rr->fmt);
    if (!im->bpp) { snprintf(why, whyn, "image DXGI format %u not supported yet:", rr->fmt); return 0; }
    im->pitch = (im->w * im->bpp + 255) & ~255u; im->slice = im->pitch * im->h;
    im->base = emu_ptr(base, (size_t)im->slice * im->d);
    if (!im->base) { snprintf(why, whyn, "image memory 0x%llX unreadable:", base); return 0; }
    return 1;
}
static uint8_t* img_texel(const ImgInfo* im, uint32_t x, uint32_t y, uint32_t z) {
    if (x >= im->w || y >= im->h || z >= im->d) return NULL;
    return im->base + (size_t)z * im->slice + (size_t)y * im->pitch + (size_t)x * im->bpp;
}
static float unorm(uint32_t v, int bits) { return (float)v / (float)((1u << bits) - 1); }
static uint32_t tounorm(float f, int bits) { f = f != f ? 0 : f < 0 ? 0 : f > 1 ? 1 : f; return (uint32_t)(f * ((1u << bits) - 1) + 0.5f); }
static void img_unpack(const ImgInfo* im, const uint8_t* p, float* c) {
    uint32_t u = 0; memcpy(&u, p, im->bpp < 4 ? im->bpp : 4);
    switch (im->fmt) {
    case 2: case 1: for (int i = 0; i < 4; i++) c[i] = f32(((const uint32_t*)p)[i]); break;
    case 10: case 11: for (int i = 0; i < 4; i++) c[i] = half2f(((const uint16_t*)p)[i]); break;
    case 16: c[0] = f32(((const uint32_t*)p)[0]); c[1] = f32(((const uint32_t*)p)[1]); break;
    case 26: c[0] = uf2f(u & 0x7FF, 6); c[1] = uf2f((u >> 11) & 0x7FF, 6); c[2] = uf2f(u >> 22, 5); c[3] = 1; break;
    case 24: case 25: c[0] = unorm(u & 1023, 10); c[1] = unorm((u >> 10) & 1023, 10); c[2] = unorm((u >> 20) & 1023, 10); c[3] = unorm(u >> 30, 2); break;
    case 27: case 28: case 29: for (int i = 0; i < 4; i++) c[i] = unorm((u >> (8 * i)) & 255, 8); break;
    case 87: case 88: case 91: c[2] = unorm(u & 255, 8); c[1] = unorm((u >> 8) & 255, 8); c[0] = unorm((u >> 16) & 255, 8); c[3] = unorm(u >> 24, 8); break;
    case 34: case 35: c[0] = half2f((uint16_t)u); c[1] = half2f((uint16_t)(u >> 16)); break;
    case 39: case 40: case 41: c[0] = f32(u); break;
    case 42: case 43: c[0] = f32(u); break;
    case 54: case 49: c[0] = half2f((uint16_t)u); break;
    case 56: case 57: c[0] = unorm(u & 0xFFFF, 16); break;
    case 61: case 62: case 65: c[0] = unorm(u & 255, 8); break;
    default: break;
    }
}
static void img_pack(const ImgInfo* im, uint8_t* p, const float* c) {
    uint32_t u = 0;
    switch (im->fmt) {
    case 2: case 1: for (int i = 0; i < 4; i++) ((uint32_t*)p)[i] = u32f(c[i]); return;
    case 10: case 11: for (int i = 0; i < 4; i++) ((uint16_t*)p)[i] = f2half(c[i]); return;
    case 16: ((uint32_t*)p)[0] = u32f(c[0]); ((uint32_t*)p)[1] = u32f(c[1]); return;
    case 26: u = f2uf(c[0], 6) | (f2uf(c[1], 6) << 11) | (f2uf(c[2], 5) << 22); break;
    case 24: case 25: u = tounorm(c[0], 10) | (tounorm(c[1], 10) << 10) | (tounorm(c[2], 10) << 20) | (tounorm(c[3], 2) << 30); break;
    case 27: case 28: case 29: for (int i = 0; i < 4; i++) u |= tounorm(c[i], 8) << (8 * i); break;
    case 87: case 88: case 91: u = tounorm(c[2], 8) | (tounorm(c[1], 8) << 8) | (tounorm(c[0], 8) << 16) | (tounorm(c[3], 8) << 24); break;
    case 34: case 35: u = f2half(c[0]) | ((uint32_t)f2half(c[1]) << 16); break;
    case 39: case 40: case 41: case 42: case 43: u = u32f(c[0]); break;
    case 54: case 49: u = f2half(c[0]); break;
    case 56: case 57: u = tounorm(c[0], 16); break;
    case 61: case 62: case 65: u = tounorm(c[0], 8); break;
    default: return;
    }
    memcpy(p, &u, im->bpp < 4 ? im->bpp : 4);
}
static volatile LONG g_tlog;
static void log_tdesc(const char* what, const uint32_t* t) {
    if (InterlockedIncrement(&g_tlog) <= 12)
        dlog("EMU %s T#: %08X %08X %08X %08X %08X %08X %08X %08X", what, t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7]);
}

/* ---- one instruction ---- */
static void step(Wave* w) {
    if (w->pc + 4 > w->codesize) { werr(w, "ran off end", NULL); return; }
    uint32_t d0 = *(const uint32_t*)(w->code + w->pc), d1 = (w->pc + 8 <= w->codesize) ? *(const uint32_t*)(w->code + w->pc + 4) : 0;
    size_t len = 4; w->steps++;
    /* ---------------- scalar ---------------- */
    if ((d0 >> 23) == 0x17F) {                                  /* SOPP */
        uint32_t op = (d0 >> 16) & 0x7F; int16_t imm = (int16_t)(d0 & 0xFFFF);
        size_t next = w->pc + 4, tgt = (size_t)((int64_t)next + 4 * (int64_t)imm);
        switch (op) {
        case OP_SOPP_S_ENDPGM: w->done = 1; return;
        case OP_SOPP_S_NOP: case OP_SOPP_S_WAITCNT: case OP_SOPP_S_CLAUSE: case OP_SOPP_S_INST_PREFETCH: case OP_SOPP_S_CODE_END: break;
        case OP_SOPP_S_BRANCH: w->pc = tgt; return;
        case OP_SOPP_S_CBRANCH_SCC0: if (!w->scc) { w->pc = tgt; return; } break;
        case OP_SOPP_S_CBRANCH_SCC1: if (w->scc) { w->pc = tgt; return; } break;
        case OP_SOPP_S_CBRANCH_VCCZ: if (!VCC(w)) { w->pc = tgt; return; } break;
        case OP_SOPP_S_CBRANCH_VCCNZ: if (VCC(w)) { w->pc = tgt; return; } break;
        case OP_SOPP_S_CBRANCH_EXECZ: if (!EXEC(w)) { w->pc = tgt; return; } break;
        case OP_SOPP_S_CBRANCH_EXECNZ: if (EXEC(w)) { w->pc = tgt; return; } break;
        default: werr(w, "unsupported", rdna2_names_SOPP[op]); return;
        }
        w->pc += 4; return;
    }
    if ((d0 >> 23) == 0x17D) {                                  /* SOP1 */
        uint32_t op = (d0 >> 8) & 0xFF, dst = (d0 >> 16) & 0x7F, s0c = d0 & 0xFF;
        if (s0c == 255) { w->lit = d1; len = 8; }
        uint32_t a = ssrc(w, s0c), r;
        switch (op) {
        case OP_SOP1_S_MOV_B32: sdst(w, dst, a); break;
        case OP_SOP1_S_MOV_B64: sdst(w, dst, a); sdst(w, dst + 1, s0c <= 105 || s0c >= 106 && s0c <= 127 ? ssrc(w, s0c + 1) : (s0c >= 193 && s0c <= 208 ? 0xFFFFFFFF : 0)); break;
        case OP_SOP1_S_NOT_B32: r = ~a; sdst(w, dst, r); w->scc = r != 0; break;
        case OP_SOP1_S_BREV_B32: { r = 0; for (int i = 0; i < 32; i++) if (a >> i & 1) r |= 1u << (31 - i); sdst(w, dst, r); break; }
        case OP_SOP1_S_AND_SAVEEXEC_B32: sdst(w, dst, EXEC(w)); EXEC(w) &= a; w->scc = EXEC(w) != 0; break;
        case OP_SOP1_S_OR_SAVEEXEC_B32: sdst(w, dst, EXEC(w)); EXEC(w) |= a; w->scc = EXEC(w) != 0; break;
        case OP_SOP1_S_ANDN2_SAVEEXEC_B32: sdst(w, dst, EXEC(w)); EXEC(w) = a & ~EXEC(w); w->scc = EXEC(w) != 0; break;
        default: werr(w, "unsupported", rdna2_names_SOP1[op]); return;
        }
        w->pc += len; return;
    }
    if ((d0 >> 23) == 0x17E) {                                  /* SOPC */
        uint32_t op = (d0 >> 16) & 0x7F, c0 = d0 & 0xFF, c1 = (d0 >> 8) & 0xFF;
        if (c0 == 255 || c1 == 255) { w->lit = d1; len = 8; }
        uint32_t a = ssrc(w, c0), b = ssrc(w, c1);
        switch (op) {
        case OP_SOPC_S_CMP_EQ_I32: case OP_SOPC_S_CMP_EQ_U32: w->scc = a == b; break;
        case OP_SOPC_S_CMP_LG_I32: case OP_SOPC_S_CMP_LG_U32: w->scc = a != b; break;
        case OP_SOPC_S_CMP_GT_I32: w->scc = (int32_t)a > (int32_t)b; break;
        case OP_SOPC_S_CMP_GE_I32: w->scc = (int32_t)a >= (int32_t)b; break;
        case OP_SOPC_S_CMP_LT_I32: w->scc = (int32_t)a < (int32_t)b; break;
        case OP_SOPC_S_CMP_LE_I32: w->scc = (int32_t)a <= (int32_t)b; break;
        case OP_SOPC_S_CMP_GT_U32: w->scc = a > b; break;
        case OP_SOPC_S_CMP_GE_U32: w->scc = a >= b; break;
        case OP_SOPC_S_CMP_LT_U32: w->scc = a < b; break;
        case OP_SOPC_S_CMP_LE_U32: w->scc = a <= b; break;
        default: werr(w, "unsupported", rdna2_names_SOPC[op]); return;
        }
        w->pc += len; return;
    }
    if ((d0 >> 28) == 0xB) {                                    /* SOPK */
        uint32_t op = (d0 >> 23) & 0x1F, dst = (d0 >> 16) & 0x7F; int16_t imm = (int16_t)(d0 & 0xFFFF);
        switch (op) {
        case OP_SOPK_S_MOVK_I32: sdst(w, dst, (uint32_t)(int32_t)imm); break;
        default: werr(w, "unsupported", rdna2_names_SOPK[op]); return;
        }
        w->pc += 4; return;
    }
    if ((d0 >> 30) == 2) {                                      /* SOP2 */
        uint32_t op = (d0 >> 23) & 0x7F, dst = (d0 >> 16) & 0x7F, c0 = d0 & 0xFF, c1 = (d0 >> 8) & 0xFF;
        if (c0 == 255 || c1 == 255 || rdna2_lit_SOP2[op]) { w->lit = d1; len = 8; }
        uint32_t a = ssrc(w, c0), b = ssrc(w, c1), r; uint64_t q;
        switch (op) {
        case OP_SOP2_S_ADD_U32: q = (uint64_t)a + b; sdst(w, dst, (uint32_t)q); w->scc = (int)(q >> 32); break;
        case OP_SOP2_S_SUB_U32: sdst(w, dst, a - b); w->scc = b > a; break;
        case OP_SOP2_S_ADD_I32: r = a + b; sdst(w, dst, r); w->scc = ((~(a ^ b)) & (a ^ r)) >> 31; break;
        case OP_SOP2_S_SUB_I32: r = a - b; sdst(w, dst, r); w->scc = ((a ^ b) & (a ^ r)) >> 31; break;
        case OP_SOP2_S_ADDC_U32: q = (uint64_t)a + b + (uint32_t)w->scc; sdst(w, dst, (uint32_t)q); w->scc = (int)(q >> 32); break;
        case OP_SOP2_S_MUL_I32: sdst(w, dst, (uint32_t)((int32_t)a * (int32_t)b)); break;
        case OP_SOP2_S_AND_B32: r = a & b; sdst(w, dst, r); w->scc = r != 0; break;
        case OP_SOP2_S_OR_B32: r = a | b; sdst(w, dst, r); w->scc = r != 0; break;
        case OP_SOP2_S_XOR_B32: r = a ^ b; sdst(w, dst, r); w->scc = r != 0; break;
        case OP_SOP2_S_ANDN2_B32: r = a & ~b; sdst(w, dst, r); w->scc = r != 0; break;
        case OP_SOP2_S_AND_B64: {                               /* wave32: only the low halves matter for exec/vcc masks */
            uint32_t ah = (c0 <= 105 || (c0 >= 106 && c0 <= 127)) ? ssrc(w, c0 + 1) : 0, bh = (c1 <= 105 || (c1 >= 106 && c1 <= 127)) ? ssrc(w, c1 + 1) : 0;
            sdst(w, dst, a & b); sdst(w, dst + 1, ah & bh); w->scc = ((a & b) | (ah & bh)) != 0; break; }
        case OP_SOP2_S_CSELECT_B32: sdst(w, dst, w->scc ? a : b); break;
        case OP_SOP2_S_LSHL_B32: r = a << (b & 31); sdst(w, dst, r); w->scc = r != 0; break;
        case OP_SOP2_S_LSHR_B32: r = a >> (b & 31); sdst(w, dst, r); w->scc = r != 0; break;
        case OP_SOP2_S_ASHR_I32: r = (uint32_t)((int32_t)a >> (b & 31)); sdst(w, dst, r); w->scc = r != 0; break;
        case OP_SOP2_S_MIN_U32: r = a < b ? a : b; sdst(w, dst, r); w->scc = a < b; break;
        case OP_SOP2_S_MAX_U32: r = a > b ? a : b; sdst(w, dst, r); w->scc = a > b; break;
        case OP_SOP2_S_MIN_I32: r = (int32_t)a < (int32_t)b ? a : b; sdst(w, dst, r); w->scc = (int32_t)a < (int32_t)b; break;
        case OP_SOP2_S_MAX_I32: r = (int32_t)a > (int32_t)b ? a : b; sdst(w, dst, r); w->scc = (int32_t)a > (int32_t)b; break;
        case OP_SOP2_S_BFE_U32: { uint32_t off = b & 31, wd = (b >> 16) & 0x7F; r = wd ? (a >> off) & (wd >= 32 ? 0xFFFFFFFF : ((1u << wd) - 1)) : 0; sdst(w, dst, r); w->scc = r != 0; break; }
        default: werr(w, "unsupported", rdna2_names_SOP2[op]); return;
        }
        w->pc += len; return;
    }
    if ((d0 >> 26) == 0x3D) {                                   /* SMEM */
        uint32_t op = (d0 >> 18) & 0xFF, sdata = (d0 >> 6) & 0x7F, sbase = (d0 & 0x3F) * 2;
        int32_t off = (int32_t)(d1 << 11) >> 11; uint32_t soff = d1 >> 25;
        uint64_t addr; int n;
        switch (op) {
        case OP_SMEM_S_LOAD_DWORD: n = 1; break; case OP_SMEM_S_LOAD_DWORDX2: n = 2; break; case OP_SMEM_S_LOAD_DWORDX4: n = 4; break;
        case OP_SMEM_S_LOAD_DWORDX8: n = 8; break; case OP_SMEM_S_LOAD_DWORDX16: n = 16; break;
        case OP_SMEM_S_BUFFER_LOAD_DWORD: n = -1; break; case OP_SMEM_S_BUFFER_LOAD_DWORDX2: n = -2; break; case OP_SMEM_S_BUFFER_LOAD_DWORDX4: n = -4; break;
        case OP_SMEM_S_BUFFER_LOAD_DWORDX8: n = -8; break; case OP_SMEM_S_BUFFER_LOAD_DWORDX16: n = -16; break;
        default: werr(w, "unsupported", rdna2_names_SMEM[op]); return;
        }
        int64_t o = off + (soff == 125 ? 0 : (int64_t)ssrc(w, soff));
        if (n > 0) addr = ((uint64_t)w->s[sbase] | ((uint64_t)(w->s[sbase + 1] & 0xFFFF) << 32)) + o;
        else { VBuf b = vbuf(&w->s[sbase]); addr = b.base + o; n = -n; }
        uint8_t* p = emu_ptr(addr, (size_t)n * 4);
        if (!p) { char m[64]; snprintf(m, sizeof m, "0x%llX", addr); werr(w, "SMEM bad address", m); return; }
        memcpy(&w->s[sdata], p, (size_t)n * 4);
        w->pc += 8; return;
    }
    /* ---------------- vector ALU ---------------- */
    if ((d0 >> 25) == 0x3F || (d0 >> 31) == 0 || (d0 >> 25) == 0x3E) {   /* VOP1 / VOP2 / VOPC */
        int isvop1 = (d0 >> 25) == 0x3F, isvopc = (d0 >> 25) == 0x3E;
        uint32_t c0 = d0 & 0x1FF, vs1 = (d0 >> 9) & 0xFF, dst = (d0 >> 17) & 0xFF;
        uint32_t op = isvop1 ? (d0 >> 9) & 0xFF : isvopc ? (d0 >> 17) & 0xFF : (d0 >> 25) & 0x3F;
        if (c0 == 255 || (!isvop1 && !isvopc && rdna2_lit_VOP2[op])) { w->lit = d1; len = 8; }
        if (c0 == 249 || c0 == 250 || c0 == 233 || c0 == 234) { werr(w, "unsupported SDWA/DPP on", isvop1 ? rdna2_names_VOP1[op] : isvopc ? rdna2_names_VOPC[op] : rdna2_names_VOP2[op]); return; }
        uint32_t ex = EXEC(w), newvcc = 0;
        for (int l = 0; l < 32; l++) {
            if (!(ex >> l & 1)) continue;
            uint32_t a = vsrc(w, c0, l), b = w->v[vs1][l], r = 0; float fa = f32(a), fb = f32(b);
            if (isvop1) {
                switch (op) {
                case OP_VOP1_V_NOP: continue;
                case OP_VOP1_V_MOV_B32: r = a; break;
                case OP_VOP1_V_CVT_F32_U32: r = u32f((float)a); break;
                case OP_VOP1_V_CVT_F32_I32: r = u32f((float)(int32_t)a); break;
                case OP_VOP1_V_CVT_U32_F32: r = fa <= 0 || fa != fa ? 0 : fa >= 4294967295.0f ? 0xFFFFFFFF : (uint32_t)fa; break;
                case OP_VOP1_V_CVT_I32_F32: r = fa != fa ? 0 : (uint32_t)(int32_t)fa; break;
                case OP_VOP1_V_RCP_F32: r = u32f(1.0f / fa); break;
                case OP_VOP1_V_RSQ_F32: r = u32f(1.0f / sqrtf(fa)); break;
                case OP_VOP1_V_SQRT_F32: r = u32f(sqrtf(fa)); break;
                case OP_VOP1_V_EXP_F32: r = u32f(exp2f(fa)); break;
                case OP_VOP1_V_LOG_F32: r = u32f(log2f(fa)); break;
                case OP_VOP1_V_FRACT_F32: r = u32f(fa - floorf(fa)); break;
                case OP_VOP1_V_FLOOR_F32: r = u32f(floorf(fa)); break;
                case OP_VOP1_V_CEIL_F32: r = u32f(ceilf(fa)); break;
                case OP_VOP1_V_TRUNC_F32: r = u32f(truncf(fa)); break;
                case OP_VOP1_V_RNDNE_F32: r = u32f(nearbyintf(fa)); break;
                case OP_VOP1_V_SIN_F32: r = u32f(sinf(fa * 6.283185307f)); break;
                case OP_VOP1_V_COS_F32: r = u32f(cosf(fa * 6.283185307f)); break;
                case OP_VOP1_V_NOT_B32: r = ~a; break;
                case OP_VOP1_V_READFIRSTLANE_B32: { int fl = 0; while (fl < 32 && !(ex >> fl & 1)) fl++; sdst(w, dst, vsrc(w, c0, fl < 32 ? fl : 0)); goto vop_done; }
                default: werr(w, "unsupported", rdna2_names_VOP1[op]); return;
                }
                w->v[dst][l] = r;
            } else if (isvopc) {
                int c;
                switch (op) {
                case OP_VOPC_V_CMP_LT_F32: c = fa < fb; break; case OP_VOPC_V_CMP_LE_F32: c = fa <= fb; break;
                case OP_VOPC_V_CMP_GT_F32: c = fa > fb; break; case OP_VOPC_V_CMP_GE_F32: c = fa >= fb; break;
                case OP_VOPC_V_CMP_EQ_F32: c = fa == fb; break; case OP_VOPC_V_CMP_NEQ_F32: c = !(fa == fb); break;
                case OP_VOPC_V_CMP_LG_F32: c = fa < fb || fa > fb; break;
                case OP_VOPC_V_CMP_EQ_U32: case OP_VOPC_V_CMP_EQ_I32: c = a == b; break;
                case OP_VOPC_V_CMP_NE_U32: case OP_VOPC_V_CMP_NE_I32: c = a != b; break;
                case OP_VOPC_V_CMP_LT_U32: c = a < b; break; case OP_VOPC_V_CMP_GT_U32: c = a > b; break;
                case OP_VOPC_V_CMP_LE_U32: c = a <= b; break; case OP_VOPC_V_CMP_GE_U32: c = a >= b; break;
                case OP_VOPC_V_CMP_LT_I32: c = (int32_t)a < (int32_t)b; break; case OP_VOPC_V_CMP_GT_I32: c = (int32_t)a > (int32_t)b; break;
                default: werr(w, "unsupported", rdna2_names_VOPC[op]); return;
                }
                if (c) newvcc |= 1u << l;
            } else {
                switch (op) {
                case OP_VOP2_V_CNDMASK_B32: r = (VCC(w) >> l & 1) ? b : a; break;
                case OP_VOP2_V_ADD_F32: r = u32f(fa + fb); break;
                case OP_VOP2_V_SUB_F32: r = u32f(fa - fb); break;
                case OP_VOP2_V_SUBREV_F32: r = u32f(fb - fa); break;
                case OP_VOP2_V_MUL_F32: r = u32f(fa * fb); break;
                case OP_VOP2_V_MIN_F32: r = u32f(fminf(fa, fb)); break;
                case OP_VOP2_V_MAX_F32: r = u32f(fmaxf(fa, fb)); break;
                case OP_VOP2_V_FMAC_F32: r = u32f(fmaf(fa, fb, f32(w->v[dst][l]))); break;
                case OP_VOP2_V_FMAMK_F32: r = u32f(fmaf(fa, f32((uint32_t)w->lit), fb)); break;
                case OP_VOP2_V_FMAAK_F32: r = u32f(fmaf(fa, fb, f32((uint32_t)w->lit))); break;
                case OP_VOP2_V_AND_B32: r = a & b; break; case OP_VOP2_V_OR_B32: r = a | b; break; case OP_VOP2_V_XOR_B32: r = a ^ b; break;
                case OP_VOP2_V_ADD_NC_U32: r = a + b; break; case OP_VOP2_V_SUB_NC_U32: r = a - b; break; case OP_VOP2_V_SUBREV_NC_U32: r = b - a; break;
                case OP_VOP2_V_LSHLREV_B32: r = b << (a & 31); break; case OP_VOP2_V_LSHRREV_B32: r = b >> (a & 31); break;
                case OP_VOP2_V_ASHRREV_I32: r = (uint32_t)((int32_t)b >> (a & 31)); break;
                case OP_VOP2_V_MUL_U32_U24: r = (a & 0xFFFFFF) * (b & 0xFFFFFF); break;
                case OP_VOP2_V_MIN_U32: r = a < b ? a : b; break; case OP_VOP2_V_MAX_U32: r = a > b ? a : b; break;
                default: werr(w, "unsupported", rdna2_names_VOP2[op]); return;
                }
                w->v[dst][l] = r;
            }
        }
        if (isvopc) VCC(w) = newvcc;
    vop_done:
        w->pc += len; return;
    }
    if ((d0 >> 26) == 0x35) {                                   /* VOP3 (incl. VOP3b) */
        uint32_t op = (d0 >> 16) & 0x3FF, dst = d0 & 0xFF, absm = (d0 >> 8) & 7, clampb = (d0 >> 15) & 1;
        uint32_t c0 = d1 & 0x1FF, c1 = (d1 >> 9) & 0x1FF, c2 = (d1 >> 18) & 0x1FF, negm = d1 >> 29, omod = (d1 >> 27) & 3;
        len = 8; if (c0 == 255 || c1 == 255 || c2 == 255) { w->lit = (w->pc + 12 <= w->codesize) ? *(const uint32_t*)(w->code + w->pc + 8) : 0; len = 12; }
        uint32_t ex = EXEC(w), cmpmask = 0; const char* nm = rdna2_names_VOP3[op];
        int is_cmp = op < 0x100;                                /* VOP3 encodings of VOPC compares write an SGPR mask */
        for (int l = 0; l < 32; l++) {
            if (!(ex >> l & 1)) continue;
            uint32_t a = vsrc(w, c0, l), b = vsrc(w, c1, l), c = vsrc(w, c2, l), r = 0;
            float fa = f32(a), fb = f32(b), fc = f32(c);
            if (absm & 1) fa = fabsf(fa); if (absm & 2) fb = fabsf(fb); if (absm & 4) fc = fabsf(fc);
            if (negm & 1) fa = -fa; if (negm & 2) fb = -fb; if (negm & 4) fc = -fc;
            if (is_cmp) {
                int cc; uint32_t vop = op;                       /* VOPC opcodes map 1:1 into VOP3 0x000-0x0FF */
                switch (vop) {
                case OP_VOPC_V_CMP_LT_F32: cc = fa < fb; break; case OP_VOPC_V_CMP_LE_F32: cc = fa <= fb; break;
                case OP_VOPC_V_CMP_GT_F32: cc = fa > fb; break; case OP_VOPC_V_CMP_GE_F32: cc = fa >= fb; break;
                case OP_VOPC_V_CMP_EQ_F32: cc = fa == fb; break; case OP_VOPC_V_CMP_NEQ_F32: cc = !(fa == fb); break;
                case OP_VOPC_V_CMP_EQ_U32: case OP_VOPC_V_CMP_EQ_I32: cc = a == b; break;
                case OP_VOPC_V_CMP_NE_U32: case OP_VOPC_V_CMP_NE_I32: cc = a != b; break;
                case OP_VOPC_V_CMP_LT_U32: cc = a < b; break; case OP_VOPC_V_CMP_GT_U32: cc = a > b; break;
                default: werr(w, "unsupported (vop3 cmp)", rdna2_names_VOPC[vop]); return;
                }
                if (cc) cmpmask |= 1u << l;
                continue;
            }
            switch (op) {
            case OP_VOP3_V_NOP: continue;
            case OP_VOP3_V_FMA_F32: r = u32f(fmaf(fa, fb, fc)); break;
            case OP_VOP3_V_MED3_F32: { float lo = fminf(fa, fb), hi = fmaxf(fa, fb); r = u32f(fminf(fmaxf(lo, fc), hi)); break; }
            case OP_VOP3_V_LSHL_ADD_U32: r = (a << (b & 31)) + c; break;
            case OP_VOP3_V_ADD3_U32: r = a + b + c; break;
            case OP_VOP3_V_MAD_U32_U24: r = (a & 0xFFFFFF) * (b & 0xFFFFFF) + c; break;
            case OP_VOP3_V_MUL_LO_U32: r = a * b; break;
            case OP_VOP3_V_BFE_U32: { uint32_t o = b & 31, wd = c & 31; r = wd ? (a >> o) & ((1u << wd) - 1) : 0; break; }
            case OP_VOP3_V_CNDMASK_B32: r = (ssrc(w, c2) >> l & 1) ? b : a; break;
            case OP_VOP3_V_MUL_F32: r = u32f(fa * fb); break;
            case OP_VOP3_V_ADD_F32: r = u32f(fa + fb); break;
            case OP_VOP3_V_SUB_F32: r = u32f(fa - fb); break;
            case OP_VOP3_V_MAX_F32: r = u32f(fmaxf(fa, fb)); break;
            case OP_VOP3_V_MIN_F32: r = u32f(fminf(fa, fb)); break;
            case OP_VOP3_V_FMAC_F32: r = u32f(fmaf(fa, fb, f32(w->v[dst][l]))); break;
            case OP_VOP3_V_MOV_B32: r = a; break;
            case OP_VOP3_V_RCP_F32: r = u32f(1.0f / fa); break;
            default: werr(w, "unsupported", nm); return;
            }
            if (omod || clampb) { float x = f32(r); if (omod == 1) x *= 2; else if (omod == 2) x *= 4; else if (omod == 3) x *= 0.5f; if (clampb) x = fminf(fmaxf(x, 0.0f), 1.0f); r = u32f(x); }
            w->v[dst][l] = r;
        }
        if (is_cmp) sdst(w, dst, cmpmask);
        w->pc += len; return;
    }
    /* ---------------- image (MIMG): load/store on linear images ---------------- */
    if ((d0 >> 26) == 0x3C) {
        uint32_t op = (d0 >> 18) & 0x7F, dmask = (d0 >> 8) & 0xF, nsa = (d0 >> 1) & 3;
        uint32_t vaddr = d1 & 0xFF, vdata = (d1 >> 8) & 0xFF, srsrc = ((d1 >> 16) & 0x1F) * 4;
        const uint32_t* t = &w->s[srsrc];
        log_tdesc(rdna2_names_MIMG[op], t);
        if (nsa) { werr(w, "unsupported NSA addressing on", rdna2_names_MIMG[op]); return; }
        if (op != OP_MIMG_IMAGE_LOAD && op != OP_MIMG_IMAGE_STORE) { werr(w, "unsupported", rdna2_names_MIMG[op]); return; }
        ImgInfo im; char why[96];
        if (!img_decode(t, &im, why, sizeof why)) { werr(w, why, rdna2_names_MIMG[op]); return; }
        int ncomp = 0; for (int i = 0; i < 4; i++) ncomp += (dmask >> i) & 1;
        uint32_t ex = EXEC(w);
        for (int l = 0; l < 32; l++) {
            if (!(ex >> l & 1)) continue;
            uint32_t x = w->v[vaddr][l], y = im.h > 1 || im.d > 1 ? w->v[vaddr + 1][l] : 0, z = im.d > 1 ? w->v[vaddr + 2][l] : 0;
            uint8_t* px = img_texel(&im, x, y, z);
            float rgba[4] = { 0, 0, 0, 1 };
            if (op == OP_MIMG_IMAGE_STORE) {
                if (!px) continue;                                /* out of bounds stores are dropped */
                if (im.bpp) img_unpack(&im, px, rgba);            /* keep components not in dmask */
                for (int c = 0, k = 0; c < 4; c++) if (dmask >> c & 1) rgba[c] = f32(w->v[vdata + k++][l]);
                img_pack(&im, px, rgba);
            } else {
                if (px) img_unpack(&im, px, rgba);
                else rgba[3] = 0;                                 /* out of bounds loads return 0 */
                for (int c = 0, k = 0; c < 4; c++) if (dmask >> c & 1) w->v[vdata + k++][l] = u32f(rgba[c]);
            }
        }
        (void)ncomp;
        w->pc += 8; return;
    }
    {
        char m[64]; snprintf(m, sizeof m, "0x%08X 0x%08X", d0, d1); werr(w, "unknown encoding", m);
    }
}

/* Run one workgroup. user[] = the user SGPRs; tgid = workgroup id; nthreads x/y = group shape. Returns 1 ok, 0 error. */
static int emu_run_group(const uint8_t* code, size_t size, const uint32_t* user, int nuser, uint32_t gx, uint32_t gy, uint32_t gz,
                         uint32_t tx, uint32_t ty, uint32_t tz, char* err, size_t errn) {
    uint32_t threads = tx * ty * tz;
    for (uint32_t base = 0; base < threads; base += 32) {
        Wave* w = (Wave*)calloc(1, sizeof(Wave)); if (!w) return 0;
        w->code = code; w->codesize = size; w->pc = 0;
        for (int i = 0; i < nuser; i++) w->s[i] = user[i];
        w->s[nuser] = gx; w->s[nuser + 1] = gy; w->s[nuser + 2] = gz;
        uint32_t ex = 0;
        for (int l = 0; l < 32; l++) {
            uint32_t t = base + l; if (t >= threads) continue;
            ex |= 1u << l;
            w->v[0][l] = t % tx; w->v[1][l] = (t / tx) % ty; w->v[2][l] = t / (tx * ty);
        }
        EXEC(w) = ex;
        while (!w->done && w->steps < 2000000) step(w);
        int ok = w->done == 1;
        if (!ok) snprintf(err, errn, "%s (after %llu instructions)", w->err[0] ? w->err : "step limit", w->steps);
        free(w);
        if (!ok) return 0;
    }
    return 1;
}
