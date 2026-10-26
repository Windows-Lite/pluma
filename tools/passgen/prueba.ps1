# The whole test of "Usar contraseña segura" in a test instance (never the user's Pluma): run in the interactive
# session, with PLUMA_EXE = a build elsewhere. Serves the pages, runs tools\passgen-e2e.js (light theme), accepts the
# engine's "save password" prompt through UI Automation (no focus taken), starts the instance again (dark theme) and
# checks that the login form gets the saved password. Photos in <exe folder>\out.
$ErrorActionPreference = 'Continue'
$L = $env:LOCALAPPDATA
$src = Split-Path (Split-Path $PSScriptRoot)
$dir = Split-Path $env:PLUMA_EXE
$out = "$dir\out"
New-Item -ItemType Directory -Force $out | Out-Null
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
function PlumaPid { (Get-CimInstance Win32_Process -Filter "Name='Pluma.exe'" | Where-Object { $_.ExecutablePath -eq $env:PLUMA_EXE }).ProcessId }
function WaitCdp { for ($i = 0; $i -lt 60; $i++) { try { Invoke-RestMethod "http://127.0.0.1:$cdp/json/version" -TimeoutSec 1 | Out-Null; return } catch { Start-Sleep -Milliseconds 500 } } }
try {
  $srv = Start-Process $py -ArgumentList "`"$src\tools\passgen\serve.py`" $pages" -WindowStyle Hidden -PassThru
  & "$src\tools\prueba.ps1" -Nombre pw -Puerto $cdp -Salir
  if (Test-Path $data) { Remove-Item -Recurse -Force $data }
  SetTheme 'claro'
  & "$src\tools\prueba.ps1" -Nombre pw -Puerto $cdp -Url "http://127.0.0.1:$pages/login.html"
  WaitCdp; Start-Sleep 2
  & "$PSScriptRoot\ventanas.ps1" -Root (PlumaPid) -Watch
  "== fill"
  & $node "$src\tools\passgen-e2e.js" $cdp $pages fill "$src\tools\captura.exe" PlumaVentana.pw $out
  Start-Sleep 1
  $root = (Get-CimInstance Win32_Process -Filter "Name='Pluma.exe'" | Where-Object { $_.ExecutablePath -eq $env:PLUMA_EXE }).ProcessId
  "== la oferta de guardar del motor"
  & "$PSScriptRoot\ventanas.ps1" -Root $root -Shot "$out\guardar"
  & "$PSScriptRoot\ventanas.ps1" -Root $root -Invoke 'Save|Guardar'
  Start-Sleep 2; "guardadas:"; Logins
  & "$src\tools\prueba.ps1" -Nombre pw -Puerto $cdp -Salir
  Start-Sleep 1
  "ini: " + ((Get-Content "$data\ajustes.ini" | Where-Object { $_ -match 'contrasenas|tema' }) -join ', ')
  SetTheme 'oscuro'
  & "$src\tools\prueba.ps1" -Nombre pw -Puerto $cdp -Url "http://127.0.0.1:$pages/login.html"
  WaitCdp; Start-Sleep 2
  & "$PSScriptRoot\ventanas.ps1" -Stop
  & "$PSScriptRoot\ventanas.ps1" -Root (PlumaPid) -Watch
  $env:PLUMA_ROOT = (Get-CimInstance Win32_Process -Filter "Name='Pluma.exe'" | Where-Object { $_.ExecutablePath -eq $env:PLUMA_EXE }).ProcessId
  $env:PLUMA_WINS = "$PSScriptRoot\ventanas.ps1"
  "== after"
  & $node "$src\tools\passgen-e2e.js" $cdp $pages after "$src\tools\captura.exe" PlumaVentana.pw $out
} finally {
  & "$PSScriptRoot\ventanas.ps1" -Stop
  & "$src\tools\prueba.ps1" -Nombre pw -Puerto $cdp -Salir
  if ($srv) { Stop-Process -Id $srv.Id -Force -ErrorAction SilentlyContinue }
}
Get-ChildItem $out | ForEach-Object { $_.Name + ' ' + $_.Length }
