# /proc Leak Audit — proot-termux (proc_isolation)

**Fecha:** 2026-08-22 (segunda ronda)
**Entorno:** Hermes sandbox (proot con --proc-isolated)
**Dispositivo:** Android 15 aarch64 (MediaTek)
**Objetivo:** Verificar que proc_isolation filtra correctamente toda la información del host desde /proc
**Nota:** POST-mejoras de hardening. Se incluye verificación de interferencia con PID del proot padre.

---

## Cambios desde la primera auditoría

| Vector | Antes | Después | Estado |
|--------|-------|---------|--------|
| PID listing | 165 PIDs + Android/MTK | 0 PIDs visibles via ls | FIXED |
| Android/MTK entries | visibles (mtk_*, ged, etc.) | eliminados | FIXED |
| /proc/self/mountinfo | topologia real del host | mounts sinteticos proot | FIXED |
| /proc/self/attr/current | u:r:untrusted_app_27:s0:c123,... | u:r:proot:s0 | FIXED |
| /proc/self/limits | limites reales (32768 fd, 44136 procs) | sinteticos (1024 fd, 4096 procs) | FIXED |
| /proc/self/io | stats reales | zeros | FIXED |
| /proc/self/sched | scheduler stats reales | sintetico (0.000000) | FIXED |
| /proc/self/oom_score | 666 | 0 | FIXED |
| /proc/self/pagemap | accesible (1GB) | ENOENT | FIXED |
| /proc/self/syscall | args reales | zeros | FIXED |
| /proc/self/stack | Permission denied | ENOENT | FIXED |
| /proc/self/root | symlink a / | symlink ilegible | FIXED |
| /proc/<hostpid>/* | ESRCH/ENOENT inconsistente | ENOENT consistente | FIXED |
| kill a PIDs | funcionaba parcialmente | No such process | FIXED |

---

## Hallazgo 1 — /proc/self/maps — PID del proot padre visible (INFORMATIVO)

El path del binario prooted contiene el PID del proceso padre:

```
$ cat /proc/self/maps | grep proot
00000000-00000000 r--p ... /data/data/com.termux/files/usr/tmp/prooted-22353-TgPUlG
0000000000-0000000000 r-xp ... /data/data/com.termux/files/usr/tmp/prooted-22353-TgPUlG
```

El sufijo temporal contiene el PID: **22353**.

**Riesgo teorico:** Un atacante podria usar este PID para intentar interferir con el proceso proot padre.

**Veredicto de interferencia:** BLOQUEADA. Todos los vectores de acceso al PID estan protegidos:

| Vector | Resultado |
|--------|-----------|
| kill -0 22353 | No such process (ESRCH) |
| kill -TERM 22353 | No such process (ESRCH) |
| kill -KILL 22353 | No such process (ESRCH) |
| /proc/22353/status | ENOENT |
| /proc/22353/cmdline | ENOENT |
| /proc/22353/exe | ENOENT |
| /proc/22353/maps | ENOENT |
| /proc/22353/fd/ | ENOENT |
| /proc/22353/io | ENOENT |
| /proc/22353/limits | ENOENT |
| /proc/22353/cwd | ENOENT |
| /proc/22353/root | ENOENT |
| /proc/22353/sched | ENOENT |
| /proc/22353/oom_score | ENOENT |
| /proc/22353/stat | ENOENT |
| /proc/22353/statm | ENOENT |
| /proc/22353/wchan | ENOENT |
| /proc/22353/task/.../children | ENOENT |

**Conclusion:** El PID es informacion visible pero NO explotable. El aislamiento es correcto.

---

## Hallazgo 2 — /proc/cpuinfo — Datos reales de CPU (NO ES LEAK)

Muestra datos reales del hardware:

```
$ head -5 /proc/cpuinfo
processor   : 0
BogoMIPS    : 26.00
Features    : fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm lrcpc dcpop asimddp
CPU implementer : 0x41
CPU architecture: 8
```

**Analisis de riesgo:**
- Fingerprinting de hardware: el guest ya corre en ese chip, ya esta expuesto
- Side-channel attacks: el guest ya puede medir tiempos directamente
- Multi-tenant: no aplica (proot personal en dispositivo propio)
- Estado anterior (zeros) rompia herramientas como nproc/lscpu

**Veredicto:** NO es un leak de seguridad. Es el comportamiento correcto para uso personal en dispositivo propio.

---

## Verificación de aislamiento de PIDs internos (guest)

Todos los PIDs del guest tambien estan bloqueados via /proc/<pid>:

| PID | kill -0 | /proc/<pid>/status |
|-----|---------|-------------------|
| 1 (init) | No such process | ENOENT |
| 2 | No such process | ENOENT |
| 1000 | No such process | ENOENT |
| 23056 (shell) | No such process | ENOENT |
| 23059 (hijo) | No such process | ENOENT |

Solo /proc/self funciona para acceder a datos del proceso actual.

---

## Vectores correctamente filtrados

| Vector | Resultado | Estado |
|--------|-----------|--------|
| ls /proc/ | 0 PIDs visibles | OK |
| Android/MTK entries | Eliminados | OK |
| /proc/<cualquier_pid>/* | ENOENT | OK |
| kill -0 a cualquier PID | No such process | OK |
| /proc/version | ENOENT | OK |
| /proc/cmdline | ENOENT | OK |
| /proc/kallsyms | ENOENT | OK |
| /proc/modules | ENOENT | OK |
| /proc/iomem | ENOENT | OK |
| /proc/interrupts | ENOENT | OK |
| /proc/slabinfo | ENOENT | OK |
| /proc/zoneinfo | ENOENT | OK |
| /proc/net/ | ENOENT | OK |
| /proc/config.gz | ENOENT | OK |
| /proc/meminfo | Zeros sinteticos | OK |
| /proc/stat | Zeros sinteticos | OK |
| /proc/loadavg | Zeros sinteticos | OK |
| /proc/uptime | Zeros sinteticos | OK |
| /proc/self/environ | ENOENT | OK |
| /proc/self/stack | ENOENT | OK |
| /proc/self/pagemap | ENOENT | OK |
| /proc/self/mountinfo | Sintetico (proot) | OK |
| /proc/self/attr/current | u:r:proot:s0 | OK |
| /proc/self/limits | Sintetico (1024/4096) | OK |
| /proc/self/io | Zeros | OK |
| /proc/self/sched | Sintetico | OK |
| /proc/self/oom_score | 0 | OK |
| /proc/self/syscall | Zeros | OK |
| /proc/self/root | Symlink ilegible | OK |

---

## Resumen

| # | Hallazgo | Severidad | Estado | Impacto |
|---|----------|-----------|--------|---------|
| 1 | /proc/self/maps (prooted PID) | INFORMATIVO | Corregido en REV 30 | Se eliminan las líneas del loader temporal; los 18 vectores siguen bloqueados |
| 2 | /proc/cpuinfo (datos reales) | N/A | No es leak | Comportamiento correcto para uso personal |

Total: 0 criticos, 0 altos, 1 informativo corregido
Correctamente filtrados: 29 vectores
Interferencia con PID proot: BLOQUEADA (18/18 vectores)
Mejoras desde primera auditoria: 12 vectores corregidos

## Verificación posterior — REV 30

Se corrigió el caso informativo del loader temporal `prooted-<pid>-XXXXXX`.
La build e instalación se realizaron localmente con:

```bash
./scripts/build-native.sh -c -i
```

La prueba local mediante `./bin/termux-isolated --termux-paths` confirmó:

- `grep -E 'prooted-|/libexec/proot/loader' /proc/self/maps` sin resultados.
- `readlink /proc/self/exe` conserva la ruta guest Termux.
- `/proc` y `mountinfo` mantienen la vista guest sintética.
- `bash tests/proot/hardening/test_phase_c.sh`: **PASS=6 FAIL=0**.
- `--no-proc-isolated` es un opt-out explícito (`mountinfo` host visible);
  `./bin/termux-isolated` sin esa opción usa la vista estricta.
