# Lo que queda (contenedores)

Ideas y límites que quedaron fuera de la 1.1.10. Nada de esto está implementado; está ordenado por lo
que más se notaría en uso real.

## Se notaría

- **Borrar o vaciar un contenedor.** Hoy el `.ini` sólo lleva el menú: quitar una fila no toca los datos.
  Vaciarlo es `ICoreWebView2Profile::ClearBrowsingDataAll` (cookies, caché e historial de ese perfil);
  borrarlo entero es `ICoreWebView2Profile8::Delete`, que falla mientras una ventana lo esté usando: hay
  que pasar esa ventana a Default primero, y "otra ventana lo tiene abierto" es una respuesta normal, no
  un error. En el panel sería una entrada del desplegable ("Vaciar este contenedor…") con confirmación en
  la misma línea, como el nombre nuevo.
- **Contenedor Temporal.** `put_IsInPrivateModeEnabled(TRUE)` sobre el mismo objeto de opciones: una
  ventana cuyos datos mueren al cerrarla y no dejan carpeta. Es una línea de motor y una entrada fija en
  el menú.
- **Contenedor por defecto de la máquina.** Hoy la ventana nueva (`Ctrl+Alt+T`) nace en Default. Un
  ajuste `StartContainer=` en la sección `[options]` del `.ini` la haría nacer en el que se elija.
- **Ver el contenedor sobre una página.** La marca con el punto sólo existe en la barra; con una página
  cargada el contenedor se ve recién al abrir el panel de opciones. La tira tiene el ancho medido al
  píxel, así que un punto más ahí obliga a tocar las métricas (`kStripUrl`, `%URLW%`) y el botón de al
  lado.

## Detalles de uso

- **Duplicar a otro contenedor.** `Ctrl+Alt+D` copia dentro del mismo, que es lo esperado; un "duplicar
  en…" sería una entrada más del menú del contenedor, no un atajo nuevo.
- **Renombrar un contenedor.** No hay API para renombrar perfiles: sería copiar la carpeta y recrear el
  perfil. Mejor no ofrecerlo que hacerlo mal.
- **Mayúsculas y acentos.** El nombre escrito es el del perfil: "Español" se guarda como "Espanol", y
  "Default" cae a "sin contenedor" (el perfil Default es la carpeta de siempre; un perfil con ese nombre
  sería la misma carpeta con otra etiqueta). Si molesta, la salida es una etiqueta aparte en el `.ini` y
  el nombre ASCII como identidad.

## Límites que conviene decirle al cliente

- Cada contenedor tiene su propia caché en disco: no se comparte y crece con el uso (el Default de esta
  máquina ya pesa 2,6 GB).
- Todos los contenedores comparten un proceso de navegador: si se cae, se caen todos.
- La IP, el user-agent y el fingerprint son los mismos en todos: separa sesiones, no anonimiza. Es
  exactamente lo que Mozilla dice de sus propios contenedores.
- No hay API para listar perfiles: la lista del menú es la del `.ini`, y un `.to` puede nombrar un
  contenedor que todavía no esté en esa lista (el motor lo crea al abrirlo).

## Prueba manual pendiente

Dos ventanas, `--container Trabajo` y `--container Personal`, una cuenta de Google en cada una; cambiar
de contenedor desde Opciones estando en una página; guardar un `.to` y reabrirlo; `Ctrl+Alt+D`.
