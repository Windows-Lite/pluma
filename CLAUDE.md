# Pluma

Navegador del usuario (25/10/2026): «uno que no consuma nada o casi nada de RAM, que vaya rápido y que se abran las
páginas instantáneo, que admita extensiones (adblock/uBlock) y los scripts de mi YT-MP3 [...] inspírate en Zas».
Luego: seguro («que no me quiten las contraseñas»), diseño minimalista y moderno como Mapea, transiciones que no vayan
a tirones en ningún PC, oscuro por defecto («es como uso los navegadores») y claro como opción. Castellano de España;
código y commits en inglés; nada de em dashes.

## Piezas
- `Pluma.exe` (C++ + WebView2, el motor de Edge que trae Windows y que Microsoft parchea solo). Una sola instancia que
  se queda en memoria al cerrar la ventana (como Zas): `Pluma.exe [url]`, `--oculto` (arranque con Windows),
  `--salir`; Ctrl+Mayús+Q cierra del todo. Ajustes en `%LOCALAPPDATA%\Pluma\ajustes.ini` (buscador, tema,
  dormir_s, descargar_s, recientes, descargar_min, reserva, residente, protocolos, `motor` = argumentos extra para el
  motor), sesión en `sesion.tsv`,
  historial en `historial.tsv`, iconos de webs en `iconos\`, registro con tiempos en `pluma.log`.
- `src/main.cpp`: ventana con marco propio (las pestañas son la barra de título; los botones de Windows 11 con su menú
  de acoplar vía HTMAXBUTTON), ajustes, historial y autocompletado, arranque.
- `src/tabs.cpp`: una vista WebView2 por pestaña **solo cuando hace falta** (pestaña nueva = ninguna; restaurada = al
  mirarla), dormir y soltar pestañas (vuelven al mismo punto: `saved_x/y`, y YouTube al mismo segundo), recortes de
  memoria (`tabs_trim`), **precarga** (`tabs_preload`: lo que el autocompletado da por seguro mientras se escribe, o un
  sitio de la pestaña nueva bajo el ratón, carga en una vista aparte que pasa a ser la pestaña al pulsar Enter;
  `tab_warm`: una pestaña soltada con el ratón encima empieza a cargar antes del clic), sesión, `ytmp3:` sin preguntar
  (`protocolos`), errores del motor.
- `src/ui.cpp`: la barra dibujada con Direct2D (nada de la interfaz es una página web: no cuesta procesos), menú,
  sugerencias, avisos, pestaña nueva. Diseño de Mapea (sus colores, Roboto Mono en `fuentes/`, radios 10/14).
  Animaciones: un hilo espera a cada refresco de la pantalla (DwmFlush) y pide un fotograma solo mientras algo se mueve.
- `src/scripts.cpp`: scripts de usuario (.user.js) sin extensión, cada uno inyectado aparte; script base de Pluma
  (Ctrl+clic y clic central abren detrás; precarga al posar el ratón, con speculation rules).
- `src/extensions.cpp`: extensiones de Chrome (carpeta `extensiones/` junto al exe, que trae uBlock Origin Lite, y
  `%LOCALAPPDATA%\Pluma\extensiones`), instalar desde la Chrome Web Store o Edge Add-ons con «Añadir a Pluma»,
  ventanita de cada extensión (con un parche para que `tabs.query` vea la pestaña de verdad).
- `instalar.ps1`: accesos, Pluma registrado como navegador (el predeterminado lo elige el usuario en Configuración),
  `-ArrancarConWindows`, `-Quitar`, y mete el botón MP3 de `Documents\YT-MP3`. Los `.ps1` con tildes, con BOM.
- Compilar: `src\compilar.cmd <ruta>` para probar (no cierra nada). `src\compilar.cmd` a secas cierra el Pluma en
  memoria (el del usuario): solo para ponerle la versión nueva, avisándole antes (el 25/10 se lo cerré usándolo).

## Pruebas (nunca con el Pluma del usuario ni sus datos, ni con su Pluma.exe: `PLUMA_EXE=<exe compilado aparte>`, con `extensiones` y `fuentes` enlazadas con `mklink /J` junto a él)
```
tools\prueba.ps1 [-Nombre t1] [-Url ...] [-Puerto 9401] [-Limpio] [-Salir]   instancia de prueba fuera de la pantalla
tools\medir.ps1 [-Nombre t1] [-Detalle]      RAM como la cuenta el Administrador de tareas, por tipo de proceso
tools\captura.exe <clase> <png> [click x y | mclick | rclick | key vk | char texto | wait ms]   foto de la ventana
node tools\cdp.js <puerto> <url> <expresión>  evalúa en una página (DevTools); tools\clic.js: clic de verdad
tools\escenario.ps1 -Nombre x [-Ajustes 'clave=valor',...]   las 7 pestañas de la comparación con Firefox, medidas
tools\tres.ps1 -Nombre x [-Ajustes ...]       3 pestañas (Wikipedia, GitHub, Google), medidas
```
Las medidas varían mucho entre pasadas (la gráfica, de 47 a 120 MB en el mismo escenario): comparar ideas lanzándolas
a la vez y repetir antes de creerse una diferencia pequeña.
Clase de la ventana de prueba: `PlumaVentana.<nombre>`; paneles `PlumaPanel`; datos en `%LOCALAPPDATA%\Pluma\prueba.<nombre>`.
`PLUMA_REPOSO_S=<s>` hace como si nadie tocara el PC desde el segundo s (para medir el recorte en reposo).
Antes de enseñar algo: captura y mirarla.

## La RAM (lo que más le importa; medir antes y después de cada cambio)
Medido el 25/10 en el portátil (i5-10300H, 16 GB, gráfica integrada), privada como el Administrador de tareas:
- Su Firefox con 7 pestañas: 3.100 MB. Pluma con las 7 de `escenario.ps1`: 887-960 MB todas vivas; con las de fondo
  congeladas 214-271; soltando las de fondo salvo las 2 últimas, 140-183. Con 3 pestañas, 134-145.
- Una pestaña ligera (Google): 183 MB usándola, 109 MB tras un rato quieto. Cerrado y en memoria: 13 MB.
- Lo que queda con una web delante: la gráfica 45-120 MB (texturas; en una gráfica integrada son RAM y no se pueden
  devolver: EmptyWorkingSet no le quita nada, y recortarla o avisarle de poca memoria no la baja), el motor 25-70,
  la página (Google ~45, YouTube 220-380, igual que en cualquier navegador), la interfaz 3-15, la red ~10.
- Cómo: pestañas de fondo congeladas a los 45 s (TrySuspend + memoria baja); soltadas al minuto salvo las 2 últimas
  vistas (`recientes`), que aguantan 20 min; caché de imágenes de la gráfica con tope de 32 MB
  (`--force-gpu-mem-discardable-limit-mb`); sin vista de reserva (`reserva=0`: crear una vista son ~35 ms); procesos
  sin nada visible (y el motor, la red y la interfaz 4 s después de cada carga) devuelven su memoria con
  EmptyWorkingSet; al cerrar la ventana se congela todo (lo que suena se suelta) y todo devuelve su memoria.
- Probado y descartado: encoger las vistas congeladas, recortar la gráfica, la gráfica NVIDIA
  (`--force_high_performance_gpu`), un «poca memoria» momentáneo en la página a la vista. Ninguno bajaba nada.
- No hacer: quitar el aislamiento de sitios (seguridad) ni recortar la gráfica o la página a la vista (tirones).

## Seguridad
- Motor de Edge, actualizado por Windows. Páginas en AppContainer (`msWebView2RendererAppContainer`), aislamiento de
  sitios activado (no quitarlo para ahorrar memoria), SmartScreen, prevención de rastreo equilibrada, uBlock Lite.
- Ninguna página puede hablar con Pluma: sin mensajes web ni objetos del host. Contraseñas con el gestor de WebView2
  (cifradas con la cuenta de Windows). Solo abren apps sin preguntar los protocolos de `protocolos` (ytmp3).

## Trampas ya vistas
- uBlock Origin clásico (MV2) se instala en WebView2 155 pero NO bloquea (webRequestBlocking muerto): uBlock Lite sí.
- Google mira la marca «Microsoft Edge WebView2» de los client hints; con la de serie dejó buscar la cuenta (probado
  con un correo inventado). Si un día bloquea: `--disable-features=msWebView2RetainUserAgentMetadataOnSetUserAgent`
  + `put_UserAgent` quita la marca (probado).
- Repintar en bucle (InvalidateRect tras cada fotograma) deja sin turno a los WM_TIMER (Windows solo los da sin
  pintura pendiente): no se cerraba, no guardaba la sesión. Por eso las animaciones van por el hilo de DwmFlush.
- `@match` con `*`: escapar todo menos el asterisco y luego `*` -> `.*` (antes quedaba `/*` y nunca casaba).
- Desde PowerShell, `FindWindowW` con título nulo pide `[NullString]::Value`.
- Abrir Pluma justo mientras el anterior se cierra del todo no hacía nada (le pasaba la url a una ventana que se
  iba): ahora la ventana que sale lleva la propiedad `saliendo` y el nuevo espera a que se vaya el mutex y arranca.
