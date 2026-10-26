# Pluma

Navegador del usuario (25/10/2026): «uno que no consuma nada o casi nada de RAM, que vaya rápido y que se abran las
páginas instantáneo, que admita extensiones (adblock/uBlock) y los scripts de mi YT-MP3 [...] inspírate en Zas».
Luego: seguro («que no me quiten las contraseñas»), diseño minimalista y moderno como Mapea, transiciones que no vayan
a tirones en ningún PC, oscuro por defecto («es como uso los navegadores») y claro como opción. El 25/10 por la tarde:
«modo claro o que el modo esté sincronizado por el sistema» (ahora «Como Windows» por defecto), poder cambiar la fuente
(«no me gusta la fuente que trae»: ahora la de Windows por defecto) y multidioma con el de Windows por defecto. Castellano de España;
código y commits en inglés; nada de em dashes.

## Piezas
- `Pluma.exe` (C++ + WebView2, el motor de Edge que trae Windows y que Microsoft parchea solo). Una sola instancia que
  se queda en memoria al cerrar la ventana (como Zas): `Pluma.exe [url]`, `--oculto` (arranque con Windows),
  `--salir`; Ctrl+Mayús+Q cierra del todo. Ajustes en `%LOCALAPPDATA%\Pluma\ajustes.ini` (buscador, tema, fuente, idioma,
  actualizar (auto | avisar | no),
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
  sugerencias, avisos, pestaña nueva. Diseño de Mapea (sus colores, radios 10/14). Fuente: la de Windows (Segoe UI
  Variable) salvo que se elija otra en el menú (Roboto y Roboto Mono de `fuentes/`, o cualquier instalada con `fuente=`).
  Tema: `tema` = claro/oscuro/sistema; un `tema` guardado sin `ajustes_v=2` era el oscuro de antes y pasa a sistema.
  Las pestañas se aciertan donde acaban, no donde las tiene la animación (`tab_slots(now, true)`): cerrar muchas seguidas
  no espera a que se cierre cada una.
- `src/lang.cpp`: idiomas (es, en, pt, fr, de, it). El código escribe los textos en castellano dentro de `T(L"...")` y la
  tabla da el del idioma elegido (`idioma`, vacío = el de Windows; si no lo tenemos, inglés). Texto nuevo de la interfaz:
  `T()` y su fila en la tabla. Los registros (`log_line`) siguen en castellano. El motor recibe el mismo idioma al
  arrancar (sus menús y el Accept-Language).
- `src/settings.cpp` + `src/settings.html`: la página de configuración (`pluma://settings`, ⋯ > Configuración), como la
  de Edge: buscador arriba, secciones a la izquierda separadas por divisores. El HTML va dentro del exe (RCDATA 200) y se
  sirve en `https://pluma.local/` (WebResourceRequested); habla con Pluma por `chrome.webview.postMessage`, encendido solo
  en esa página (las demás lo tienen apagado) y con el origen comprobado. Sus textos van en castellano en `data-t` y Pluma
  los traduce con `T()`. Idioma, actualizaciones, YouTube y «sugerir» salieron del menú ⋯ y viven aquí.
- `src/update.cpp`: actualizaciones desde las releases del repo (público). La versión va dentro del exe
  (`PLUMA_VERSION`, la pone release.yml); una build local es «dev» y nunca se actualiza. Busca 30 s después de arrancar y
  cada 6 h; baja `pluma-vX.Y.Z.zip`, lo compara con su `.sha256` y lo deja en `actualizacion\`. Entra al cerrar la
  ventana (si está residente, se reinicia oculto), al salir del todo o con «Reiniciar ahora»: Pluma.exe pasa a
  Pluma.old.exe y el nuevo ocupa su sitio; el nuevo copia el resto al arrancar. Carpeta sin permiso de escritura: solo
  avisa. Pruebas: `PLUMA_ACTUALIZAR_URL` apunta a una release falsa servida en 127.0.0.1.
  Animaciones: un hilo espera a cada refresco de la pantalla (DwmFlush) y pide un fotograma solo mientras algo se mueve.
- `src/scripts.cpp`: scripts de usuario (.user.js) sin extensión, cada uno inyectado aparte; script base de Pluma
  (Ctrl+clic y clic central abren detrás; precarga al posar el ratón, con speculation rules).
- `src/extensions.cpp`: extensiones de Chrome (carpeta `extensiones/` junto al exe, que trae uBlock Origin Lite, y
  `%LOCALAPPDATA%\Pluma\extensiones`), instalar desde la Chrome Web Store o Edge Add-ons con «Añadir a Pluma»,
  ventanita de cada extensión (con un parche para que `tabs.query` vea la pestaña de verdad).
- `src/video.cpp`: los vídeos de YouTube en el reproductor propio de Pluma (apartado de abajo).
- `src/youtube.cpp` + `src/youtube.html`: YouTube ligero (apartado de abajo).
- `src/reader.cpp` + `src/reader.html` + `src/readability/`: modo lectura (apartado de abajo).
- `instalar.ps1`: accesos, Pluma registrado como navegador (el predeterminado lo elige el usuario en Configuración),
  `-ArrancarConWindows`, `-Quitar`, y mete el botón MP3 de `Documents\YT-MP3`. Los `.ps1` con tildes, con BOM.
- `Pluma.exe` no va en git (25/10, el usuario: «no debería estar en el repo»): cada push a `main` lo compila una Action
  (`release.yml`, MSVC en windows-latest: el código usa WRL y el cargador estático del SDK) y publica
  `pluma-vX.Y.Z.zip` (una carpeta con el exe, `extensiones`, `fuentes`, `instalar.ps1`, `tools\motor-video.ps1`, que es lo
  que espera el laboratorio de windows-lite); los PR los compila `ci.yml`.
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
  sonido cambia en el instante y la imagen sigue en el mismo sitio (solo cambian los controles). Su línea tiene un servidor de caché de Google que a veces no contesta: esperas de red cortas
  (`network-timeout` 10, `curl-connect-timeout` 4, WinHTTP 4-10 s) y un vigilante: mpv listo en 12 s o se cierra y se
  intenta una vez más (una vez se quedó quieto minutos); mientras, suena la página.
- mpv no pregunta la posición en cada fotograma (`time-pos` y `demuxer-cache-time` no se observan; se leen al
  necesitarlas, 4 veces por segundo mientras se ven los controles): sus avisos tenían la cola de la ventana siempre
  llena y los temporizadores se quedaban sin turno.
- Gráfica: mpv dibuja con la que Windows le da al proceso (en el portátil, la NVIDIA, con la pantalla en la Intel).
  Con la Intel (`d3d11-adapter`) iría sin copias entre gráficas, pero su memoria de vídeo sale de la RAM y cuenta como
  de Pluma: 282 MB en vez de 91 (mpv solo, 140-180 frente a 72). Se queda la NVIDIA, por la RAM. En DXGI, en un
  portátil con dos gráficas la pantalla «cuelga» de la del proceso; la de verdad, con EnumDisplayDevices.
- El reproductor de Pluma va EN EL HUECO DEL VÍDEO de la página, no ocupándola entera: al usuario le parecía «un
  zoom raro» a los 5 s de cada vídeo, y al salir de pantalla completa se quedaba en él (25/10). Alrededor, una foto
  de la página hecha por ella misma justo antes (`CapturePreview`, PNG en memoria, decodificada solo al pintar);
  la página duerme detrás igual. Un clic o la rueda sobre la foto (o la rueda sobre el vídeo) despierta la página,
  que sigue con el vídeo en ese segundo y recibe ese clic o esa rueda (`Input.dispatchMouseEvent`); el reproductor
  vuelve con una foto nueva cuando el PC lleva 4 s sin que se toque nada (`GetLastInputInfo`). Si la página cambia de
  tamaño (ventana, una franja de aviso) o el vídeo se cogió en pantalla completa, la página despierta (la foto ya no
  vale). El cambio espera a tener la foto reciente (< 3 s) y a que mpv ya dibuje al tamaño del hueco.
- Elegido por el usuario el 25/10, con las cifras delante: el reproductor de Pluma siempre (~150 MB), no la página
  normal con Pluma solo en pantalla completa (~385 en la ventana) ni la página siempre (~385). La página viva con su
  vídeo parado y el reproductor de Pluma en su hueco no ahorra nada (322 MB la página sola): el ahorro es dormirla.
- Pluma solo coge el vídeo mientras la página lo reproduce (en su grabación lo cogió en mitad de un doble clic en el
  reproductor de la página, en pausa en 0:01, y ahí se quedó). Si mpv deja de avanzar 8 s sin pausa (30 cargando),
  vuelve la página en ese segundo. La pantalla completa que pide la página va de golpe, sin zoom (el zoom de la
  página entera se veía doble). Al recargar, el reproductor de Pluma vuelve a cogerlo (la recarga no cambia la
  dirección: se mira al acabar de cargar; antes se quedaba en la página).
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

## YouTube ligero (`src/youtube.cpp`, `src/youtube.html`)
Plan aprobado el 25/10: que YouTube abra casi al instante y gaste mucha menos memoria, sin perder la cuenta.
- Cuando la pestaña va a www.youtube.com (`/`, `/results`, `/feed/subscriptions`, `/feed/history`, `/playlist`,
  `/watch` y la portada o los vídeos de un canal), Pluma contesta ese documento (WebResourceRequested, solo
  documentos, solo la dirección que dio el NavigationStarting de la pestaña) con su página (RCDATA 203) en la misma
  dirección: mismo origen, así que las cookies y la sesión siguen. La página llama a la API de YouTube
  (`/youtubei/v1/browse|search|next|navigation/resolve_url|like|subscription`, cliente WEB, firmada con
  SAPISIDHASH + SAPISID1PHASH/3PHASH si hay sesión, como YouTube.js y la propia web) y pinta. Sugerencias de
  `suggestqueries-clients6.youtube.com` (`xhr=t&oe=utf-8`, CORS abierto a youtube.com).
- Ninguna página habla con Pluma: tema, idioma y textos (`t('...')` en la página, traducidos con `T()`) se escriben en
  el HTML al servirlo (`/*PLUMA_CFG*/`). Ajuste `youtube_ligero` (Configuración > YouTube y vídeo), encendido.
- Lo que no hace (directos, Shorts, Studio, compras, music.youtube.com, otras pestañas del canal) es el YouTube de
  siempre. Si una llamada o la lectura falla, la página va a la misma dirección con `pluma_normal=1` (esa dirección,
  en esa pestaña); el botón «YouTube normal», con `pluma_normal=2` (todo YouTube en esa pestaña hasta salir de
  YouTube, con un aviso para volver). Pluma cancela esa navegación y va a la dirección sin la marca: YouTube y la
  barra nunca la ven.
- Vídeo: sin el reproductor de YouTube. Al cargar la página, `video.cpp` le pide `__pluma.extract()` (la respuesta
  visionOS con su HLS, como a la página de YouTube) y mpv entra en `#movie_player` en cuanto dibuja, sin esperar a
  ninguna página. La página se queda despierta y a la vista (no hay foto ni sueño: es ligera); el reproductor está
  fijo arriba (sticky) y la página corre por debajo. Cuatro veces por segundo `__pluma.tick()`: el segundo de mpv
  entra; el sitio del hueco, el segundo de una marca de tiempo pulsada y si las sugerencias tapan el hueco (mpv se
  esconde: nada de la página se puede pintar encima de su ventana) salen. Al acabar, «A continuación» (la lista o el
  recomendado) a los 8 s; «Página de YouTube» o un fallo de mpv, el YouTube normal en ese segundo.
- Historial: las URL de `playbackTracking` (de la respuesta visionOS), con `s.` cambiado a `www.` y `ver=2`, `c=web`,
  `cver`, `cpn` como YouTube.js: `playback` al empezar y `watchtime` cada 30 s y al salir.
- Pruebas de los lectores: `node tools\youtube-test.js` (respuestas reales recortadas en `tools\yt-respuestas`).
  Probar el flujo con sesión no se puede en la instancia de prueba: va sin cuenta.

## Modo lectura (`src/reader.cpp`, `src/reader.html`)
Plan aprobado el 26/10 («Webs ligeras»): cualquier artículo sin basura, como la vista de lectura de Firefox.
- Readability de Mozilla 0.6.0 sin tocar (`src/readability/`, Apache-2.0; su licencia va en el zip, `licencias\`).
  En cada página (DOMContentLoaded; otra vez al acabar de cargar y 1,5 s después de un cambio de dirección hecho por
  la propia página) `isProbablyReaderable` en la página (1-3 ms; nunca la portada de un sitio: ruta `/`). Si es un
  artículo, sale el libro en la barra, delante de la estrella. Clic o F9: Readability lee una copia del documento en la
  página (15-70 ms) y Pluma carga esa misma dirección con `pluma_lector=1` al final, contestando ese documento
  (WebResourceRequested, un filtro solo para esa dirección que se quita al servir) con `reader.html` (RCDATA 210) y
  el artículo dentro; la página quita la marca enseguida (`history.replaceState`). Así el modo lectura es una entrada
  propia del historial (cargar la misma dirección sin marca la reemplazaba y Atrás se saltaba el artículo) y la
  barra, la estrella, el historial y la sesión ven la dirección limpia (comprobado: la marca no queda en
  `historial.tsv` ni `sesion.tsv`). Mismo origen: icono e imágenes como siempre; ningún script de la web corre.
  Salir (libro, F9, «Salir del modo lectura», Atrás): el artículo otra vez, el suyo (se sirve una sola vez), y el
  automático no lo vuelve a coger.
- Seguridad: el HTML del artículo no es de fiar. CSP propia (solo el script de la página, con un nonce por página;
  nada de frames, formularios ni conexiones), el JSON del artículo sin ningún `<` y `clean()` en la página: lista
  blanca de etiquetas y atributos, solo direcciones http(s) (y `data:image` de mapa de bits), los id con `r-` delante.
  Prueba: `node tools\reader-test.js <puerto> tools\captura.exe PlumaVentana.<nombre> click <x> <y del libro>`
  (sirve un artículo con 23 vectores de XSS).
- La página no habla con Pluma (como todas): pide imágenes a `https://lector.pluma.local/<token>?size=|always=|exit=`,
  que Pluma contesta (204) solo si el token es el de la página de esa pestaña. Letra: `lectura_letra` en
  `ajustes.ini`; «Abrir siempre en modo lectura» por sitio en `lectura.tsv` (un host por línea, sin `www.`; desde
  incógnito no se escribe); entonces sus artículos (los que dicen serlo: `og:type` article o JSON-LD Article/Posting)
  pasan solos al modo lectura en DOMContentLoaded, salvo la dirección de la que se acaba de salir. Configuración >
  Modo lectura lista esos sitios para quitarlos.
- Medido el 26/10, perfil limpio cada vez. A mano (el artículo cargado y luego el libro): la memoria no baja (The
  Verge 344 -> 340 y 338 -> 336 MB; El País 140 -> 172 y 137 -> 172: el proceso del sitio sigue vivo) y los datos
  son los de la página (1,7-3 MB) más los del modo lectura (18-155 KB, sus fotos): mejora la lectura, no ahorra.
  Automático: la página se corta en DOMContentLoaded y el modo lectura sale a los 1,0-1,8 s; datos The Verge 1,9-2,0
  MB frente a 3,0, El País 1,4-1,5 frente a 1,75.

## Pestaña nueva
- «Tus páginas»: las que el usuario añade con la estrella al final de la barra de direcciones (o Ctrl+D), en
  `paginas.tsv` (url, título). En cuanto hay una, salen solo las suyas. Sin ninguna, «Lo que más visitas» (se puede
  apagar en el menú: «Sugerir lo que más visito», `sugerir_visitadas`). La × de cada fila (al pasar el ratón) quita
  la página suya, o el sitio de las sugerencias para siempre (`ocultas.tsv`), con «Deshacer». Pedido el 25/10: «que
  solo aparezcan páginas que yo haya añadido, como YouTube... que las pueda añadir fácilmente como un marcador».

## Incógnito
- Ventana de incógnito (⋯ > Nueva ventana de incógnito, Ctrl+Mayús+N, o «Abrir en una ventana de incógnito» en el menú
  de un enlace), como la InPrivate de Edge: otro Pluma.exe (`--incognito`, ventana, mutex y sesión propios, mismo motor
  y ajustes) donde todas las pestañas son de incógnito; todo oscuro, barra y páginas, no se
  queda en memoria al cerrarla y no escribe sesión, historial, fijadas, posición ni actualizaciones. Las extensiones
  activas se ponen en su perfil en cuanto se lee la lista (los navegadores las apagan en incógnito; Pluma no). Vistas InPrivate de WebView2 (`CreateCoreWebView2ControllerWithOptions`, perfil `incognito`): cookies,
  caché e historial del motor solo en memoria, y se van con su última vista. Pluma no apunta nada suyo: ni historial,
  ni sesión, ni pestañas cerradas, ni iconos en `iconos\`, ni títulos o direcciones en `pluma.log`; las listas del
  reproductor de vídeo se borran del disco al cerrarse. Nunca se precarga nada desde una (cargaría en el perfil normal),
  nunca se descargan (se perderían sus sesiones), sus enlaces y ventanas siguen en incógnito, y se cierran al cerrar la
  ventana. El perfil de incógnito recibe las mismas extensiones activas (uBlock). Comprobado el 25/10 con un perfil
  limpio: marca.com en incógnito no quedó en ningún archivo; sus cookies, aparte de las de las pestañas normales.

## Extensiones
- La ventanita de una extensión (su popup) se crea a la vista antes de meterle su página: hecha en una ventana oculta,
  la página se quedaba «hidden» y no pintaba nada (salía vacía, 25/10). Se mide varias veces en su primer segundo
  (uBlock la rellena poco a poco y salía cortada).
- Clic derecho en el botón de una extensión: Opciones, Desactivar, Quitar de Pluma (como Chrome). Las que trae Pluma
  (uBlock) quitadas quedan marcadas en `extensiones.tsv` y ⋯ > Extensiones ofrece «Volver a poner uBlock Origin Lite».

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
- Pantalla completa (zoom.cpp), del hueco del vídeo a la pantalla y vuelta: una ventana encima de todo, transparente
  salvo lo que pinta (DirectComposition), con el fotograma que se ve (`screenshot-raw video` de mpv, en un hilo, con
  su propio `mpv_create_client`) y una capa negra. Al entrar, el fotograma crece y el negro tapa la página; con todo
  tapado, la ventana cambia debajo. Al salir, la ventana cambia primero, debajo, y el fotograma encoge a su hueco
  mientras el negro descubre la página. Lo anima Windows mismo a la frecuencia de la pantalla. Medido: cuadros cada
  10-15 ms, sin congelones; el zoom arranca a los 20-80 ms (el primer `screenshot-raw` tarda ~230: se hace uno por
  adelantado 1,5 s después de entrar el reproductor). Historia, todo del 25/10: de golpe («tosco, laggy»); fundido a
  negro («tosca, no fluida»); el vídeo en vivo movido con los márgenes de mpv (`video-margin-ratio-*`, cuatro cambios
  por cuadro que mpv dibujaba uno a uno: «super choppy»); y una FOTO DE LA PANTALLA (BitBlt): Windows congelaba la
  pantalla ~320 ms al leerla («se laguea todo»). Nunca leer la pantalla con BitBlt para una animación. Trampas: la
  ventana del zoom en un hilo sin mensajes sacaba el cursor de espera (va en el hilo de la ventana, `HTTRANSPARENT`);
  sin `DWMWA_TRANSITIONS_FORCEDISABLED` en ella, Windows la fundía al aparecer; la página dormida no cambia de tamaño
  en la pantalla completa del reproductor (`tabs_layout_views`). La que pide la página va sin animación de Windows.
- Scroll de YouTube a tirones al abrir un vídeo (25/10): el recorte de memoria (EmptyWorkingSet del motor, la gráfica
  y la página) caía justo después de cargar y cada 20 s, y todo lo recortado vuelve a fallos de página al moverse;
  y la página se dormía a los 2,5 s y la primera rueda la despertaba en frío. Ahora: recortes solo con el PC sin
  tocar 3-5 s (`user_away`); la página se duerme a los 15 s sin el ratón encima (o 30 s sin tocar nada), y con el
  ratón sobre la foto se despierta detrás (`warm_page`); una foto al principio y otra justo antes del cambio, no cada
  1,5 s. Medido con `tirones.js` tras la rueda, a los 20 s: 0 cuadros de más de 25 ms (antes 3, uno de más de 50).
- Pruebas de transiciones: fuera de la pantalla la foto sale negra. Con `PLUMA_A_LA_VISTA=x,y` la instancia de prueba
  se pone en la pantalla, siempre encima, para grabarla con `tools\grabar.exe`. Así, Chromium la da por tapada a los
  4 s (su detección de ventanas ocultas) y YouTube encoge su reproductor a 144x0: en ese perfil de prueba,
  `motor=--disable-features=CalculateNativeWinOcclusion` en `ajustes.ini`. Y las instancias de prueba acumulan
  pestañas en `sesion.tsv` (46 el 25/10): `cdp.js` habla con la primera que casa, no con la de delante; borrarla.
- Doble clic en el vídeo: el segundo «soltar» se ignora (llegaba a otra ventana tras el cambio de tamaño).
- Los controles del reproductor son ventanas propiedad de la ventana de Pluma: al minimizarla, Windows las esconde
  por su cuenta (`WM_SHOWWINDOW` con `SW_PARENTCLOSING`) y al restaurarla las vuelve a sacar (`SW_PARENTOPENING`)
  aunque Pluma las haya escondido entretanto. Salían con su último dibujo y se quedaban encima de una pestaña nueva
  con el vídeo ya cerrado (25/10). `layer_proc` ignora esos dos avisos y `layer_hide` esconde lo que se ve, no lo que
  Pluma cree que se ve.
- Abrir Pluma justo mientras el anterior se cierra del todo no hacía nada (le pasaba la url a una ventana que se
  iba): ahora la ventana que sale lleva la propiedad `saliendo` y el nuevo espera a que se vaya el mutex y arranca.
