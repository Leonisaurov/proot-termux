# Benchmarks de rendimiento

Esta página reúne los benchmarks ejecutables actualmente disponibles en el
repositorio. Las cifras son específicas del dispositivo y deben compararse
entre modos dentro de una misma corrida.

## `termux-isolated` persistente

Script: [`test_termux_isolated_benchmark.sh`](../../tests/termux-isolated/performance/test_termux_isolated_benchmark.sh)

Mide, dentro de sesiones interactivas persistentes con pseudo-terminal, el
tiempo de 5,000 ejecuciones de `/bin/true`. La comparación entre las dos
variantes aisladas usa 10 pares: el orden se aleatoriza de forma reproducible,
se descarta un par de calentamiento y se reportan las 10 parejas restantes.
Compara una shell Termux directa con
`termux-isolated --termux-paths`, activando y desactivando explícitamente
`--proc-isolated`.

Corrida del 2026-08-26, en Termux/aarch64:

| Caso | Tiempo | Relación contra shell directa |
|---|---:|---:|
| Shell Termux directa | 0.013 s | 1.000x |
| `termux-isolated --proc-isolated` | 0.047 s | 3.557x |
| `termux-isolated --no-proc-isolated` | 0.060 s | 4.478x |

Repetición:

```bash
bash ./proot/tests/termux-isolated/performance/test_termux_isolated_benchmark.sh
```

El benchmark mide el intervalo de los comandos una vez establecida la sesión;
incluye la interacción con la shell persistente, pero no el arranque inicial
de cada sesión en el intervalo medido. El `sleep(0.5)` previo solo permite que
la sesión esté lista. En este launcher `--proc-isolated` no es el valor
predeterminado, por lo que ambos flags se pasan explícitamente para que la
comparación sea válida. La corrida observó `0.040–0.052 s` con proc isolation
y `0.049–0.071 s` sin ella. En las 10 parejas, proc isolation fue más rápido
en las 10; la prueba de signos pareada unilateral produjo `p=0.000977` para
la hipótesis de que `--no-proc-isolated` es más lento. Esto demuestra el efecto
en este workload y dispositivo, pero no prueba que proc isolation acelere
aplicaciones arbitrarias.

## Ejecuciones anidadas

Script: [`test_nested_benchmark.sh`](../../tests/proot/performance/test_nested_benchmark.sh)

Mide ejecuciones frías y repetidas de proot anidado. La profundidad 1 es la
invocación directa; las profundidades 2 y 3 agregan una capa adicional por
nivel mediante [`nested_helper.sh`](../../tests/proot/performance/nested_helper.sh).
Con `NESTED_BENCH_FULL=1` se prueban cuatro comandos (`true`, `sh`, `ls` y
`readlink`) en vez de solo `true`.

```bash
NESTED_BENCH_FULL=1 bash \
  ./proot/tests/proot/performance/test_nested_benchmark.sh
```

### Resultados observados

Corrida del 2026-08-26, en Termux/aarch64. Los tiempos están en segundos:

| Profundidad | Comando | Fría | Repetida |
|---:|---|---:|---:|
| 1 | `true` | 0.212995 | 0.168535 |
| 1 | `sh` | 0.142327 | 0.151838 |
| 1 | `ls` | 0.155642 | 0.162728 |
| 1 | `readlink` | 0.164084 | 0.163154 |
| 2 | `true` | 2.122117 | 2.712218 |
| 2 | `sh` | 1.846460 | 1.925618 |
| 2 | `ls` | 2.204659 | 2.190789 |
| 2 | `readlink` | 2.065308 | 2.052998 |

En esa corrida el script se detuvo al iniciar la profundidad 3, antes de
obtener una muestra válida de esa profundidad y antes de ejecutar el caso
`--supervise/--exec`. Por ello no se presenta un resultado inventado para
esas mediciones; el script usa `set -e` y aborta ante el primer caso fallido.

Estos tiempos incluyen crear las instancias de proot, sus binds y la shell del
guest. No son una medición de throughput ni de latencia de una aplicación
persistente.

## Política de red

La medición específica de `net_policy`, con `off`, `allow` y `deny`, está en
[`net-policy-benchmark.md`](net-policy-benchmark.md).

## Reglas de ejecución

Todos estos scripts resuelven temporales mediante `$TMPDIR`; en Termux debe
ser una ruta escribible del entorno nativo. Los benchmarks que ejecutan proot
deben lanzarse en el entorno Termux real, no dentro del sandbox del agente.
