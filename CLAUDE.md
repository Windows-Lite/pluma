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
  `--salir`, `--perfil=<id>` (otro perfil, apartado de abajo); Ctrl+Mayús+Q cierra del todo. Ajustes en `%LOCALAPPDATA%\Pluma\ajustes.ini` (buscador, tema, fuente, idioma,
  actualizar (auto | avisar | no),
  dormir_s, descargar_s, recientes, descargar_min, reserva, residente, protocolos, `motor` = argumentos extra para el
  motor, efecto, eq, graves, reverb, tono = efectos de sonido del reproductor), sesión en `sesion.tsv`,
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
- `src/lite.cpp` + `src/youtube.html`, `src/music.html`, `src/reddit.html`, `src/lite.html`: webs ligeras, YouTube, YouTube Music y Reddit
  (apartados de abajo).
- `src/reader.cpp` + `src/reader.html` + `src/readability/`: modo lectura (apartado de abajo).
- `src/passgen.cpp` + `src/passgen.js`: «Usar contraseña segura» en los formularios de registro (apartado de abajo).
- `src/vault.cpp`: las contraseñas guardadas, verlas y copiarlas (Configuración > Contraseñas y la llave de la barra;
  apartado de abajo).
- `src/downloads.cpp`: las descargas son de Pluma, no del panel del motor (apartado de abajo).
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
tools\captura.exe <clase> <png> [click x y | mclick | rclick | key vk | char texto | wait ms | drag x1 y1 x2 y2 pasos]
                                              foto de la ventana; <clase>|<palabras>: la de esa clase con esas palabras
                                              en el título (con varias ventanas, FindWindow da la de más arriba)
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
- Cómo: pestañas de fondo congeladas a los 45 s (TrySuspend + memoria baja). Desde el 26/10 NUNCA se sueltan por
  defecto (`descargar_s=0`, `descargar_min=0`; Ajustes > «Descargar las pestañas que no ves»): el usuario: «no me
  gusta que pasado un tiempo las pestañas pierdan memoria, es un coñazo». Soltarlas al minuto (antes) dejaba las 7 en
  118 MB frente a 214-271 congeladas; esa memoria es la de quedarse con todo. Solo con el PC al 85 % de memoria se
  suelta la más antigua. Caché de imágenes de la gráfica con tope de 32 MB
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
- Bucle (pedido el 26/10: «tengo el modo loop activado, estoy en otra tab y se queda pausado el video»): fila «Bucle»
  del menú del reproductor (`loop-file=inf`), y el bucle de la página de YouTube pasa al reproductor y vuelve. En bucle
  el final nunca devuelve el vídeo a la página. Una pestaña de fondo con el reproductor sonando no se congela ni se
  suelta aunque esté en silencio (`video_playing`): soltarla cerraba el reproductor.
- Efectos de sonido (pedido el 26/10: «equalizer para poner bass boost etc, configurable», «ajustar pitch, modo
  nightcore, filtros»): `src/sound.cpp` arma el `af` de mpv. Presets: Desactivado, Más graves, Graves fuertes, Voces
  claras, Más agudos, Nightcore, Slowed + reverb, 8D, Sala, Personalizado; ecualizador de 10 bandas, graves,
  reverberación y el tono (-6..+6 semitonos, aparte: vale con cualquiera). El tono, con el `rubberband` de mpv (que
  entonces hace también la velocidad); Nightcore y Slowed suben o bajan velocidad y tono juntos (velocidad de mpv = la
  elegida × la del efecto: el menú enseña la elegida). El resto, un grafo de lavfi: `equalizer` por banda, `bass`,
  `apulsator` (8D), `aecho` y `alimiter` al final (sin su nivel automático) tras quitar la mitad del mayor realce.
  Desactivado y tono 0: ningún filtro. Se aplica al momento a todos los reproductores, también a la música de YouTube
  Music ligero (`audio_apply_sound`, con su velocidad). Prueba de las cadenas: `tools\sonido-test.cpp`
  (`c++ -std=c++17`; `--grafos` las saca para probarlas con `ffmpeg -af`).
- Panel de sonido (26/10, el usuario: «la parte de ecualización está como muy mal metida, está en el apartado de
  ajustes... no me mola mucho»): donde se escucha, no en Configuración (allí queda una línea que dice dónde está,
  para el buscador). Menú del reproductor > «Sonido» (abre el panel en el sitio del menú, `g_menu_page` 5, con el
  aspecto del menú «delhi») y ⋯ > «Sonido» mientras algo suena en el reproductor de Pluma (`video_sound_useful`: un
  vídeo, o la música de Music ligero en cualquier pestaña), con los colores de Pluma (claro/oscuro) bajo ⋯
  (`g_snd`, otra capa `PlumaControlesVideo`). Un solo dibujo (`snd_draw`) con dos aspectos (`SndLook`): chips de los
  presets, la curva de las 10 bandas (Catmull-Rom; se arrastran sus puntos, con el valor encima), graves,
  reverberación, tono y «Restablecer». Pulsar ya pone el valor (un clic basta, como en cualquier deslizador); mover
  la EQ, los graves o la reverberación pasa a Personalizado desde lo que sonaba. Mientras se arrastra, a mpv como
  mucho cada 90 ms (cada cambio rehace sus filtros) y se guarda al soltar. Sobre el vídeo, a la derecha encima de los
  controles, entre 340 y 560 DIP de ancho (los chips en dos filas, para que quepa sobre un vídeo a 1080p); si no
  cabe, sube sobre la página hasta justo debajo de la barra de Pluma y la rueda lo mueve. El de ⋯ se cierra con un clic fuera,
  Esc, otro menú o pidiéndolo otra vez (las páginas no avisan de sus clics: lo vigila un temporizador, `snd_watch`).
  Mirado antes cómo lo hacen otros: Spotify (presets + puntos que se arrastran, pero escondido en Ajustes, que es
  justo lo que no le gustaba; https://support.spotify.com/us/article/equalizer/), las extensiones tipo «Ears» y
  «Turn It Up» (panel desde el botón de la barra, presets como fichas, mezcla con todo a la vista;
  https://github.com/jestemKox123/Turn-It-Up-Son-Volume-Booster-Mixer, https://github.com/addavriance/ReEQ).
  Copiado: fichas, curva con puntos, todo en vivo y un «Restablecer». No copiado: preamplificador y limitador a la
  vista (el nuestro es automático), EQ por web, analizador de espectro (gasta CPU), varios perfiles guardados.
- Pruebas: `tools\foto.ps1 -Salida x.png [-Raton x,y]` (la foto la hace Pluma: desde fuera las capas salen opacas);
  en las instancias de prueba mpv escribe `mpv.log` en su carpeta de datos. `Pluma.exe pluma-prueba:sonido` abre (o
  cierra) el panel de sonido bajo ⋯. Un script que calcule coordenadas tiene que ser consciente del DPI
  (`SetProcessDpiAwarenessContext(-4)` antes de nada): si no, a 125 % Windows le da todo escalado a 96 ppp y los
  clics caen en otro sitio.

## Webs ligeras (`src/lite.cpp`)
- Una tabla de webs (`kSites`): origen que se contesta, RCDATA de su página, ajuste, qué direcciones hace y el aviso de
  «la versión normal en esta pestaña». La marca `pluma_normal=1|2`, la caída a la página normal y el aviso son los mismos
  para todas. Lo que comparten las páginas (colores de los dos temas, la barra con el botón de la versión normal, `t()`,
  `fallback`) va en `lite.html` (RCDATA 206) y Pluma lo pone en el sitio de `<!--PLUMA_COMUN-->` al servirlas; la de
  YouTube lleva lo suyo. Configuración > Webs ligeras: un interruptor por web (`youtube_ligero`, `music_ligero`, `reddit_ligero`).

## YouTube ligero (`src/lite.cpp`, `src/youtube.html`)
Plan aprobado el 25/10: que YouTube abra casi al instante y gaste mucha menos memoria, sin perder la cuenta.
- Cuando la pestaña va a www.youtube.com (`/`, `/results`, `/feed/subscriptions`, `/feed/history`, `/playlist`,
  `/watch` y la portada o los vídeos de un canal), Pluma contesta ese documento (WebResourceRequested, solo
  documentos, solo la dirección que dio el NavigationStarting de la pestaña) con su página (RCDATA 203) en la misma
  dirección: mismo origen, así que las cookies y la sesión siguen. La página llama a la API de YouTube
  (`/youtubei/v1/browse|search|next|navigation/resolve_url|like|subscription`, cliente WEB, firmada con
  SAPISIDHASH + SAPISID1PHASH/3PHASH si hay sesión, como YouTube.js y la propia web) y pinta. Sugerencias de
  `suggestqueries-clients6.youtube.com` (`xhr=t&oe=utf-8`, CORS abierto a youtube.com).
- Ninguna página habla con Pluma: tema, idioma y textos (`t('...')` en la página, traducidos con `T()`) se escriben en
  el HTML al servirlo (`/*PLUMA_CFG*/`). Ajuste `youtube_ligero` (Configuración > Webs ligeras), encendido.
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

## Reddit ligero (`src/reddit.html`, RCDATA 205)
Plan aprobado el 26/10. www.reddit.com y reddit.com (old.reddit.com no se toca): portada, subreddit y sus órdenes,
publicación con comentarios, persona, búsqueda.
- Reddit no da datos a una página sin su app (su `.json` da 403 sin sesión, desde aquí y desde el PC), pero su servidor
  dibuja todo en HTML: la página pide ese HTML con las cookies (la misma dirección para una publicación; para las
  listas, las piezas que carga su página: `/svc/shreddit/community-more-posts/<orden>/?name=`,
  `feeds/home-feed?sort=` (sin sesión da la popular), `profiles/profile_overview-more-posts/new/?name=`,
  `search/?q=`), lo lee con DOMParser (inerte) y pinta. La siguiente página es el `faceplate-partial` con `after=` o
  `cursor=` del final. Sus partes llegan en `<template for=...>` (streaming): `unfold()` las pone en su sitio antes de
  leer. Los comentarios se anidan por el DOM (el comentario más cercano), no por `depth`; «Más respuestas» y «Más
  comentarios» son POST a `/svc/shreddit/more-comments/...` con sus campos ocultos y `csrf_token` de la cookie, como su
  página. Votar, comentar y responder: la página normal de esa publicación (`pluma_normal=1`).
- El HTML de los textos no es de fiar (la página corre en www.reddit.com con la sesión): `cleanBody()` deja una lista
  corta de etiquetas y atributos, enlaces http(s) o del sitio, fotos solo de los servidores de Reddit.
- Vídeos (v.redd.it, HLS) en el reproductor de Pluma, como en YouTube ligero (`__pluma.extract/tick`). Trampa (26/10):
  la lista maestra va firmada para la página que la pidió (desde Pluma, 403 con cualquier cabecera); las listas de
  cada calidad y los trozos son abiertos. Así que la página lee la maestra y se la da a Pluma (`master`), y Pluma solo
  acepta calidades de v.redd.it. Si el vídeo no va, la publicación sigue ligera y el hueco ofrece el Reddit normal.
- Trampas: un perfil sin cookies de Reddit puede recibir «Prove your humanity» (`blocked()`): la página normal, que la
  resuelve, y la siguiente ya es ligera. Una vez en el Reddit normal, su app se queda con las navegaciones del mismo
  origen (Navigation API) hasta una carga entera: Pluma no ve esos documentos. Su service worker no estorba (las
  navegaciones llegan a Pluma igual, comprobado con uno registrado).
- Pruebas: `node tools\reddit-test.js` (HTML real recortado en `tools\reddit-respuestas`; necesita linkedom fuera del
  repo, en `%LOCALAPPDATA%\fp-tools\linkedom`); medidas con `node tools\lite-medir.js <puerto> <url> [cold]`.

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

## Traducción (`src/translate.cpp`, `src/translate.js`, `src/translate_core.h`)
Pedido el 26/10: «necesito una opción para traducir», como Edge.
- Apagada por defecto (26/10, el usuario: «que el traductor esté desactivado por defecto, que solo traduzca al activarlo
  en esa página»): nada se traduce ni se ofrece solo. Se traduce al pedirlo: icono de la barra, fila «Traducir página» /
  «Mostrar original» en ⋯ o el clic derecho. Solo esa página: la siguiente de la pestaña sale sin traducir.
- Icono en la barra (delante del libro o la estrella) en toda página en otro idioma, discreto (el color del libro y la
  estrella); en el acento solo traducida. Idioma de la página: su `lang` (o el meta), salvo que el texto diga claramente
  otro (`tr_guess_lang`: escritura y palabras frecuentes, en Pluma; nada sale hasta que se pide traducir). Destino:
  `traducir_a`, vacío = el de Pluma.
- Clic en el icono: la tarjeta (rediseñada el 26/10: «rediséñalo que se ve fatal»; antes era una lista), como la burbuja
  de Chrome (`chrome/browser/ui/views/translate/translate_bubble_view.cc`) y la de Edge: icono, «¿Traducir esta página?»
  y «Página en X», ⋯ y ×; la píldora del idioma de la página, una flecha y la del destino (clic: la lista de idiomas);
  «Ahora no» y «Traducir» (principal, a la derecha). Traduciendo, el título lleva el % y una barra fina; traducida,
  «Página traducida», «Ahora en X», «Mostrar original» y «Listo». Copiado: título + idiomas + botones + menú de opciones.
  No copiado: las pestañas de Chrome como interruptor (aquí, botones que dicen lo que hacen). Es el menú de Pluma
  (`PAGE_TRANSLATE`, filas 10-12 de `MItem`, `card_part`/`card_draw`); el ⋯ es `PAGE_TR_OPTIONS`: Traducir a,
  «Traducción automática» y, solo con ella encendida, Siempre / Nunca este idioma / Nunca este sitio; Configuración.
  Varias ventanas: la tarjeta, la oferta y la burbuja se abren sobre la ventana de la pestaña; sus botones actúan en la
  ventana donde está la pestaña entonces (`WinScope(win_of_tab(t))`) y un cambio de la traducción redibuja la tarjeta
  en la suya (como `ui_downloads_changed`). Probado moviendo la pestaña a otra ventana (`mover-nueva`) y traduciendo allí.
- Traducción automática (`traducir_ofrecer`, Configuración > Idioma > Traducción, apagada): la tarjeta sale sola una vez
  por sitio e idioma y sesión (quieta: no coge el foco, se va con un clic fuera, Esc, otra pestaña o 12 s sin tocarla; sus
  botones no traen Pluma delante), los idiomas de `traducir_siempre` se traducen sin preguntar y un idioma traducido a
  mano sigue traducido en las páginas siguientes de esa pestaña (como Chrome). Apagada, las listas se
  guardan pero no actúan. Un `ajustes.ini` de antes de `ajustes_v=3` la tenía encendida de serie: se lee como apagada.
  `traducir_nunca` en `ajustes.ini`; sitios en `traducir-nunca.tsv` (no se escribe desde incógnito).
- Cómo (copiado de TWP, https://github.com/FilipePS/Traduzir-paginas-web `src/contentScript/pageTranslator.js`, y de
  Firefox, `toolkit/components/translations/content/translations-document.sys.mjs`): `translate.js` (RCDATA 221) se
  mete con ExecuteScript (pasa la CSP), junta en «trozos» los textos de cada bloque (`<a i=N>` por nodo, para que la
  frase vaya entera), los de la pantalla primero, y Pluma los pide cada 700 ms (`take`), los manda a
  `translate.googleapis.com/translate_a/t?client=gtx` (POST, varios `q`, `format=html`; WinHTTP en un hilo) y los
  devuelve (`put`). Se escriben con `nodeValue`, nunca HTML: la maquetación, los enlaces y los scripts de la página no
  cambian (prueba de la página de `tools\traducir-prueba.js`: el mismo número de elementos y, al mostrar el original, el
  mismo HTML). Lo nuevo (MutationObserver, 250 ms) se traduce igual; caché por trozo. No se traduce script, style,
  code, pre, textarea, inputs, contenteditable, `translate="no"`, `.notranslate`; sí `placeholder`, `title` y el título.
  No copiado: el modelo local de Firefox (cientos de MB) ni el iframe de TWP.
- Google mueve a veces el texto fuera de los marcadores: va al marcador anterior (`tr_split_markers`). Los espacios entre
  nodos y la puntuación los arregla `apply` (sin «mantequilla .» ni dobles espacios).
- Selección: «Traducir «…»» en el menú del clic derecho (tras Copiar) y una burbuja de Pluma junto a la selección,
  con copiar. Fallos (sin red, 429/403/503): aviso y la página se queda como estaba.
- Configuración > Idioma > Traducción: traducción automática sí/no, destino y las listas para quitar.
- Pruebas: `tools\traducir-test.cpp` (marcadores y detección; `cl /EHsc /std:c++17`), `tools\traducir-prueba.ps1
  -Salida <carpeta>` con `PLUMA_EXE` (de punta a punta: un `ajustes.ini` viejo con la oferta encendida; la página de
  prueba; Wikipedia en francés, en castellano con destino inglés y en japonés sin nada solo (`typeof __plumaTr` y el
  log); la tarjeta, su ⋯ y sus botones; la automática encendida sí ofrece; YouTube, Configuración, burbuja, menús;
  capturas hechas por Pluma, claro y oscuro). Órdenes de prueba: `Pluma.exe pluma-prueba:traducir|original|seleccion|
  menu|menu-principal|oferta|clic-derecho|destino=xx|tarjeta=1|2|opciones|automatica=0|1|foto=<nombre>` (solo en
  instancias de prueba; `menu` es la tarjeta, `tarjeta=1|2` pulsa su botón principal o el otro). La oferta deja
  «traducción: ofrecida (xx)» en `pluma.log`.
- El menú del clic derecho es del motor: no sale en las capturas. `clic-derecho` hace que el siguiente no se enseñe:
  sus filas van a `pluma.log` y se elige la de traducir, como un clic (`tools\clic.js ... right` para abrirlo).
- RAM (26/10, 3 pestañas, `tres.ps1` a la vez): 151 MB sin esto, 156 con esto (dentro de lo que varía).
## YouTube Music ligero (`src/music.html`, RCDATA 204)
Plan aprobado el 26/10 (las webs que más visita: YouTube Music 1.590 visitas en 120 días).
- music.youtube.com (`/`, `/search?q=`, `/browse/<id>`, `/channel/UC...`, `/playlist?list=`, `/library`, `/watch?v=`): la
  página llama a su API como su web y ytmusicapi (cliente WEB_REMIX, versión = la fecha de ayer, firmada con SAPISIDHASH
  si hay sesión). Inicio, búsqueda, artista, álbum, lista, biblioteca y «Canciones que me gustan» (con sesión).
- No recarga nunca: los enlaces cambian la vista con `history.pushState`, así que la música sigue. Lo que no hace
  (explorar, podcasts...) es un enlace normal y carga el YouTube Music de siempre.
- La música suena en el reproductor de Pluma, solo sonido (`video.cpp`, «sound only»: un mpv sin ventana, `vid=no`, el
  mejor audio de la lista HLS de visionOS, bajada a `cache-video\`). La cola, siguiente, aleatorio y repetir son de la
  página; Pluma le pregunta 4 veces por segundo (1 si la pestaña no se ve) `__pluma.music(estado)` y hace lo que
  devuelve (`play`, `pause`, `seek`, `volume`, `stop`). Mientras suena, la pestaña cuenta como audible y no se congela.
  Silenciar la pestaña silencia ese mpv. Al ir a otro documento en esa pestaña (o cerrarla) la música para.
- Trampa (26/10): sin sesión, visionOS da LOGIN_REQUIRED a casi todas las canciones (las «Art Track»); los vídeos
  musicales sí van. IOS da enlaces directos que dan 403 pasado el primer MB (piden PO token); WEB/WEB_REMIX piden la
  firma del reproductor. Una canción que no va pasa a la siguiente; tres seguidas, la abre el YouTube Music normal con
  la cola. Con sesión no se pudo probar (la instancia de prueba va sin cuenta).
- Sin teclas multimedia (el reproductor de Pluma no las tiene). Pruebas: `node tools\music-test.js`
  (respuestas en `tools\ytm-respuestas`); medidas con `node tools\lite-medir.js <puerto> <url> [cold]`.

## Sincronizar (`src/sync.cpp`, `src/backup.h`, `src/monocypher/`)
Pedido el 26/10: «sincronizar todo por si el día de mañana reinstalamos Windows ... con contraseñas y todo»; destino
elegido: «google drive mismo de la propia cuenta ... o onedrive o dropbox». Y luego: «que se guarde absolutamente toda la
info: contraseñas, historial, cookies, etc. etc., todo del navegador», y todo cifrado, nunca en claro en ningún sitio.
- Una copia cifrada de todos los perfiles: ficheros de Pluma (`pluma_files`, `pluma_dirs`, `perfiles\`) y **el perfil
  entero del motor** de cada uno (`Default`, `WV2Profile_pluma-<id>`): contraseñas, cookies, Local Storage, Session
  Storage, IndexedDB, Service Worker (registros, `ScriptCache` y `CacheStorage`), File System, WebStorage, Web Data
  (autorrelleno, tarjetas, direcciones), History, Favicons, Top Sites, Preferences y Secure Preferences (permisos de
  sitios, zoom), extensiones y todo su estado, Trust Tokens... Fuera, a cualquier profundidad (`bk::engine_skip_dir`,
  `engine_skip_file`), lo que el motor rehace solo: Cache, Code Cache, GPUCache, Dawn*Cache, GrShaderCache, ShaderCache,
  GPUPersistentCache, Media Cache, blob_storage, Crashpad, cachés de componentes y de extensiones bajadas, Safe Browsing
  Network, Shared Dictionary, optimization_guide_*, JumpListIcons*, y los `LOCK`, `LOG`, `LOG.old` de leveldb, los `-shm`
  y los temporales. `bk::allowed_target` (lista blanca de lo que una restauración puede escribir): el perfil del motor
  menos eso, nunca `..`, unidades, rutas absolutas ni nada fuera de un perfil (`motor/Local State` no).
- Cómo lo deciden otros: bleachbit (lo que se puede borrar de Chrome sin perder nada, su lista «cache»:
  https://github.com/bleachbit/bleachbit/blob/master/cleaners/google_chrome.xml), Duplicati (sus filtros de caché:
  https://github.com/duplicati/duplicati/blob/master/Duplicati/Library/Utility/FilterGroups.cs; no copiado su
  `*Current*`, que rompería el `CURRENT` de cada leveldb, ni que deje fuera las cookies), Chromium (la caché es aparte
  del perfil en Linux y Mac: https://chromium.googlesource.com/chromium/src/+/main/docs/user_data_dir.md) y Firefox
  (copia el perfil entero; su caché vive en otra carpeta). Hekasoft Backup & Restore no documenta qué deja fuera.
- `CacheStorage` se queda: es dato de las webs (la Cache API, lo que una web guarda para ir sin conexión; se borra con
  «cookies y datos de sitios», no con la caché). En el perfil del usuario (26/10) es el grueso: 62 MB de 82.
- Medido en su perfil el 26/10 (solo tamaños): antes la copia llevaba 70 MB (34 comprimidos; casi todo `extensiones\`
  de Pluma, 61 MB); ahora 145 MB (unos 62 comprimidos): el perfil del motor son 82 MB (31 comprimidos) de 601; lo
  que queda fuera, 519 MB (Cache 330, Code Cache 176, GPUCache 10...). El fichero mayor, 20 MB; la ruta más larga al
  restaurar, 239 caracteres (aun así todo lo de la copia usa `\\?\`).
- Coherente: se lee por «bloques» que van juntos (una carpeta leveldb, la que tiene `CURRENT`, entera; una base SQLite
  con su `-wal` y su `-journal`; lo demás, de uno en uno), con el motor en marcha (lectura compartida), y se repite si
  algún fichero del bloque cambió mientras se leía (tamaño u hora vistos en los handles abiertos); a la cuarta, entra
  lo último y se apunta («cambiando» en el log). Nunca media base.
- El motor tiene bloqueadas para sí (ni leerlas se puede) solo las cookies (`Network\Cookies`, comprobado en su perfil:
  2 ficheros sin leer, los de `Network`): cada arranque copia, antes de abrir el motor, `bk::engine_locked_dbs` (cookies
  y contraseñas, con sus diarios) a `sync-motor\` dentro de la carpeta de Pluma, solo si cambiaron (6-7 ms); un bloque
  bloqueado entra entero de esa copia. Las cookies de la copia son las del último arranque.
- Contraseñas y cookies van cifradas con la llave del motor (`Local State`, os_crypt, DPAPI de esta cuenta: en un Windows
  nuevo no se abre). La copia lleva esa llave (`meta/clave`) sellada con la contraseña de la copia y la restauración la
  vuelve a proteger con DPAPI para la cuenta nueva (`engine_key_put`); mientras espera al reinicio, también con DPAPI.
- Fichero: llave de la contraseña con Argon2id (Monocypher, 64 MB, 3 pasadas), AES-256-GCM, cabecera autenticada (solo
  formato, sal, cuándo, equipo, pista: ningún nombre de fichero ni de sitio en claro). Formato 2 (desde que va el
  navegador entero): el archivo se comprime en bloques de 4 MB según se llena, directo al fichero, y se cifra en el
  sitio; así la memoria es la copia comprimida más un bloque, no el perfil dos o tres veces (con 100 MB de datos de una
  web que no se comprimen, en la prueba: pico de Pluma 411 MB con todo en memoria, 274 así, y vuelve a 55 al acabar;
  copia en 1,5-1,8 s). Restaurar sí lo tiene todo en memoria un momento (fichero, descifrado y entradas). El formato 1 (los de antes) se sigue abriendo; un Pluma de
  antes no abre uno de formato 2 (dice que no es una copia: no toca nada).
- Un solo trabajador (en el proceso del perfil por defecto): cada hora si cambió algo (huella de nombres, tamaños y
  horas; el contenido solo de los ficheros pequeños de Pluma), al cerrar la ventana como mucho cada 10 minutos y al
  salir del todo siempre (Pluma espera hasta 2 min a que acabe, ya sin ventana ni mutex); guarda las 5 últimas.
  Restaurar: se prepara en `restore-pending\` (dentro de la carpeta de Pluma) y Pluma se reinicia y lo aplica antes de
  abrir el motor: cada perfil del motor de la copia sustituye entero al que había (que va, cachés incluidas, a
  `antes-de-restaurar-<fecha>`, igual que los ficheros de Pluma). Una copia de antes (sin `Preferences`) solo pone lo
  que traía (`engine_paths_v1`) y deja el resto (sin probar de punta a punta; `APLICAR` lleva el formato). Contraseña mal: no se toca nada.
- Destinos: una carpeta (funciona), Google Drive appData, OneDrive App Folder, Dropbox App folder (OAuth con PKCE y
  `state`, vuelta a `http://127.0.0.1:47615/`). Google Drive: cliente «App de escritorio» del proyecto «Pluma» de la
  cuenta del usuario (solo `drive.appdata`); su ID y secreto en los secretos del repo (`GOOGLE_CLIENT_ID`,
  `GOOGLE_CLIENT_SECRET`) y release.yml los escribe en `src/secrets.h` (fuera de git) antes de compilar; una build
  local va sin ellos salvo que se cree ese fichero o se pongan en `ajustes.ini` `[sync]`. OneDrive y Dropbox, vacíos.
  Google Drive sube en una sola petición multipart (unos 62 MB con su perfil; Google la documenta para hasta 5 MB y
  pide la subida reanudable para más: si falla con copias grandes, ese es el cambio); Dropbox, si un día se activa,
  no pasa de 150 MB por petición. Volumen: la huella cambia casi cada hora que se navega, así que son unos 62 MB por
  hora de uso, más al cerrar la ventana (como mucho cada 10 min) y al salir.
- Pruebas: `tools\sync-test.cpp` (vector de Argon2id, ida y vuelta en formato 1 y 2, contraseña mal, copia alterada,
  bloques con longitudes que mienten, nombres no permitidos: cachés, `..`, fuera de un perfil). De punta a punta:
  `tools\sync\prueba.ps1 [-GrandeMB 100]` en la sesión interactiva con `PLUMA_EXE` (un sitio local, `servidor.js`, que
  inicia sesión con cookie persistente HttpOnly, localStorage e IndexedDB, y una extensión con `chrome.storage`; copia a
  una carpeta, comprueba que la copia no contiene la marca, el sitio ni nombres de ficheros, en UTF-8 ni UTF-16, cuando
  el perfil sí; y restaura en una instancia limpia, que vuelve con la sesión iniciada y la extensión con lo suyo).
  Instancias de prueba: `PLUMA_SYNC_CARPETA` y `PLUMA_SYNC_FICHERO` saltan los diálogos, `PLUMA_SYNC_PRIMERA=1` enseña la
  oferta de restaurar.
- Sin probar aquí (un solo Windows): que `Secure Preferences` sobreviva en otro equipo (sus firmas llevan el id de la
  máquina: el motor puede reponer los ajustes protegidos) y la subida a Google Drive de una copia grande.
- `antes-de-restaurar-<fecha>` guarda ahora el perfil del motor entero de antes, caché incluida (hasta ~600 MB en el
  suyo), y nada lo borra. Si una restauración falla a medias, `restore-pending\` se queda (descifrado, dentro de la
  carpeta de Pluma) para mirarlo.

## Pestaña nueva
- «Tus páginas»: las que el usuario añade con la estrella al final de la barra de direcciones (o Ctrl+D), en
  `paginas.tsv` (url, título). En cuanto hay una, salen solo las suyas. Sin ninguna, «Lo que más visitas» (se puede
  apagar en el menú: «Sugerir lo que más visito», `sugerir_visitadas`). La × de cada fila (al pasar el ratón) quita
  la página suya, o el sitio de las sugerencias para siempre (`ocultas.tsv`), con «Deshacer». Pedido el 25/10: «que
  solo aparezcan páginas que yo haya añadido, como YouTube... que las pueda añadir fácilmente como un marcador».

## Contraseñas seguras (`src/passgen.cpp`, `src/passgen.js`)
Pedido el 26/10: «Sugerir contraseñas seguras» como Chrome y Edge (la de Edge necesita la sincronización de Microsoft,
que el motor no tiene).
- `passgen.js` (RCDATA 220) va en cada página y marco (AddScriptToExecuteOnDocumentCreated, antes que los scripts de la
  web) con sus textos (`T()`) y el ajuste escritos dentro (`/*PLUMA_CFG*/`). Un solo `focusin` en la ventana; nada más
  corre hasta que una contraseña coge el foco. Nunca en `pluma.local`, en youtube.com ni en las páginas ligeras de
  Pluma (las que tienen `__pluma`, `lite.cpp`).
- Cuándo, las reglas locales de Chromium: `autocomplete="new-password"`, dos campos de contraseña (nueva y repetir; con
  tres, el primero es la actual), nombres que lo dicen (new, confirm, repeat, nueva, regist...) o un formulario de
  registro; nunca `current-password` ni un campo solo en un formulario de entrar.
- La contraseña, como el generador de Chromium (`password_generator.cc`): 15 caracteres sin los que se confunden
  (l I 1 O 0 o), minúscula, mayúscula, número y un símbolo de `-_.:!`, sin `--` ni `__`; respeta minlength, maxlength y
  pattern. `crypto.getRandomValues` sin sesgo de módulo.
- Se dibuja en una sombra cerrada en la capa de arriba (popover; dentro del `dialog` abierto si lo hay), estilos propios
  (adoptedStyleSheets: sin `<style>` ni innerHTML, por Trusted Types y CSP), claro u oscuro con la página. Intro solo
  con la fila marcada (Abajo), Esc la quita; rellena con el setter nativo y `input`/`change` (React lo ve).
- Guardarla: la guarda el gestor del motor al enviar el formulario. OJO: `IsPasswordAutosaveEnabled` no guarda solo
  (WebView2Feedback #4857): el motor PREGUNTA («¿Guardar la contraseña?», una ventana suya) y guarda al pulsar Guardar.
  Comprobado: con Guardar, tras reiniciar el formulario de entrar se rellena con la generada. Pedido el 26/10 que no
  se pierda si la pregunta se cierra: la pregunta sigue siendo de la persona (Pluma no pulsa nada: con UI Automation
  funciona sin foco, pero enciende la accesibilidad del motor para toda la sesión, medido en edge://accessibility; un
  Enter enviado no hace nada). Pluma ve la pregunta (WinEvent EVENT_OBJECT_SHOW del proceso del motor, ventana
  `Chrome_WidgetWin_1` cuyo dueño es la de Pluma; por su título, guardar o actualizar), espera a que se vaya y, si
  el motor no tiene ese usuario y el sitio no es de «nunca», escribe la fila con `passwords_keep` (el pendiente de
  `passwords.cpp`, entra al siguiente arranque) y lo dice en el aviso. Sin pregunta vista no escribe nada; la de
  actualizar no se toca.
- Para saber que es una sugerida: la contraseña la hace Pluma (la página la pide al enfocar) y la página avisa al
  enviar, ambas con XHR síncronos a `<su origen>/.well-known/pluma-passgen/new|sent`, que Pluma contesta en
  WebResourceRequested sin que salga nada (mismo origen: la CSP del sitio lo deja pasar; `sendBeacon` no llega al
  handler, ERR_BLOCKED_BY_CLIENT). Solo vale si la cabecera Origin y la url son el origen de la pestaña (ni marcos de
  otro sitio ni opacos) y la contraseña es una que Pluma hizo para esa pestaña: una página no puede plantar un login.
  Nada en `window` (la clave sigue en el cierre del script). Con una CSP sin 'self', la hace la página y queda solo la
  pregunta del motor. Ni la contraseña, ni el usuario, ni el sitio van al log.
  Probado sin efecto: la preferencia `credentials_enable_autosave` y un mundo aislado por CDP.
- Ajuste `sugerir_contrasenas` (Configuración > Privacidad y datos), encendido. Al cambiarlo (o el idioma) cada vista
  recibe el script nuevo y las páginas abiertas un evento cuyo nombre solo sabe Pluma (`ExecuteScript`); los marcos,
  en su próxima página. En incógnito sale igual y dice que no se guarda.
- Pruebas: `node tools\passgen-test.js` (el generador) y `tools\passgen\prueba.ps1` en la sesión interactiva con
  `PLUMA_EXE` (páginas en `tools\passgen`, una con CSP estricta y Trusted Types, otra que se inventa el aviso con un
  marco de otro sitio; `tools\passgen-e2e.js`): Guardar pulsado como la persona, la escrita a mano y la inventada sin
  tocar, la pregunta cerrada deja la fila pendiente que entra al reiniciar, y un sitio con «nunca» (`nunca.py`) nada.
  OJO: el motor pone su pregunta y su lista de autorrelleno en la pantalla más cercana aunque la ventana de prueba esté fuera; `tools\passgen\ventanas.ps1 -Watch` las saca al instante.

## Contraseñas guardadas (`src/vault.cpp`, lectura en `src/passwords.cpp`)
Pedido el 26/10: «que en el apartado de ajustes se puedan ver las contraseñas guardadas, y se puedan ver y copiar» y
«un icono de un ojo para mostrar la contraseña o copiarla al portapapeles» en la lista de autorrelleno del motor.
- La lista de autorrelleno es del motor (una ventana suya, `Chrome_WidgetWin_1`): Pluma no puede ponerle iconos. En su
  lugar, como la llave de Edge: si el sitio de la pestaña tiene contraseñas guardadas sale una llave en la barra (antes
  del icono de traducir, el libro y la estrella); su panel (página `PAGE_PASSWORDS` del menú, filas `kind 13`) tiene
  cada usuario con copiar arriba y la contraseña con el ojo y copiar abajo, y «Gestionar contraseñas».
- Configuración > Contraseñas: sitio, usuario, contraseña tapada; ojo, copiar contraseña, copiar usuario, abrir el
  sitio y quitar; buscador propio, número y texto sin ninguna. La búsqueda de la configuración la encuentra en los 6
  idiomas (`data-k` con contraseña, password, senha, mot de passe, Passwort...). La lista va aparte del estado
  (`{op:"pw"}`), solo a esa página.
- Lectura: una copia del `Login Data` vivo (y su `-wal`; el motor lo deja leer) del perfil de la ventana, en un hilo;
  si no se puede leer, la de `sync-motor\` del arranque y la página dice «según el último arranque de Pluma» (ese
  arranque copia ya con las contraseñas pendientes metidas: `sync_snapshot_engine` va tras `passwords_apply_pending`).
  La llave mira si cambió el fichero como mucho cada 2 s. Nada se descifra para la lista. En incógnito, no hay.
- Ver o copiar pide que Windows compruebe que es la persona, como Edge y Chromium
  (`chrome/browser/password_manager/password_manager_util_win.cc`, `device_reauth/win`): Windows Hello
  (`UserConsentVerifier`, `RequestVerificationForWindowAsync`) y, sin Hello, `CredUIPromptForWindowsCredentials` con
  `CREDUIWIN_ENUMERATE_CURRENT_USER` comprobado con `LsaLogonUser` contra el SID de esta cuenta (la ventana deja
  cambiar de cuenta). Hecho, esa página (o el panel) puede ver y copiar durante 60 s (el `kAuthValidityPeriod` de
  Chromium); se pierde al recargar o salir de la página y al cerrar el panel. Una contraseña se descifra solo al
  pedirla, de una en una (v10: AES-GCM con la llave de `Local State`; una v20 del motor, «Pluma no puede leerla»), y va
  solo a la página de configuración que la pidió (su dirección se vuelve a mirar al tener la respuesta: si se fue, no
  se manda nada) o al portapapeles. Copiar usuario no pide nada (como Edge).
- Portapapeles: con `ExcludeClipboardContentFromMonitorProcessing`, `CanIncludeInClipboardHistory`=0 y
  `CanUploadToCloudClipboard`=0 (fuera de Win+V y de la nube); a los 30 s se vacía si sigue siendo lo que puso Pluma
  (`GetClipboardSequenceNumber`, sin guardar el valor).
- Quitar: el motor tiene `Login Data` abierto mientras corre (escribir a la vez se arriesga a su base), así que la fila queda en
  `passwords-delete.tsv` (sitio, usuario y su cifrado) y se quita al siguiente arranque, antes del motor y con copia de
  `Login Data`, solo si sigue igual (y sus notas y avisos de filtración). La página dice «Se quita al reiniciar Pluma» y
  deja deshacer. El autorrelleno del motor la ofrece hasta entonces.
- Nunca al registro contraseñas, usuarios ni sitios: solo «contraseñas: verificado», «copiada», «portapapeles vaciado».
- Las páginas de `pluma.local` van con `X-Frame-Options: DENY` y `frame-ancestors 'none'`.
- RAM (26/10, `tres.ps1` con y sin esto a la vez, y otra vez cambiados de sitio): interfaz 13 MB en los dos; total 160
  frente a 153 (las páginas, 65 y 69). Las dos primeras pasadas daban más al segundo lanzado, fuera cual fuera.
- Pruebas: `tools\contrasenas\prueba.ps1` en la sesión interactiva con `PLUMA_EXE` (una guardada por el motor con su
  Guardar, cuatro inventadas con `pluma-prueba:sembrar`, la lista, ver la del motor y una de Pluma, copiar con los
  formatos y el vaciado, quitar al reiniciar, una web sin `chrome.webview`, sin verificar no llega nada, la página que
  se va mientras Windows pregunta no recibe nada, el registro sin nada de eso; fotos claro y oscuro). `-Hello`: la
  pregunta de verdad (sin nada que la conteste) sale, Pluma no revela nada y se va al cerrar la instancia; en un PC sin
  Hello es la de la contraseña de la cuenta, que no acepta cerrarse desde fuera ni sale en PrintWindow (negra), y
  `pluma-prueba:cancelar` solo cancela la de Hello. Solo en instancias de prueba (sus datos y su
  llave son los suyos, en `prueba.<nombre>`; `PLUMA_INSTANCIA` solo vale con 1-32 letras, cifras, - o _, para que
  esa carpeta nunca sea otra): `PLUMA_PRUEBA_VERIFICAR=si|no|lento` contesta sin Windows,
  `PLUMA_PRUEBA_PORTAPAPELES_S` acorta el vaciado; órdenes `claves`, `clave-ver|clave-copiar|clave-usuario=<n>`,
  `lista`, `sembrar`, `cancelar`.

## Descargas (`src/downloads.cpp`)
Arreglado el 26/10 («el PDF descargado no se abre»): con Edge desinstalado (el motor 155 sigue), «Abrir archivo» del
panel de descargas del motor tumbaba el motor entero (volcado: acceso a memoria ya liberada en msedge.dll, proceso
del navegador; claves de Crashpad con el visor de PDF de Edge y Windhawk inyectado). WebView2 no deja entrar en ese
botón, así que cada vista (pestañas, reserva, popups de extensiones) marca sus descargas como suyas en
`DownloadStarting` (`Handled`: el panel del motor no sale); el motor sigue eligiendo el nombre, guardando y pasando
SmartScreen. Salvo los tipos por los que el motor pregunta «¿conservar?» (programas, scripts...: la lista de Chromium
`download_file_types.asciipb`, peligro DANGEROUS o ALLOW_ON_USER_GESTURE en Windows, en `engine_asks`): WebView2 no
tiene cómo contestar eso y con `Handled` se quedaban al 100 % para siempre (.exe, .msi; probado también sin
SmartScreen). Esos siguen con el panel del motor (su «Conservar» va en «Más acciones»; su «Abrir archivo», el de
antes), y la lista de Pluma los sigue sin saltar. El panel es una página del menú (`PAGE_DOWNLOADS`, fila `kind 9`): nombre, barra mientras baja,
estado, cancelar, mostrar en la carpeta, «Abrir carpeta de descargas»; sale solo (quieto, sin foco) al empezar y al
acabar una, y con Ctrl+J y ⋯ > Descargas. Abrir: PDF, páginas, imágenes, SVG y texto en una pestaña (`file://`); lo
demás con ShellExecute («Abrir con» si no hay programa). La lista solo vive en memoria (el historial del motor ya no se
ve). Además de `StateChanged`, un sondeo cada 500 ms mientras baja algo (WebView2Feedback #5638). En las instancias de
prueba abrir con Windows y mostrar en la carpeta solo se apuntan en `pluma.log`. Prueba: `tools\descargas\prueba.ps1`
en la sesión interactiva con `PLUMA_EXE` (`pluma-prueba:descargas|descarga-abrir=n|descarga-carpeta=n|descarga-cancelar=n`;
`tools\descargas\panel-motor.js` lee el panel del motor, `edge://downloads-hub`, que fuera de la pantalla solo se
actualiza con `motor=--disable-features=CalculateNativeWinOcclusion`).
RAM igual (3 pestañas a la vez, dos pasadas: interfaz 13 MB en las dos).

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

## Perfiles (`src/profiles.cpp`)
Pedido el 26/10: separar sus cuentas como en Edge y pasarlas a Pluma de golpe. Plan aprobado: ventana propia por perfil.
- Cada perfil, su perfil de WebView2 (`put_ProfileName`, `pluma-<id>`) en la misma carpeta y el mismo motor que los
  demás: cookies, caché, extensiones y contraseñas aparte. Y sus archivos de Pluma en `g_data` = `perfiles\<id>\`
  (historial, marcadores, sesión, pestaña nueva, extensiones.tsv, `lectura.tsv`, `cache-video`, contraseñas pendientes). Ajustes,
  iconos de webs, scripts y `pluma.log` son de todos (`g_state`).
- El de siempre es «Personal»: sus archivos y el perfil `Default` del motor siguen donde estaban (no se mueve nada).
  `perfiles.tsv` (id, nombre, color); un id no se reutiliza nunca (el motor borra la carpeta de uno quitado al cerrarse).
- Cada perfil que no es el de siempre es otro Pluma.exe (`--perfil=<id>`, ventana y mutex `.perfil.<id>`), como el
  incógnito; no se queda en memoria al cerrarlo y no se actualiza (solo el de siempre, y no mientras haya otros abiertos).
  Lo arranca el de siempre (`CreateProcess`, también si lo pide otro perfil por WM_COPYDATA) para que el Administrador
  de tareas los agrupe. El motor rechaza una ventana con otras opciones que la primera: los hijos reciben las del padre
  (`PLUMA_MOTOR_IDIOMA`, `PLUMA_MOTOR_ARGS`).
- Avatar del perfil en la barra, justo después de la dirección (círculo con inicial y color; como Edge, que no lo pone
  en las pestañas; pedido el 26/10), con su nombre de aviso al posar el ratón. Su panel, como el de Edge: el perfil en
  grande, «Configuración del perfil», «Otros perfiles» (abre su ventana o la trae delante), «Añadir perfil» y «Nueva
  ventana de incógnito». En incógnito, en su sitio, la etiqueta «Incógnito» (su panel: «Cerrar ventana de incógnito»).
  Prueba: `Pluma.exe pluma-prueba:perfil` lo abre sin coger el foco. Configuración > Perfiles (nombre, color, quitar con confirmación,
  que cierra su ventana, borra sus archivos y marca su perfil del motor para borrar con `ICoreWebView2Profile8::Delete`).
  Con más de un perfil, el título dice cuál es y el botón de la barra de tareas lleva su círculo (`SetOverlayIcon`).
- Los enlaces de otros programas van al último perfil usado (`perfil-ultimo.txt`, al activar una ventana); abrir Pluma
  sin enlace abre el de siempre.
- Recortar memoria: solo las páginas de la propia ventana (el motor es compartido; nunca la página que otro perfil tiene
  a la vista).
- Importar: «Todos los perfiles de Edge» crea (o reutiliza por nombre) un perfil de Pluma por cada uno de Edge (su
  nombre, o el correo si Edge le dejó el nombre de serie) y le pone sus marcadores e historial, uno detrás de otro con
  progreso. A un perfil cerrado se le escriben sus archivos cambiando `g_data` un momento; uno abierto guarda lo suyo
  antes (WM_APP+41) y lo vuelve a leer después (WM_APP+40). Contraseñas: un CSV por perfil, en la ventana de ese perfil.
  Pruebas: `PLUMA_EDGE_DIR=<copia de User Data>` (solo en instancias de prueba).

## Extensiones
- La ventanita de una extensión (su popup) se crea a la vista antes de meterle su página: hecha en una ventana oculta,
  la página se quedaba «hidden» y no pintaba nada (salía vacía, 25/10). Se mide varias veces en su primer segundo
  (uBlock la rellena poco a poco y salía cortada).
- Sus avisos (26/10, «se queda como congelado» tras copiar un código de Authenticator): `chrome.permissions.request`
  (Authenticator pide `clipboardWrite` al copiar), `alert`... salen en una ventana del motor propiedad de la ventanita,
  que desactiva la ventanita y la ventana de Pluma hasta que se contesta. Cogía el foco, Pluma cerraba la ventanita
  (clic fuera), el aviso se iba con ella y la ventana de Pluma se quedaba desactivada: la página no recibía clics.
  Ahora la ventanita sigue mientras su aviso está arriba (si al contestarlo no vuelve a ella, se cierra) y, si se
  cierra con el aviso arriba, Pluma reactiva su ventana. En las pruebas la ventanita se queda fuera de la pantalla y
  no coge el foco. Prueba: `tools\popup\prueba.ps1 [-Authenticator <su carpeta>]` en la sesión interactiva con
  `PLUMA_EXE` (una extensión de prueba que copia como Authenticator y se cierra sola; mira el estado de las ventanas,
  no un clic por DevTools, que llega a la página aunque la ventana esté desactivada).
- Importar las de Edge (26/10, casilla «Extensiones (con sus ajustes)» del asistente, también en «Todos los perfiles»):
  las que instaló la persona salen de `Secure Preferences` (`location` 1, no `was_installed_by_default`; las de Edge
  son 5 y 10). Su carpeta `Extensions\<id>\<versión>` se copia a `extensiones\<id>` del perfil de Pluma (sin
  `_metadata`: un nombre con «_» arriba impide cargarla) y se carga con `AddBrowserExtension`; el manifiesto trae la
  `key` de la tienda, así que el id es el mismo (comprobado). Sus ajustes (`Local Extension Settings\<id>`, `Sync
  Extension Settings\<id>`, su IndexedDB) se copian al perfil del motor antes de cargarla; si el motor los tiene
  abiertos (ya estaba) esperan en `extensiones-ajustes\` al siguiente arranque. Las MV2 se intentan cargar; si no cargan, se
  dice que es por eso; uBlock Origin (Lite o no) no se trae: Pluma ya lleva uBOL. Para otro perfil se carga con una vista oculta de
  su perfil del motor. Las que no cargan se borran (si no, se reintentarían en cada arranque).
- Clic derecho en el botón de una extensión: Opciones, Desactivar, Quitar de Pluma (como Chrome). Las que trae Pluma
  (uBlock) quitadas quedan marcadas en `extensiones.tsv` y ⋯ > Extensiones ofrece «Volver a poner uBlock Origin Lite».

## Seguridad
- Motor de Edge, actualizado por Windows. Páginas en AppContainer (`msWebView2RendererAppContainer`), aislamiento de
  sitios activado (no quitarlo para ahorrar memoria), SmartScreen, prevención de rastreo equilibrada, uBlock Lite.
- Ninguna página puede hablar con Pluma: sin mensajes web ni objetos del host. OJO: `chrome.webview.postMessage`
  existe y no falla en cualquier página (visto el 26/10); lo que protege es que Pluma mira el origen de cada mensaje
  (`settings_page_url` en `WebMessageReceived`), probado en `tools\contrasenas\prueba.ps1` (una web pide copiar: nada). Contraseñas con el gestor de WebView2
  (cifradas con la cuenta de Windows); verlas o copiarlas en Pluma pide Windows Hello o la contraseña de la cuenta. Solo abren apps sin preguntar los protocolos de `protocolos` (ytmp3).

## Trampas ya vistas
- uBlock Origin clásico (MV2) se instala en WebView2 155 pero NO bloquea (webRequestBlocking muerto): uBlock Lite sí.
- Google mira la marca «Microsoft Edge WebView2» de los client hints; con la de serie dejó buscar la cuenta (probado
  con un correo inventado). Si un día bloquea: `--disable-features=msWebView2RetainUserAgentMetadataOnSetUserAgent`
  + `put_UserAgent` quita la marca (probado).
- Repintar en bucle (InvalidateRect tras cada fotograma) deja sin turno a los WM_TIMER (Windows solo los da sin
  pintura pendiente): no se cerraba, no guardaba la sesión. Por eso las animaciones van por el hilo de DwmFlush.
- `small` es una macro de Windows (`rpcndr.h`: `char`): un campo que se llame así no compila con mensajes absurdos.
- `@match` con `*`: escapar todo menos el asterisco y luego `*` -> `.*` (antes quedaba `/*` y nunca casaba).
- Desde PowerShell, `FindWindowW` con título nulo pide `[NullString]::Value`.
- `tools\passgen\ventanas.ps1` solo mira procesos `msedgewebview2.exe`: con todos, un id de proceso reutilizado metió en
  el árbol el Administrador de tareas del usuario (26/10; Windows no dejó moverlo).
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
- `tools\prueba.ps1 -Salir` tardaba siempre 10 s y un Pluma de prueba arrancado justo después a veces no salía, sin
  dejar nada en `pluma.log` (26/10): su `Mutex.TryOpenExisting` dejaba el handle abierto y el mutex seguía vivo con
  Pluma ya fuera; el nuevo lo veía ocupado, no encontraba ventana y se iba a los 5 s. Ahora cierra el handle.
- Abrir Pluma justo mientras el anterior se cierra del todo no hacía nada (le pasaba la url a una ventana que se
  iba): ahora la ventana que sale lleva la propiedad `saliendo` y el nuevo espera a que se vaya el mutex y arranca.

## Ventanas (`src/main.cpp`, `tabs_move` y el arrastre en `src/ui.cpp`)
Pedido el 26/10: «abrir pestañas diferentes, sacar una pestaña a ventana diferente, juntar una pestaña a otra ventana»,
como Edge. Plan aprobado: todas las ventanas en el mismo proceso (uno por perfil).
- `Window` (pluma.h): sus pestañas, la activa, pantalla completa. `g_cw` es la ventana «actual» y `g_win`, `g_bar`,
  `g_edit`, `g_tabs`, `g_active`, `g_fullscreen` son macros sobre ella: el código de siempre trabaja en la actual.
  `WinScope ws(w)` la cambia mientras dura; cada procedimiento de ventana, el bucle de mensajes y cada respuesta del
  motor (`tab_by_id` + `WinScope ws(win_of_tab(t))`) ponen la suya. Lo de toda la app (temporizadores de historial,
  sesión, actualizaciones, recortes, precarga; `ui_post`; los fotogramas de animación) va en `g_app`, una ventana solo
  de mensajes. Un TIMERPROC mata su temporizador con el HWND que recibe. El estado de la barra (escala, dibujo, iconos,
  ratón, aviso) es de cada ventana (`UiWin`, al final de ui.cpp); los paneles (menú, sugerencias, burbuja) son de
  todas y se abren sobre la ventana actual.
- Una pestaña pasa a otra ventana VIVA (`tab_move`: `put_ParentWindow` de su vista y `SetParent` de la ventana de mpv):
  nada se recarga, un vídeo sigue sonando (comprobado: 15,8 -> 19,9 s movido, 34,2 -> 38,2 s tras volver arrastrado)
  y lo escrito en un formulario sigue ahí. Los controles del reproductor y la foto de la página son uno para todas:
  van a la ventana cuya pestaña a la vista tiene el reproductor (`g_ctl_win`).
- Nueva ventana: Ctrl+N, ⋯ > Nueva ventana, «Abrir en una ventana nueva» en el menú de un enlace, y abrir Pluma (su
  icono, clic central en la barra de tareas) con una ventana ya a la vista, como Edge. Si no hay ninguna a la vista,
  vuelve la de antes. Otro perfil que pide traer la ventana usa `WM_APP+4` (delante), no `WM_APP+1` (abriría otra).
- Cerrar una de varias: se cierra y sus pestañas van a «Reabrir pestaña cerrada». La última: residente como siempre
  (las demás que quedaran ocultas se cierran). Ctrl+Mayús+Q guarda todas.
- Menú de la pestaña: «Mover a una ventana nueva» y «Mover a Ventana N: <título>» por cada otra.
- Arrastre, copiado de `TabDragController` de Chromium: más de 5 px empieza; dentro de la tira (15 DIP de margen arriba
  y abajo) solo reordena; fuera, la pestaña pasa a una ventana nueva del tamaño de la de antes que sigue al ratón;
  sobre la tira de otra ventana entra en el hueco de debajo. La única pestaña de una ventana mueve la ventana entera
  y, si entra en otra, esa ventana se cierra. Soltada fuera de todo, la ventana nueva se queda ahí. Todo en píxeles de
  pantalla de `GetMessagePos` (la ventana que sigue al ratón ya se movió cuando llega el mensaje); con escala distinta
  en otra pantalla, el agarre se recalcula y `WM_DPICHANGED` no mueve la ventana arrastrada.
- Sesión: un bloque por ventana; desde la segunda, una línea `ventana <izq> <arriba> <der> <abajo> <maximizada>` sin
  tabulador (un Pluma anterior la salta y abre todo en una). La primera sigue en `ventana.bin`.
- Pruebas: `Pluma.exe pluma-prueba:ventana=<id>` (sobre cuál van las siguientes), `ventana-nueva`, `ventanas` (lista en
  `pluma.log`), `mover-nueva`, `mover=<id>`, `menu-pestana`, `cerrar`, `pos` (segundo del vídeo),
  `arrastre-empieza=<n>`, `arrastre=dx,dy`, `arrastre-a=<id>,x,y`, `arrastre-suelta`; y el arrastre con mensajes de
  ratón de verdad, `captura.exe "PlumaVentana.<nombre>|<título>" x.png drag x1 y1 x2 y2 pasos`. Las ventanas de prueba
  van una debajo de otra fuera de la pantalla (la segunda 960 px más abajo).
- Medido el 26/10 (example.com y example.org): 136 MB con una ventana, 134 con dos (la interfaz, 13 MB en los dos casos).
- Sin probar en las pruebas (todo fuera de la pantalla): el cambio de escala al pasar de una pantalla a otra
  arrastrando, la captura del ratón real y el clic central en la barra de tareas.
