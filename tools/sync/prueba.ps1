# Backup of the whole browser, end to end, in two test instances (never the user's Pluma): run in the interactive
# session with PLUMA_EXE = a build elsewhere. In t213a a local site signs in (a persistent HttpOnly cookie, a value in
# localStorage, another in IndexedDB) and a test extension keeps a value in chrome.storage; Pluma restarts (the
# engine's locked cookies are copied at start), backs up to a folder (PLUMA_SYNC_CARPETA) and the file is checked to
# hold nothing readable of it (the same strings are in the raw profile: control). A clean t213b restores it
# (PLUMA_SYNC_FICHERO) and must come back signed in with the three values, and the extension with its own.
# -GrandeMB: that much noise in the site's Cache Storage first (time and memory of a backup of a real-sized profile).
param([string]$Salida = '', [int]$GrandeMB = 0)
$ErrorActionPreference = 'Continue'
$L = $env:LOCALAPPDATA
$src = Split-Path (Split-Path $PSScriptRoot)
$out = if ($Salida) { $Salida } else { "$(Split-Path $env:PLUMA_EXE)\out" }
New-Item -ItemType Directory -Force $out | Out-Null
$env:PLUMA_SIN_SONIDO = '1'
$node = "$L\fp-tools\node\node.exe"
$cap = "$src\tools\captura.exe"
$site = 'http://127.0.0.1:8213/'
$ext = 'chrome-extension://jecpbebebpkhogpohgkojjbolacghlod/pagina.html'
$M = 'q' + (-join ((1..20) | ForEach-Object { '{0:x}' -f (Get-Random -Maximum 16) }))
$PW = 'prueba-' + (Get-Random)
$copias = "$out\copias"
function FreePort($p) { while ($p -lt 9499) { try { Invoke-RestMethod "http://127.0.0.1:$p/json/version" -TimeoutSec 1 | Out-Null; $p++ } catch { return $p } }; $p }
$pa = FreePort 9441; $pb = FreePort ($pa + 1)
$fails = 0
function Check($what, $ok) { if ($ok) { "ok    $what" } else { "FALLA $what"; $script:fails++ } }
function WaitFor($sb, $ms = 15000) { for ($i = 0; $i -lt $ms / 250; $i++) { if (& $sb) { return $true }; Start-Sleep -Milliseconds 250 }; return $false }
function Eval($port, $url, $expr) {
  $r = & $node "$src\tools\cdp.js" $port $url $expr 2>$null
  if ($LASTEXITCODE -ne 0 -or -not $r -or $r -eq 'undefined') { return $null }
  try { ($r -join "`n") | ConvertFrom-Json } catch { $null }   # (an exception from the page, or a page still loading)
}
function Open($name, $url) { $env:PLUMA_INSTANCIA = $name; & $env:PLUMA_EXE $url }
function Engine($name) { @(Get-CimInstance Win32_Process -Filter "Name='msedgewebview2.exe'" | Where-Object { $_.CommandLine -like "*prueba.$name\*" }) }
function Plumas { @(Get-CimInstance Win32_Process -Filter "Name='Pluma.exe'" | Where-Object { $_.ExecutablePath -eq $env:PLUMA_EXE }) }
# the next one starts only once this one and its engine are gone (one started meanwhile hands its address over and ends)
function Quit($name, $port) {
  & "$src\tools\prueba.ps1" -Nombre $name -Puerto $port -Salir
  if (-not (WaitFor { (Plumas).Count -eq 0 -and (Engine $name).Count -eq 0 } 60000)) { "   ($name sigue vivo tras 60 s: Pluma $((Plumas).Count), motor $((Engine $name).Count))" }
}
# a byte string in a file, as ASCII/UTF-8 and as UTF-16LE
function Holds([byte[]]$b, [string]$s) {
  $t = [Text.Encoding]::GetEncoding(28591).GetString($b)
  $t.Contains($s) -or $t.Contains((-join ($s.ToCharArray() | ForEach-Object { "$_`0" })))
}
function Raw($dir, $s) {
  $n = 0
  Get-ChildItem -LiteralPath $dir -Recurse -File -Force -ErrorAction SilentlyContinue | ForEach-Object {
    try { $fs = [IO.File]::Open($_.FullName, 'Open', 'Read', 'ReadWrite, Delete'); $b = New-Object byte[] $fs.Length; [void]$fs.Read($b, 0, $b.Length); $fs.Close(); if (Holds $b $s) { $n++ } } catch {}
  }
  $n
}

$server = $null; $done = $false
try {
  foreach ($n in 't213a', 't213b') { Quit $n 0; Remove-Item -Recurse -Force "$L\Pluma\prueba.$n" -ErrorAction SilentlyContinue }
  Remove-Item -Recurse -Force $copias -ErrorAction SilentlyContinue
  $server = Start-Process $node -ArgumentList "`"$PSScriptRoot\servidor.js`"", 8213, $M -WindowStyle Hidden -PassThru
  $da = "$L\Pluma\prueba.t213a"; $db = "$L\Pluma\prueba.t213b"
  New-Item -ItemType Directory -Force "$da\extensiones" | Out-Null
  Copy-Item -Recurse "$PSScriptRoot\extension" "$da\extensiones\pluma-sync-test"

  "-- t213a: entrar en el sitio de prueba y guardar en la extension"
  $env:PLUMA_SYNC_CARPETA = $copias
  & "$src\tools\prueba.ps1" -Nombre t213a -Puerto $pa -Url "${site}entrar"
  $ok = WaitFor { $e = Eval $pa $site 'window.__estado || null'; $e -and $e.cookie -and $e.ls -and $e.idb } 30000
  Check 'sesion iniciada en t213a (cookie, localStorage, IndexedDB)' $ok
  Open t213a $ext
  $ok = WaitFor { (Eval $pa $ext "chrome.storage.local.set({ t213: 'ext$M' }).then(() => chrome.storage.local.get('t213')).then((r) => r.t213)") -eq "ext$M" } 20000
  Check 'la extension guarda su valor' $ok
  if ($GrandeMB) {
    Open t213a "${site}?grande=$GrandeMB"
    Check "$GrandeMB MB en Cache Storage" (WaitFor { (Eval $pa "${site}?grande" 'window.__grande === true') -eq $true } 60000)
  }
  Start-Sleep 35   # the engine writes its cookies to disk every 30 s
  Quit t213a $pa

  "-- t213a otra vez: copia en una carpeta"
  & "$src\tools\prueba.ps1" -Nombre t213a -Puerto $pa -Url 'pluma://settings'
  if (-not (WaitFor { (Eval $pa 'pluma.local' "typeof sync") -eq 'function' } 45000)) { "   (sin Configuracion en t213a: Pluma $((Plumas).Count), motor $((Engine t213a).Count))" }
  $proc = @(Get-CimInstance Win32_Process -Filter "Name='Pluma.exe'" | Where-Object { $_.ExecutablePath -eq $env:PLUMA_EXE })[0]
  $peak0 = $proc.PeakPageFileUsage
  [void](Eval $pa 'pluma.local' "sync({ cmd: 'folder' }); sync({ cmd: 'password', pw: '$PW', hint: '' }); 'ok'")
  $ok = WaitFor { @(Get-ChildItem "$copias\Pluma-*.plumabk" -ErrorAction SilentlyContinue).Count -ge 1 -and @(Get-ChildItem "$copias\*.tmp" -ErrorAction SilentlyContinue).Count -eq 0 } 120000
  Check 'copia hecha' $ok
  if (-not $ok) { Get-Content "$da\pluma.log" -Tail 25 | ForEach-Object { "   log: $_" }; throw 'sin copia' }
  $file = @(Get-ChildItem "$copias\Pluma-*.plumabk" | Sort-Object Name)[-1].FullName
  Start-Sleep 2
  [void](Eval $pa 'pluma.local' "document.getElementById('sincronizar').scrollIntoView(); 'ok'")
  Start-Sleep 1
  & $cap 'PlumaVentana.t213a' "$out\1-copia-hecha.png" | Out-Null
  Get-Content "$da\pluma.log" | Select-String 'copia de seguridad hecha|copia de las bases' | ForEach-Object { "   " + $_.Line }
  "   tamano de la copia: {0:N0} KB" -f ((Get-Item $file).Length / 1KB)
  $peak1 = (Get-CimInstance Win32_Process -Filter "ProcessId=$($proc.ProcessId)").PeakPageFileUsage
  "   memoria privada maxima de Pluma: {0:N0} MB antes de la copia, {1:N0} MB despues; ahora {2:N0} MB" -f ($peak0 / 1KB), ($peak1 / 1KB), ((Get-Process -Id $proc.ProcessId).PrivateMemorySize64 / 1MB)
  Check 'en la carpeta solo hay copias' (@(Get-ChildItem $copias -Force | Where-Object { $_.Name -notlike 'Pluma-*.plumabk' }).Count -eq 0)

  "-- nada legible en la copia (y si en el perfil: control)"
  $raw = Raw "$da\webview2\EBWebView\Default" $M
  Check "control: la marca esta en $raw ficheros del perfil" ($raw -ge 3)
  $b = [IO.File]::ReadAllBytes($file)
  foreach ($s in @($M, "c$M", "ls$M", "idb$M", "ext$M", '127.0.0.1', '8213', 'indexeddb', 'Local Storage', 'Cookies', 'Login Data', 'jecpbebeb', 't213db', 'Preferences')) {
    Check "la copia no contiene '$s'" (-not (Holds $b $s))
  }
  Quit t213a $pa

  "-- t213b limpia: restaurar"
  $env:PLUMA_SYNC_FICHERO = $file
  & "$src\tools\prueba.ps1" -Nombre t213b -Puerto $pb -Url 'pluma://settings'
  [void](WaitFor { (Eval $pb 'pluma.local' "typeof sync") -eq 'function' } 30000)
  $f = $file -replace '\\', '\\'
  [void](Eval $pb 'pluma.local' "sync({ cmd: 'restore', path: '$f', password: '$PW' }); 'ok'")
  $ok = WaitFor { (Test-Path "$db\pluma.log") -and (Select-String -Path "$db\pluma.log" -Pattern 'restauraci.n aplicada' -Quiet) } 60000
  Check 'restauracion aplicada' $ok
  Get-Content "$db\pluma.log" | Select-String 'restauraci' | ForEach-Object { "   " + ($_.Line -replace ' en [A-Z]:\\.*$', '') }
  Check 'sin fallos' (Select-String -Path "$db\pluma.log" -Pattern 'restauraci.n aplicada \(0 fallos\)' -Quiet)
  Check 'restore-pending borrado' (-not (Test-Path "$db\restore-pending"))
  Check 'lo de antes, aparte' (@(Get-ChildItem "$db\antes-de-restaurar-*" -Directory -ErrorAction SilentlyContinue).Count -ge 1)
  [void](WaitFor { (Eval $pb 'pluma.local' "1") -ne $null -or (Eval $pb 'about:' "1") -ne $null } 30000)
  Start-Sleep 3
  Open t213b $site
  $e = $null
  $ok = WaitFor { $script:e = Eval $pb $site 'window.__estado || null'; $script:e -ne $null } 30000
  Check 'cookie de sesion de vuelta' ($e -and $e.cookie)
  Check 'localStorage de vuelta' ($e -and $e.ls)
  Check 'IndexedDB de vuelta' ($e -and $e.idb)
  Start-Sleep 1
  & $cap 'PlumaVentana.t213b' "$out\2-restaurada-sesion.png" | Out-Null
  Open t213b $ext
  $v = $null
  [void](WaitFor { $script:v = Eval $pb $ext "chrome.storage.local.get('t213').then((r) => r.t213 || '')"; $script:v -ne $null } 20000)
  Check 'la extension conserva su valor' ($v -eq "ext$M")
  $done = $true
} finally {
  foreach ($n in @(@('t213a', $pa), @('t213b', $pb))) { & "$src\tools\prueba.ps1" -Nombre $n[0] -Puerto $n[1] -Salir }
  [void](WaitFor { (Plumas).Count -eq 0 } 60000)
  if ($server) { Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue }
  # test data only (the site's marker); kept for a look when something failed
  if ($done -and $fails -eq 0) {
    Remove-Item -Recurse -Force $copias -ErrorAction SilentlyContinue
    foreach ($n in 't213a', 't213b') { Remove-Item -Recurse -Force "$L\Pluma\prueba.$n" -ErrorAction SilentlyContinue }
  }
}
if (-not $done) { "FALLA la prueba no llego al final"; $fails++ }
"fallos: $fails"
