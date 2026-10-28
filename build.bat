@echo off
rem Compila dinput8.dll (x64). Usa MinGW-w64 (gcc) se achar; senao tenta o MSVC (cl).
cd /d "%~dp0"
where gcc >nul 2>nul
if %errorlevel%==0 (
    gcc -O2 -shared -o dinput8.dll win_glue.c emu_core.c dinput8.def -static-libgcc
    goto fim
)
where cl >nul 2>nul
if %errorlevel%==0 (
    cl /nologo /O2 /LD /Fe:dinput8.dll win_glue.c emu_core.c /link /DEF:dinput8.def
    goto fim
)
echo Nenhum compilador encontrado. Instale o MSYS2 (gcc) ou abra o "x64 Native Tools Command Prompt".
exit /b 1
:fim
if exist dinput8.dll (echo OK: dinput8.dll gerado. Copie para a pasta do azimuth.exe.) else (echo FALHOU: veja as mensagens acima.)
