# Monitor de actividad para adultos mayores

Sensor PIR + ESP32-C3 SuperMini que avisa cuando una persona que vive sola **no** se mueve durante un tiempo configurable. Los datos se guardan en Supabase, la configuración y el historial se ven en una app web (PWA) y las alertas urgentes llegan como notificación push por ntfy.

```
ESP32 ──latido + eventos (cada 20 s)──▶ Supabase ◀── PWA (estado, historial, configuración)
ESP32 ◀──configuración (en cada latido)─┘
ESP32 ──alertas──────────────────────▶ ntfy ──push──▶ celular (app ntfy)
```

## Estructura

| Ruta | Contenido |
| --- | --- |
| `sensor_pir_ntfy_cmd__1_.ino` | Firmware del ESP32 |
| `secrets.example.h` | Plantilla de datos privados (copiar como `secrets.h`) |
| `supabase/schema.sql` | Tablas, seguridad (RLS) y funciones de la base de datos |
| `web/` | App web (HTML, CSS y JS sin frameworks + Chart.js) |

## Puesta en marcha

### 1. Supabase

1. Crear una cuenta y un proyecto en https://supabase.com (plan gratuito).
2. En **SQL Editor**, pegar el contenido de `supabase/schema.sql` y ejecutarlo.
3. En **Project Settings → API**, copiar la **Project URL** y la **anon public key**.

### 2. App web

1. Completar `web/config.js` con la URL y la anon key.
2. Probarla localmente sirviendo la carpeta `web/` (por ejemplo `npx serve web` o la extensión Live Server de VS Code) y abriendo `http://localhost:...`.
3. Crear una cuenta, ingresar y registrar el aparato. La app muestra un **token una sola vez**: copiarlo.
4. Publicarla en GitHub Pages (ver la sección siguiente).
5. Opcional: en Supabase, **Authentication → Providers → Email**, desactivar los registros nuevos cuando ya estén creadas las cuentas de la familia.

### 3. ESP32

1. Copiar `secrets.example.h` como `secrets.h` y completar WiFi, tema de ntfy, URL, anon key y el token del aparato.
2. Arduino IDE: placa **ESP32C3 Dev Module**, **USB CDC On Boot: Enabled**. Cargar el programa.
3. En el Monitor Serie (115200) deberían verse `Supabase (boot) -> HTTP 200` y luego los latidos.

### 4. Notificaciones

Instalar la app **ntfy** en el celular y suscribirse al tema definido en `NTFY_TOPIC`.

## Publicar en GitHub Pages

El workflow `.github/workflows/pages.yml` publica la carpeta `web/` en cada push a `main` que la modifique.

1. Crear un repositorio vacío en GitHub (sin README ni .gitignore).
2. Subir el código:
   ```
   git remote add origin https://github.com/<usuario>/<repositorio>.git
   git push -u origin main
   ```
3. En el repositorio: **Settings → Pages → Build and deployment → Source: GitHub Actions**.
4. En **Actions**, volver a ejecutar el workflow si el primer intento falló por no tener Pages activado. La URL queda en `https://<usuario>.github.io/<repositorio>/`.
5. En Supabase, **Authentication → URL Configuration**: poner esa URL en **Site URL** y agregarla en **Redirect URLs**, para que los emails de confirmación lleven a la app publicada.

`secrets.h` está excluido por `.gitignore`: nunca debe subirse.

## Seguridad

- La app web solo ve los aparatos de la cuenta que ingresó (Row Level Security).
- El ESP32 no tiene usuario: se identifica con su token, y en la base solo se guarda el hash SHA-256.
- La anon key de Supabase es pública por diseño; lo que protege los datos son las políticas RLS.
- `secrets.h` no debe compartirse ni subirse a un repositorio.

## Limitaciones conocidas

- Si no hay internet en el momento de un movimiento, ese evento no queda registrado en Supabase (la detección y la alerta local siguen funcionando con la configuración guardada en el ESP32).
- La app muestra "Aparato sin conexión" cuando no recibe latidos, pero no manda un push por eso: requeriría una tarea programada del lado del servidor.
- Para instalar la PWA, algunos navegadores piden íconos PNG de 192 y 512 px además del SVG.
