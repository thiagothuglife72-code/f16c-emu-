/*
 * emu_core: emulacao por software de instrucoes que o Sandy Bridge (i7-2600K) nao tem.
 *
 * Cobre:
 *   F16C : VCVTPH2PS, VCVTPS2PH (formas 128 e 256 bits)
 *   BMI1 : ANDN, BEXTR, BLSI, BLSMSK, BLSR
 *   MOVBE: MOVBE (carga e armazenamento, 16/32/64 bits)
 *
 * O nucleo e portavel (nao usa API do Windows) para poder ser testado em qualquer SO.
 * LZCNT/TZCNT NAO sao cobertos: em CPUs antigas elas executam como BSR/BSF sem gerar
 * excecao, entao nao existe como captura-las por #UD.
 */
#ifndef EMU_CORE_H
#define EMU_CORE_H

#include <stdint.h>
#include <stddef.h>

#define EMU_FLAG_CF 0x0001u
#define EMU_FLAG_PF 0x0004u
#define EMU_FLAG_AF 0x0010u
#define EMU_FLAG_ZF 0x0040u
#define EMU_FLAG_SF 0x0080u
#define EMU_FLAG_OF 0x0800u

#define EMU_MXCSR_IE 0x01u
#define EMU_MXCSR_DE 0x02u
#define EMU_MXCSR_ZE 0x04u
#define EMU_MXCSR_OE 0x08u
#define EMU_MXCSR_UE 0x10u
#define EMU_MXCSR_PE 0x20u

typedef struct EmuState {
    uint64_t gpr[16];      /* ordem de codificacao: rax rcx rdx rbx rsp rbp rsi rdi r8..r15 */
    uint64_t rip;
    uint32_t eflags;
    uint32_t mxcsr;
    uint8_t  ymm[16][32];  /* bytes 0..15 = XMM, bytes 16..31 = metade alta do YMM */
    int      have_ymm_hi;  /* 0 = metade alta indisponivel: formas de 256 bits nao sao emuladas */
} EmuState;

typedef struct EmuMem {
    int (*read)(void *user, uint64_t addr, void *dst, size_t n);        /* 1 = ok */
    int (*write)(void *user, uint64_t addr, const void *src, size_t n); /* 1 = ok */
    void *user;
} EmuMem;

typedef enum {
    EMU_OK = 0,          /* instrucao emulada; s->rip ja avancou */
    EMU_UNSUPPORTED = 1, /* nao e uma instrucao que este emulador conhece */
    EMU_MEMFAULT = 2     /* acesso a memoria falhou; estado nao foi alterado */
} EmuResult;

/* Emula a instrucao em `code` (bytes em s->rip, `avail` bytes disponiveis, ate 15). */
EmuResult emu_step(EmuState *s, const EmuMem *mem, const uint8_t *code, size_t avail, unsigned *len);

/* Conversoes expostas para teste. rc: 0=par mais proximo, 1=-inf, 2=+inf, 3=truncar. */
uint16_t emu_f32_to_f16(uint32_t f, unsigned rc, uint32_t *mxcsr_flags);
uint32_t emu_f16_to_f32(uint16_t h, uint32_t *mxcsr_flags);

#endif
