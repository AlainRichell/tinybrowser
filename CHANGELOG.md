# Novedades

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
