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
- `src/video.cpp`: los vídeos de YouTube en el reproductor propio de Pluma (apartado de abajo).
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
node tools\clics.js <puerto> <url> <n>        lo que tarda un clic en llegar a la página y en verse (ms)
node tools\tirones.js <puerto> <url> <s>      fotogramas lentos al hacer scroll; tools\scroll.ps1 lo pasa por 3 webs
node tools\rueda.js <puerto> <url> <pasos>    scroll con la rueda de verdad; con captura.exe ... burst n ms y
                                              tools\hoja.py, una hoja de fotogramas para ver qué parpadea
tools\grabar.exe <s> <carpeta> [ancho]             graba la pantalla de verdad a su frecuencia (Desktop Duplication
                                              en la gráfica de la pantalla; va con preferencia «ahorro» en Windows)
tools\rafaga.ps1 [-Fotos 14] [-Cada 35] [-Acciones ...]  fotos seguidas hechas por Pluma (lentas: ~0,4 s cada una,
                                              sirven para ver estados, no fluidez; los tiempos, en pluma.log)
tools\foto.ps1 [-Nombre yt] -Salida x.png [-Acciones 'move x,y','click x,y','key 0x4B','char <','wait ms']
                                              foto hecha por Pluma con los controles del reproductor (captura.exe
                                              los saca opacos); las acciones van al reproductor y a sus controles
```
En las pruebas la pantalla completa (tecla F al reproductor) se queda fuera de la pantalla.
Las medidas varían mucho entre pasadas (la gráfica, de 47 a 120 MB en el mismo escenario): comparar ideas lanzándolas
a la vez y repetir antes de creerse una diferencia pequeña.
Clase de la ventana de prueba: `PlumaVentana.<nombre>`; paneles `PlumaPanel`; datos en `%LOCALAPPDATA%\Pluma\prueba.<nombre>`.
`PLUMA_REPOSO_S=<s>` hace como si nadie tocara el PC desde el segundo s (para medir el recorte en reposo).
Antes de enseñar algo: captura y mirarla.

## La RAM (lo que más le importa; medir antes y después de cada cambio)
Medido el 25/10 en el portátil (i5-10300H, 16 GB, gráfica integrada), privada como el Administrador de tareas:
- Su Firefox con 7 pestañas: 3.100 MB. Pluma con las 7 de `escenario.ps1` (Google delante): **118 MB** con todo lo de
  hoy (gráfica 16, motor 40, página 44, red 10, interfaz 4); antes, 887-960 todas vivas y 214-271 solo congelándolas.
  3 pestañas: 122-130. Medir de una en una: con el PC por encima de ~80 % de memoria Windows recorta todo por su cuenta
  y salen cifras falsas (70 MB); `escenario.ps1` apunta la memoria del PC en uso.
- Una pestaña ligera (Google): 183 MB usándola, 109 MB tras un rato quieto. Cerrado y en memoria: 13 MB.
- Viendo YouTube a 1080p en el reproductor de Pluma: 123-149 MB en total (mpv 77 con los sombreadores ya guardados,
  92 el primer vídeo de cada PC; la interfaz 14; lo web, dormido y recortado cada 20 s, 30-55), frente a 420-528 con
  la página. mpv solo, a 1080p por HLS: el búfer de 4 MB (10 MB: +17), audio de Windows +4; probados sin ganancia
  720p o 144p (el coste es fijo, de dibujar), gpu-next, opengl, direct3d, copiar la decodificación, modo «dumb».
- Lo que queda con una web delante: la gráfica 45-55 MB recortada (sin recortar se queda 90-120 con lo que dejan los
  vídeos y las webs pesadas, y no lo suelta sola), el motor 25-70, la página (Google ~45, YouTube 220-380, igual que en
  cualquier navegador), la interfaz 3-15, la red ~10.
- Cómo: pestañas de fondo congeladas a los 45 s (TrySuspend + memoria baja); soltadas al minuto salvo las 2 últimas
  vistas (`recientes`), que aguantan 20 min; caché de imágenes de la gráfica con tope de 32 MB
  (`--force-gpu-mem-discardable-limit-mb`); sin vista de reserva (`reserva=0`: crear una vista son ~35 ms); procesos
  sin nada visible (y el motor, la red, la gráfica y la interfaz 4 s después de cada carga) devuelven su memoria con
  EmptyWorkingSet; al cerrar la ventana se congela todo (lo que suena se suelta) y todo devuelve su memoria.
- Recortar la gráfica tras cada carga: medido con `tools\tirones.js` (25 s de scroll a 144 Hz, recorte a mitad) igual
  que sin recortar (4 y 3 fotogramas de más de 25 ms; mediana 6,9 ms en los dos). Un EmptyWorkingSet lanzado desde
  PowerShell no le hace nada (no tiene permiso): probar siempre desde Pluma.
- Probado y descartado: encoger las vistas congeladas, la gráfica NVIDIA (`--force_high_performance_gpu`), un «poca
  memoria» momentáneo en la página a la vista. Ninguno bajaba nada.
- No hacer: quitar el aislamiento de sitios (seguridad) ni recortar la página a la vista mientras se usa.

## YouTube en el reproductor de Pluma (`src/video.cpp`)
Pedido el 25/10: que viendo YouTube no pese lo que pesa su página, sin esperar los 10 s de yt-dlp, con controles «lo
más parecidos posible a los de YouTube», sin tocar Zas y sin depender de otra app («que la gente que descargue Pluma
ya tenga esa funcionalidad»).
- Motor: libmpv propio en `motor-video\libmpv-2.dll` junto al exe (la compilación oficial de shinchiro, versión fijada y
  comprobada con su SHA-256 en `tools\motor-video.ps1`; `instalar.ps1` la baja si falta; fuera de git). Se carga con el
  primer vídeo. Ajuste `video_propio` (menú: «YouTube en el reproductor de Pluma»).
- Cómo: la página abre y reproduce como siempre. Cuando su vídeo suena (sin anuncio y arriba del todo), Pluma pide desde
  la página (sus cookies, su visitorData) la respuesta del cliente VISIONOS y coge su `hlsManifestUrl`; baja la lista
  con WinHTTP, elige variante (lo que cabe en el reproductor; VP9, luego H.264, luego AV1) y el audio de su grupo. mpv
  la carga en pausa 1,2 s por delante y, cuando la página llega a ese segundo, la página se pausa y sigue mpv: sin corte.
  A los 2,5 s la página, en pausa, se congela y devuelve su memoria. Volumen, silencio, velocidad, subtítulos y la
  normalización de volumen de YouTube pasan de la página al reproductor y vuelven.
- Trampas (25/10): los enlaces directos (`adaptiveFormats`) de visionOS dan 403 pasados ~20 MB y su audio ni empieza
  (YouTube sin token); además solo aceptan rangos cerrados (`bytes=0-` da 403; mpv pide por trozos con
  `curl-max-request-size`). La lista HLS va en trozos de ~5 s y baja entera. yt-dlp 2026.10.10 hace lo mismo
  (visionos + HLS), pero tarda 13 s.
- Las listas y los subtítulos los baja Pluma (WinHTTP, a la vez, solo de googlevideo.com y de la API de subtítulos)
  y mpv los abre del disco (`cache-video\`): arranca en ~1,5 s en vez de 4-5. Nunca una orden de mpv que espere a la
  red en el hilo de la ventana: los subtítulos pedidos por mpv tardaron 10 s una vez, y con `mpv_command` la ventana
  se habría quedado colgada ese tiempo (van con `mpv_command_async`).
- No `stopVideo` en la página al dormirla: luego YouTube ignora el salto al segundo y empieza desde 0. Tampoco
  `autoload-files=no` en mpv: deja el audio sin elegir (vídeo mudo).
- El cambio de la página al reproductor tiene que verse y llegar pronto: al usuario le llegaba a los 15-19 s y de
  golpe («se pone así»). Ahora: las listas se piden ya mientras la página carga el vídeo; el margen de mpv sobre la
  página se ajusta a lo que tardan aquí las listas y cada búsqueda (con 1,5 s fijos llegaba tarde una y otra vez); el
  sonido cambia en el instante y la imagen crece desde el sitio del vídeo en la página (zoom.cpp) si Pluma está
  delante. Su línea tiene un servidor de caché de Google que a veces no contesta: esperas de red cortas
  (`network-timeout` 10, `curl-connect-timeout` 4, WinHTTP 4-10 s) y un vigilante: mpv listo en 12 s o se cierra y se
  intenta una vez más (una vez se quedó quieto minutos); mientras, suena la página.
- mpv no pregunta la posición en cada fotograma (`time-pos` y `demuxer-cache-time` no se observan; se leen al
  necesitarlas, 4 veces por segundo mientras se ven los controles): sus avisos tenían la cola de la ventana siempre
  llena y los temporizadores se quedaban sin turno.
- Gráfica: mpv dibuja con la que Windows le da al proceso (en el portátil, la NVIDIA, con la pantalla en la Intel).
  Con la Intel (`d3d11-adapter`) iría sin copias entre gráficas, pero su memoria de vídeo sale de la RAM y cuenta como
  de Pluma: 282 MB en vez de 91 (mpv solo, 140-180 frente a 72). Se queda la NVIDIA, por la RAM. En DXGI, en un
  portátil con dos gráficas la pantalla «cuelga» de la del proceso; la de verdad, con EnumDisplayDevices.
- Clic en el vídeo: pausa a los 250 ms, como YouTube (medido en su página: espera a ver si hay un segundo clic, y en
  un doble clic no pausa nunca). Doble clic: pantalla completa y el vídeo sigue igual.
- Controles: los del reproductor nuevo de YouTube («delhi»), medidos en su página: sus iconos SVG, píldoras
  `rgba(0,0,0,.3)` de 40 px, barra `#ff0033` que acaba en `#ff2791`, a 59 px del borde y a 12 de los lados, menú
  `rgba(0,0,0,.6)` con radio 12 y filas de 48, avisos con la tecla, el «bezel» del centro. Son ventanas con capas
  aparte; la ventana de mpv va deshabilitada para que el ratón y las teclas lleguen a Pluma.
  Teclas: k/espacio, j/l, flechas, m, f, c, 0-9, Inicio/Fin, < y >, Esc; las de Pluma siguen funcionando.
- «Página de YouTube» vuelve a la página en el mismo segundo (y ese vídeo se queda ya en la página). Al acabar, vuelve
  a la página para que salga su pantalla final y pase al siguiente. Si mpv falla, la página sigue donde iba.
- Calidad: «Automática» como la de YouTube (la menor que llena el reproductor: 1080p en una pantalla de 1080p);
  el menú llega a lo que dé la lista (2160p a 60 fps en vídeos 4K; prefiere VP9 normal al HDR). Las listas de todas
  las calidades se bajan nada más empezar, para que el cambio no espere al servidor de listas (1-3 s cada una). El
  búfer es de ~10 s del vídeo elegido, como poco 4 MB: 4K a 60 fps (28 Mb/s, aquí bajan 24-30 Mb/s por conexión) se
  paraba a cargar con 4 MB; con 33 MB no se para. 4K: 196 MB en total, la gráfica Intel al 57 %, la CPU al 3 %.
- Pruebas: `tools\foto.ps1 -Salida x.png [-Raton x,y]` (la foto la hace Pluma: desde fuera las capas salen opacas);
  en las instancias de prueba mpv escribe `mpv.log` en su carpeta de datos.

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
- El motor se apaga solo cuando se cierra su última vista (todas las pestañas soltadas, una pestaña nueva, una
  precarga cancelada) y la siguiente página espera un arranque en frío de más de 1 s: por eso existe `g_keeper`, una
  vista que no carga nada, de tamaño cero. Pasó el 25/10 con la reserva quitada («el motor se ha cerrado»).
- Recortar memoria (EmptyWorkingSet) tarda 9-90 ms: nunca en el hilo de la ventana, que comparte la entrada con las
  páginas (un hilo de la ventana ocupado retrasa los clics en la web). Va en un hilo aparte; y la interfaz de Pluma no
  se recorta a sí misma mientras se ve. Medido con `clics.js` en YouTube: mediana 6 ms hasta verse el clic.
- Pantalla completa (zoom.cpp): con un zoom, como los móviles y los Mac: foto de la pantalla por encima de todo, su
  vídeo crece hasta llenarla (o encoge a su sitio) mientras lo demás se oscurece, la ventana cambia debajo y la foto
  se desvanece cuando el reproductor ya dibuja al tamaño nuevo (240 ms de zoom, 120 de salida). Lo anima
  DirectComposition, Windows mismo a la frecuencia de la pantalla. Antes: de golpe («tosco, laggy») y luego un fundido
  a negro («tosca, no fluida», 25/10; con temporizadores de la ventana daba 3-6 pasos). Sin la animación de maximizar
  de Windows durante el cambio. En las pruebas la pantalla completa ocurre fuera de la pantalla, del tamaño del monitor
  (sin SetWindowPlacement, que devuelve a la pantalla una ventana que está fuera), y la foto sale negra: el aspecto
  solo se ve en la pantalla de verdad (`tools\grabar.exe`).
- Doble clic en el vídeo: el segundo «soltar» se ignora (llegaba a otra ventana tras el cambio de tamaño).
- Abrir Pluma justo mientras el anterior se cierra del todo no hacía nada (le pasaba la url a una ventana que se
  iba): ahora la ventana que sale lleva la propiedad `saliendo` y el nuevo espera a que se vaya el mutex y arranca.
