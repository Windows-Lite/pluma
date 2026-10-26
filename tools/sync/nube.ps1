# Backup to Google Drive, OneDrive and Dropbox, end to end, against a fake of the three (nube-falsa.js: no account
# needed) in a test instance (never the user's Pluma): run in the interactive session with PLUMA_EXE = a build
# elsewhere and sync-test.exe built next to it (or -SyncTest). The instance fills ~30 MB of site data that does not
# compress, so each upload goes in pieces; the fake drops the second piece of each (Drive and OneDrive keep half of it,
# Dropbox all of it without answering) and Pluma has to go on from where the provider says, never from 0. Each
# provider: sign in (OAuth to the fake), a backup, and what reached the fake has to open with the password.
param([string]$Salida = '', [string]$SyncTest = '', [int]$GrandeMB = 30)
$ErrorActionPreference = 'Continue'
$L = $env:LOCALAPPDATA
$src = Split-Path (Split-Path $PSScriptRoot)
$out = if ($Salida) { $Salida } else { "$(Split-Path $env:PLUMA_EXE)\out" }
$st = if ($SyncTest) { $SyncTest } else { "$(Split-Path $env:PLUMA_EXE)\sync-test.exe" }
New-Item -ItemType Directory -Force $out | Out-Null
$env:PLUMA_SIN_SONIDO = '1'
$node = "$L\fp-tools\node\node.exe"
$cap = "$src\tools\captura.exe"
$fake = 'http://127.0.0.1:8215'
$site = 'http://127.0.0.1:8216/'
$nube = "$out\nube"
$PW = 'prueba-' + (Get-Random)
$name = 't213n'
$dn = "$L\Pluma\prueba.$name"
function FreePort($p) { while ($p -lt 9499) { try { Invoke-RestMethod "http://127.0.0.1:$p/json/version" -TimeoutSec 1 | Out-Null; $p++ } catch { return $p } }; $p }
$pn = FreePort 9451
$fails = 0
function Check($what, $ok) { if ($ok) { "ok    $what" } else { "FALLA $what"; $script:fails++ } }
function WaitFor($sb, $ms = 15000) { for ($i = 0; $i -lt $ms / 250; $i++) { if (& $sb) { return $true }; Start-Sleep -Milliseconds 250 }; return $false }
function Eval($port, $url, $expr) {
  $r = & $node "$src\tools\cdp.js" $port $url $expr 2>$null
  if ($LASTEXITCODE -ne 0 -or -not $r -or $r -eq 'undefined') { return $null }
  try { ($r -join "`n") | ConvertFrom-Json } catch { $null }
}
function Open($url) { $env:PLUMA_INSTANCIA = $name; & $env:PLUMA_EXE $url }
function Plumas { @(Get-CimInstance Win32_Process -Filter "Name='Pluma.exe'" | Where-Object { $_.ExecutablePath -eq $env:PLUMA_EXE }) }
function State { try { Invoke-RestMethod "$fake/estado" -TimeoutSec 5 } catch { $null } }

$procs = @(); $done = $false
try {
  & "$src\tools\prueba.ps1" -Nombre $name -Puerto $pn -Salir
  [void](WaitFor { (Plumas).Count -eq 0 } 60000)
  Remove-Item -Recurse -Force $dn, $nube -ErrorAction SilentlyContinue
  New-Item -ItemType Directory -Force $dn, $nube | Out-Null
  # any app id will do for the fake; the real ones never take part
  Set-Content "$dn\ajustes.ini" "[sync]`r`ngoogle_client_id=prueba`r`ngoogle_client_secret=prueba`r`nonedrive_client_id=prueba`r`ndropbox_client_id=prueba" -Encoding Unicode
  $procs += Start-Process $node -ArgumentList "`"$PSScriptRoot\nube-falsa.js`"", 8215, "`"$nube`"" -WindowStyle Hidden -PassThru
  $procs += Start-Process $node -ArgumentList "`"$PSScriptRoot\servidor.js`"", 8216, 'nube' -WindowStyle Hidden -PassThru
  $env:PLUMA_SYNC_NUBE_PRUEBA = $fake
  & "$src\tools\prueba.ps1" -Nombre $name -Puerto $pn -Url "${site}?grande=$GrandeMB"
  Check "$GrandeMB MB de datos de un sitio" (WaitFor { (Eval $pn "${site}?grande" 'window.__grande === true') -eq $true } 60000)
  Open 'pluma://settings'
  Check 'Configuracion abierta' (WaitFor { (Eval $pn 'pluma.local' "typeof sync") -eq 'function' } 30000)
  [void](Eval $pn 'pluma.local' "sync({ cmd: 'password', pw: '$PW', hint: '' }); 'ok'")
  Start-Sleep 2

  foreach ($p in 'gdrive', 'onedrive', 'dropbox') {
    "-- $p"
    $before = @((State).$p.done).Count
    [void](Eval $pn 'pluma.local' "sync({ cmd: 'connect', provider: '$p' }); 'ok'")
    $ok = WaitFor { @((State).$p.done).Count -gt $before } 180000
    Check "$p`: copia subida" $ok
    Check "$p`: cuenta conectada" ((Eval $pn 'pluma.local' "S.sync && S.sync.dest === '$p' && S.sync.account") -eq 'prueba@pluma.test')
    $s = (State).$p
    "   piezas $($s.pieces), cortadas $($s.drops), preguntas $($s.asks)"
    Check "$p`: en piezas" ($s.pieces -ge 3)
    Check "$p`: una pieza cortada" ($s.drops -ge 1)
    Check "$p`: siguio desde lo que tenia (pregunto)" ($s.asks -ge 1)
    $f = @(Get-ChildItem "$nube\$p\Pluma-*.plumabk" -ErrorAction SilentlyContinue | Sort-Object Name)[-1]
    if ($f) {
      "   {0:N0} KB" -f ($f.Length / 1KB)
      $r = & $st $f.FullName $PW
      Check "$p`: la copia subida abre con la contrasena ($r)" ($LASTEXITCODE -eq 0)
    } else { Check "$p`: fichero en la nube falsa" $false }
    Start-Sleep 3   # the list and the clean-up after the upload
  }
  $e = (State).errors
  Check "la nube falsa no vio nada mal hecho ($(@($e).Count))" (@($e).Count -eq 0)
  @($e) | Select-Object -First 5 | ForEach-Object { "   $_" }
  Check 'tres copias en el registro' (@(Select-String -Path "$dn\pluma.log" -Pattern 'copia de seguridad hecha').Count -ge 3)
  [void](Eval $pn 'pluma.local' "document.getElementById('sincronizar').scrollIntoView(); 'ok'")
  Start-Sleep 1
  & $cap "PlumaVentana.$name" "$out\3-nube-conectada.png" | Out-Null
  $done = $true
} finally {
  & "$src\tools\prueba.ps1" -Nombre $name -Puerto $pn -Salir
  [void](WaitFor { (Plumas).Count -eq 0 } 60000)
  foreach ($x in $procs) { Stop-Process -Id $x.Id -Force -ErrorAction SilentlyContinue }
  Remove-Item Env:PLUMA_SYNC_NUBE_PRUEBA -ErrorAction SilentlyContinue
  if ($done -and $fails -eq 0) { Remove-Item -Recurse -Force $dn, $nube -ErrorAction SilentlyContinue }
}
if (-not $done) { "FALLA la prueba no llego al final"; $fails++ }
"fallos: $fails"
