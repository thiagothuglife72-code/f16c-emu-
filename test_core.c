/*
 * Teste diferencial do emu_core: executa cada instrucao NATIVAMENTE (exige CPU com
 * F16C, BMI1 e MOVBE) e depois emula os mesmos bytes, comparando registradores,
 * memoria, flags e MXCSR. Compilar apenas em Linux/x86-64:
 *   gcc -O1 -mno-red-zone -mavx2 -mf16c -mbmi -mmovbe test_core.c emu_core.c -o test_core
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include "emu_core.h"

typedef struct {
    uint64_t gpr_in[16];      /* 0   */
    uint64_t gpr_out[16];     /* 128 */
    uint64_t flags_out;       /* 256 */
    uint32_t csr_in, csr_out; /* 264, 268 */
    uint64_t code_s, code_e;  /* 272, 280 */
    uint8_t  ymm_in[16][32];  /* 288 */
    uint8_t  ymm_out[16][32]; /* 800 */
} Aux;
_Static_assert(offsetof(Aux, gpr_out) == 128 && offsetof(Aux, flags_out) == 256 &&
               offsetof(Aux, csr_in) == 264 && offsetof(Aux, code_s) == 272 &&
               offsetof(Aux, ymm_in) == 288 && offsetof(Aux, ymm_out) == 800, "layout");

static uint8_t buf[512] __attribute__((aligned(64)));
static uint8_t gmem[64] __attribute__((used, aligned(16)));

#define LY(i) "vmovups " #i "*32+288(%%rsi), %%ymm" #i "\n\t"
#define SY(i) "vmovups %%ymm" #i ", " #i "*32+800(%%rsi)\n\t"
#define ALL16(M) M(0) M(1) M(2) M(3) M(4) M(5) M(6) M(7) M(8) M(9) M(10) M(11) M(12) M(13) M(14) M(15)
#define LG(n, i) "mov " #i "*8(%%rsi), %%" #n "\n\t"
#define SG(n, i) "mov %%" #n ", 128+" #i "*8(%%rsi)\n\t"
#define LOADGPR LG(rax,0) LG(rcx,1) LG(rdx,2) LG(rdi,7) LG(r8,8) LG(r9,9) LG(r12,12) LG(r13,13) LG(r14,14) LG(r15,15)
#define STOREGPR SG(rax,0) SG(rcx,1) SG(rdx,2) SG(rdi,7) SG(r8,8) SG(r9,9) SG(r12,12) SG(r13,13) SG(r14,14) SG(r15,15)

#define XMMCLOB "xmm0","xmm1","xmm2","xmm3","xmm4","xmm5","xmm6","xmm7","xmm8","xmm9","xmm10","xmm11","xmm12","xmm13","xmm14","xmm15"

enum { K_HALF, K_FLOAT, K_GPR };

/* id, instrucao (AT&T), tipo de dado, mascara de flags a comparar (0 = nao compara) */
#define CF_ZF_SF_OF (EMU_FLAG_CF | EMU_FLAG_ZF | EMU_FLAG_SF | EMU_FLAG_OF)
#define CF_ZF_OF    (EMU_FLAG_CF | EMU_FLAG_ZF | EMU_FLAG_OF)
#define CASES(X) \
 X(f01, "vcvtph2ps %%xmm5, %%ymm3", K_HALF, 0) \
 X(f02, "vcvtph2ps %%xmm5, %%xmm3", K_HALF, 0) \
 X(f03, "vcvtph2ps %%xmm13, %%ymm11", K_HALF, 0) \
 X(f04, "vcvtph2ps 16(%%rax), %%ymm2", K_HALF, 0) \
 X(f05, "vcvtph2ps (%%rax,%%rcx,4), %%xmm9", K_HALF, 0) \
 X(f06, "vcvtph2ps 0x100(%%r12), %%ymm1", K_HALF, 0) \
 X(f07, "vcvtph2ps 8(%%r13), %%xmm1", K_HALF, 0) \
 X(f08, "vcvtph2ps gmem(%%rip), %%ymm6", K_HALF, 0) \
 X(f09, "vcvtph2ps (%%r13), %%ymm4", K_HALF, 0) \
 X(f10, "vcvtps2ph $0, %%ymm2, %%xmm4", K_FLOAT, 0) \
 X(f11, "vcvtps2ph $1, %%ymm2, %%xmm4", K_FLOAT, 0) \
 X(f12, "vcvtps2ph $2, %%ymm2, %%xmm4", K_FLOAT, 0) \
 X(f13, "vcvtps2ph $3, %%ymm2, %%xmm4", K_FLOAT, 0) \
 X(f14, "vcvtps2ph $4, %%ymm2, %%xmm4", K_FLOAT, 0) \
 X(f15, "vcvtps2ph $0, %%xmm2, %%xmm4", K_FLOAT, 0) \
 X(f16, "vcvtps2ph $3, %%ymm10, %%xmm12", K_FLOAT, 0) \
 X(f17, "vcvtps2ph $4, %%xmm9, %%xmm14", K_FLOAT, 0) \
 X(f18, "vcvtps2ph $0, %%ymm2, 16(%%rax)", K_FLOAT, 0) \
 X(f19, "vcvtps2ph $1, %%xmm2, (%%rax,%%rcx,8)", K_FLOAT, 0) \
 X(f20, "vcvtps2ph $4, %%ymm9, gmem(%%rip)", K_FLOAT, 0) \
 X(f21, "vcvtps2ph $2, %%xmm2, 0x7f(%%r12)", K_FLOAT, 0) \
 X(f22, "vcvtps2ph $4, %%ymm3, %%xmm3", K_FLOAT, 0) \
 X(b01, "andn %%rdx, %%rdi, %%r8", K_GPR, CF_ZF_SF_OF) \
 X(b02, "andn %%edx, %%edi, %%r9d", K_GPR, CF_ZF_SF_OF) \
 X(b03, "andn (%%rax), %%r14, %%r15", K_GPR, CF_ZF_SF_OF) \
 X(b04, "andn 8(%%r13,%%rcx,4), %%edi, %%edx", K_GPR, CF_ZF_SF_OF) \
 X(b05, "bextr %%rdi, %%rdx, %%r8", K_GPR, CF_ZF_OF) \
 X(b06, "bextr %%edi, %%edx, %%r9d", K_GPR, CF_ZF_OF) \
 X(b07, "bextr %%r14, (%%rax), %%r15", K_GPR, CF_ZF_OF) \
 X(b08, "blsi %%rdx, %%r8", K_GPR, CF_ZF_SF_OF) \
 X(b09, "blsi %%edx, %%r9d", K_GPR, CF_ZF_SF_OF) \
 X(b10, "blsi (%%rax), %%r14", K_GPR, CF_ZF_SF_OF) \
 X(b11, "blsmsk %%rdx, %%r8", K_GPR, CF_ZF_SF_OF) \
 X(b12, "blsmsk %%edi, %%r15d", K_GPR, CF_ZF_SF_OF) \
 X(b13, "blsmsk 16(%%r12), %%r8d", K_GPR, CF_ZF_SF_OF) \
 X(b14, "blsr %%rdx, %%r9", K_GPR, CF_ZF_SF_OF) \
 X(b15, "blsr %%r14d, %%edi", K_GPR, CF_ZF_SF_OF) \
 X(b16, "blsr (%%rax,%%rcx,2), %%r8", K_GPR, CF_ZF_SF_OF) \
 X(m01, "movbe (%%rax), %%edx", K_GPR, 0) \
 X(m02, "movbe 8(%%rax), %%rdi", K_GPR, 0) \
 X(m03, "movbe (%%rax,%%rcx,2), %%r8w", K_GPR, 0) \
 X(m04, "movbe %%r9d, 4(%%r12)", K_GPR, 0) \
 X(m05, "movbe %%rdx, 8(%%r13)", K_GPR, 0) \
 X(m06, "movbe %%di, (%%rax)", K_GPR, 0) \
 X(m07, "movbe gmem(%%rip), %%r14", K_GPR, 0) \
 X(m08, "movbe %%r15w, gmem(%%rip)", K_GPR, 0)

#define DEFCASE(id, insn, kind, fm) \
static void id(Aux *a) { \
    __asm__ volatile( \
        ALL16(LY) LOADGPR \
        "vldmxcsr 264(%%rsi)\n\t" \
        "lea 1f(%%rip), %%r10\n\t" \
        "lea 2f(%%rip), %%r11\n\t" \
        "1: " insn "\n\t" \
        "2:\n\t" \
        "vstmxcsr 268(%%rsi)\n\t" \
        "pushfq\n\t" "popq 256(%%rsi)\n\t" \
        STOREGPR \
        "mov %%r10, 272(%%rsi)\n\t" "mov %%r11, 280(%%rsi)\n\t" \
        ALL16(SY) \
        : : "S"(a) \
        : "memory", "cc", "rax", "rcx", "rdx", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", XMMCLOB); \
}
CASES(DEFCASE)

typedef struct { const char *insn; void (*fn)(Aux *); int kind; uint32_t fmask; } Case;
#define ENTRY(id, insn, kind, fm) { insn, id, kind, fm },
static const Case cases[] = { CASES(ENTRY) };

/* ---------------- gerador de dados ---------------- */
static uint64_t rs[2] = { 0x9E3779B97F4A7C15ull, 0xD1B54A32D192ED03ull };
static uint64_t rnd(void)
{
    uint64_t s1 = rs[0], s0 = rs[1];
    rs[0] = s0; s1 ^= s1 << 23;
    rs[1] = s1 ^ s0 ^ (s1 >> 18) ^ (s0 >> 5);
    return rs[1] + s0;
}

static uint32_t gen_float(void)
{
    uint64_t r = rnd();
    unsigned k = r & 31;
    uint32_t sign = (uint32_t)(r >> 8) & 1;
    if (k == 0) return (sign << 31) | 0x7F800000u | (uint32_t)(rnd() & 0x7FFFFF);
    if (k == 1) return sign << 31;
    if (k == 2) return (sign << 31) | (uint32_t)(rnd() & 0x7FFFFF);
    if (k == 3) {
        static const uint32_t b[] = { 0x477FE000, 0x477FF000, 0x47800000, 0x38800000, 0x387FC000,
                                      0x33800000, 0x33000000, 0x33000001, 0x477FEFFF, 0x477FF001 };
        uint32_t v = b[(r >> 16) % 10];
        v += (uint32_t)((int)((r >> 32) % 5) - 2);
        return (sign << 31) | v;
    }
    uint32_t e = 95 + (uint32_t)((r >> 12) % 60);
    return (sign << 31) | (e << 23) | (uint32_t)(rnd() & 0x7FFFFF);
}

static uint64_t gen_val(void)
{
    uint64_t r = rnd();
    switch (r & 15) {
    case 0: return 0;
    case 1: return 1;
    case 2: return ~0ull;
    case 3: return 1ull << (rnd() & 63);
    case 4: return 0x80000000ull;
    case 5: return 0xFFFFFFFFull;
    case 6: return (1ull << (rnd() & 63)) | (1ull << (rnd() & 63));
    case 7: return 0x8000000000000000ull;
    default: return rnd();
    }
}

static void fill_inputs(Aux *a, int kind, uint8_t *init_buf, uint8_t *init_g)
{
    memset(a, 0, sizeof *a);
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 8; j++) {
            if (kind == K_FLOAT) { uint32_t f = gen_float(); memcpy(&a->ymm_in[i][j * 4], &f, 4); }
            else { uint32_t v = (uint32_t)rnd(); memcpy(&a->ymm_in[i][j * 4], &v, 4); }
        }
    for (unsigned i = 0; i < 512; i++) init_buf[i] = (uint8_t)rnd();
    for (unsigned i = 0; i < 64; i++) init_g[i] = (uint8_t)rnd();
    if (kind == K_FLOAT || kind == K_HALF) {
        for (unsigned i = 0; i < 512; i += 4) { uint32_t f = gen_float(); memcpy(init_buf + i, &f, 4); }
    }
    for (int i = 0; i < 16; i++) a->gpr_in[i] = gen_val();
    uint64_t ctl = (rnd() & ~0xFFFFull) | (rnd() % 72) | ((rnd() % 72) << 8);
    if (rnd() & 1) a->gpr_in[7] = ctl;             /* rdi serve de controle do BEXTR */
    if (rnd() & 1) a->gpr_in[14] = ctl;            /* r14 tambem */
    a->gpr_in[0]  = (uint64_t)(uintptr_t)(buf + 32);   /* rax */
    a->gpr_in[1]  = 3;                                  /* rcx */
    a->gpr_in[12] = (uint64_t)(uintptr_t)(buf + 64);   /* r12 */
    a->gpr_in[13] = (uint64_t)(uintptr_t)(buf + 8);    /* r13 */
    a->csr_in = 0x1F80u | ((uint32_t)(rnd() & 3) << 13);
}

static int mem_rd(void *u, uint64_t addr, void *d, size_t n) { (void)u; memcpy(d, (void *)(uintptr_t)addr, n); return 1; }
static int mem_wr(void *u, uint64_t addr, const void *s, size_t n) { (void)u; memcpy((void *)(uintptr_t)addr, s, n); return 1; }

static const int GPR_CHECK[] = { 0, 1, 2, 7, 8, 9, 12, 13, 14, 15 };

static int compare_once(const Case *c, Aux *a, const uint8_t *init_buf, const uint8_t *init_g, char *why, size_t whylen)
{
    memcpy(buf, init_buf, 512);
    memcpy(gmem, init_g, 64);
    c->fn(a);                                  /* execucao nativa */
    uint8_t nat_buf[512], nat_g[64];
    memcpy(nat_buf, buf, 512);
    memcpy(nat_g, gmem, 64);
    memcpy(buf, init_buf, 512);
    memcpy(gmem, init_g, 64);

    EmuState st;
    memset(&st, 0, sizeof st);
    for (int i = 0; i < 16; i++) st.gpr[i] = a->gpr_in[i];
    st.rip = a->code_s;
    st.mxcsr = a->csr_in;
    st.have_ymm_hi = 1;
    memcpy(st.ymm, a->ymm_in, sizeof st.ymm);
    EmuMem m = { mem_rd, mem_wr, NULL };
    unsigned len = 0;
    size_t n = (size_t)(a->code_e - a->code_s);
    EmuResult r = emu_step(&st, &m, (const uint8_t *)(uintptr_t)a->code_s, n, &len);
    if (r != EMU_OK) { snprintf(why, whylen, "emu_step retornou %d", r); return 0; }
    if (len != n) { snprintf(why, whylen, "tamanho %u, esperado %zu", len, n); return 0; }
    if (st.rip != a->code_e) { snprintf(why, whylen, "rip final errado"); return 0; }
    for (int i = 0; i < 16; i++)
        if (memcmp(st.ymm[i], a->ymm_out[i], 32)) { snprintf(why, whylen, "ymm%d difere", i); return 0; }
    for (unsigned k = 0; k < sizeof GPR_CHECK / sizeof *GPR_CHECK; k++) {
        int i = GPR_CHECK[k];
        if (st.gpr[i] != a->gpr_out[i]) {
            snprintf(why, whylen, "gpr[%d] emu=%016llx nativo=%016llx", i,
                     (unsigned long long)st.gpr[i], (unsigned long long)a->gpr_out[i]);
            return 0;
        }
    }
    if (memcmp(buf, nat_buf, 512)) { snprintf(why, whylen, "memoria (buf) difere"); return 0; }
    if (memcmp(gmem, nat_g, 64)) { snprintf(why, whylen, "memoria (gmem) difere"); return 0; }
    if (c->fmask && ((st.eflags ^ (uint32_t)a->flags_out) & c->fmask)) {
        snprintf(why, whylen, "flags emu=%03x nativo=%03x (mascara %03x)", st.eflags & c->fmask,
                 (uint32_t)a->flags_out & c->fmask, c->fmask);
        return 0;
    }
    if (c->kind != K_GPR && ((st.mxcsr ^ a->csr_out) & 0x39u)) {
        snprintf(why, whylen, "mxcsr emu=%02x nativo=%02x", st.mxcsr & 0x39, a->csr_out & 0x39);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    long iters = argc > 1 ? atol(argv[1]) : 100000;
    int failures = 0;
    uint8_t ib[512], ig[64];
    char why[160];
    Aux a;

    for (size_t ci = 0; ci < sizeof cases / sizeof *cases; ci++) {
        const Case *c = &cases[ci];
        long bad = 0;
        for (long it = 0; it < iters; it++) {
            fill_inputs(&a, c->kind, ib, ig);
            if (!compare_once(c, &a, ib, ig, why, sizeof why)) {
                if (bad < 3) printf("  FALHA [%s] iter %ld: %s\n", c->insn, it, why);
                bad++;
            }
        }
        printf("%-48s %s (%ld falhas)\n", c->insn, bad ? "FALHOU" : "ok", bad);
        failures += bad != 0;
    }

    /* varredura exaustiva dos 65536 valores half em VCVTPH2PS */
    {
        long bad = 0;
        for (uint32_t base = 0; base < 65536; base += 8) {
            fill_inputs(&a, K_HALF, ib, ig);
            for (int j = 0; j < 8; j++) { uint16_t h = (uint16_t)(base + j); memcpy(&a.ymm_in[5][j * 2], &h, 2); }
            if (!compare_once(&cases[0], &a, ib, ig, why, sizeof why)) { if (bad < 3) printf("  FALHA exaustiva base %u: %s\n", base, why); bad++; }
        }
        printf("%-48s %s (%ld falhas)\n", "vcvtph2ps: todos os 65536 halves", bad ? "FALHOU" : "ok", bad);
        failures += bad != 0;
    }

    /* instrucoes que NAO devem ser tratadas */
    {
        static const uint8_t vpaddd[] = { 0xC5, 0xED, 0xFE, 0xD9 };   /* vpaddd ymm3,ymm2,ymm1 (AVX2) */
        static const uint8_t lzcnt[]  = { 0xF3, 0x48, 0x0F, 0xBD, 0xC1 };
        static const uint8_t movbe_reg[] = { 0x0F, 0x38, 0xF0, 0xC1 }; /* forma invalida */
        EmuState st; EmuMem m = { mem_rd, mem_wr, NULL }; unsigned len;
        memset(&st, 0, sizeof st); st.have_ymm_hi = 1;
        int ok = emu_step(&st, &m, vpaddd, sizeof vpaddd, &len) == EMU_UNSUPPORTED &&
                 emu_step(&st, &m, lzcnt, sizeof lzcnt, &len) == EMU_UNSUPPORTED &&
                 emu_step(&st, &m, movbe_reg, sizeof movbe_reg, &len) == EMU_UNSUPPORTED;
        printf("%-48s %s\n", "instrucoes nao suportadas sao recusadas", ok ? "ok" : "FALHOU");
        failures += !ok;
    }

    printf(failures ? "\nRESULTADO: %d grupo(s) com falha\n" : "\nRESULTADO: tudo ok\n", failures);
    return failures != 0;
}
