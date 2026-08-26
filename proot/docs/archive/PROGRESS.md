# PROGRESS — api-fix-5 / api-fix-6

Fecha: 2026-08-23

## Estado actual

El trabajo está implementado parcialmente y tiene un commit dedicado:

```text
0d7b379408 fix(control): mediate unknown network and external path access via PRCT
```

Después de ese commit quedaron cambios posteriores sin commitear relacionados
con la corrección de la mediación durante `execve`, la prueba portable de
Python y el pentest de `control-fd`. Deben revisarse y commitearse en la
continuación.

El árbol contiene muchos archivos `untracked` preexistentes (`.hermes-tmp.*`,
`api-fix-*.md`, `tasks/`, `control-api/rust/target/`, etc.). No incluirlos ni
borrarlos.

## Cambios implementados

- `UNKNOWN` no se convierte en `VIRTUAL`.
- Solo `*`, `tcp://*` y `udp://*` permiten hand-off de `UNKNOWN` a PRCT.
- Sin control FD listo, o con fallo de protocolo, la red continúa fail-closed.
- Las rutas externas solicitan `PATH_ACCESS_REQUEST` aunque un bind permita
  lectura.
- Las aprobaciones PRCT no elevan permisos físicos de bindings `:ro`/`:wo`.
- Se preservan `other_path` y las reglas dinámicas de path.
- Exenciones guest fijas: CWD y descendientes, infraestructura Android
  (`/system`, `/system_ext`, `/product`, `/vendor`, `/apex`, `/odm`,
  `/linkerconfig`, `/proc`, `/sys`, `/dev`) y el prefix de Termux.
- La traducción interna del ejecutable durante `execve` se marca con
  `tracee->exec_path_translation` para no generar prompts de filesystem.
- `TERMUX_PKG_REVISION` debe quedar en 45 después del último cambio de fuente;
  el commit anterior aún tenía 44.

## Archivos modificados después del commit anterior

Revisar `git diff` y conservar estos cambios:

- `proot-source/src/tracee/tracee.h`
  - campo `exec_path_translation`.
- `proot-source/src/execve/enter.c`
  - marca el contexto alrededor de `translate_path()`.
- `proot-source/src/extension/net_policy/net_policy.c`
  - no media traducción interna de ejecutables.
- `packages/proot/build.sh`
  - revisión 44 → 45.
- `control-api/python/test_control_api.py`
  - `printf integration-ok` → `echo -n integration-ok`, porque el `/bin/sh`
    de este Termux no tiene `printf` builtin ni ejecutable resoluble.
- `pentest/test_control_fd.sh`
  - afirma que una aprobación PRCT no vence un bind `:ro`.
  - regla proactiva de lectura sobre `/control/file`.
  - el caso de reveal usa glob/builtins para no agotar forks.
  - el harness drena solicitudes internas posteriores y solo exige que
    `/control/file` no vuelva a pedir autorización.

## Build y tests ya comprobados

La build debe ejecutarse con el flujo oficial, no con `make` directo:

```bash
export TMPDIR=/data/data/com.termux/files/usr/tmp
./scripts/build-native.sh -j 1 --skip-package
```

La build elevada terminó correctamente y produjo:

```text
/data/data/com.termux/files/usr/bin/proot
```

Resultados elevados previos:

- `test_net_policy.sh`: PASS 9/9.
- `test_phase_c.sh`: PASS 6/6.
- Python control API: PASS 15/15 después de cambiar `printf` a `echo -n`.
- Rust `cargo test --lib`: PASS 3/3.
- Bun: PASS 3/3.
- `test_control_fd.sh`: los tres primeros casos pasan; el caso reveal
  recursivo todavía devuelve `exit=1` y requiere análisis.

## Regla importante de entorno

Para referencias Termux usar:

```bash
./termux-isolated -- sh -c \
  'cd /home/Develop/Patch/proot-termux && PROOT=/usr/bin/proot ./pentest/test_control_fd.sh'
```

Dentro de `termux-isolated`, el prefix guest es `/usr` y el binario real es
`/usr/bin/proot`; fuera del wrapper es:

```text
/data/data/com.termux/files/usr/bin/proot
```

La primera reproducción dentro del wrapper falló por el propio test:
cada `subprocess.Popen(..., env={"PATH": ...})` elimina `LD_LIBRARY_PATH`, y
el binario `/usr/bin/proot` no encuentra `libtalloc.so.2`. El pentest debe
construir el entorno conservando el `LD_LIBRARY_PATH` de Termux, por ejemplo
con `env = os.environ.copy()` y luego ajustar `PATH`.

## Fallo pendiente: REVEAL_SHADOW

El caso relevante está en la segunda mitad de `pentest/test_control_fd.sh`,
función `run_reveal(scope, expected_visible)`:

```text
run_reveal(1, False)  # pasa
run_reveal(2, True)   # falla: (2, 1, '', '')
```

La secuencia envía `REVEAL_SHADOW` con `scope=CONTROL_SHADOW_RECURSIVE` para
`/etc`, recibe `COMMAND_RESULT` correcto, y luego el guest no observa
`/etc/passwd`. No asumir que es un problema de fork: primero verificar los
frames y el estado de `config->shadows`.

Puntos de código a inspeccionar:

- `control_apply_command()` en
  `proot-source/src/extension/net_policy/net_policy.c`.
- `net_policy_shadow_access()`.
- `control_shadow_hides_entry()` y el filtrado de `getdents`.
- `canonicalize()`/`translate_path()` cuando el path está bajo un shadow
  revelado recursivamente.

Instrumentación recomendada temporalmente: imprimir/loggear solo guest paths,
scope, `revealed`, `recursive` y el resultado de `net_policy_shadow_access()`;
no exponer host paths.

## Siguiente sesión: orden exacto

1. Ejecutar:

   ```bash
   git status --short
   git diff -- proot-source packages/proot control-api pentest
   rg -n 'TERMUX_PKG_REVISION|exec_path_translation' \
      packages/proot/build.sh proot-source/src
   ```

2. Corregir el entorno de `pentest/test_control_fd.sh` para preservar
   `LD_LIBRARY_PATH` bajo `termux-isolated`.

3. Ejecutar el pentest dentro de `./termux-isolated` usando `/usr/bin/proot`.

4. Analizar y corregir `REVEAL_SHADOW` recursivo; no cambiar la expectativa a
   `False` para ocultar el fallo.

5. Incrementar la revisión si se modifica `proot-source/src/` nuevamente.

6. Rebuild oficial:

   ```bash
   export TMPDIR=/data/data/com.termux/files/usr/tmp
   ./scripts/build-native.sh -j 1 --skip-package
   ```

7. Ejecutar elevada y dentro de `termux-isolated` la matriz completa:
   Python, Rust `--lib`, Bun, `test_control_fd.sh`, `test_net_policy.sh` y
   `test_phase_c.sh`.

8. Solo cerrar cuando el reveal recursivo pase, `git diff --check` sea limpio,
   no haya rutas temporales accidentales y el commit incluya todos los cambios
   de esta corrección. No hacer push.
