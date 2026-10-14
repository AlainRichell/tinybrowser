# tinybrowser

<img src="tools/icono.png" width="110" align="right" alt="El icono de tinybrowser: una dona pixel-art">

Un navegador de escritorio de **160x90 píxeles**: una ventana sin marco, siempre encima, para
tener un sitio a la vista mientras trabajás en otra cosa. No es un visor de una URL fija — le
escribís una dirección (o buscás) y navega ahí mismo, con el motor de Edge embebido.

**203.776 bytes.** Un solo `.exe`, sin Python, sin .NET y sin la redistribuible de Visual C++:
se copia a cualquier PC con Windows y anda.

## Usar

- Doble clic en `tinybrowser.exe`. Abre pegada a la esquina superior derecha, siempre encima.
- También acepta una dirección: `tinybrowser.exe es.wikipedia.org` (sirve para accesos directos).
- Escribís la dirección y Enter. Sin puntos = búsqueda en Google.
- Cada página se escala sola para que entre a lo ancho: con 90 px de alto, sin eso verías "una
  esquina" de la página. `Ctrl` + rueda para acercar a mano.
- `Ctrl+Alt+L` vuelve a la barra desde cualquier página; `Ctrl+Alt+Q` cierra.
- Para moverla: la franja punteada arriba de la barra, o `Alt` + arrastrar en cualquier parte
  (incluso encima de una página cargada).
- Atrás / adelante / recargar están en la barra; el `x` de la derecha cierra.
- Los enlaces que abren pestaña nueva se cargan en la misma ventana.
- Una sola instancia: si la abrís de nuevo, trae al frente la que ya está.
- Cookies y sesiones viven en `%LOCALAPPDATA%\tinybrowser`, así que los logins persisten.

## Requisitos

- Windows 10 u 11, 64 bits.
- El motor **WebView2** (el mismo de Edge). No va dentro del `.exe`: ya viene en Windows 11 y
  en la mayoría de los Windows 10, porque viene con Edge. Si faltara, al arrancar sale un
  cartel que ofrece instalarlo: busca `MicrosoftEdgeWebview2Setup.exe` al lado del `.exe` y lo
  corre en silencio (~150 KB, copialo si vas a distribuir a PCs viejas), o si no abre la página
  de descarga de Microsoft.

## Compilar

    build.bat

Necesita **Visual Studio Build Tools 2022 con la carga de C++** (compilador `cl.exe`, `rc.exe` y
el SDK de Windows). El build es autocontenido: si falta el SDK de WebView2,
`tools/fetch-sdk.ps1` lo baja de NuGet solo (~10 MB que quedan en `sdk/`, fuera del repo) y
después compila el código y el icono como recurso. Sale un único `.exe` enlazado estáticamente
(`/MT`, `/O2`, `/OPT:REF` e `/OPT:ICF`).

## Arranque

La ventana se pinta al instante (~90 ms, con el fondo oscuro) y el motor queda listo en ~0,6 s
en caliente. La primerísima ejecución tarda más: es el arranque en frío de WebView2 (creación
del perfil), no del programa.

## Cómo está hecho

| Qué | Dónde |
| --- | --- |
| Todo el programa | `tinybrowser.cpp` (Win32 + WebView2) |
| Icono | `tinybrowser.ico` + `tinybrowser.rc`; se regenera con `tools/make_icon.py` |
| Compilación | `build.bat` |
| SDK de WebView2 | lo baja `tools/fetch-sdk.ps1` a `sdk/` (NuGet `Microsoft.Web.WebView2` 1.0.4258.31) |
| Tamaño de la ventana | `kWidth` / `kHeight` |
| Buscador | `kSearchPrefix` |
| Atajos | `kHotkeyBar` / `kHotkeyQuit` |
| Carpeta de datos | `UserDataFolder()` |
| Ajuste de ancho por página | `FitPageWidth()` / `ApplyFitZoom()` |
| Mover la ventana | `MoveBy()` (lo disparan la barra y `Alt`+arrastrar) |
| Normalizar direcciones | `Normalize()` |

Tres decisiones que explican el resto:

- **WebView2 embebido, no empaquetado.** Meter un motor entero daría un `.exe` de decenas de MB;
  como el motor ya vive en Windows, el programa son cientos de KB. El precio es el requisito de
  arriba.
- **Un solo hilo tocando el motor.** El intento anterior en Python se congelaba (AppHangB1)
  porque una capa intermedia llamaba a COM desde otro hilo. Acá todas las llamadas a WebView2
  salen del mismo hilo que el bucle de mensajes.
- **El icono se dibuja por código, no en un editor.** `tools/make_icon.py` genera el `.ico`
  (entradas BMP de 4 bits, paleta VGA de 16 colores, 16/32/48 px, 2,7 KB en total) y además un
  pliego con todas las variantes en `tools/preview.png`, para poder comparar y volver atrás sin
  depender de la memoria.

## Estado

Anda: navegar, buscar, atrás/adelante/recargar, scroll, zoom, arrastre, atajos globales, instancia
única, sesiones persistentes, icono propio y cierre limpio (sin procesos huérfanos).

Todavía no: historial navegable, autocompletado en la barra, pestañas ni diálogos para elegir el
perfil. El zoom se reajusta por página, así que una página muy ancha queda con letra chica.

## Licencia

MIT — ver [`LICENSE`](LICENSE).
