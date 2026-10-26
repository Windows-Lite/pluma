# The whole test of "Usar contraseña segura" in a test instance (never the user's Pluma): run in the interactive
# session, with PLUMA_EXE = a build elsewhere. Serves the pages, runs tools\passgen-e2e.js (light theme), presses
# the engine's Guardar as the person would (UI Automation, no focus taken), checks that typed or forged passwords are
# left alone, starts the instance again (dark theme) and checks that the login form gets the saved password; then the
# prompt closed without saving (Pluma's own row, applied at the next start) and a site with "never" (nothing kept).
# Photos in <exe folder>\out.
$ErrorActionPreference = 'Continue'
$L = $env:LOCALAPPDATA
$src = Split-Path (Split-Path $PSScriptRoot)
$dir = Split-Path $env:PLUMA_EXE
$out = "$dir\out"
New-Item -ItemType Directory -Force $out, "$out\pluma" | Out-Null
Get-ChildItem $out -Filter *.png | Remove-Item -ErrorAction SilentlyContinue
$node = "$L\fp-tools\node\node.exe"
$py = (Get-ChildItem "$L\Programs\Python" -Recurse -Filter python.exe | Where-Object { $_.FullName -notmatch 'venv' } | Select-Object -First 1).FullName
$env:PLUMA_SIN_SONIDO = '1'
$data = "$L\Pluma\prueba.pw"
$pages = 9513; $cdp = 9503
$srv = $null
function SetTheme($t) {
  New-Item -ItemType Directory -Force $data | Out-Null
  $ini = "$data\ajustes.ini"
  $lines = @(if (Test-Path $ini) { Get-Content $ini | Where-Object { $_ -notmatch '^(tema|ajustes_v|idioma)=' -and $_ -ne '[' } } else { '[pluma]' })
  if (-not ($lines -contains '[pluma]')) { $lines = @('[pluma]') + $lines }
  $i = [array]::IndexOf($lines, '[pluma]')
  $new = @($lines[0..$i]) + @("tema=$t", 'ajustes_v=2', 'idioma=es') + @(if ($i + 1 -lt $lines.Count) { $lines[($i + 1)..($lines.Count - 1)] })
  Set-Content $ini $new -Encoding ASCII
}
function Logins {
  Copy-Item "$data\webview2\EBWebView\Default\Login Data" "$env:TEMP\ld-pw.db" -Force
  & $py -c "import sqlite3;c=sqlite3.connect(r'$env:TEMP\ld-pw.db');print(c.execute('select origin_url,username_value,length(password_value) from logins').fetchall())"
  Remove-Item "$env:TEMP\ld-pw.db"
}
function LoginCount { (Logins) -ne '[]' }
$fails = 0
function Check($name, $ok) { if ($ok) { "ok    $name" } else { "FALLA $name"; $script:fails++ } }
function PlumaPid {
  # the instance's main process (the oldest with that exe: a --salir helper may be there a moment)
  $p = @(Get-CimInstance Win32_Process -Filter "Name='Pluma.exe'" | Where-Object { $_.ExecutablePath -eq $env:PLUMA_EXE } | Sort-Object CreationDate)
  if ($p.Count) { [int]$p[0].ProcessId } else { 0 }
}
# closing can keep the instance's mutex ~11 s (tools\prueba.ps1 waits 10): a start before it is gone goes to the old one
function Quit {
  & "$src\tools\prueba.ps1" -Nombre pw -Puerto $cdp -Salir
  for ($i = 0; $i -lt 60; $i++) { $m = $null; if (-not [System.Threading.Mutex]::TryOpenExisting('PlumaNavegadorUnico.pw', [ref]$m)) { break }; $m.Dispose(); Start-Sleep -Milliseconds 500 }
  # and its engine gone too (it holds the devtools port and the profile a moment longer)
  for ($i = 0; $i -lt 40; $i++) { try { Invoke-RestMethod "http://127.0.0.1:$cdp/json/version" -TimeoutSec 1 | Out-Null; Start-Sleep -Milliseconds 500 } catch { break } }
}
function WaitCdp {
  # the instance's page up (a fresh profile takes longer: it adds its extension first)
  for ($i = 0; $i -lt 60; $i++) {
    try { if (@(Invoke-RestMethod "http://127.0.0.1:$cdp/json/list" -TimeoutSec 1 | Where-Object { $_.type -eq 'page' -and $_.url -like 'http*' }).Count) { return $true } } catch {}
    Start-Sleep -Milliseconds 500
  }
  return $false
}
# start the instance on the login page; once more if it did not come up (a start too close to the last one's end
# can go to the one that is leaving)
function Start-Pluma {
  for ($k = 0; $k -lt 2; $k++) {
    & "$src\tools\prueba.ps1" -Nombre pw -Puerto $cdp -Url "http://127.0.0.1:$pages/login.html"
    if (WaitCdp) { Start-Sleep 2; return }
    "sin pagina en 30 s, otra vez"
    Quit
  }
}
try {
  $srv = Start-Process $py -ArgumentList "`"$src\tools\passgen\serve.py`" $pages" -WindowStyle Hidden -PassThru
  Quit
  if (Test-Path $data) { Remove-Item -Recurse -Force $data }
  SetTheme 'claro'
  Start-Pluma
  & "$PSScriptRoot\ventanas.ps1" -Root (PlumaPid) -Watch
  "== fill"
  & $node "$src\tools\passgen-e2e.js" $cdp $pages fill "$src\tools\captura.exe" PlumaVentana.pw $out
  Start-Sleep 2
  $root = PlumaPid
  "== la persona pulsa Guardar en la pregunta del motor: Pluma no escribe nada"
  & "$PSScriptRoot\ventanas.ps1" -Root $root -Invoke 'Guardar|Save' | Select-String 'pulsado'
  Start-Sleep 4
  $l = Logins; "guardadas: $l"
  Check 'la sugerida, guardada por el motor' ($l -match "ana@example.com")
  Check 'Pluma ve que el motor la tiene: sin pendientes' (-not (Test-Path "$data\passwords-pending.tsv"))
  "== una entrada escrita a mano: su pregunta no se toca"
  & $node "$src\tools\passgen-e2e.js" $cdp $pages typed "$src\tools\captura.exe" PlumaVentana.pw $out
  Start-Sleep 2
  $w = & "$PSScriptRoot\ventanas.ps1" -Root $root
  $l = Logins; "guardadas: $l"
  Check 'la escrita a mano no se guarda sola' (-not ($l -match "otra@example.com"))
  Check 'su pregunta sigue ahi' (($w | Where-Object { $_ -match 'Chrome_WidgetWin_1' }).Count -ge 1)
  & "$PSScriptRoot\ventanas.ps1" -Root $root -Shot "$out\pregunta" | Out-Null
  & "$PSScriptRoot\ventanas.ps1" -Root $root -Invoke 'Close|Cerrar' | Out-Null
  Start-Sleep 3
  Check 'cerrada, Pluma tampoco la guarda' (-not (Test-Path "$data\passwords-pending.tsv"))
  "== una pagina que se inventa el aviso (y un marco de otro sitio): no se guarda nada"
  & $node "$src\tools\passgen-e2e.js" $cdp $pages forge "$src\tools\captura.exe" PlumaVentana.pw $out
  Start-Sleep 2
  $w = & "$PSScriptRoot\ventanas.ps1" -Root $root
  $l = Logins; "guardadas: $l"
  Check 'forja: el motor no la guarda solo' (-not ($l -match "forja@example.com"))
  Check 'forja: su pregunta sigue ahi' (($w | Where-Object { $_ -match 'Chrome_WidgetWin_1' }).Count -ge 1)
  & "$PSScriptRoot\ventanas.ps1" -Root $root -Invoke 'Close|Cerrar' | Out-Null
  Start-Sleep 3
  Check 'forja: cerrada, Pluma tampoco la guarda' (-not (Test-Path "$data\passwords-pending.tsv"))
  $log = Get-Content "$data\pluma.log" -ErrorAction SilentlyContinue
  Check 'forja: el aviso de la pagina, ignorado' (@($log | Select-String 'aviso de env.o ignorado').Count -ge 1)
  Check 'forja: el marco de otro sitio, ignorado' (@($log | Select-String 'otro origen, ignorada').Count -ge 1)
  Quit
  Start-Sleep 1
  SetTheme 'oscuro'
  Start-Pluma
  & "$PSScriptRoot\ventanas.ps1" -Stop
  & "$PSScriptRoot\ventanas.ps1" -Root (PlumaPid) -Watch
  $env:PLUMA_ROOT = PlumaPid
  $env:PLUMA_WINS = "$PSScriptRoot\ventanas.ps1"
  "== after"
  & $node "$src\tools\passgen-e2e.js" $cdp $pages after "$src\tools\captura.exe" PlumaVentana.pw $out
  # a person who closes the engine's prompt without saving
  "== la pregunta se cierra sin guardar: la guarda Pluma"
  Quit
  Start-Sleep 1
  Remove-Item -Recurse -Force $data
  SetTheme 'claro'
  Start-Pluma
  & "$PSScriptRoot\ventanas.ps1" -Stop
  $root = PlumaPid
  & "$PSScriptRoot\ventanas.ps1" -Root $root -Watch
  & $node "$src\tools\passgen-e2e.js" $cdp $pages gen "$src\tools\captura.exe" PlumaVentana.pw $out
  Start-Sleep 1
  & "$PSScriptRoot\ventanas.ps1" -Root $root -Invoke 'Close|Cerrar' | Select-String 'pulsado'
  Start-Sleep -Milliseconds 2600
  & "$src\tools\captura.exe" PlumaVentana.pw "$out\aviso-pendiente.png"
  $l = Logins; "guardadas por el motor: $l"
  Check 'cerrada sin guardar: el motor no la tiene' (-not ($l -match "ana@example.com"))
  Check 'Pluma la deja para el siguiente arranque' (Test-Path "$data\passwords-pending.tsv")
  Quit
  Start-Sleep 1
  Start-Pluma
  & "$PSScriptRoot\ventanas.ps1" -Stop
  & "$PSScriptRoot\ventanas.ps1" -Root (PlumaPid) -Watch
  $env:PLUMA_ROOT = PlumaPid
  Check 'el archivo de pendientes ya se aplico' (-not (Test-Path "$data\passwords-pending.tsv"))
  Copy-Item "$out\guardada.txt" "$out\pluma\guardada.txt" -Force
  "== after (la que guardo Pluma)"
  & $node "$src\tools\passgen-e2e.js" $cdp $pages after "$src\tools\captura.exe" PlumaVentana.pw "$out\pluma"
  Quit
  Remove-Item -Recurse -Force $data
  SetTheme 'claro'
  Start-Pluma
  & "$PSScriptRoot\ventanas.ps1" -Stop
  $root = PlumaPid
  & "$PSScriptRoot\ventanas.ps1" -Root $root -Watch
  # a page with "Referrer-Policy: no-referrer" (its calls say "Origin: null"): kept all the same
  "== pagina sin referer: la guarda Pluma igual"
  $env:PLUMA_PRUEBA_PAGINA = 'noref.html'; $env:PLUMA_PRUEBA_CORREO = 'noref@example.com'
  & $node "$src\tools\passgen-e2e.js" $cdp $pages gen "$src\tools\captura.exe" PlumaVentana.pw "$out\pluma"
  Remove-Item Env:PLUMA_PRUEBA_PAGINA, Env:PLUMA_PRUEBA_CORREO
  Start-Sleep 1
  & "$PSScriptRoot\ventanas.ps1" -Root $root -Invoke 'Close|Cerrar' | Out-Null
  Start-Sleep 4
  Check 'sin referer: Pluma la deja para el siguiente arranque' (Test-Path "$data\passwords-pending.tsv")
  Quit
  Remove-Item "$data\passwords-pending.tsv" -ErrorAction SilentlyContinue   # (a clean site for the next case)
  # a site the person said "never" to (Chromium's row for it, written while the instance is closed): nothing kept
  # (the engine does not even ask there; Pluma checks that row too before writing)
  "== sitio con nunca: Pluma no la guarda"
  $ld = "$data\webview2\EBWebView\Default\Login Data"
  & $py "$PSScriptRoot\nunca.py" $ld "http://127.0.0.1:$pages/"
  Start-Pluma
  & "$PSScriptRoot\ventanas.ps1" -Stop
  $root = PlumaPid
  & "$PSScriptRoot\ventanas.ps1" -Root $root -Watch
  $env:PLUMA_PRUEBA_CORREO = 'nunca@example.com'
  & $node "$src\tools\passgen-e2e.js" $cdp $pages gen "$src\tools\captura.exe" PlumaVentana.pw "$out\pluma"
  Remove-Item Env:PLUMA_PRUEBA_CORREO
  Start-Sleep 1
  & "$PSScriptRoot\ventanas.ps1" -Root $root -Invoke 'Close|Cerrar' | Out-Null
  Start-Sleep 4
  Check 'nunca: sin pendientes' (-not (Test-Path "$data\passwords-pending.tsv"))
} finally {
  & "$PSScriptRoot\ventanas.ps1" -Stop
  Quit
  if ($srv) { Stop-Process -Id $srv.Id -Force -ErrorAction SilentlyContinue }
}
Get-ChildItem $out -Recurse -File | ForEach-Object { $_.Name + ' ' + $_.Length }
if ($fails) { "$fails fallos en prueba.ps1" } else { 'prueba.ps1: todo bien' }
