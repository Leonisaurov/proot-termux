# Progreso de la auditoría y puesta a punto

Fecha de corte: 2026-09-06  
Estado: implementación y validación local completadas; cambios aún sin commit/push.

## Objetivo

Auditar y pulir el producto completo: núcleo PRoot en C, aislamiento `/proc`, red virtual y política de red, supervisor `--supervise/--exec`, límites de recursos, launchers Termux, `proot-exec`, control-api Python/Rust/Bun, harness, pruebas, benchmarks, empaquetado y GitHub Actions.

Se adoptó el criterio de permitir cambios incompatibles cuando estén justificados por seguridad o corrección, acompañados de migración. Se conservaron los cambios locales que ya existían al comenzar.

## Trabajo realizado

### Auditoría inicial y entorno

- Se revisaron `AGENTS.md`, la estructura vigente, el flujo de build, los workflows y la documentación de seguridad/rendimiento.
- Se detectó y respetó el estado inicial sucio del árbol: había modificaciones documentales y de tests, `proot-exec.conf` eliminado, `codex.conf` nuevo y documentación de benchmarks nueva.
- Preflight real de Termux ejecutado con autorización elevada:
  - arquitectura `aarch64`, Android API 35, ABI `arm64-v8a`;
  - aproximadamente 12 GB de RAM total y 6 GB disponibles;
  - 15 GB libres en el filesystem del proyecto;
  - Clang 21.1.8, Cargo/Rust 1.98.1, Bun 1.3.14, actionlint y gita disponibles;
  - `$TMPDIR=/data/data/com.termux/files/usr/tmp`, escribible y usado en los nuevos scripts.
- Se comprobó que no se deben trasladar rutas `/tmp` de fixtures guest al flujo nativo de Termux.

### Seguridad y corrección del protocolo PRCT

- En `proot/src/extension/net_policy/net_policy.c` se endureció la recepción de decisiones:
  - se exige tamaño exacto del payload;
  - `COMMAND_RESULT` ya no puede actuar como decisión;
  - el tipo (`ALLOW_*`/`DENY_*`) debe coincidir con el byte `decision`;
  - una respuesta inválida o una secuencia sin decisión deja el canal en estado fallido y deniega.
- En el cliente Python síncrono:
  - se validan strings terminados en NUL y UTF-8 válido como `InvalidFrame`;
  - `send_frame` maneja escrituras parciales, backpressure, EOF y deadline absoluto.
- En `async_channel.py`:
  - los eventos ya parseados se entregan antes de convertir EOF en error;
  - `flush` no reutiliza canales terminales y trata una escritura de cero bytes como EOF.
- En Rust:
  - se valida familia según operación, incluyendo `SOCKET` con dominio no cero y `PUBLICATION` con familia cero;
  - se rechazan request IDs incorrectos en resultados de comandos;
  - se rechazan rutas con NUL, strings sin terminador y envíos desde canales cerrados;
  - se añadió escritura con deadline y cierre/recolección acotados del proceso.
- En Bun:
  - se añadieron las mismas validaciones de familia, strings y request IDs;
  - se corrigió el uso de `performance.now()` para deadlines y limpieza de timers;
  - se añadió cierre idempotente y espera con escalado TERM/KILL;
  - se corrigió la creación del socket desde FD usando `node:net.Socket` y la selección de libc.

### Supervisor `--supervise/--exec`

- En `supervise.c` se añadió una deadline de un segundo para la recepción inicial de una petición.
- Se exige `SO_PEERCRED` válido y mismo UID.
- Se usa `accept4(..., SOCK_CLOEXEC)` con fallback seguro y se comprueba `FD_CLOEXEC`.
- Se validan todos los descriptores `SCM_RIGHTS`: exactamente tres, sin truncamiento; los extras se cierran.
- Se valida que `cwd` tenga terminador NUL antes de usarlo.
- Se conservó la autenticación por UID para no romper clientes `--exec` legítimos del mismo usuario.
- Se añadió una regresión de peticiones malformadas, descriptores ausentes/excesivos, clientes inactivos y recuperación posterior.

### Limpieza de temporales

- Se reemplazó la limpieza basada en `chdir`, nombres mutables y `rmdir` recursivo por limpieza relativa a descriptores (`openat`, `O_NOFOLLOW`, `unlinkat`).
- Se evita seguir symlinks o borrar fuera del directorio temporal configurado si una ruta es sustituida durante la limpieza.
- Se preserva el directorio de trabajo del proceso.
- Se añadió `test_temp_cleanup.sh` y el probe C correspondiente para comprobar symlinks, subdirectorios con permisos 000, confinamiento y preservación de un sentinel externo.

### Tests y cobertura

- Se creó `test_channel_regressions.py` para transporte PRCT Python.
- Se añadieron tests equivalentes de Rust y Bun.
- Se añadió `test_control_fd_invalid_decisions.sh` con fixture de decisiones inválidas para red y rutas.
- Se añadió `test_supervise_protocol.sh`.
- `proot/tests/run.sh` ahora ejecuta Python, Cargo y Bun dentro de `control-api`, usando `cargo test --offline` y `CARGO_TARGET_DIR` explícito.
- Se añadió una configuración aislada para tests de `proot-exec`, evitando depender del `proot-exec.conf` personal eliminado del árbol.
- Se corrigieron benchmarks para validar resultados del workload y usar valores por defecto acotados (`100` iteraciones y `10` pares), manteniendo ejecución ampliada mediante variables de entorno.
- Se añadió `test_audit_comparison.sh`, opt-in mediante `AUDIT_BASELINE`, para comparar binarios por hash, latencia, CPU y RSS en casos plain/proc/PRCT/red/anidamiento.

### Build, empaquetado y CI

- `proot/scripts/build-native.sh` valida `$TMPDIR` al inicio.
- Se incrementó `TERMUX_PKG_REVISION` de 92 a 93.
- Build local oficial ejecutado con `./proot/scripts/build-native.sh -i -j 2`: compilación, instalación y paquete completados; binario aarch64 de 388 KiB y paquete local generado.
- El workflow amplió sus filtros para scripts y empaquetado relevantes.
- Se versionó la clave de caché con SO, arquitectura y scripts/toolchain.
- La recolección de artefactos ahora exige paquete no vacío, `xz -t`, valida `.PKGINFO`, genera `SHA256SUMS` y falla si falta el artefacto.
- El job de release solo publica desde `master`, verifica la suma y fija el tag al commit de la ejecución.
- `actionlint` solo reportó que su catálogo local no reconoce `ubuntu-26.04`; GitHub lo ofrece como imagen pública en preview. No se cambió ese runner.

## Validaciones que pasaron

- Build nativo, instalación y empaquetado locales.
- Hardening Phase C: 6/6.
- Syscalls D4/E1/E3/E6: 39/39.
- Link2symlink: 3/3.
- Política de red: 9/9.
- Control-fd existente: fragmentación, reglas, shadows, hide y mask.
- Supervisor malformado y recuperación: PASS.
- Regresiones PRCT Python nuevas: 6/6.
- Rust: 7 tests, todos PASS.
- Bun antes de la última corrección: 7 PASS; después de cambiar `fromFd`/libc se requiere repetir la suite.
- Cleanup temporal: PASS después de fijar el `umask` del fixture.
- Launcher Termux: cwd 3/3, proc, PTY, shell por defecto 2/2, shebang 2/2, fastpath 3/3, storage 4/4.
- Benchmark de Termux aislado: 10 pares; `--proc-isolated` fue más rápido que `--no-proc-isolated` en las 10 parejas de esa corrida.
- Benchmark de política de red: `allow/off=1.243x`, `deny/off=1.077x` en el dispositivo de prueba.
- Benchmark anidado: profundidades 1, 2 y 3 completadas con el binario corregido.
- Batería completa final: todos los grupos de `./proot/tests/run.sh all` terminaron correctamente, incluidos hardening B/C, syscalls (39/39), networking, benchmarks, procfs, `termux-isolated`, harness y control-api.
- Control-api final: Python 46 tests, Rust 7 tests, Bun 9 tests; todos PASS.
- Regresiones nuevas finales: decisiones PRCT inválidas, protocolo del supervisor y cleanup temporal; todas PASS.
- Build final: `./proot/scripts/build-native.sh -i -j 2` completó instalación y paquete aarch64. Se verificó `xz -t`, `.PKGINFO` y SHA-256 (`e8706800f29389ed463c0f97d12c3b17e10d57413580dae7966dac476df1d2fe`).
- `bash -n`, `git diff --check` y `actionlint` (ignorando únicamente el catálogo local obsoleto para `ubuntu-26.04`) pasan.

## Pendiente de entrega

1. Revisar el diff completo una última vez para separar cambios intencionales de artefactos generados. El directorio `proot/artifacts/audit/` está ignorado; `proot/tests/reports/` conserva reportes revisables.
2. Mantener la decisión del usuario de conservar eliminado `proot-exec.conf`; los tests usan el perfil versionado `proot/tests/harness/fixtures/proot-exec-isolated.conf`.
3. Ejecutar la secuencia de entrega obligatoria: `git add -A`, commit convencional, `git push origin master` y `gita notify build-proot.yml`.
4. Como trabajo posterior opcional: sanitizadores, fuzzing prolongado y reproducción con el binario real de Proton Drive/Hermes.

## Riesgos y limitaciones conocidos

- No se inspeccionó ni modificó ningún checkout externo de Hermes o Proton Drive. El documento `proot-proc-self-stat-fix.md` indica que Hermes ya tiene correcciones propias para tilde y caché stale, pero el error de Proton Drive/Bun no quedó reproducido con shell.
- No es correcto declarar resuelto `/proc/self/stat` para Proton Drive sin ejecutar el binario real de Proton Drive; lo demostrado es el contrato de PRoot y la lectura shell en ambos modos.
- `ubuntu-26.04` requiere una imagen GitHub pública en preview; el actionlint local puede seguir marcándola hasta actualizar su catálogo.
- El sandbox reporta RAM sintética cero; los datos de recursos válidos son los obtenidos en Termux local.
- Las cifras de rendimiento son específicas del dispositivo y deben repetirse tras la última build.
- No se han ejecutado sanitizadores ni fuzzing prolongado; quedan como mejora posterior a la estabilización funcional.
- La corrección de `/proc/self/stat` quedó cubierta por el contrato de procfs y las regresiones shell; no se afirma compatibilidad específica con Proton Drive sin su binario real.
- No se ha hecho commit ni push. La revisión 93 está preparada para la entrega.

## Criterio de finalización

La implementación local está terminada: las suites, el binario final y el paquete son verificables y no quedan fallos conocidos en el alcance auditado. La entrega administrativa queda completada al crear el commit, publicarlo y confirmar el workflow CI.
