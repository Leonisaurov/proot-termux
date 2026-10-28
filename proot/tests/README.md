# Tests

Las pruebas están separadas por la capa que ejercitan:

| Área | Directorio | Qué valida |
|---|---|---|
| build | [`build/`](build/) | generación de dependencias y árbol de build local |
| PRoot | [`proot/`](proot/) | syscalls, aislamiento, red y hardening |
| `termux-isolated` | [`termux-isolated/`](termux-isolated/) | cwd, shell, storage, PTY, proc y políticas |
| control-api | [`control-api/`](control-api/) | protocolo PRCT, launcher y harness |
| harness | [`harness/`](harness/) | integración PTY/PRCT y `proot-exec` |
| rootfs | [`rootfs/`](rootfs/) | wrappers y fixtures Alpine |

Desde la raíz del repositorio, primero revisa y después ejecuta el script:

```bash
bash -n proot/tests/termux-isolated/cwd/test_termux_isolated_cwd.sh
git diff --check
```

Las pruebas que ejecutan binarios, PRoot o Android deben ejecutarse en el
Termux local con autorización elevada. Los temporales deben estar bajo
`$TMPDIR`; las rutas `/tmp` que aparecen dentro de probes pertenecen al
rootfs guest o al contenedor CI y no deben copiarse al flujo Termux.

La batería completa puede ser costosa. Ejecuta primero el caso mínimo sin
protocolo, después `control-fd` y finalmente el harness que atiende eventos
durante toda la vida del proceso.

La batería se puede seleccionar por capa desde la raíz del repositorio:

```bash
./proot/tests/run.sh proot
./proot/tests/run.sh termux-isolated
./proot/tests/run.sh harness
./proot/tests/run.sh control-api
./proot/tests/run.sh all
```

`run.sh` no aborta en el primer fallo: ejecuta toda la capa seleccionada y
resume los tests fallidos al final. Los tests que dependen del rootfs Alpine
hacen SKIP limpio (exit 0) cuando no está instalado, para que la batería siga
siendo ejecutable en un Termux sin ese contenedor.

Entre las regresiones relevantes de `termux-isolated` están
`shell/test_termux_isolated_shebang.sh` y
`shell/test_termux_isolated_shebang_fastpath.sh`; ambas cubren la ejecución de
shebangs, incluida la ruta rápida del launcher.

## Subdirectorios de `proot/`

| Subdirectorio | Tests | Qué cubren |
|---|---|---|
| `hardening/` | `test_b1_b2_b7.sh`, `test_b3.sh`, `test_b5.sh`, `test_b6.sh`, `test_b8.sh` | fase B: fds filtrados a los clientes `--exec`, barrido de `fd_map`, integridad de `SO_PEERCRED`, hijos del bridge y fugas talloc |
| `hardening/` | `test_phase_c.sh`, `test_phase_c_final.sh` | fase C: `MS_RDONLY`, `/etc` `:ro`, vista `proc`, `PEERCRED` y permisos emulados (`test_phase_c_final.sh` es el punto de entrada estable) |
| `hardening/` | `test_fake_id0_meta_symlink.sh` | contrato de los meta files de `fake_id0` y qué hace realmente `-0` |
| `hardening/` | `test_temp_cleanup.sh` | destructor del directorio temporal (`PROOT_TMP_DIR`) |
| `hardening/` | `test_supervise_protocol.sh` | protocolo de `--supervise` y `--exec` |
| `syscalls/` | `test_d4_e1_e3_e6.sh` | semántica emulada de `socket`, `renameat2`, `uname` y `mknod` |
| `syscalls/` | `test_talloc.sh` | gestión de memoria talloc |
| `syscalls/` | `test_upstream_link2symlink.sh` | regresiones de `link2symlink` portadas del upstream |
| `proc/` | `test_proc_maps_filter.sh` | filtrado de `maps`: lecturas parciales, `dup`, reutilización de fd y saturación |
| `proc/` | `test_meminfo_machine_summary.sh` | `/proc/meminfo` real y contadores sintéticos (`uptime`, `loadavg`, `stat`) |
| `proc/` | `test_mountinfo_bindings.sh` | `mountinfo` expone cada binding como mount |
| `proc/` | `test_nesting_pthread.sh` | anidamiento con hilos y con el supervisor |
| `resource/` | `test_fd_and_proc_limit.sh` | `--fd-limit` heredado al guest y `--proc-limit` cerrando el fork con `EAGAIN` |
| `compat/` | `test_ashmem_memfd.sh` | `--ashmem-memfd` deja un fd de memoria utilizable |
| `performance/` | `test_nested_benchmark.sh`, `nested_helper.sh` | coste del anidamiento; `nested_helper.sh` reproduce la profundidad de anidamiento |
| `performance/` | `test_audit_comparison.sh` | comparación entre dos builds; es opt-in y hace SKIP si no se define `AUDIT_BASELINE` |
| `performance/` | `test_net_policy_benchmark.sh` | coste de `--net-policy` |
| `networking/` | `test_net_policy.sh` | reglas estáticas de la política de red |
| `networking/` | `test_control_fd.sh` | canal PRCT: peticiones, decisiones y sombras |
| `networking/` | `test_control_fd_invalid_decisions.sh` | una decisión malformada nunca autoriza una operación |
| `networking/` | `test_unix_socket_scope.sh` | alcance de los sockets Unix del guest |
| `networking/` | `test_accept_matrix.sh` | matriz de `accept`/`accept4` con y sin `--proxy` |
| `networking/fixtures/` | `accept_fixture.c`, `control_invalid_decisions.py`, `test_httpd_debug.py`, `test_srv.py`, `test_vnet.py` | programas auxiliares que compilan o ejecutan los tests de red |
| `networking/manual/` | `dns_access`, `outside_proxy_test` | comprobaciones manuales, fuera de la batería automática |
| `probes/` | `escape_test2.c`, `fork_gate.c`, `memfd_probe.c`, `memtest.c`, `temp_cleanup.c` | programas C que compilan los tests anteriores |

## Subdirectorios de `termux-isolated/`

| Subdirectorio | Test | Qué cubre |
|---|---|---|
| `cwd/` | `test_termux_isolated_cwd.sh` | `--cwd` en modo comando y en modo shell interactivo |
| `shell/` | `test_termux_isolated_shebang.sh` | shebangs de Termux en los dos modos de rutas |
| `shell/` | `test_termux_isolated_shebang_fastpath.sh` | ruta rápida del launcher, sin el preload de `termux-exec` |
| `shell/` | `test_termux_isolated_default_shell.sh` | shell configurado en Termux (`$SHELL`) en modo interactivo |
| `pty/` | `test_termux_isolated_nvim.sh` | Neovim arranca y sale desde una PTY interactiva del sandbox |
| `proc/` | `test_termux_isolated_proc_paths.sh` | `proc_isolation` clasifica rutas canónicas y relativas antes de exponer procfs del host |
| `proc/` | `test_termux_isolated_root_visibility.sh` | enlaces de raíz guest y binds intencionales de rutas Android/Termux |
| `compat/` | `test_termux_isolated_go_runtime.sh` | `/proc/self/maps` conserva rangos de direcciones válidos para runtimes Go/cgo |
| `policy/` | `test_termux_isolated_proot_args.sh` | formas literal y agrupada de `--proot-arg` y `--proot-args` |
| `storage/` | `test_termux_isolated_storage.sh` | el almacenamiento es opt-in y usa binds `:mask` por defecto |
| `performance/` | `test_termux_isolated_benchmark.sh` | benchmark pareado de sesiones interactivas persistentes |

## Tests de `control-api/`

| Fichero | Qué cubre |
|---|---|
| `python/test_control_api.py` | codec, estados del canal y handshake `HELLO` |
| `python/test_channel_regressions.py` | regresiones de transporte PRCT con `socketpair`, sin guest |
| `python/test_async_harness.py` | canal asíncrono y política de sesión |
| `python/test_tmpdir_control.py` | cobertura real de `control-fd` con un `TMPDIR` escribible |
| `python/test_terminal.py` | terminal externa y sus dimensiones |
| `python/test_fish_integration.py` | integración real con fish bajo el harness PRCT |
| `python/test_neovim_integration.py` | sockets locales de Neovim bajo el harness PRCT |
| `python/test_tui_smoke.py` | smoke headless con PTY real del harness de ejemplo |

Los tests de Rust (`control-api/rust`) y de Bun (`control-api/bun`) no viven en
este árbol: los ejecuta `run.sh control-api` con `cargo test` y `bun test`.

## `build/` y `harness/`

| Área | Fichero | Qué cubre |
|---|---|---|
| `build/` | `test_stale_deps.sh` | los `.d` generados toleran cabeceras del sysroot eliminadas (`-MP`) |
| `build/` | `test_clang_lto_loader_embedding.sh` | `clang` + `-flto`: el objeto sigue siendo un ELF legible y el loader queda incrustado en el binario; `ashmem_memfd` declara `<string.h>` |
| `harness/proot-exec/` | `test_proot_exec.sh` | parseo de configuración, generación de argv, expansión de variables y ejecución |
| `harness/proot-exec/` | `test_proot_exec_nested.sh` | `proot-exec` anidado |
| `harness/fixtures/` | `proot-exec*.conf` | configuraciones de `proot-exec` usadas por los tests |
| `harness/legacy/` | `simple-harness` | harness mínimo, de una sola respuesta |

`run.sh` descubre los tests con `find -name 'test_*.sh'`, así que añadir un
directorio temático nuevo basta para que entre en la batería.

Los probes de `probes/` siguen la convención de nombre del `.gitignore`: el
prefijo `p_` (`p_fs.c`, `p_sys.c`, …) está reservado a los probes del pentest
local y **no se trackea**. Un probe que forma parte de una regresión commiteada
debe llamarse sin ese prefijo (`fork_gate.c`, `memfd_probe.c`); si no, el test
que lo compila falla en un clone limpio.
