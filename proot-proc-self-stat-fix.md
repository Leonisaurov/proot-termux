# proot-proc-self-stat-fix

Fecha: 2026-09-05

## Alcance

Este informe documenta la investigación y el fix realizado únicamente dentro del checkout de Hermes:

`/data/data/com.termux/files/home/Develop/Patch/hermes-termux`

No se modificó el código fuente de proot/proot-termux, no se cambió su binario instalado, no se instalaron paquetes y no se usó `adb`, `su` ni un puente Android.

El nombre del informe refleja el error observado originalmente:

```text
couldn't read /proc/self/stat: Inappropriate ioctl for device
Aborted
exit code: 134
```

## Veredicto

Hay dos bugs de Hermes corregidos:

1. Rutas remotas con prefijo `~` podían convertirse en rutas literales `./~`.
2. La caché de file tools podía conservar un entorno antiguo después de que el terminal sustituyera el backend activo.

El error de Proton Drive en `/proc/self/stat` no se reprodujo con los procesos shell de Hermes. El wrapper de Hermes pasa las opciones procfs correctamente y los smoke tests reales de proot pasan en ambos modos. Con la evidencia disponible, el bloqueo restante queda acotado a la interacción del binario Android/Bun con el runtime/fork proot, no a un cambio adicional seguro dentro de Hermes.

No se aplicó ningún fallback automático que reduzca el aislamiento.

## Problema 1: creación de directorios literales `~`

### Evidencia inicial

`hermes-literal-tilde-cwd-report.md` documenta un árbol materializado como:

```text
./~/.hermes
./~/.codex
./~/.config
./~/.local
./~/.ssh
./~/storage
```

La causa concreta encontrada en Hermes era esta frontera:

```python
shlex.quote("~/.hermes/cache")
```

El resultado es una cadena equivalente a:

```sh
'~/.hermes/cache'
```

La tilde dentro de comillas simples no es una expansión de shell. El comando operaba sobre un componente llamado literalmente `~`.

El problema afectaba a los helpers de `tools/environments/file_sync.py`:

- `quoted_mkdir_command()`
- `quoted_rm_command()`

También existía un segundo riesgo: `tools/file_tools.py` resolvía `~/.hermes/...` contra el HOME del proceso Hermes antes de entregarlo al shell remoto. Para SSH, Daytona y Vercel, eso puede convertir una ruta válida para el shell remoto en una ruta host incorrecta.

### Fix aplicado

`tools/environments/file_sync.py` ahora usa `_quote_remote_shell_path()`:

- `~` se representa como `"$HOME"`.
- `~/resto` conserva únicamente el prefijo `$HOME` y escapa el resto.
- Las rutas que no comienzan con `~` conservan `shlex.quote()`.

Esto mantiene la seguridad contra inyección y evita producir `./~`.

`tools/file_tools.py` ahora conserva `~` sin resolver para los backends cuyo contrato es un shell remoto:

```text
ssh
 daytona
 vercel_sandbox
```

La expansión queda para `ShellFileOperations`, que ejecuta el comando en el shell del backend y obtiene allí el HOME efectivo.

### Evidencia TDD

RED inicial:

```text
scripts/run_tests.sh tests/tools/test_remote_tilde_paths.py
3 failed
```

Los fallos fueron los esperados:

- `mkdir` recibía `'~/.hermes/cache/images'`.
- `rm` intentaba borrar la ruta literal `'~/.hermes/cache/old.txt'`.
- `_resolve_path_for_task()` devolvía el HOME host en vez de preservar `~`.

GREEN después del fix:

```text
scripts/run_tests.sh tests/tools/test_remote_tilde_paths.py
4 passed
```

Los tests ejecutan realmente `/bin/sh -c` con un HOME temporal y comprueban que:

- el directorio se crea bajo `$HOME`;
- el archivo se elimina bajo `$HOME`;
- no aparece un directorio literal `~`;
- la resolución remota no usa el HOME host.

## Problema 2: backend obsoleto en `write_file`

### Causa

`tools/file_tools.py::_get_file_ops()` devolvía una entrada cacheada únicamente porque coincidía el `task_id`.

Si el terminal reemplazaba el entorno activo —por ejemplo, al cambiar de local a proot o al recrear el sandbox— la caché de file tools podía seguir apuntando al objeto anterior. Eso explicaba el incidente documentado en `hermes-sandbox-proc-fix.md`, donde el terminal ya estaba fuera del sandbox pero `write_file` seguía intentando escribir mediante un `ProotSandboxEnvironment` distinto.

El cambio de backend no debe inferirse solo del valor de `TERMINAL_ENV`: la identidad efectiva es el objeto guardado en `_active_environments`.

### Fix aplicado

`_get_file_ops()` ahora compara:

```text
cached.env is active_environment
```

Si no coincide:

1. invalida la entrada vieja;
2. conserva el entorno activo actual;
3. reconstruye `ShellFileOperations` contra ese entorno.

No se toca el aislamiento ni se fuerza un backend. Solo se elimina una caché stale.

### Evidencia TDD

RED:

```text
scripts/run_tests.sh tests/tools/test_remote_tilde_paths.py
3 passed, 1 failed
```

El test fallido demostraba que `_get_file_ops()` devolvía el objeto viejo cuando `_active_environments["default"]` ya apuntaba a otro entorno.

GREEN:

```text
scripts/run_tests.sh tests/tools/test_remote_tilde_paths.py
4 passed
```

## Problema 3: `/proc/self/stat` y Proton Drive

### Contratos verificados en Hermes

`tools/environments/proot_sandbox.py` mantiene un único builder común para fresh y persistent, con:

```text
--proc-isolation
--bind=/proc:/proc:rw
```

Además, `sandbox.isolate_proc` controla exclusivamente:

```text
--proc-isolated
```

Por tanto:

- `isolate_proc=True`: mantiene `--proc-isolated`, `--proc-isolation` y el bind `/proc`.
- `isolate_proc=False`: omite solo `--proc-isolated`, pero conserva `--proc-isolation` y el bind `/proc`.

La configuración se propaga desde `hermes_cli/config.py` hacia `tools/terminal_tool.py` y luego a `ProotSandboxEnvironment`.

No se cambió ese código de producción porque ya implementaba el workaround reversible descrito en `hermes-sandbox-proc-fix.md`.

### Tests añadidos

Se añadieron tests de contrato en:

- `tests/tools/test_proot_sandbox.py`
- `tests/tools/test_proot_sandbox_e2e.py`

Cubren:

- presencia/ausencia de `--proc-isolated` según configuración;
- presencia de `--proc-isolation` en ambos modos;
- exactamente un `--bind=/proc:/proc:rw`;
- lectura real de `/proc/self/stat` bajo proot con `isolate_proc=True`;
- lectura real de `/proc/self/stat` bajo proot con `isolate_proc=False`.

### Resultado real

```text
scripts/run_tests.sh tests/tools/test_proot_sandbox.py tests/tools/test_proot_sandbox_e2e.py
75 passed
```

El binario instalado fue comprobado sin modificarlo:

```text
/data/data/com.termux/files/usr/bin/proot
proot-latest-452-gef9cbd41-dirty
```

Los smoke tests reales de Hermes/proot pasan en ambos modos, incluyendo `/proc/self/stat`.

### Por qué no se declara resuelto Proton Drive

No hay un binario `proton-drive` dentro de este checkout. La búsqueda del repositorio no encontró `proton-drive` ni `proton-drive.real`.

Por la instrucción de trabajar solo aquí, no se inspeccionó ni modificó el checkout externo de Proton Drive. Tampoco se ejecutó una operación de autenticación, red o restauración.

Así que no es correcto afirmar que el binario Proton Drive ya funciona. Lo que sí queda demostrado es:

1. Hermes construye el contrato procfs esperado.
2. El bind `/proc` está presente una sola vez.
3. Hermes puede leer `/proc/self/stat` mediante comandos shell reales bajo proot.
4. El fallo específico de Bun/Proton Drive no ocurre en ese smoke mínimo.

La hipótesis restante es una incompatibilidad del binario Android/Bun con la implementación procfs o con `--proc-isolated` del fork proot instalado.

## Validación ejecutada

### Suite Hermes/proot y sincronización

```text
scripts/run_tests.sh \
  tests/tools/test_credential_files.py \
  tests/tools/test_file_sync.py \
  tests/tools/test_file_sync_back.py \
  tests/tools/test_file_operations_edge_cases.py \
  tests/tools/test_container_cwd_sanitize.py \
  tests/tools/test_proot_prct_wiring.py \
  tests/tools/test_proot_control_harness.py \
  tests/tools/test_proot_sandbox.py \
  tests/tools/test_proot_sandbox_e2e.py \
  tests/hermes_cli/test_sandbox_status.py

224 passed
```

### Resolución de rutas

```text
scripts/run_tests.sh \
  tests/tools/test_file_tools_container_config.py \
  tests/tools/test_file_tools_tilde_profile.py \
  tests/tools/test_remote_tilde_paths.py

10 passed
```

Los grupos amplios de `test_file_tools.py` y `test_file_tools_cwd_resolution.py` tienen fallos preexistentes del entorno actual: `pytest` crea temporales bajo `$PREFIX/tmp`, y la protección de escritura los clasifica como rutas de sistema. No se debilitó esa protección para hacer pasar los tests.

### Checks estáticos

```text
python -m py_compile \
  tools/environments/file_sync.py \
  tools/file_tools.py \
  tests/tools/test_remote_tilde_paths.py \
  tests/tools/test_proot_sandbox.py \
  tests/tools/test_proot_sandbox_e2e.py

OK

git diff --check
OK

ruff check <archivos modificados>
All checks passed!
```

## Archivos modificados por esta intervención

Código:

- `tools/environments/file_sync.py`
- `tools/file_tools.py`

Tests:

- `tests/tools/test_remote_tilde_paths.py`
- `tests/tools/test_proot_sandbox.py`
- `tests/tools/test_proot_sandbox_e2e.py`

No se modificó `tools/environments/proot_sandbox.py` durante esta intervención.

## Conclusión operativa

El fix de rutas y la re-vinculación de file tools están terminados y verificados.

El error original de Proton Drive queda documentado como no resuelto a nivel de aplicación porque requiere ejecutar el binario real. Si el binario funciona con `isolate_proc=false` y falla con `true`, el conflicto queda confirmado específicamente entre Bun y `--proc-isolated`. Si falla en ambos modos mientras los smoke tests anteriores siguen pasando, el siguiente propietario técnico es proot/procfs o el runtime Bun, no un cambio silencioso en Hermes.

No se debe eliminar `--proc-isolation`, quitar el bind `/proc`, desactivar `sandbox.enabled` ni añadir reintentos automáticos que degraden aislamiento.
