# Solicitud de corrección: PRCT debe interceptar lecturas externas

Esta incidencia afecta la política de filesystem de Hermes. El fork actual
solo emite `PATH_ACCESS_REQUEST` cuando la política estática de bindings
rechaza una operación. Hermes monta el root del sistema como `:ro`, por lo
que una lectura externa es estáticamente válida y atraviesa proot sin
aprobación.

## Fallo reproducido

Con el backend proot de Hermes, sin callback de aprobación interactivo:

```python
env = ProotFreshEnvironment(cwd="<cwd-del-sandbox>", timeout=5)
result = env.execute("cat /etc/hosts")
```

Resultado observado:

```text
returncode = 0
```

No llega ningún `PATH_ACCESS_REQUEST` al harness. `/etc/hosts` no está dentro
del CWD ni de las raíces runtime permitidas automáticamente por Hermes; por
tanto debería requerir aprobación. El mismo problema afecta metadatos y
lecturas de cualquier ruta externa visible bajo el bind raíz `/:ro`.

## Causa

En `proot-source/src/path/path.c`, el flujo actual es esencialmente:

```c
status = check_binding_access(tracee, requested_guest_path, is_write);
if (status < 0)
    status = net_policy_path_access(...);
```

`net_policy_path_access()` solo se llama después de un error estático
(`EROFS`, shadow o política de binding). Una lectura de un bind `:ro` no
produce error, así que nunca se consulta PRCT.

El diseño actual protege las escrituras, pero no puede expresar la política
de Hermes: “CWD y raíces runtime seleccionadas automáticas; toda ruta externa
requiere aprobación, incluso READ/METADATA”.

## Corrección solicitada

Añadir un modo explícito de control de paths mediante PRCT, habilitado cuando
exista un `--control-fd` válido y el handshake haya terminado. En ese modo:

1. Cada operación de filesystem relevante debe pasar por una decisión PRCT:
   `READ`, `METADATA`, `WRITE`, `CREATE`, `DELETE` y `RENAME`.
2. La solicitud debe emitirse con la ruta guest canónica y `other_path` para
   rename/link cuando corresponda.
3. La decisión PRCT debe aplicarse aunque el binding estático sea `:ro` y la
   operación sea una lectura que normalmente tendría éxito.
4. Si el canal no está listo, falla, expira o se desincroniza, la operación
   debe fallar cerrada.
5. La política estática de bindings debe continuar aplicándose: una
   aprobación PRCT de WRITE/CREATE/DELETE/RENAME no convierte un bind `:ro`
   en `:rw`.
6. Debe existir una forma segura de evitar prompts repetidos para
   infraestructura conocida sin permitir lecturas externas por accidente.
   Puede ser una combinación de reglas estáticas guest explícitas y PRCT,
   por ejemplo:

   - CWD actual: permitido automáticamente;
   - `/system`, `/vendor`, `/apex`, `/odm`, `/proc`, `/dev` y `$PREFIX`:
     READ/METADATA automáticos;
   - cualquier otra ruta: siempre PRCT.

   La selección de estas raíces no debe derivarse de rutas host ni aceptar
   reglas arbitrarias enviadas por el guest.

## Compatibilidad y seguridad

- No resolverlo cambiando `--net-policy` ni desactivando el control API.
- No convertir todo el root bind en `:rw`.
- No hacer que una respuesta `ALLOW_ALWAYS` cree un bind `:rw`; esa decisión
  solo debe autorizar la operación PRCT durante la sesión.
- Si se añade una opción CLI, debe documentar que `--control-fd` es necesario
  y que sin handshake el resultado es deny.
- Las solicitudes y logs deben contener únicamente rutas guest; nunca rutas
  host derivadas.

## Regresiones requeridas

Con el proot real de Termux y temporales bajo `$TMPDIR`:

- READ externo sin callback: `DENY_ONCE` y guest bloqueado;
- READ externo aprobado una vez: funciona solo esa solicitud;
- READ externo con `ALLOW_ALWAYS`: funciona durante esa sesión proot;
- READ dentro de CWD: funciona sin ventana;
- READ/METADATA de raíces runtime: funciona sin ventana;
- WRITE externo aprobado por PRCT pero bind `:ro`: sigue fallando físicamente;
- `RENAME` externo muestra ambos paths guest y requiere aprobación;
- shadowed path genera evento `SHADOW_EVENT`, nunca una autorización implícita;
- EOF, timeout o frame inválido del canal termina el guest;
- múltiples lecturas dentro del CWD no generan prompts ni fugas de sockets.

La prueba Python de Hermes `test_external_path_requires_approval` debe dejar
de ser una prueba solamente del harness y comprobar una lectura real contra el
binario de proot. La suite debe ejecutarse con `-W error::ResourceWarning` y
sin procesos proot huérfanos.

## Criterios de aceptación

- `cat /etc/hosts` sin callback ya no devuelve éxito silenciosamente.
- Una lectura externa aprobada llega a PRCT antes de que el guest reciba los
  datos.
- CWD y raíces runtime siguen siendo utilizables sin interacción.
- PRCT no reemplaza la protección física de los binds.
- Hermes puede mantener su harness actual sin implementar un segundo codec.
