# Novedades

## 1.1.6 - 2026-11-05

- **`Ctrl+Alt+S` ya no te saca de la página.** En una ventana con página cargada abre una tira de una
  sola línea, la pantalla de inicio comprimida, flotando 4 px por encima de la ventana: algo más
  estrecha que ella, encima de las demás ventanas y sin tocar el tamaño ni las colisiones. Las
  opciones que no caben se recorren en horizontal, con la rueda. El `Ctrl+Alt+S` la muestra y la
  oculta como un interruptor, y en cuanto el ratón no está encima baja a media transparencia, con una
  transición corta, y vuelve a opaca al pasar por ella. En una ventana sin página, el atajo sigue
  abriendo la pantalla de inicio de siempre.
- La versión sube a **1.1.6**.

## 1.1.5 - 2026-11-05

- **`Ctrl+Alt+T` abre una ventana nueva aunque tiny no esté abierto.** Instalar deja un proceso
  invisible que arranca con Windows y espera ese atajo, sin ventana, sin motor y sin tocar la página
  que tengas delante. El atajo se registra como atajo global del sistema: si otro programa ya lo tiene
  tomado, gana ese y tiny no se queda con las teclas. Desinstalar lo apaga y borra el arranque
  automático junto con todo lo demás, así que no hay nada que configurar aparte.
- La versión sube a **1.1.5**.

## 1.1.4 - 2026-11-05

- **Los archivos del escritorio ya no parpadean al abrir una ventana nueva o duplicar.** Cada arranque
  revisaba la asociación de `.to` y avisaba a Windows aunque no hubiera cambiado nada, y Explorer
  recargaba iconos como si se pulsara F5. Ahora solo avisa cuando la asociación realmente cambia.
- La versión sube a **1.1.4**.

## 1.1.2 - 2026-11-04

- **La ventana se queda encima de la barra de tareas.** Al hacer clic en la barra, al hacer clic en el
  escritorio o al pulsar `Win+D`, tiny vuelve a ponerse delante en un frame en vez de quedarse detrás o
  de tardar unos 110 ms en recuperarse. El menú Inicio y el panel de notificaciones siguen pudiendo
  taparla, que es lo correcto.
- **Seleccionar texto arrastrando vuelve a funcionar.** El motor recibía los movimientos del ratón sin
  el botón pulsado y los tomaba por un simple hover; ahora cada evento lleva el estado real de los
  botones (y `Shift`/`Ctrl`, para extender una selección).
- **Arrastrar archivos a la página vuelve a funcionar en el modo normal.** Mientras no haya
  transparencia activa, la ventana usa el motor con ventana, que arrastra archivos de forma nativa; al
  elegir un color con la pipeta cambia al motor de composición, el único que puede hacer transparente
  un color. Ese cambio recarga la página de esa ventana y conserva su sitio, su tamaño y el zoom. En
  modo transparencia el arrastre de archivos sigue sin funcionar: es una limitación del motor de
  WebView2 en composición (issue #5124), no de la app.
- **Quitar ya no falla.** La desinstalación borra también `tinybrowser.ini` de la carpeta instalada: sin
  ese archivo la carpeta no quedaba vacía y Windows se negaba a eliminarla. Si otra copia está corriendo
  desde el exe instalado, se aparta a `%TEMP%` como ya se hacía con la propia.
- La versión sube a **1.1.2**: el panel de instalación la ofrece como **Actualizar** sobre la 1.1.1.

## 1.1.1 - 2026-11-04

- **`Ctrl+Alt+W` oculta una sola ventana**, la que estás usando, y el siguiente `Ctrl+Alt+W` devuelve
  esa misma en vez de esconder otra: un conmutador de una a la vez. `Ctrl+Alt+C` queda para todas
  (esconde si hay alguna a la vista, muestra si no hay ninguna).

## 1.1.0 - 2026-11-04

- **El icono vive en el área de notificaciones.** La ventana ya no ocupa un botón en la barra de tareas:
  queda **un solo icono para toda la app**, aunque haya varias copias abiertas. El clic izquierdo muestra
  u oculta todas (lo mismo que `Ctrl+Alt+C`), el clic derecho abre el menú con mostrar/ocultar todas,
  ventana nueva y salir, y el tooltip dice cuántas ventanas hay.
- En Windows 11 el icono nuevo aparece dentro del desplegable `^` del área de notificaciones hasta que lo
  arrastres una vez a la vista (o lo actives en Configuración → Personalización → Barra de tareas).
- **Opción "Solo en notificaciones"**, encendida de fábrica. Se cambia desde el panel de opciones y se
  aplica al momento, sin reiniciar: vale para todas las ventanas (`TrayOnly` en `tinybrowser.ini`).
  Apagada, el botón de la barra de tareas vuelve.
- El icono vuelve solo si el Explorador se reinicia, y si cierras la copia que lo tenía otra copia lo
  toma en el acto; se va con la última.
- La versión sube a **1.1.0**: el panel de instalación la ofrece como **Actualizar** sobre la copia
  1.0.0 que esté instalada.
