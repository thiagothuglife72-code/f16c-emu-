# f16c_emu: emulador de F16C, BMI1 e MOVBE para o i7-2600K

Objetivo: permitir que o port Azimuth do Forza Horizon 2 rode em CPUs Sandy Bridge, que não têm
essas instruções. O crash analisado no dump foi em `vcvtps2ph xmm0, xmm0, 0` (F16C), não em AVX2.

## O que está coberto

| Conjunto | Instruções |
|---|---|
| F16C | `VCVTPH2PS`, `VCVTPS2PH` (formas de 128 e 256 bits, registrador e memória) |
| BMI1 | `ANDN`, `BEXTR`, `BLSI`, `BLSMSK`, `BLSR` |
| MOVBE | `MOVBE` (16/32/64 bits, carga e armazenamento) |

**Não coberto:**
- `LZCNT` e `TZCNT`: em CPUs antigas elas executam como `BSR`/`BSF` **sem gerar exceção**, então
  um hook de `#UD` nunca as vê (e o resultado fica errado quando a entrada é zero).
- AVX2, FMA3 e BMI2: ainda não implementados. A estrutura permite adicionar; o log (abaixo) mostra
  o que de fato aparece.

## Estado de validação

- **`emu_core.c` (o emulador): testado.** `test_core.c` executa cada instrução nativamente numa CPU
  real e depois emula os mesmos bytes, comparando registradores, memória, flags e MXCSR. 47 formas de
  codificação (SIB, disp8/32, RIP-relativo, REX/VEX estendidos), 300 mil entradas aleatórias cada,
  mais os 65536 valores half de `VCVTPH2PS`: zero divergências.
- **`win_glue.c` (a parte do Windows): NÃO foi compilado nem executado.** Não tenho um compilador
  Windows aqui. A lógica é curta e usa só APIs padrão, mas é a parte mais provável de ter um erro de
  compilação ou de comportamento. Se algo falhar, me mande a mensagem.

## Compilar (Windows x64)

MinGW-w64:

    x86_64-w64-mingw32-gcc -O2 -shared -o dinput8.dll win_glue.c emu_core.c dinput8.def -static-libgcc

MSVC (Prompt de Comando do desenvolvedor x64):

    cl /O2 /LD /Fe:dinput8.dll win_glue.c emu_core.c /link /DEF:dinput8.def

## Compilar sem instalar nada (GitHub Actions)

1. Crie um repositório no GitHub e envie todos os arquivos desta pasta.
2. Crie o arquivo `.github/workflows/build.yml` com o conteúdo do `build.yml` incluído aqui.
3. Aba **Actions**, rode o fluxo "build" (ou ele roda sozinho no envio). Ao terminar, baixe o artefato
   `dinput8-dll`: é o `dinput8.dll` pronto, 64 bits.

Ou, com um compilador instalado, dê dois cliques no `build.bat`.

## Usar

1. Copie o `dinput8.dll` gerado para a **mesma pasta do `azimuth.exe`**.
2. Abra o jogo normalmente.
3. Veja o arquivo `f16c_emu.log` (criado ao lado da DLL): ele registra a primeira instrução emulada,
   a contagem a cada ~1 milhão e **as instruções que o emulador não conhece**, com os bytes.

Se o log não aparecer, o Azimuth provavelmente não carrega `dinput8.dll` da pasta do executável;
nesse caso é preciso injetar a DLL de outra forma (me avise).

## Limitações

- **Desempenho:** cada instrução emulada passa por uma exceção do Windows (alguns microssegundos).
  Se o F16C estiver num laço quente (por exemplo, a cada quadro), o jogo pode ficar lento. O log com
  a contagem mostra se isso é um problema; a solução seguinte seria trocar os pontos quentes por um
  desvio para código próprio.
- Exceções de ponto flutuante desmascaradas não são disparadas (só as flags do MXCSR são atualizadas),
  e `MXCSR.DAZ/FTZ` são ignorados.
- Se `LocateXStateFeature` não achar o estado AVX no contexto, as formas de 256 bits são recusadas e
  as de 128 bits não zeram a metade alta do YMM.

## Rodar os testes (Linux x86-64, CPU com F16C, BMI1 e MOVBE)

    gcc -O1 -mno-red-zone -mavx2 -mf16c -mbmi -mmovbe test_core.c emu_core.c -o test_core
    ./test_core 300000

## Arquivos

- `emu_core.h` / `emu_core.c`: emulador portável (decodificador VEX/legado + semântica).
- `win_glue.c`, `dinput8.def`: DLL proxy para Windows com o handler de exceção.
- `test_core.c`: teste diferencial contra a CPU real.
