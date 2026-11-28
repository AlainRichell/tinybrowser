# Novedades

## 1.1.15 - 2026-11-27

- **Los dos botones laterales del mouse navegan.** El de atrás hace lo mismo que `Ctrl+Alt+Z` y el de
  adelante va al otro lado, con el puntero sobre la ventana o sobre la tira. En la pantalla de inicio
  no hacen nada: ahí no hay página que recorrer.
- Cuenta la ventana que está debajo del puntero, no todas: si tiny está tapado por otra aplicación, el
  botón va a la de delante, como en cualquier navegador.
- La versión sube a **1.1.15**.

## 1.1.14 - 2026-11-27

- **La barra de inicio pierde la franja de puntos y el alto que ocupaba.** La ventana de inicio pasa
  de 141×77 a **141×66**: los dos botones de arriba quedan igual, con la lupa encima de la X, y el
  espacio que gastaba la franja ahora no está.
- La ventana se sigue moviendo desde la barra arrastrando sobre cualquier parte vacía de ella (la
  línea de la marca, los huecos entre botones); los botones y los paneles no arrastran.
- La versión sube a **1.1.14**.

## 1.1.13 - 2026-11-27

- **La pantalla de inicio se ve igual en cualquier monitor.** El ancho de la barra (141 px) y el de
  la página (160×90) son medidas de diseño, no píxeles del monitor. En un portátil al 125 % o 150 %
  la barra se abría con menos sitio del que necesita: los botones de la última fila quedaban cortados
  y la lupa descuadrada de la X. Ahora la ventana se abre al tamaño que le toca en cada pantalla, con
  todos los botones a la vista y la lupa justo encima de la X.
- Una ventana que se arrastra a un monitor con otro escalado toma el tamaño que Windows propone, así
  que no se encoge ni se recorta al cambiar de pantalla. La tira flotante sigue la misma regla.
- **La línea de la marca nombra también el perfil Default**, con un punto gris neutro. Antes quedaba
  vacía justo en las ventanas que no eligen contenedor, que son casi todas.
- La versión sube a **1.1.13**.

## 1.1.12 - 2026-11-23

- **Ctrl+clic en un enlace abre otra ventana de tiny.** El enlace se abre en una instancia nueva,
  del mismo tamaño y en el primer hueco libre al lado (el mismo reparto que `Ctrl+Alt+D`), en el
  mismo contenedor y con las mismas esquinas. La página nueva arranca limpia: no hereda los colores
  transparentados de la ventana desde la que hiciste clic.
- El gesto cuenta solo si el Ctrl+clic fue real. Una página que llame a `window.open` por su cuenta
  sigue cambiando la ventana actual, como antes; `target="_blank"` con clic normal también.
- Si no hay hueco libre no se abre nada, igual que al duplicar. Los enlaces que no son `http`/`https`
  (`mailto:`, `javascript:`, `blob:`) no abren ventana.
- La versión sube a **1.1.12**.

## 1.1.11 - 2026-11-23

- **Esquinas redondeadas por ventana.** El panel de Opciones tiene un deslizador **Esquinas** de 0 a
  100: en 0 la ventana sigue siendo el rectángulo de siempre, y en 100 cada esquina toma el radio
  máximo que permiten cuatro esquinas iguales —la mitad del lado más corto—, así que una ventana de
  160×90 queda como una cápsula y una de 60×60 como un círculo. Se ve mientras se arrastra, sin
  recargar la página.
- Es una propiedad de la ventana, como el pin: cada una tiene la suya, el `.to` la guarda, Ctrl+Alt+D
  la copia y `tiny.exe --radius N` la abre desde un acceso directo. No se escribe en el `.ini`.
- La barra de inicio limita su propio radio a 10 px para que el campo de dirección y los botones no
  queden cortados por la forma.
- La versión sube a **1.1.11**.

## 1.1.10 - 2026-11-20

- **Contenedores: una ventana puede tener todos sus datos aparte.** Cada ventana puede vivir en un
  contenedor con nombre, y el contenedor es un perfil del motor dentro de la misma carpeta de datos:
  cookies, sesiones, almacenamiento, caché, permisos e historial quedan separados, así que dos cuentas
  de Google pueden estar abiertas al mismo tiempo sin verse entre sí. No cuesta un navegador extra por
  contenedor, que es lo que costaría una carpeta de datos aparte.
- El contenedor se elige en el panel de Opciones —con la lista de los que ya existen y un nombre nuevo
  escrito en la misma línea— o desde el panel sobre la página. Cambiarlo recarga esa página en el otro
  perfil; la ventana, su tamaño y su zoom se quedan.
- La línea de la marca de la barra muestra el contenedor con su punto de color (nada en el perfil
  Default), el `.to` lo guarda y Ctrl+Alt+D duplica dentro del mismo contenedor. Desde un acceso
  directo también sirve `tiny.exe --container Trabajo`.
- Las ventanas que no eligen contenedor siguen en el perfil Default, que es la carpeta de siempre: no
  hay nada que migrar y no se pierde ninguna sesión.
- La versión sube a **1.1.10**.

## 1.1.9 - 2026-11-11

- **El clic ya no se lo queda tiny cuando la ventana está sin fijar y la tapa otra.** El programa
  escucha el mouse en todo el escritorio para poder redimensionar desde cualquier borde, y bastaba
  con que el punto del clic cayera dentro del rectángulo de tiny —aunque por delante hubiera otra
  ventana— para tragarse ese clic y convertirlo en un arrastre de borde: la ventana tapada se
  redimensionaba sola y el clic no llegaba a la aplicación de delante. Ahora el gesto solo empieza
  si el clic cae de verdad sobre tiny. En las ventanas fijadas no cambia nada, porque nada puede
  taparlas.
- **El cursor de redimensionar ya no depende de una clase en `<html>`.** Una web que reescribe esa
  clase en cada repintado —las que se redibujan solas lo hacen— dejaba los bordes sin cursor hasta
  sacar el puntero y volver a entrar. El cursor ahora vive en una hoja de estilo propia que la
  página no toca, y vuelve a ponerse sola si el repintado se la lleva.
- La versión sube a **1.1.9**.

## 1.1.8 - 2026-11-05

- **Cada ventana se puede fijar o dejar de fijar, con un botón de chincheta en la tira**, a la
  izquierda del aspa. Fijada es lo de siempre: por encima de todo. Sin fijar, la ventana pasa a ser una
  ventana normal de Windows: otras pueden taparla y, para no perderla cuando la tapa una maximizada,
  gana su hueco en la barra de tareas y en Alt+Tab. La tira se va con ella en el orden de capas.
  Empieza fijada en cada ventana nueva y es cosa de cada ventana, no una opción de la aplicación.
- **El `.to` guarda el pin.** `Ctrl+Alt+G` lo escribe y al abrir el archivo la ventana vuelve fijada o
  sin fijar, como estaba. Los `.to` viejos, que no lo dicen, abren fijados como siempre.
- La tira baja sus botones a 18 px para que la chincheta entre sin que se recorte nada: con siete
  botones mide 146 px y sigue cabiendo en la ventana de 160.
- La versión sube a **1.1.8**.

## 1.1.7 - 2026-11-05

- **La tira ya no se cierra al pulsar un botón.** Navegar, ir atrás o adelante cambia la página por
  debajo y la tira se queda donde está, con el campo siguiendo la dirección de la página (también si
  cambias de página con un enlace).
- **Opciones, ayuda e instalar se abren encima de la página, sin irse de ella.** El panel aparece como
  una capa sobre lo que estás viendo; al cerrarlo vuelves exactamente al mismo sitio, con el scroll y
  el estado de la página intactos: no navega y no recarga. Pulsar el mismo botón otra vez lo cierra, y
  también el aspa, un clic fuera o `Esc`.
- **La tira solo la quitan el atajo `Ctrl+Alt+S` y el aspa**, que además cierra la ventana.
- **Las flechas de atrás y adelante se apagan cuando no hay a dónde ir**, en vez de estar siempre
  encendidas: las consulta al motor después de cada página y al mostrar la tira. La de atrás no se
  enciende en el primer documento de la ventana, aunque la propia página ensucie su historial al
  cargar (WhatsApp Web lo hace): atrás solo aparece desde la segunda página de verdad.
- **Y se puede volver al comportamiento anterior.** En Opciones hay una casilla nueva, *La tira queda
  abierta*, marcada por defecto; al desmarcarla la tira se quita en cada pulsación, como antes. Es una
  opción de la app, así que todas las ventanas la comparten.
- La versión sube a **1.1.7**.

## 1.1.6 - 2026-11-05

- **`Ctrl+Alt+S` ya no te saca de la página.** En una ventana con página cargada abre una tira de una
  sola línea, la pantalla de inicio comprimida, flotando 4 px por encima de la ventana y centrada. La
  tira mide lo que miden sus botones, nunca más que la ventana: los botones mandan, y el campo de URL
  (con su botón de ir) solo aparece si la ventana es lo bastante ancha para que quepa todo; si no,
  simplemente no está. Encima de las demás ventanas y sin tocar el tamaño ni las colisiones. Lo que
  aun así no quepa se recorre en horizontal, con la rueda. El `Ctrl+Alt+S` la muestra y la oculta como
  un interruptor, y en cuanto el ratón no está encima baja a media transparencia, con una transición
  corta, y vuelve a opaca al pasar por ella. En una ventana sin página, el atajo sigue abriendo la
  pantalla de inicio de siempre.
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
