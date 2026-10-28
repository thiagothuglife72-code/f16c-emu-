/*
 * f16c_emu para Windows x64.
 *
 * Compila como "dinput8.dll": colocada na mesma pasta do executavel, o Windows a carrega no lugar da
 * original; ela instala um Vectored Exception Handler que captura #UD (0xC000001D), emula a
 * instrucao com o emu_core e continua a execucao. As chamadas reais do DirectInput sao repassadas
 * para a dinput8.dll do sistema.
 *
 * Gera "f16c_emu.log" ao lado da DLL com: primeira emulacao, contagem periodica e as instrucoes
 * que o emulador NAO conhece (bytes e RIP), para sabermos o que implementar em seguida.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "emu_core.h"

#define XSTATE_FEATURE_AVX 2

typedef PVOID (WINAPI *LocateXStateFeature_t)(PCONTEXT, DWORD, PDWORD);

static LocateXStateFeature_t g_locate;
static WCHAR g_log_path[MAX_PATH];
static volatile LONG g_emulated;
static volatile LONG g_unsup_count;
static uint64_t g_unsup_seen[256];

/* ---------------------------- log ---------------------------- */

static void log_line(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof line - 3) n = (int)sizeof line - 3;
    line[n++] = '\r'; line[n++] = '\n';
    HANDLE h = CreateFileW(g_log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(h, line, (DWORD)n, &w, NULL);
    CloseHandle(h);
}

static void log_unsupported(uint64_t rip, const uint8_t *code, size_t n)
{
    LONG idx = g_unsup_count;
    for (LONG i = 0; i < idx && i < 256; i++)
        if (g_unsup_seen[i] == rip) return;                 /* ja registrado */
    idx = InterlockedIncrement(&g_unsup_count) - 1;
    if (idx >= 256) return;
    g_unsup_seen[idx] = rip;
    char hex[64];
    size_t k = 0;
    for (size_t i = 0; i < n && i < 15; i++) k += (size_t)snprintf(hex + k, sizeof hex - k, "%02X ", code[i]);
    log_line("SEM SUPORTE rip=%016llX bytes=%s", (unsigned long long)rip, hex);
}

/* ------------------------- acesso a memoria ------------------------- */

static int mem_read(void *u, uint64_t a, void *d, size_t n)
{
    SIZE_T got = 0;
    (void)u;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)a, d, n, &got) && got == n;
}

static int mem_write(void *u, uint64_t a, const void *s, size_t n)
{
    SIZE_T got = 0;
    (void)u;
    return WriteProcessMemory(GetCurrentProcess(), (LPVOID)(uintptr_t)a, s, n, &got) && got == n;
}

/* ---------------------- CONTEXT <-> EmuState ---------------------- */

static void load_state(EmuState *s, const CONTEXT *c, const M128A *hi)
{
    s->gpr[0] = c->Rax;  s->gpr[1] = c->Rcx;  s->gpr[2] = c->Rdx;  s->gpr[3] = c->Rbx;
    s->gpr[4] = c->Rsp;  s->gpr[5] = c->Rbp;  s->gpr[6] = c->Rsi;  s->gpr[7] = c->Rdi;
    s->gpr[8] = c->R8;   s->gpr[9] = c->R9;   s->gpr[10] = c->R10; s->gpr[11] = c->R11;
    s->gpr[12] = c->R12; s->gpr[13] = c->R13; s->gpr[14] = c->R14; s->gpr[15] = c->R15;
    s->rip = c->Rip;
    s->eflags = c->EFlags;
    s->mxcsr = c->MxCsr;
    for (int i = 0; i < 16; i++) {
        memcpy(s->ymm[i], &c->FltSave.XmmRegisters[i], 16);
        if (hi) memcpy(s->ymm[i] + 16, &hi[i], 16);
        else memset(s->ymm[i] + 16, 0, 16);
    }
    s->have_ymm_hi = hi != NULL;
}

static void store_state(CONTEXT *c, const EmuState *s, M128A *hi)
{
    c->Rax = s->gpr[0];  c->Rcx = s->gpr[1];  c->Rdx = s->gpr[2];  c->Rbx = s->gpr[3];
    c->Rbp = s->gpr[5];  c->Rsi = s->gpr[6];  c->Rdi = s->gpr[7];
    c->R8 = s->gpr[8];   c->R9 = s->gpr[9];   c->R10 = s->gpr[10]; c->R11 = s->gpr[11];
    c->R12 = s->gpr[12]; c->R13 = s->gpr[13]; c->R14 = s->gpr[14]; c->R15 = s->gpr[15];
    c->Rip = s->rip;
    c->EFlags = s->eflags;
    c->MxCsr = s->mxcsr;
    c->FltSave.MxCsr = s->mxcsr;
    for (int i = 0; i < 16; i++) {
        memcpy(&c->FltSave.XmmRegisters[i], s->ymm[i], 16);
        if (hi) memcpy(&hi[i], s->ymm[i] + 16, 16);
    }
}

/* ------------------------------ handler ------------------------------ */

static LONG CALLBACK veh(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;

    PCONTEXT c = ep->ContextRecord;
    uint8_t code[16];
    size_t n = 15;
    while (n && !mem_read(NULL, c->Rip, code, n)) n--;       /* instrucao pode terminar no fim de uma pagina */
    if (!n) return EXCEPTION_CONTINUE_SEARCH;

    M128A *hi = NULL;
    DWORD hilen = 0;
    if (g_locate) hi = (M128A *)g_locate(c, XSTATE_FEATURE_AVX, &hilen);

    EmuState st;
    EmuMem m = { mem_read, mem_write, NULL };
    unsigned len = 0;
    load_state(&st, c, hi);
    EmuResult r = emu_step(&st, &m, code, n, &len);

    if (r == EMU_OK) {
        store_state(c, &st, hi);
        LONG cnt = InterlockedIncrement(&g_emulated);
        if (cnt == 1) log_line("primeira instrucao emulada: rip=%016llX", (unsigned long long)(st.rip - len));
        else if ((cnt & 0xFFFFF) == 0) log_line("%ld instrucoes emuladas ate agora", cnt);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (r == EMU_UNSUPPORTED) log_unsupported(c->Rip, code, n);
    return EXCEPTION_CONTINUE_SEARCH;                        /* deixa o jogo/Windows tratar */
}

/* ------------------------- proxy do dinput8 ------------------------- */

static HMODULE g_real;

static FARPROC real_proc(const char *name)
{
    if (!g_real) {
        WCHAR p[MAX_PATH];
        UINT n = GetSystemDirectoryW(p, MAX_PATH);
        if (!n || n + 14 >= MAX_PATH) return NULL;
        wcscat(p, L"\\dinput8.dll");
        g_real = LoadLibraryW(p);
    }
    return g_real ? GetProcAddress(g_real, name) : NULL;
}

HRESULT WINAPI proxy_DirectInput8Create(HINSTANCE hinst, DWORD ver, REFIID riid, LPVOID *out, LPUNKNOWN outer)
{
    typedef HRESULT (WINAPI *fn_t)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
    fn_t f = (fn_t)real_proc("DirectInput8Create");
    return f ? f(hinst, ver, riid, out, outer) : E_FAIL;
}

HRESULT WINAPI proxy_DllCanUnloadNow(void)
{
    typedef HRESULT (WINAPI *fn_t)(void);
    fn_t f = (fn_t)real_proc("DllCanUnloadNow");
    return f ? f() : S_FALSE;
}

HRESULT WINAPI proxy_DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID *ppv)
{
    typedef HRESULT (WINAPI *fn_t)(REFCLSID, REFIID, LPVOID *);
    fn_t f = (fn_t)real_proc("DllGetClassObject");
    return f ? f(clsid, riid, ppv) : E_FAIL;
}

HRESULT WINAPI proxy_DllRegisterServer(void)
{
    typedef HRESULT (WINAPI *fn_t)(void);
    fn_t f = (fn_t)real_proc("DllRegisterServer");
    return f ? f() : E_FAIL;
}

HRESULT WINAPI proxy_DllUnregisterServer(void)
{
    typedef HRESULT (WINAPI *fn_t)(void);
    fn_t f = (fn_t)real_proc("DllUnregisterServer");
    return f ? f() : E_FAIL;
}

/* ------------------------------ DllMain ------------------------------ */

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        GetModuleFileNameW(h, g_log_path, MAX_PATH);
        WCHAR *slash = wcsrchr(g_log_path, L'\\');
        if (slash) wcscpy(slash + 1, L"f16c_emu.log");
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        if (k32) g_locate = (LocateXStateFeature_t)GetProcAddress(k32, "LocateXStateFeature");
        AddVectoredExceptionHandler(1, veh);
        log_line("f16c_emu carregado (XSTATE %s)", g_locate ? "disponivel" : "indisponivel");
    }
    return TRUE;
}
