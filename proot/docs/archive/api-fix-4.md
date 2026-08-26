# Limpieza de recursos y `ResourceWarning` en `control-api`

La funcionalidad del protocolo y el launcher real ya pasan, pero la suite
Python todavía emite `ResourceWarning` por sockets sin cerrar:

```text
ResourceWarning: unclosed <socket.socket ...>
```

Estos warnings no deben ignorarse: pueden ocultar fugas reales en procesos
persistentes de Hermes y hacen que la suite no sea limpia bajo
`-W error::ResourceWarning`.

## Corrección solicitada en los tests

Los tests que crean `socket.socketpair()` deben cerrar ambos extremos incluso
si la aserción falla. Usar `try/finally` o `contextlib.ExitStack`; no depender
del recolector de basura.

Como mínimo, revisar:

- `test_bad_size`;
- `test_unknown_type_terminal`;
- `test_legacy_first_frame_rejected_and_terminal`;
- `test_invalid_path_terminal`;
- `test_reason_and_ids`;
- `test_serve_does_not_answer_events`;
- cualquier test nuevo que use `socketpair` o lance `ProotProcess`.

Añadir una validación de CI equivalente a:

```bash
python -W error::ResourceWarning -m unittest discover \
  -s control-api/python -p 'test_*.py'
```

## Contrato de ownership de la API

Documentar explícitamente:

- `ControlChannel.from_fd(fd)` toma ownership del descriptor y `close()` lo
  cierra.
- `ControlChannel.from_socket(sock)` debe documentar si toma ownership del
  socket; el comportamiento debe ser consistente con `from_fd`.
- `ProotProcess.close()` debe cerrar el canal, esperar/terminar el proceso y
  cerrar o transferir claramente ownership de stdout/stderr.
- Si `ProotProcess.spawn()` falla después de crear el socketpair, ambos
  descriptores deben cerrarse y el proceso hijo debe terminarse si ya fue
  creado.

## Pruebas de lifecycle solicitadas

Añadir tests para:

- `close()` idempotente;
- spawn fallido sin descriptores abiertos;
- cierre del canal después de EOF/timeout;
- launcher terminado sin procesos hijos vivos;
- suite completa sin `ResourceWarning`.

## Evaluación

Esto no bloquea el uso funcional inmediato de la API: los tests de protocolo,
los launchers y el handshake real ya pasan. Sí bloquea considerar la suite
completamente limpia y lista para CI estricta o sesiones largas de Hermes.
