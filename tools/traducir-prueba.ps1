# Prueba de la traducción de páginas de punta a punta, en una instancia de prueba (nunca el Pluma del usuario):
#   $env:PLUMA_EXE = '<Pluma.exe compilado aparte>'; tools\traducir-prueba.ps1 -Salida <carpeta> [-Nombre tr] [-Puerto 9473]
# Lanzarlo en la sesión con escritorio (las vistas no dibujan por ssh), sin sonido. Deja en -Salida las capturas y un
# resumen (resumen.txt). Pasos: la página de prueba de tools\traducir-prueba.js (qué se traduce, qué no, el texto
# nuevo, «Mostrar original» deja el HTML igual), Wikipedia en francés (nada sale solo; la tarjeta del icono, sus
# botones y su «⋯»; burbuja de la selección; la página siguiente de esa pestaña no se traduce sola), una en castellano con destino inglés (el caso del 26/10), en japonés,
# YouTube (resultados de búsqueda), la traducción automática encendida (la oferta sí sale) y Configuración > Idioma,
# en oscuro y en claro, y una pestaña movida a otra ventana (su tarjeta, traducir y el original allí). El ajustes.ini de partida es uno de antes (ajustes_v=2, traducir_ofrecer=1): debe quedar apagada.
param([string]$Salida, [string]$Nombre = 'tr', [int]$Puerto = 9473)
$ErrorActionPreference = 'Continue'
$exe = $env:PLUMA_EXE
if (-not $exe -or -not (Test-Path $exe)) { 'PLUMA_EXE: falta el exe de prueba'; exit 2 }
$tools = $PSScriptRoot
$root = Split-Path $tools
$node = Join-Path $env:LOCALAPPDATA 'fp-tools\node\node.exe'
if (-not (Test-Path $node)) { $node = 'node' }
New-Item -ItemType Directory -Force $Salida | Out-Null
$env:PLUMA_INSTANCIA = $Nombre
$env:PLUMA_DEPURAR = "$Puerto"
$env:PLUMA_SIN_SONIDO = '1'
$cls = "PlumaVentana.$Nombre"
$log = Join-Path $Salida 'resumen.txt'
Set-Content $log "prueba de traducción $(Get-Date -Format s)" -Encoding UTF8
function Note($s) { Add-Content $log $s -Encoding UTF8; $s }
# extensiones y fuentes junto al exe (enlaces, nada copiado)
$dir = Split-Path $exe
foreach ($d in 'extensiones', 'fuentes') { if (-not (Test-Path (Join-Path $dir $d))) { cmd /c mklink /J "$(Join-Path $dir $d)" "$(Join-Path $root $d)" | Out-Null } }
# datos limpios, en castellano
$data = Join-Path $env:LOCALAPPDATA "Pluma\prueba.$Nombre"
& (Join-Path $tools 'prueba.ps1') -Nombre $Nombre -Salir
Remove-Item -Recurse -Force $data -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $data | Out-Null
Set-Content (Join-Path $data 'ajustes.ini') "[pluma]`r`nidioma=es`r`najustes_v=2`r`ntema=oscuro`r`ntraducir_ofrecer=1`r`n" -Encoding Unicode

function Pluma([string]$what) { & $exe $what; Start-Sleep -Milliseconds 400 }
function Shot([string]$name) {
    # the picture Pluma takes of itself, its panels with their real transparency (captura.exe gets them opaque)
    $f = Join-Path $data "$name.png"
    Remove-Item $f -ErrorAction SilentlyContinue
    & $exe "pluma-prueba:foto=$name"
    for ($i = 0; $i -lt 20 -and -not (Test-Path $f); $i++) { Start-Sleep -Milliseconds 250 }
    Start-Sleep -Milliseconds 300
    if (Test-Path $f) { Move-Item -Force $f (Join-Path $Salida "$name.png"); Note "captura: $name.png" | Out-Null }
    else { & (Join-Path $tools 'captura.exe') $cls (Join-Path $Salida "$name.png") | Out-Null; Note "captura (captura.exe): $name.png" | Out-Null }
}
function Cdp([string]$part, [string]$expr) { & $node (Join-Path $tools 'cdp.js') $Puerto $part $expr 2>&1 | Out-String }
function WaitPage([string]$part, [int]$s = 25) {
    for ($i = 0; $i -lt $s * 2; $i++) { $r = Cdp $part 'document.readyState'; if ($r -match 'complete') { return $true }; Start-Sleep -Milliseconds 500 }
    Note "no carga: $part"; return $false
}
function WaitTranslated([string]$part, [int]$s = 40) {
    for ($i = 0; $i -lt $s * 2; $i++) {
        $r = Cdp $part 'window.__plumaTr ? JSON.stringify(__plumaTr.stats()) : "no"'
        if ($r -match '\\"done\\":(\d+).*\\"left\\":0' -and [int]$Matches[1] -gt 0) { Note "traducida ${part}: $($r.Trim())" | Out-Null; return $true }
        Start-Sleep -Milliseconds 500
    }
    Note "no se tradujo del todo: $part $r"; return $false
}
function Quiet([string]$part, [switch]$OnlyPage) {
    # nothing by itself: no translator in the page and no offer in the log (-OnlyPage: once the log already has one)
    $r = (Cdp $part 'typeof window.__plumaTr').Trim()
    $pl = Join-Path $data 'pluma.log'
    $offered = -not $OnlyPage -and (Test-Path $pl) -and (Select-String -Path $pl -Pattern 'traducción: ofrecida' -Quiet)
    $good = $r -match 'undefined' -and -not $offered
    Note "sin traducir ni ofrecer solo (${part}): $(if ($good) { 'bien' } else { "MAL ($r, ofrecida: $offered)" })" | Out-Null
    return $good
}
$ok = $true
try {
    & (Join-Path $tools 'prueba.ps1') -Nombre $Nombre -Puerto $Puerto -Url 'https://example.com/'
    if (-not (WaitPage 'example.com')) { throw 'example.com no carga' }
    # 1. the fixture
    foreach ($step in 'prep', 'TRADUCIR', 'check', 'dyn', 'ORIGINAL', 'restored') {
        if ($step -eq 'TRADUCIR') { Pluma 'pluma-prueba:traducir'; continue }
        if ($step -eq 'ORIGINAL') { Shot 'fixture-traducida'; Pluma 'pluma-prueba:original'; continue }
        $r = & $node (Join-Path $tools 'traducir-prueba.js') $Puerto $step 2>&1 | Out-String
        Note $r.Trim() | Out-Null
        if ($LASTEXITCODE -ne 0) { $ok = $false }
    }
    # 2. French: the offer, the selection's bubble, translated, its popup and the main menu
    Pluma 'https://fr.wikipedia.org/wiki/Croissant_(viennoiserie)'
    WaitPage 'fr.wikipedia.org' | Out-Null
    Start-Sleep 2
    Note ("idioma detectado: " + (Cdp 'fr.wikipedia.org' 'document.documentElement.lang').Trim()) | Out-Null
    if (-not (Quiet 'fr.wikipedia.org')) { $ok = $false }
    Shot 'fr-sin-oferta'
    Cdp 'fr.wikipedia.org' "(() => { const p = [...document.querySelectorAll('#mw-content-text p')].find(x => x.textContent.trim().length > 80); scrollTo(0, 0); getSelection().selectAllChildren(p); return p.textContent.slice(0, 60); })()" | Out-Null
    Pluma 'pluma-prueba:seleccion'
    Start-Sleep 3
    Shot 'fr-burbuja'
    Cdp 'fr.wikipedia.org' 'getSelection().removeAllRanges()' | Out-Null
    Pluma 'pluma-prueba:menu-principal'
    Start-Sleep 1
    Shot 'menu-principal'
    # the card (the icon's click) and its "Traducir"
    Pluma 'pluma-prueba:menu'
    Start-Sleep 1
    Shot 'tarjeta-oscuro'
    Pluma 'pluma-prueba:tarjeta=opciones'
    Start-Sleep 1
    Shot 'tarjeta-opciones-oscuro'
    Pluma 'pluma-prueba:menu'
    Start-Sleep 1
    Pluma 'pluma-prueba:tarjeta=1'
    if (-not (WaitTranslated 'fr.wikipedia.org')) { $ok = $false }
    Shot 'fr-traducida'
    Pluma 'pluma-prueba:menu'
    Start-Sleep 1
    Shot 'tarjeta-traducida-oscuro'
    Pluma 'pluma-prueba:menu-principal'
    Start-Sleep 1
    Shot 'menu-principal-traducida'
    # "Mostrar original" from the card
    Pluma 'pluma-prueba:menu'
    Start-Sleep 1
    Pluma 'pluma-prueba:tarjeta=2'
    Start-Sleep 2
    Shot 'fr-original'
    $r = Cdp 'fr.wikipedia.org' "document.querySelector('#firstHeading').textContent"
    Note "original otra vez: $($r.Trim())" | Out-Null
    $r = Cdp 'fr.wikipedia.org' 'window.__plumaTr ? JSON.stringify(__plumaTr.stats()) : "no"'
    Note "tras mostrar original: $($r.Trim())" | Out-Null
    # 2b. a Spanish page with English as the target (what the person saw on 26/10): nothing by itself
    Pluma 'pluma-prueba:destino=en'
    Pluma 'https://es.wikipedia.org/wiki/Churro'
    WaitPage 'es.wikipedia.org' | Out-Null
    Start-Sleep 3
    if (-not (Quiet 'es.wikipedia.org')) { $ok = $false }
    Shot 'es-destino-en'
    Pluma 'pluma-prueba:menu'
    Start-Sleep 1
    Shot 'tarjeta-es-en'
    Pluma 'pluma-prueba:destino=es'
    # 3. Japanese
    Pluma 'https://ja.wikipedia.org/wiki/%E5%AF%BF%E5%8F%B8'
    WaitPage 'ja.wikipedia.org' | Out-Null
    Start-Sleep 2
    if (-not (Quiet 'ja.wikipedia.org')) { $ok = $false }
    Pluma 'pluma-prueba:traducir'
    if (-not (WaitTranslated 'ja.wikipedia.org')) { $ok = $false }
    Shot 'ja-traducida'
    # 4. YouTube: search results (dynamic), and more of them after scrolling
    Pluma 'https://www.youtube.com/results?search_query=documentaire+fran%C3%A7ais+histoire'
    Start-Sleep 8
    # right click on selected French text (what the person did): the engine's menu is logged, never shown, and
    # Pluma's row chosen as a click would
    $r = Cdp 'youtube.com' "(() => { const e = [...document.querySelectorAll('body *')].find(x => !x.children.length && x.textContent.length > 40 && /\b(les|des|une|est|dans)\b/.test(x.textContent)); if (!e) return 'none'; e.id = 'pluma-sel'; e.scrollIntoView({block: 'center'}); getSelection().selectAllChildren(e); return e.textContent.slice(0, 60); })()"
    Note "selección en youtube: $($r.Trim())" | Out-Null
    Pluma 'pluma-prueba:clic-derecho'
    & $node (Join-Path $tools 'clic.js') $Puerto 'youtube.com' '#pluma-sel' right | Out-Null
    Start-Sleep 3
    Shot 'youtube-clic-derecho'
    Cdp 'youtube.com' 'getSelection().removeAllRanges(); scrollTo(0, 0); 1' | Out-Null
    Start-Sleep 1
    Pluma 'pluma-prueba:traducir'
    WaitTranslated 'youtube.com' | Out-Null
    Shot 'youtube-traducida'
    Cdp 'youtube.com' 'scrollTo(0, document.documentElement.scrollHeight); 1' | Out-Null
    Start-Sleep 6
    Shot 'youtube-bajando'
    Cdp 'youtube.com' 'scrollTo(0, 0); 1' | Out-Null
    $r = Cdp 'youtube.com' 'window.__plumaTr ? JSON.stringify(__plumaTr.stats()) : "no"'
    Note "youtube tras bajar: $($r.Trim())" | Out-Null
    # 4b. the automatic translation turned on: the offer opens by itself on a new site
    Pluma 'pluma-prueba:automatica=1'
    Pluma 'https://de.wikipedia.org/wiki/Brezel'
    WaitPage 'de.wikipedia.org' | Out-Null
    Start-Sleep 3
    $pl = Join-Path $data 'pluma.log'
    $offered = (Test-Path $pl) -and (Select-String -Path $pl -Pattern 'traducción: ofrecida \(de\)' -Quiet)
    Note "automática encendida, oferta sola: $(if ($offered) { 'bien' } else { 'MAL' })" | Out-Null
    if (-not $offered) { $ok = $false }
    Shot 'oferta-automatica'
    Pluma 'pluma-prueba:automatica=0'
    # 5. settings, dark then light; the bubble in light
    Pluma 'https://pluma.local/settings#idioma'
    WaitPage 'pluma.local' | Out-Null
    Start-Sleep 1
    Cdp 'pluma.local' "document.getElementById('tr-card').scrollIntoView({block: 'center'}); 1" | Out-Null
    Start-Sleep 1
    Shot 'ajustes-oscuro'
    Cdp 'pluma.local' "chrome.webview.postMessage({ op: 'set', key: 'theme', value: 0 }); 1" | Out-Null
    Start-Sleep 2
    Cdp 'pluma.local' "document.getElementById('tr-card').scrollIntoView({block: 'center'}); 1" | Out-Null
    Start-Sleep 1
    Shot 'ajustes-claro'
    Pluma 'https://fr.wikipedia.org/wiki/Baguette'
    WaitPage 'Baguette' | Out-Null
    Start-Sleep 2
    Cdp 'Baguette' "(() => { const p = [...document.querySelectorAll('#mw-content-text p')].find(x => x.textContent.trim().length > 80); getSelection().selectAllChildren(p); return 1; })()" | Out-Null
    Pluma 'pluma-prueba:seleccion'
    Start-Sleep 3
    Shot 'claro-burbuja'
    Cdp 'Baguette' 'getSelection().removeAllRanges(); 1' | Out-Null
    Pluma 'pluma-prueba:menu'
    Start-Sleep 1
    Shot 'tarjeta-claro'
    Pluma 'pluma-prueba:automatica=1'   # (its ... with the automatic translation on: all its rows)
    Pluma 'pluma-prueba:tarjeta=opciones'
    Start-Sleep 1
    Shot 'tarjeta-opciones-claro'
    Pluma 'pluma-prueba:automatica=0'
    Pluma 'pluma-prueba:menu'
    Start-Sleep 1
    Pluma 'pluma-prueba:tarjeta=1'
    WaitTranslated 'Baguette' | Out-Null
    Pluma 'pluma-prueba:menu'
    Start-Sleep 1
    Shot 'tarjeta-traducida-claro'
    # the next page in that tab: not translated by itself (only the page it was asked on)
    Cdp 'Baguette' "location.href = 'https://fr.wikipedia.org/wiki/Pain_au_chocolat'; 1" | Out-Null
    Start-Sleep 2
    WaitPage 'Pain_au_chocolat' | Out-Null
    Start-Sleep 2
    if (-not (Quiet 'Pain_au_chocolat' -OnlyPage)) { $ok = $false }
    # 6. the tab moved to a second window: its card opens there, and translating and showing the original work there
    Pluma 'pluma-prueba:mover-nueva'
    Start-Sleep 2
    Pluma 'pluma-prueba:ventanas'
    Pluma 'pluma-prueba:menu'
    Start-Sleep 1
    Shot 'ventana2-tarjeta'
    Pluma 'pluma-prueba:tarjeta=1'
    if (-not (WaitTranslated 'Pain_au_chocolat')) { $ok = $false }
    Shot 'ventana2-traducida'
    Pluma 'pluma-prueba:menu'
    Start-Sleep 1
    Pluma 'pluma-prueba:tarjeta=2'
    Start-Sleep 2
    $r = Cdp 'Pain_au_chocolat' 'window.__plumaTr ? JSON.stringify(__plumaTr.stats()) : "no"'
    $back = $r -match '\\"on\\":false'
    Note "segunda ventana, original otra vez: $(if ($back) { 'bien' } else { "MAL $($r.Trim())" })" | Out-Null
    if (-not $back) { $ok = $false }
    # what the log says
    $pl = Join-Path $data 'pluma.log'
    if (Test-Path $pl) { Get-Content $pl -Encoding UTF8 | Select-String 'traducción' | ForEach-Object { Note "log: $_" | Out-Null } }
} catch { Note "FALLA: $_" | Out-Null; $ok = $false }
finally {
    & (Join-Path $tools 'prueba.ps1') -Nombre $Nombre -Salir
    Start-Sleep 1
    # anything of this instance still alive (its exe, or its engine on its data folder): only ours
    Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -eq $exe -or ($_.CommandLine -and $_.CommandLine -like "*prueba.$Nombre*") } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}
Note ($(if ($ok) { 'RESULTADO: bien' } else { 'RESULTADO: con fallos' })) | Out-Null
Get-Content $log
