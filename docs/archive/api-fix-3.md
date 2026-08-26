# Revisión de integración real: constructor Python de `control-api`

La suite de codec pasa, pero la prueba local contra el binario real de proot
descubrió que el launcher Python documentado no puede utilizarse tal como se
expone en `README.md`.

## Fallo reproducido

El ejemplo documenta:

```python
ProotConfig(
    args=("-r", "/rootfs"),
    guest_command=("/bin/sh", "-c", "echo ok"),
)
```

Pero `ProotConfig` usa `@dataclass` con atributos sin anotaciones de tipo:

```python
@dataclass
class ProotConfig:
    proot_path='proot'; args=(); guest_command=(); env=None; cwd=None
```

Por ello el dataclass no registra esos atributos como campos y el constructor
rechaza cualquier argumento:

```text
TypeError: ProotConfig.__init__() got an unexpected keyword argument 'proot_path'
```

## Corrección solicitada

Declarar todos los campos con anotaciones de tipo y valores predeterminados,
por ejemplo:

```python
@dataclass
class ProotConfig:
    proot_path: str = "proot"
    args: tuple[str, ...] = ()
    guest_command: tuple[str, ...] = ()
    env: dict[str, str] | None = None
    cwd: str | None = None
    timeout: float = 1.0
    keep_stdin: bool = True
    grace_period: float = 0.5
```

Se deben conservar las validaciones actuales del launcher y añadir un test que
construya `ProotConfig` con argumentos, ejecute un guest mínimo y compruebe que
el handshake `HELLO` se completa.

## Resultado de la prueba local

Con asignación manual posterior a `ProotConfig()` el launcher sí funcionó
contra el proot real de Termux:

```text
stdout=integration-ok
returncode=0
```

Por tanto el problema está aislado al constructor público de `ProotConfig`, no
al `socketpair`, `--control-fd` o handshake del launcher.
