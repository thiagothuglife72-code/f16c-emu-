#include "emu_core.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* Conversoes half <-> single                                          */
/* ------------------------------------------------------------------ */

uint32_t emu_f16_to_f32(uint16_t h, uint32_t *flags)
{
    uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    uint32_t exp  = (h >> 10) & 0x1Fu;
    uint32_t man  = h & 0x3FFu;

    if (exp == 0x1Fu) {
        if (man == 0) return sign | 0x7F800000u;              /* infinito */
        if (!(man & 0x200u)) *flags |= EMU_MXCSR_IE;          /* SNaN */
        return sign | 0x7FC00000u | (man << 13);              /* vira QNaN */
    }
    if (exp == 0) {
        if (man == 0) return sign;                            /* zero */
        int e = -14;                                          /* denormal: normaliza */
        while (!(man & 0x400u)) { man <<= 1; e--; }
        man &= 0x3FFu;
        return sign | ((uint32_t)(e + 127) << 23) | (man << 13);
    }
    return sign | ((exp - 15u + 127u) << 23) | (man << 13);
}

uint16_t emu_f32_to_f16(uint32_t f, unsigned rc, uint32_t *flags)
{
    uint32_t sign = f >> 31;
    uint32_t exp  = (f >> 23) & 0xFFu;
    uint32_t man  = f & 0x7FFFFFu;
    uint16_t s16  = (uint16_t)(sign << 15);

    if (exp == 0xFFu) {
        if (man == 0) return s16 | 0x7C00u;                   /* infinito */
        if (!(man & 0x400000u)) *flags |= EMU_MXCSR_IE;       /* SNaN */
        return s16 | 0x7E00u | (uint16_t)((man >> 13) & 0x3FFu);
    }
    if (exp == 0 && man == 0) return s16;                     /* zero */

    int e;
    uint64_t m;
    if (exp == 0) { e = -126; m = man; }                      /* denormal de 32 bits */
    else          { e = (int)exp - 127; m = man | 0x800000u; }

    /* resultado maximo/infinito em caso de overflow, conforme o arredondamento */
    #define EMU_OVERFLOW() do { \
        *flags |= EMU_MXCSR_OE | EMU_MXCSR_PE; \
        int to_inf = (rc == 0) || (rc == 1 && sign) || (rc == 2 && !sign); \
        return s16 | (to_inf ? 0x7C00u : 0x7BFFu); } while (0)

    if (e > 15) EMU_OVERFLOW();

    int shift = 13 + (e < -14 ? (-14 - e) : 0);
    uint64_t q, rem, half;
    int inexact;
    if (shift >= 40) {                                        /* valor muito menor que o menor denormal */
        q = 0; rem = 1; half = 2; inexact = 1;
    } else {
        q = m >> shift;
        rem = m & ((1ull << shift) - 1);
        half = 1ull << (shift - 1);
        inexact = rem != 0;
    }

    int inc = 0;
    switch (rc & 3u) {
    case 0: inc = (rem > half) || (rem == half && (q & 1)); break;
    case 1: inc = sign && rem != 0; break;
    case 2: inc = !sign && rem != 0; break;
    default: inc = 0; break;
    }
    q += (uint64_t)inc;
    if (inexact) *flags |= EMU_MXCSR_PE;

    uint32_t out;
    if (e >= -14) {
        out = (uint32_t)((((uint32_t)(e + 15) - 1u) << 10) + q);
        if (out >= 0x7C00u) EMU_OVERFLOW();
    } else {
        out = (uint32_t)q;                                    /* denormal (q==1024 vira o menor normal) */
        if (inexact) {
            /* x86 mede "tiny" depois de arredondar com 11 bits e expoente ilimitado:
               so deixa de ser tiny se o valor arredondado chega a 2^-14 */
            int tiny = 1;
            if (e == -15) {
                uint64_t q11 = m >> 13, r13 = m & 0x1FFFu;
                int inc11;
                switch (rc & 3u) {
                case 0: inc11 = (r13 > 0x1000u) || (r13 == 0x1000u && (q11 & 1)); break;
                case 1: inc11 = sign && r13 != 0; break;
                case 2: inc11 = !sign && r13 != 0; break;
                default: inc11 = 0; break;
                }
                if (q11 + (uint64_t)inc11 >= 0x800u) tiny = 0;
            }
            if (tiny) *flags |= EMU_MXCSR_UE;
        }
    }
    #undef EMU_OVERFLOW
    return s16 | (uint16_t)out;
}

/* ------------------------------------------------------------------ */
/* Decodificacao                                                       */
/* ------------------------------------------------------------------ */

typedef struct { const uint8_t *p; size_t avail, pos; } Cur;

static int get8(Cur *c, uint8_t *o)
{
    if (c->pos >= c->avail) return 0;
    *o = c->p[c->pos++];
    return 1;
}
static int peek8(const Cur *c, uint8_t *o)
{
    if (c->pos >= c->avail) return 0;
    *o = c->p[c->pos];
    return 1;
}
static int get32(Cur *c, int32_t *o)
{
    if (c->pos + 4 > c->avail) return 0;
    memcpy(o, c->p + c->pos, 4);
    c->pos += 4;
    return 1;
}

typedef struct {
    int      is_reg;
    unsigned reg;      /* campo reg estendido (REX.R / VEX.R) */
    unsigned digit;    /* campo reg sem extensao (/digito) */
    unsigned rm;       /* registrador rm estendido (quando is_reg) */
    uint64_t ea;       /* endereco efetivo (quando !is_reg), sem o RIP relativo */
    int      rip_rel;
    int32_t  rip_disp;
} ModRM;

static int decode_modrm(Cur *c, const EmuState *s, unsigned R, unsigned X, unsigned B, ModRM *o)
{
    uint8_t m;
    if (!get8(c, &m)) return 0;
    unsigned mod = m >> 6, regf = (m >> 3) & 7, rm = m & 7;
    memset(o, 0, sizeof *o);
    o->reg = regf | (R << 3);
    o->digit = regf;
    if (mod == 3) { o->is_reg = 1; o->rm = rm | (B << 3); return 1; }

    uint64_t ea = 0;
    int32_t disp = 0;
    if (rm == 4) {
        uint8_t sib;
        if (!get8(c, &sib)) return 0;
        unsigned idx = ((sib >> 3) & 7) | (X << 3);
        unsigned base = sib & 7;
        if (idx != 4) ea += s->gpr[idx] << (sib >> 6);
        if (base == 5 && mod == 0) { if (!get32(c, &disp)) return 0; }
        else ea += s->gpr[base | (B << 3)];
    } else if (rm == 5 && mod == 0) {
        if (!get32(c, &disp)) return 0;
        o->rip_rel = 1;
        o->rip_disp = disp;
        disp = 0;
    } else {
        ea += s->gpr[rm | (B << 3)];
    }
    if (mod == 1) { uint8_t d; if (!get8(c, &d)) return 0; disp = (int8_t)d; }
    else if (mod == 2) { if (!get32(c, &disp)) return 0; }
    o->ea = ea + (uint64_t)(int64_t)disp;
    return 1;
}

static uint64_t operand_ea(const ModRM *m, const EmuState *s, size_t len)
{
    return m->rip_rel ? s->rip + len + (uint64_t)(int64_t)m->rip_disp : m->ea;
}

/* ------------------------------------------------------------------ */
/* F16C                                                                */
/* ------------------------------------------------------------------ */

static EmuResult do_cvtph2ps(EmuState *s, const EmuMem *mem, const ModRM *m, uint64_t ea, int L)
{
    unsigned n = L ? 8 : 4;
    uint16_t h[8];
    uint32_t out[8], fl = 0;
    if (L && !s->have_ymm_hi) return EMU_UNSUPPORTED;
    if (m->is_reg) memcpy(h, s->ymm[m->rm], n * 2);
    else if (!mem->read(mem->user, ea, h, n * 2)) return EMU_MEMFAULT;
    for (unsigned i = 0; i < n; i++) out[i] = emu_f16_to_f32(h[i], &fl);
    memset(s->ymm[m->reg], 0, 32);
    memcpy(s->ymm[m->reg], out, n * 4);
    s->mxcsr |= fl;
    return EMU_OK;
}

static EmuResult do_cvtps2ph(EmuState *s, const EmuMem *mem, const ModRM *m, uint64_t ea, int L, uint8_t imm)
{
    unsigned n = L ? 8 : 4;
    uint32_t f[8], fl = 0;
    uint16_t h[8];
    unsigned rc = (imm & 4) ? ((s->mxcsr >> 13) & 3u) : (imm & 3u);
    if (L && !s->have_ymm_hi) return EMU_UNSUPPORTED;
    memcpy(f, s->ymm[m->reg], n * 4);
    for (unsigned i = 0; i < n; i++) h[i] = emu_f32_to_f16(f[i], rc, &fl);
    if (m->is_reg) {
        memset(s->ymm[m->rm], 0, 32);
        memcpy(s->ymm[m->rm], h, n * 2);
    } else if (!mem->write(mem->user, ea, h, n * 2)) {
        return EMU_MEMFAULT;
    }
    s->mxcsr |= fl;
    return EMU_OK;
}

/* ------------------------------------------------------------------ */
/* BMI1                                                                */
/* ------------------------------------------------------------------ */

#define FLAGS_ARITH (EMU_FLAG_CF | EMU_FLAG_ZF | EMU_FLAG_SF | EMU_FLAG_OF)

static uint64_t width_mask(int w64) { return w64 ? ~0ull : 0xFFFFFFFFull; }

static void gpr_write(EmuState *s, unsigned r, uint64_t v, int w64)
{
    s->gpr[r] = w64 ? v : (v & 0xFFFFFFFFull);
}

static void set_flags(EmuState *s, uint64_t res, int w64, int cf, int zf_override)
{
    uint64_t mask = width_mask(w64);
    uint64_t sbit = w64 ? (1ull << 63) : (1ull << 31);
    res &= mask;
    s->eflags &= ~FLAGS_ARITH;
    if (cf) s->eflags |= EMU_FLAG_CF;
    if (zf_override < 0 ? res == 0 : zf_override) s->eflags |= EMU_FLAG_ZF;
    if (res & sbit) s->eflags |= EMU_FLAG_SF;
}

static EmuResult read_rm_gpr(EmuState *s, const EmuMem *mem, const ModRM *m, uint64_t ea, int w64, uint64_t *v)
{
    if (m->is_reg) { *v = s->gpr[m->rm] & width_mask(w64); return EMU_OK; }
    *v = 0;
    if (!mem->read(mem->user, ea, v, w64 ? 8 : 4)) return EMU_MEMFAULT;
    return EMU_OK;
}

static EmuResult do_bmi1(EmuState *s, const EmuMem *mem, unsigned op, const ModRM *m,
                         uint64_t ea, unsigned vvvv, int w64)
{
    uint64_t src, mask = width_mask(w64), res;
    EmuResult r = read_rm_gpr(s, mem, m, ea, w64, &src);
    if (r != EMU_OK) return r;

    if (op == 0xF2) {                                   /* ANDN: reg = ~vvvv & rm */
        res = (~s->gpr[vvvv] & src) & mask;
        gpr_write(s, m->reg, res, w64);
        set_flags(s, res, w64, 0, -1);
        return EMU_OK;
    }
    if (op == 0xF7) {                                   /* BEXTR: reg = (rm >> start) & mask(len) */
        uint64_t ctl = s->gpr[vvvv];
        unsigned start = ctl & 0xFF, len = (ctl >> 8) & 0xFF, bits = w64 ? 64 : 32;
        res = 0;
        if (start < bits) {
            res = src >> start;
            if (len < bits) res &= (1ull << len) - 1;
        }
        res &= mask;
        gpr_write(s, m->reg, res, w64);
        s->eflags &= ~(EMU_FLAG_CF | EMU_FLAG_OF | EMU_FLAG_ZF);
        if (res == 0) s->eflags |= EMU_FLAG_ZF;
        return EMU_OK;
    }
    /* op == 0xF3: grupo 17 */
    switch (m->digit) {
    case 1: res = (src & (src - 1)) & mask;  set_flags(s, res, w64, src == 0, -1); break; /* BLSR */
    case 2: res = (src ^ (src - 1)) & mask;  set_flags(s, res, w64, src == 0, 0);  break; /* BLSMSK */
    case 3: res = (src & (0 - src)) & mask;  set_flags(s, res, w64, src != 0, -1); break; /* BLSI */
    default: return EMU_UNSUPPORTED;
    }
    gpr_write(s, vvvv, res, w64);
    return EMU_OK;
}

/* ------------------------------------------------------------------ */
/* MOVBE                                                               */
/* ------------------------------------------------------------------ */

static EmuResult do_movbe(EmuState *s, const EmuMem *mem, int store, const ModRM *m,
                          uint64_t ea, unsigned size)
{
    uint8_t buf[8];
    if (m->is_reg) return EMU_UNSUPPORTED;               /* MOVBE so aceita memoria */
    if (!store) {
        if (!mem->read(mem->user, ea, buf, size)) return EMU_MEMFAULT;
        uint64_t v = 0;
        for (unsigned i = 0; i < size; i++) v = (v << 8) | buf[i];   /* big-endian -> valor */
        if (size == 2)      s->gpr[m->reg] = (s->gpr[m->reg] & ~0xFFFFull) | (v & 0xFFFF);
        else if (size == 4) s->gpr[m->reg] = v & 0xFFFFFFFFull;
        else                s->gpr[m->reg] = v;
    } else {
        uint64_t v = s->gpr[m->reg];
        for (unsigned i = 0; i < size; i++) buf[i] = (uint8_t)(v >> (8 * (size - 1 - i)));
        if (!mem->write(mem->user, ea, buf, size)) return EMU_MEMFAULT;
    }
    return EMU_OK;
}

/* ------------------------------------------------------------------ */
/* Despacho                                                            */
/* ------------------------------------------------------------------ */

EmuResult emu_step(EmuState *s, const EmuMem *mem, const uint8_t *code, size_t avail, unsigned *len_out)
{
    Cur c = { code, avail > 15 ? 15 : avail, 0 };
    uint8_t b;
    int has66 = 0;
    unsigned rex = 0;

    if (!peek8(&c, &b)) return EMU_UNSUPPORTED;
    if (b == 0x66) { has66 = 1; c.pos++; if (!peek8(&c, &b)) return EMU_UNSUPPORTED; }
    if ((b & 0xF0) == 0x40) { rex = b; c.pos++; if (!peek8(&c, &b)) return EMU_UNSUPPORTED; }
    c.pos++;                                              /* consome o byte `b` */

    ModRM m;
    EmuResult res;
    size_t len;

    if (b == 0xC4 || b == 0xC5) {                         /* VEX */
        if (has66 || rex) return EMU_UNSUPPORTED;
        uint8_t v1, v2 = 0, op;
        unsigned R, X = 0, B = 0, map = 1, W = 0, vvvv, L, pp;
        if (!get8(&c, &v1)) return EMU_UNSUPPORTED;
        if (b == 0xC5) {
            R = !((v1 >> 7) & 1); vvvv = (~(v1 >> 3)) & 0xF; L = (v1 >> 2) & 1; pp = v1 & 3;
        } else {
            if (!get8(&c, &v2)) return EMU_UNSUPPORTED;
            R = !((v1 >> 7) & 1); X = !((v1 >> 6) & 1); B = !((v1 >> 5) & 1); map = v1 & 0x1F;
            W = (v2 >> 7) & 1; vvvv = (~(v2 >> 3)) & 0xF; L = (v2 >> 2) & 1; pp = v2 & 3;
        }
        if (!get8(&c, &op)) return EMU_UNSUPPORTED;

        if (map == 2 && pp == 1 && op == 0x13 && !W && vvvv == 0) {            /* VCVTPH2PS */
            if (!decode_modrm(&c, s, R, X, B, &m)) return EMU_UNSUPPORTED;
            len = c.pos;
            res = do_cvtph2ps(s, mem, &m, operand_ea(&m, s, len), (int)L);
        } else if (map == 3 && pp == 1 && op == 0x1D && !W && vvvv == 0) {     /* VCVTPS2PH */
            uint8_t imm;
            if (!decode_modrm(&c, s, R, X, B, &m) || !get8(&c, &imm)) return EMU_UNSUPPORTED;
            len = c.pos;
            res = do_cvtps2ph(s, mem, &m, operand_ea(&m, s, len), (int)L, imm);
        } else if (map == 2 && pp == 0 && !L && (op == 0xF2 || op == 0xF7 || op == 0xF3)) { /* BMI1 */
            if (!decode_modrm(&c, s, R, X, B, &m)) return EMU_UNSUPPORTED;
            if (op == 0xF3 && (m.digit < 1 || m.digit > 3)) return EMU_UNSUPPORTED;
            len = c.pos;
            res = do_bmi1(s, mem, op, &m, operand_ea(&m, s, len), vvvv, (int)W);
        } else {
            return EMU_UNSUPPORTED;
        }
    } else if (b == 0x0F) {                                /* MOVBE: 0F 38 F0/F1 */
        uint8_t b2, op;
        if (!get8(&c, &b2) || b2 != 0x38 || !get8(&c, &op) || (op != 0xF0 && op != 0xF1))
            return EMU_UNSUPPORTED;
        if (!decode_modrm(&c, s, (rex >> 2) & 1, (rex >> 1) & 1, rex & 1, &m)) return EMU_UNSUPPORTED;
        len = c.pos;
        unsigned size = (rex & 8) ? 8 : (has66 ? 2 : 4);
        res = do_movbe(s, mem, op == 0xF1, &m, operand_ea(&m, s, len), size);
    } else {
        return EMU_UNSUPPORTED;
    }

    if (res == EMU_OK) {
        s->rip += len;
        if (len_out) *len_out = (unsigned)len;
    }
    return res;
}
