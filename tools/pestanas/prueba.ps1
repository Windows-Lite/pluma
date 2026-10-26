# Hidden tabs keep what they would lose (tabs.cpp, tabs_tick), end to end in a test instance (never the user's Pluma):
# run in the interactive session with PLUMA_EXE = a build elsewhere. Frozen at 10 s. Something is typed into a form,
# four more tabs are opened (one with nothing to lose, a call, a WebSocket with a word typed, and the last one on
# screen), and after a minute: the form tab frozen with its text, its counter going on from where it froze; the call
# (fake camera and microphone) alive and never frozen (the engine would); the WebSocket back and receiving. The tab
# with nothing to lose, by -Modo:
#   apagado   the default ("Descargar las pestañas que no ves" off): nothing is let go, it comes back without loading
#   encendido that setting on, shortened (25 s, no recent ones kept): only it is let go (it loads again)
#   presion   the default with the PC at 90 % of its memory (PLUMA_MEMORIA_PC): only it is let go
# Photos made by Pluma in <exe folder>\out (p207-<modo>-*.png). -Migrar: an ajustes.ini saved by an older version
# (ajustes_v=2 and the old 60/2/20) must read as never (descargar_s=0, descargar_min=0).
param([string]$Nombre = 'p207', [ValidateSet('apagado', 'encendido', 'presion')][string]$Modo = 'apagado', [switch]$Migrar)
$ErrorActionPreference = 'Continue'
$L = $env:LOCALAPPDATA; $b = Split-Path $PSScriptRoot -Parent | Split-Path -Parent
$out = "$(Split-Path $env:PLUMA_EXE)\out"; New-Item -ItemType Directory -Force $out | Out-Null
$node = "$L\fp-tools\node\node.exe"; $cdp = 9541; $port = 9551; $u = "http://127.0.0.1:$port"
$Memoria = if ($Modo -eq 'presion') { 90 } else { 40 }
$env:PLUMA_SIN_SONIDO = '1'; $env:PLUMA_MEMORIA_PC = "$Memoria"; $env:PLUMA_PRUEBA_MEDIOS = '1'   # (camera and microphone: fake ones, said yes to)
$data = "$L\Pluma\prueba.$Nombre"
function WaitCdp { for ($i = 0; $i -lt 60; $i++) { try { Invoke-RestMethod "http://127.0.0.1:$cdp/json/version" -TimeoutSec 1 | Out-Null; return } catch { Start-Sleep -Milliseconds 500 } } }
function Cmd($x) { $env:PLUMA_INSTANCIA = $Nombre; $env:PLUMA_DEPURAR = "$cdp"; Start-Process $env:PLUMA_EXE -ArgumentList $x -Wait; Start-Sleep -Milliseconds 700 }
function Js($part, $expr) { (& $node "$b\tools\cdp.js" $cdp $part $expr 2>$null | Out-String).Trim() }
function Pages { (Invoke-RestMethod "http://127.0.0.1:$cdp/json/list") | ? { $_.type -eq 'page' -and $_.url -like "$u*" } | % { $_.url } }
$srv = $null
try {
  $srv = Start-Process $node -ArgumentList "`"$PSScriptRoot\servidor.js`" $port" -WindowStyle Hidden -PassThru
  & "$b\tools\prueba.ps1" -Nombre $Nombre -Puerto $cdp -Salir
  if (Test-Path $data) { Remove-Item -Recurse -Force $data }
  New-Item -ItemType Directory -Force $data | Out-Null
  $unload = if ($Modo -eq 'encendido') { "`r`ndescargar_s=25`r`nrecientes=0`r`ndescargar_min=1" } else { '' }
  Set-Content "$data\ajustes.ini" "[pluma]`r`najustes_v=3`r`ntema=claro`r`ndormir_s=10$unload`r`nmotor=--use-fake-device-for-media-stream" -Encoding ASCII
  "== mode: $Modo (memory in use as seen by Pluma: $Memoria %)"
  & "$b\tools\prueba.ps1" -Nombre $Nombre -Puerto $cdp -Url "$u/form"
  WaitCdp; Start-Sleep 3
  "typed: " + (& $node "$PSScriptRoot\escribir.js" $cdp '/form' '#txt' 'Texto sin guardar: esto no se puede perder')
  foreach ($p in '/plain?b', '/llamada', '/ws', '/plain?c') {
    Cmd "$u$p"; Start-Sleep 1
    if ($p -eq '/ws') { Start-Sleep 1; & $node "$PSScriptRoot\escribir.js" $cdp '/ws' '#txt' 'x' | Out-Null }   # (kept: frozen, never let go)
  }
  Start-Sleep 2
  $tb = Js '/plain?b' 'performance.timeOrigin'
  "B loaded at $($tb)"
  Start-Sleep 60
  "== after 60 s hidden, the pages alive:"; Pages | % { "  $_" }
  Cmd 'pluma-prueba:pestanas'
  "== back to the form"
  Cmd 'pluma-prueba:pestana=1'; Start-Sleep 2
  "form: " + (Js '/form' "JSON.stringify({ text: document.querySelector('#txt').value, ticks: window.ticks, events: window.events })")
  Cmd "pluma-prueba:foto=p207-$Modo-form"
  "== the call"
  "call: " + (Js '/llamada' "JSON.stringify({ live: window.stream ? window.stream.getTracks().filter((x) => x.readyState === 'live').length : -1, ticks: window.ticks, events: window.events })")
  "== the WebSocket, frozen and back (typed into, so it is kept)"
  Cmd 'pluma-prueba:pestana=4'; Start-Sleep 2
  "ws shown: " + (Js '/ws' "JSON.stringify({ got: window.got, opens: window.opens, closes: window.closes, ticks: window.ticks, events: window.events })")
  Start-Sleep 3
  "ws 3 s later: " + (Js '/ws' "JSON.stringify({ got: window.got, opens: window.opens, closes: window.closes, ticks: window.ticks })")
  "== back to the one with nothing to lose"
  Cmd 'pluma-prueba:pestana=2'; Start-Sleep 4
  $tb2 = Js '/plain?b' 'performance.timeOrigin'
  "B loaded at $($tb2) (reloaded: $($tb -ne $tb2))"
  Cmd "pluma-prueba:foto=p207-$Modo-plain"
  "== log"
  Get-Content "$data\pluma.log" | Select-String 'pestaña|memoria' | % { "  $_" }
  Get-ChildItem $data -Filter 'p207-*.png' | % { Copy-Item $_.FullName "$out\$($_.Name)"; "  shot: $($_.Name)" }
  if ($Migrar) {
    "== settings saved by the previous version"
    & "$b\tools\prueba.ps1" -Nombre $Nombre -Puerto $cdp -Salir
    Set-Content "$data\ajustes.ini" "[pluma]`r`najustes_v=2`r`ntema=claro`r`ndormir_s=45`r`ndescargar_s=60`r`nrecientes=2`r`ndescargar_min=20" -Encoding ASCII
    & "$b\tools\prueba.ps1" -Nombre $Nombre -Puerto $cdp; WaitCdp; Start-Sleep 2
    $n = @(Get-Content "$data\pluma.log" | Select-String 'descargar_s=').Count
    for ($i = 0; $i -lt 20 -and @(Get-Content "$data\pluma.log" | Select-String 'descargar_s=').Count -le $n; $i++) { Cmd 'pluma-prueba:pestanas'; Start-Sleep 1 }
    Get-Content "$data\pluma.log" | Select-String 'descargar_s=' | Select-Object -Last 1 | % { "  $_" }
  }
} finally {
  & "$b\tools\prueba.ps1" -Nombre $Nombre -Puerto $cdp -Salir
  if ($srv) { Stop-Process -Id $srv.Id -Force -ErrorAction SilentlyContinue }
}
