# Benchmark de `net_policy`

## Resumen

Se midió el coste de la política estática de red para un proceso proot que
ejecuta 1,000 `sendto()` UDP hacia `127.0.0.1`. En la corrida registrada:

| Modo | Mediana por ejecución | Relación contra `off` |
|---|---:|---:|
| `off` | 751.785 ms | 1.000x |
| `allow` + regla explícita | 922.777 ms | 1.227x |
| `deny` | 780.579 ms | 1.038x |

La política `allow` añadió aproximadamente 170.992 ms por ejecución, o
0.171 ms por `sendto()` dentro de este escenario. `deny` añadió
aproximadamente 28.794 ms por ejecución, o 0.029 ms por intento denegado.
Estas conversiones por operación son orientativas porque cada muestra también
incluye el arranque y salida completos de proot, Python y el proceso guest.

## Metodología

El benchmark reproducible es
[`test_net_policy_benchmark.sh`](../../tests/proot/performance/test_net_policy_benchmark.sh).
Ejecuta el launcher real `termux-isolated --termux-paths`, crea un receptor UDP
local para los casos permitidos y mide siete muestras después de una muestra de
calentamiento. Cada muestra crea un proceso proot nuevo y ejecuta:

```text
socket(AF_INET, SOCK_DGRAM)
sendto("x", 127.0.0.1:puerto) × 1000
close()
```

Los casos son:

- `off`: sin extensión activa de política.
- `allow`: `--net-policy allow` y `--net-allow 127.0.0.1:puerto`.
- `deny`: `--net-policy deny`, por lo que cada `sendto()` se rechaza antes de
  llegar al kernel de red.

Para repetir la medición:

```bash
NET_POLICY_BENCH_ITERATIONS=1000 \
NET_POLICY_BENCH_REPEATS=7 \
./proot/tests/proot/performance/test_net_policy_benchmark.sh
```

El script usa `$TMPDIR` para el entorno Termux y no requiere crear archivos
temporales persistentes.

## Interpretación

El coste medido en `allow` corresponde a la ruta de autorización: proot lee la
dirección del guest, clasifica IP/puerto y compara la operación con las reglas.
En `deny`, el rechazo temprano evita parte del trabajo posterior, por eso no es
un sustituto de una medición de tráfico permitido.

El resultado no representa throughput de una conexión persistente. Para ese
caso habría que añadir otro benchmark que mantenga un socket abierto y mida
`send()`/`recv()` sostenidos; esta medición está enfocada en el coste de
mediación por syscall y en procesos cortos que arrancan proot repetidamente.

## Entorno registrado

- Fecha: 2026-08-26.
- Arquitectura: `aarch64`.
- Ejecutor: Termux nativo, mediante `proot/bin/termux-isolated`.
- Iteraciones: 1,000 `sendto()` por muestra.
- Muestras reportadas: 7 por modo, tras una muestra de calentamiento.

Los tiempos son específicos del dispositivo, la versión de Android/Termux,
la carga térmica y la versión de proot. Deben compararse entre modos dentro de
la misma corrida; no deben interpretarse como una garantía universal de
porcentaje.
