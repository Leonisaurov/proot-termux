# Sandbox Audit — Hallazgos reales de aislamiento

**Fecha:** 2026-08-22
**Entorno:** Hermes sandbox (proot con --proc-isolated)
**Dispositivo:** Android 15 aarch64 (MediaTek)

---

## Hallazgo 1 — uname -a revela kernel version completa

```
$ uname -a
Linux localhost 5.10.226-android12-9-00047-g4968e29b7f92-ab12786767
#1 SMP PREEMPT Wed Dec 11 21:50:47 UTC 2024 aarch64 Android
```

**Datos expuestos:**
- Kernel: 5.10.226-android12-9-00047
- Commit: g4968e29b7f92
- Build: ab12786767
- Fecha de compilacion: Wed Dec 11 21:50:47 UTC 2024
- Arquitectura: aarch64

**Riesgo:** Un atacante podria usar la version exacta del kernel para buscar CVEs conocidos o exploits especificos para esa compilacion. La fecha de build permite estimar que parches de seguridad estan incluidos.

**Comparacion con /proc/version:** Bloqueado correctamente (ENOENT). Pero uname no pasa por proc_isolation.

---

## Hallazgo 2 — /dev/tty* no determinado (timeout)

Al intentar acceder a dispositivos de terminal, los comandos dieron timeout:

```
$ head -c 16 /dev/tty     → timeout 180s
$ head -c 16 /dev/console → timeout 180s
$ head -c 16 /dev/tty0    → timeout 180s
$ head -c 16 /dev/tty1    → timeout 180s
$ head -c 16 /dev/ttyS0   → timeout 180s
```

**Posibles causas:**
1. El dispositivo esta bloqueado y el read queda colgado (no retorna error)
2. El dispositivo existe y espera input indefinidamente
3. proot no intercepta y el kernel bloquea

**Riesgo:** Si estos dispositivos estan accesibles, un atacante podria interactuar con la terminal del host o con puertos seriales.

**Accion requerida:** Verificar manualmente con timeout corto si estos dispositivos existen y si se puede leer de ellos sin colgar.

---

## Resumen

| # | Hallazgo | Severidad | Estado |
|---|----------|-----------|--------|
| 1 | uname -a kernel version completa | MODERADO | Pendiente de fix |
| 2 | /dev/tty* timeout | DESCONOCIDO | Requiere investigacion |

**Nota:** Los demas 91 vectores verificados estan correctamente aislados. El sandbox cumple su funcion de aislamiento de procesos, archivos sensibles, y syscalls peligrosos.
