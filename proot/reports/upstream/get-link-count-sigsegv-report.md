# SIGSEGV en `get_link_count()` (introducido por `05a93edfc8`) con `-O3 -flto`

**Estado:** borrador para reportar aguas arriba. No publicado. Lo encontró este fork al portar la serie de
link2symlink a `v5.1.107.99`, y está corregido en el commit `009340ab` de la rama `sync/upstream-5.1.107.99`.

## Resumen

`get_link_count()` en `src/extension/link2symlink/link2symlink.c` recorre los cuatro dígitos previos al punto
con un índice derivado de un `size_t`:

```c
	size_t i;
	...
	for (i = 1; i <= 4; i++) {
		if (!isdigit((unsigned char) dot[-i]))
			return -1;
	}
```

`-i` sobre un `size_t` produce el valor envuelto `2^64 - i`, y `dot + (2^64 - i)` es aritmética de punteros
con desbordamiento: comportamiento indefinido. Con el build oficial de Termux (NDK r30, clang 21.1.8,
`-O3 -flto -ffunction-sections -fdata-sections`) el compilador materializa el primer acceso como una
**constante** en lugar de `dot - 1`, y la load apunta a `0xFFFFFFFFFFFFFFFF`:

```
   0x55555a8d4c <get_link_count+76>:  mov  x8, #0xffffffffffffffff
=> 0x55555a8d50 <get_link_count+80>:  ldrb w8, [x8]
```

El resultado es un SIGSEGV de PRoot (no del tracee) al **abrir** un enlace fingido.

## Cómo reproducir

Android aarch64 sin root, Termux, proot construido con las banderas del paquete oficial
(`-O3 -flto`); falla en la punta `968ae1be84` y también en `05a93edfc8`; **no** falla en `03aea83617`.

```sh
mkdir -p /tmp/l2s/links && printf 'content\n' > /tmp/l2s/links/o
proot --link2symlink sh -c '
  cd /tmp/l2s/links && ln o l && exec 3< l && readlink /proc/self/fd/3'
# proot warning: signal 11 received from process -1
```

Sin `PROOT_L2S_DIR`, es decir con la nominación por defecto (`<dir>/.l2s.<nombre><NNNN>[.<count>]`). La ruta
`readlink(/proc/self/fd/N)` de un enlace fingido ya existía antes de `05a93edfc8`; lo que cambia es que ahora
pasa por `get_link_count()`.

Pila observada (gdb, sobre un binario no despojado de símbolos):

```
#0 get_link_count ()
#1 remember_opened_link ()
#2 link2symlink_callback ()
#3 translate_path ()
#4 translate_path2 ()
#5 translate_syscall_enter ()
```

Detalle relevante para quien lo intente reproducir: un *probe* aislado con esa misma función compilada con
`-O3 -flto` **no** reproduce. Hace falta el contexto de inlining del binario completo (`is_l2s_file()` y
`l2s_link_to_host_path()` se fusionan con el llamador), así que hay que construir el proot entero.

## Consecuencia

No es un fallo de sintaxis de un caso raro: `exec 3< enlace` es algo que hacen shells y utilidades reales
dentro del guest, y el proceso que muere es PRoot mismo. En Termux/Android rompe `link2symlink` para
cualquier binary construido con ese toolchain.

## Arreglo propuesto

Índice con signo (no envuelto) y comprobación explícita del punto, que hoy sólo se descarta por una
comentario que asume que `PREFIX` termina en punto:

```c
	const char *dot;
	...
	dot = strrchr(name, '.');
	if (dot == NULL)
		return -1;
	if ((size_t) (dot - name) < strlen(PREFIX) + 4)
		return -1;

	for (i = 1; i <= 4; i++) {
		if (!isdigit((unsigned char) dot[0 - (ssize_t) i]))
			return -1;
	}
```

El cambio de código generado es local a este bucle; el comportamiento observable no varía en ningún caso
válido. Aplicado en este fork en `009340ab` con la regresión
`tests/proot/syscalls/test_link2symlink_link_count.sh` (caso A: descriptor de un enlace fingido en nominación
por defecto; sin el arreglo, SIGSEGV).

## Nota sobre la otra mitad de `05a93edfc8`

La lectura de conteos de 10000 o más (`length` entre 4 y 9 dígitos) es correcta y este fork la portó tal
cual; sólo el bucle de validación de los 4 dígitos del sufijo del intermedio tiene el problema de índice.
