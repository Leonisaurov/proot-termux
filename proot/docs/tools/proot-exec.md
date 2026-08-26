# proot-exec

`proot-exec` ejecuta PRoot desde un archivo declarativo TOML. Si no se indica
`--config`, carga `./proot-exec.conf`. El archivo puede
llamarse `proot-exec.conf`; no se evalúa como shell y el comando guest se pasa
como `argv`, por lo que espacios y metacaracteres no crean comandos adicionales.

## Uso

```bash
./bin/proot-exec --config ./proot-exec.conf
./bin/proot-exec --config ./proot-exec.conf --print
./bin/proot-exec --config ./proot-exec.conf --dry-run
./bin/proot-exec --config ./proot-exec.conf -- nvim --clean
```

La opción `--` permite reemplazar `proot.command` para una ejecución puntual.
`--print` muestra el `argv` generado y solo los overrides de entorno declarados;
`--dry-run` además
valida el rootfs, `launcher_cwd` y el `control_fd`, pero no ejecuta PRoot.

Antes de ejecutar PRoot, `proot-exec` elimina siempre `LD_PRELOAD` y
`LD_LIBRARY_PATH` del entorno heredado. Esto evita que `termux-exec` aplique su
traducción del shell host dentro de la vista guest. `--print` declara esta
limpieza mediante `environment_removed`.

## Formato

El archivo usa TOML y contiene una tabla `[proot]`, una tabla `[env]` y cero o
más tablas `[[binds]]`:

```toml
[proot]
executable = "${PREFIX}/bin/proot"
rootfs = "${HOME}/rootfs/alpine"
cwd = "/work"
proxy = "devnet"
net_policy = "deny"
net_allow = ["*"]
control_fd = 3
inherit_environment = true
launcher_cwd = "${PWD}"
flags = ["--kill-on-exit", "--proc-isolated"]
extra_args = ["--change-id=0:0"]
command = ["/bin/sh", "-i"]

[[binds]]
host = "${PWD}"
guest = "/work"
mode = "ro"

[[binds]]
host = "${TMPDIR}"
guest = "/tmp"
mode = "rw"

[env]
EDITOR = "nvim"
APP_MODE = "isolated"
```

Campos disponibles:

| Campo | Resultado |
|---|---|
| `executable` | Binario PRoot; un nombre sin `/` se resuelve por `PATH`. |
| `rootfs` | `--rootfs`; es una ruta host. |
| `cwd` | `--cwd`; es una ruta guest absoluta. |
| `binds` | `--bind host:guest[:ro\|rw\|wo\|mask]`. |
| `proxy` | `--proxy NAME`. |
| `net_policy` | `--net-policy off\|deny\|allow`. |
| `net_allow` | Repite `--net-allow DESTINATION`. `*` entrega desconocidos al control-fd cuando corresponde. |
| `control_fd` | `--control-fd FD`; el FD debe estar abierto al ejecutar. `none` lo omite. |
| `flags` | Banderas PRoot completas, como `--kill-on-exit` o `--proc-isolated`. |
| `extra_args` | Argumentos PRoot adicionales, en orden; usa la sintaxis que PRoot documenta. |
| `command` | `argv` inicial del guest. Puede ser una lista `command = [...]` en la raíz o en `[proot]`, o `argv = [...]` dentro de `[command]`. |
| `env` | Variables añadidas o reemplazadas. |
| `inherit_environment` | Conserva el entorno del launcher; por defecto `true`. |
| `launcher_cwd` | Cwd host del proceso PRoot, distinto de `proot.cwd`. |

Las rutas host relativas se resuelven respecto al directorio del archivo de
configuración. Las rutas guest deben ser absolutas. Las variables `$NAME` y
`${NAME}` se expanden desde el entorno del launcher; una variable inexistente
es un error. Las variables de `[env]` también pueden referirse a variables
anteriores de esa tabla.

También se acepta `cmd` como alias de `command`, y `[command].cmd` como alias
de `[command].argv`. Siempre deben ser arrays de strings; nunca se interpretan
como una línea de shell.

## Control-fd

`proot-exec` no crea un socket ni atiende PRCT: solo conserva el descriptor que
el proceso padre ya abrió y lo pasa a PRoot. Por ejemplo, un supervisor puede
abrir FD 3, escribir la configuración con `control_fd = 3` y ejecutar:

```bash
./bin/proot-exec --config proot-exec.conf
```

Si el FD no está abierto, la ejecución falla antes de lanzar PRoot. Esto evita
creer que existe un control-fd funcional cuando no hay peer PRCT.

## Seguridad y límites

- No se usa `shell=True`, `eval` ni concatenación de comandos.
- `extra_args` y `flags` son explícitos y se incorporan sin reinterpretación.
- `--print` permite auditar el comando antes de ejecutarlo.
- El programa no agrega binds, presets Termux ni políticas implícitas.
- `--proc-isolated` es responsabilidad del archivo; para TUI/PTY debe probarse
  que el aislamiento elegido no oculte la ruta del terminal.
