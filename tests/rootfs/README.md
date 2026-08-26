# Wrappers de rootfs

Estos launchers son auxiliares para ejecutar probes y regresiones dentro del
rootfs Alpine de `proot-distro`. No son presets de producción ni forman parte
de `termux-isolated`.

```bash
./tests/rootfs/alpine_rootfs /bin/true
./tests/rootfs/alpine_rootfs_hardened /bin/true
./tests/rootfs/alpine_rootfs_memwd /bin/sleep 1
```

Los probes del checkout se entregan al guest como `/pentest`, desde
`tests/proot/probes/`. Los wrappers resuelven la raíz del repositorio, por lo
que pueden invocarse desde cualquier directorio.

`commands/` contiene comandos auxiliares históricos; `fixtures/captured/`
contiene capturas de entorno y no se ejecuta automáticamente.
