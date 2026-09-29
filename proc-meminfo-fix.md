# FIX — `/proc/meminfo` devuelve ceros: bug de `proc_isolation`

**Tipo:** BUG cerrado en REV 103 (implementado y verificado en device)
**Severidad:** media-alta para cualquier consumidor que decida con RAM
**Capa:** proot — `proot/src/extension/proc_isolation/proc_isolation.c`
**Detectado:** 2026-09-28, desde el consumidor Hermes (`hermes-termux`)
**Verificado de nuevo y corregido:** 2026-09-28 (binario local `build-native.sh -i`)

## 1. Síntoma

Dentro del sandbox, `/proc/meminfo` no describe el dispositivo:

```text
$ head -3 /proc/meminfo          # dentro del sandbox
MemTotal:           0 kB
MemFree:            0 kB
MemAvailable:       0 kB

$ head -3 /proc/meminfo          # en el host, mismo momento
MemTotal:       12012656 kB
MemFree:          469076 kB
MemAvailable:    3273804 kB
```

Lo mismo ocurre con `uptime`, `loadavg`, `stat` y `statm`: el guest recibe texto
sintético constante.

## 2. Por qué es un bug y no una decisión cosmética

- Un agente (o un script de build) que decide con `MemAvailable`/`MemFree` lee
  `0 kB` y concluye que el dispositivo está al borde del OOM. La lectura falsa
  provoca abortar trabajo que cabía, o degradarlo sin motivo.
- El dato no está degradado ni ausente: está **presentado como válido y es
  falso**. Un consumidor no tiene forma de distinguirlo de una máquina real sin
  memoria.
- Herramientas estándar (`free`, monitores basados en `/proc`, scripts que
  calculan paralelismo según RAM) leen ceros sin avisar.
- `proc-leaks.md` acepta cpuinfo real argumentando que no es un leak; por la
  misma lógica, unos totales de máquina reales no son un leak y **sí** son
  necesarios.

## 3. Causa raíz (exacta)

En `proot/src/extension/proc_isolation/proc_isolation.c` (numeración previa al
fix, verificada línea por línea):

| Paso | Símbolo | Línea |
|---|---|---|
| `meminfo` entra en la lista de nombres con "vista guest-safe" | `hpc_is_allowed_proc_root_name()` | 306 |
| `meminfo` se clasifica como sintético | `hpc_proc_synth_kind()` | 443 (caso 482) |
| El texto se genera con ceros hardcodeados | `hpc_proc_synth_text()`, `case PROC_SYNTH_MEMINFO` | 649 (661-663) |
| El fd sintético se registra y la lectura se sirve de ahí | `hpc_track_synth_open()`, `hpc_handle_synth_read_exit()` | 580, 730 |

No hay ninguna ruta que reenvíe la lectura real del host: el contenido se
construye en el filtro, no se lee del procfs. Confirmado con el fd real del
`open`: `/proc/meminfo -> /dev/null` y la lectura sale del texto sintético.

### 3.1 Hallazgo colateral: `cpuinfo` pasaba real por accidente

`PROC_SYNTH_NONE` vale 0 y el enum empezaba con `PROC_SYNTH_CPUINFO`, que por
autoincremento también valía 0. Consecuencias medidas:

- `hpc_proc_synth_kind("/proc/cpuinfo")` devolvía 0 = "no sintético", así que
  `cpuinfo` nunca se redirigía a `/dev/null` y el guest leía el archivo real
  (70-72 líneas reales, `/proc/cpuinfo` como target del fd) — exactamente el
  comportamiento que `proc-leaks.md` documenta como aceptado.
- El `case PROC_SYNTH_CPUINFO` de `hpc_proc_synth_text` era código muerto.

Era un bug latente: cualquier refactor que hubiera "ordenado" el enum habría
convertido `cpuinfo` en ceros sin que ninguna prueba lo notara.

## 4. El workaround del consumidor NO funciona (remedido en device)

Añadir un bind del archivo real **después** del bind de procfs no cambia nada:

```text
--bind=/proc:/proc:rw
--bind=/proc/meminfo:/proc/meminfo:ro     # bind añadido después
```

Resultado medido antes del fix: el guest **seguía leyendo ceros**. La síntesis
ocurre en el filtro por ruta (con el fd sintético ya registrado), no en el
contenido del archivo montado. Consecuencia: ningún cambio en el consumidor
puede arreglar esto; intentarlo es código muerto. **El arreglo pertenece a
proot.**

## 5. Fix aplicado

**Opción A, recortada a lo que Android realmente permite.** El documento original
pedía passthrough de `meminfo`, `uptime`, `loadavg` y los contadores de `stat`.
La medición mostró que eso sólo es posible para `meminfo`:

```text
/proc/meminfo  r--r--r-- root  -> legible por el tracee (host y guest)
/proc/stat, /proc/uptime, /proc/loadavg -> Permission denied (EACCES)
```

Con `uptime`/`loadavg`/`stat` reales el `open()` del guest pasaría de "éxito
sintético" a fallar. Por eso:

- `meminfo` se comporta como `cpuinfo`: no se sintetiza, su open no se redirige a
  `/dev/null` y el guest lee el archivo real del host.
- `uptime`, `loadavg` y los contadores globales de `stat` conservan la vista
  sintética (documentado en `proc-leaks.md`).
- Todo lo per-PID sigue sintético.
- El enum deja de colisionar con `PROC_SYNTH_NONE`: `PROC_SYNTH_STAT = 1` como
  base explícita, `cpuinfo`/`meminfo` sin kind de síntesis y los `case` muertos
  eliminados.

No se añadió el interruptor `--proc-summary-real` de la Opción B: el default
seguro es el mismo que ya rige para `cpuinfo`, y un flag nuevo sólo tendría
sentido en un despliegue multi-tenant.

## 6. Análisis de seguridad (por qué el passthrough es defendible)

- `meminfo` es dato **de máquina**, no de proceso: no revela PIDs, `cmdline`,
  entorno ni trabajo de terceros. No hay fuga per-PID.
- Es la misma clase de dato que `cpuinfo`, ya expuesto y aceptado en
  `proc-leaks.md` (Hallazgo 2).
- Sigue oculto lo que debe estarlo: entradas per-PID, `cmdline`, listado de
  procesos del host (filtro `hpc_is_proot_pid`), `/proc/<pid host>`, `/proc/net`.
- Exposición residual: tamaño de RAM y swap del dispositivo. Irrelevante en
  dispositivo de un solo usuario.

## 7. Regresión añadida

`proot/tests/proot/proc/test_meminfo_machine_summary.sh` (entra por
`./proot/tests/run.sh proot` y por `all`):

- `MemTotal` y `SwapTotal` > 0 e **iguales** a los del host; `MemFree` y
  `MemAvailable` > 0 (no se comparan por igualdad: son volátiles);
- el archivo trae el juego completo de campos del host (`Active(anon)`, etc.);
- `uptime`, `loadavg` y `stat` siguen sintéticos, sin cambio para consumidores
  existentes;
- el aislamiento se mantiene: `/proc/1/status` inalcanzable, el listado no
  incluye PIDs host y un tracee del guest sí es visible (listado y
  `/proc/<pid>/status`), `/proc/self/status` reporta el PID guest y
  `TracerPid: 0`.

Estado: RED verificado con el binario anterior (falla exactamente en los
asserts de totales reales), GREEN tras `./proot/scripts/build-native.sh -i`.

## 8. Estado

- Bug cerrado en REV 103. Verificación en device: test propio PASS=15/15,
  `cpuinfo` sigue real (72 líneas), `self/status` y `mounts` siguen sintéticos.
- `proc-leaks.md` actualizado: `meminfo` pasa a "datos reales del host" y queda
  registrado el hallazgo 3 (ceros) y la nota del enum.
- Mientras REV 103 no esté en un release, un consumidor que necesite RAM real en
  un binario anterior sigue midiendo **fuera** del sandbox (en Hermes:
  `terminal(..., elevate=True)`); no se debe desactivar `isolate_proc` sólo para
  leer memoria.
