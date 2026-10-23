# `tests/upstream/` — suite de termux/proot vendorizada

Copia **cruda** (sin parchear) de los 29 archivos de `tests/` de upstream tocados por el rango portado
`7266fb3e85..968ae1be84` (= `v5.1.107.92..v5.1.107.99`), tomados de `upstream/master`.

- Origen: `git show upstream/master:tests/<ruta>`
- Punta vendorizada: `968ae1be84`
- No se editan: si algo hay que adaptar, se adapta en el wrapper, no aquí.

## Qué hay dentro

| tipo | cuántos | qué son |
|---|---|---|
| `test-*.sh` | 19 | regresiones por commit de upstream, con el sha en el nombre |
| `*.c` | 10 | programas auxiliares (`getdents.c`, `link.c`, `fork_regs.c`, `open_tree.c`, `fchmodat2.c`, `execveat.c`, `renameat2.c`, `open_nofollow.c`, `test-571fb953.c`) |

La suite **completa** de upstream son 177 archivos; aquí sólo está lo que este sync tocó. El resto sigue
viviendo aguas arriba (`make -C tests check` con su propio `GNUmakefile` y su rootfs).

## Cómo se ejecutan desde este repo

`tests/proot/upstream/test_upstream_suite.sh` (descubrible por `tests/run.sh`). Son tests de host Linux que
asumieron cosas que Termux no tiene, así que el wrapper:

- los corre **dentro** de un proot al que se le traduce `/tmp` a un directorio escribible (18 de 19 escriben
  en `/tmp` literal, y Android no permite crearlo);
- pasa `PROOT`, `PROOT_LOADER` y `PROOT_TMP_DIR` apuntando a nuestro binario;
- deja el `--bind` en el nivel externo: el proot que un test arranca por su cuenta no hereda la traducción;
- clasifica `PASS` / `SKIP(125)` (el propio test declaró entorno insuficiente) / `SKIP-ENV` (causa conocida,
  listada en el wrapper con su motivo) / `FAIL` (todo lo demás).

Estado medido en este dispositivo con el port aplicado: **4 PASS, 9 SKIP por autodeclaración del test,
6 bloqueados por entorno**. Los comportamientos que estos 6 cubren y que importan al fork están fijados, con
aserciones propias y ejecutables, en:

- `tests/proot/syscalls/test_link2symlink_link_count.sh` (conteo de enlaces y apertura por descriptor)
- `tests/proot/proc/test_hidden_files_getdents.sh` (`getdents(2)` fallido, 64 bits)
- `tests/proot/compat/test_arm64_load_addresses.sh` (hueco de direcciones de 256 GiB)
- `tests/proot/syscalls/test_upstream_link2symlink.sh` (los tres commits previos ya portados)

Lo que sigue sin cubrir aquí es el caso de **tracee de 32 bits** de `b8b3e6280b` (necesita `gcc -m32` y
ejecutar ARM32 anidado) y el bucle de 9999 enlaces de `05a93edfc8` por tiempo. Quedan anotados, no ocultados.
