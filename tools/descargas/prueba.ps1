# Downloads handled by Pluma (downloads.cpp), end to end in a test instance (never the user's Pluma): run in the
# interactive session with PLUMA_EXE = a build elsewhere. Serves a PDF, a ZIP, a slow file and a program as attachments
# (descargas\servidor.js), clicks each link for real, opens the PDF from the panel (a tab with the file:// address),
# "opens" the ZIP and shows it in its folder (test instances only log those: Windows would land on the person's
# screen), cancels the slow one, checks the engine never crashed nor showed its own flyout for those, and that a
# program (a type the engine asks about) gets the engine's flyout with its question (never its "Open file" here).
# Photos made by Pluma in <exe folder>\out. The files it downloads (prueba-192*, lento-192*) are deleted from Downloads at the end.
$ErrorActionPreference = 'Continue'
$lang = "es"
$L = $env:LOCALAPPDATA; $b = Split-Path $PSScriptRoot -Parent | Split-Path -Parent
$out = "$(Split-Path $env:PLUMA_EXE)\out"; New-Item -ItemType Directory -Force $out | Out-Null
$node = "$L\fp-tools\node\node.exe"; $cdp = 9521; $pages = 9531
$env:PLUMA_SIN_SONIDO = '1'
$data = "$L\Pluma\prueba.dl"
$dl = (New-Object -ComObject Shell.Application).NameSpace('shell:Downloads').Self.Path
function WaitCdp { for ($i = 0; $i -lt 60; $i++) { try { Invoke-RestMethod "http://127.0.0.1:$cdp/json/version" -TimeoutSec 1 | Out-Null; return } catch { Start-Sleep -Milliseconds 500 } } }
function Cmd($x) { $env:PLUMA_INSTANCIA = 'dl'; Start-Process $env:PLUMA_EXE -ArgumentList $x -Wait; Start-Sleep -Milliseconds 700 }
function Click($part, $sel) { & $node "$b\tools\clic.js" $cdp $part $sel }
function Targets { (Invoke-RestMethod "http://127.0.0.1:$cdp/json/list") | ? { $_.type -eq 'page' } | % { "  tab: $($_.url)" } }
$srv = $null
try {
  $srv = Start-Process $node -ArgumentList "`"$PSScriptRoot\servidor.js`" $pages" -WindowStyle Hidden -PassThru
  & "$b\tools\prueba.ps1" -Nombre dl -Puerto $cdp -Salir
  if (Test-Path $data) { Remove-Item -Recurse -Force $data }
  New-Item -ItemType Directory -Force $data | Out-Null
  # (off the screen Chromium takes the window for hidden and the engine's flyout stops updating)
  Set-Content "$data\ajustes.ini" "[pluma]`r`nidioma=$lang`r`ntema=oscuro`r`najustes_v=2`r`nmotor=--disable-features=CalculateNativeWinOcclusion" -Encoding ASCII
  & "$b\tools\prueba.ps1" -Nombre dl -Puerto $cdp -Url "http://127.0.0.1:$pages/"
  WaitCdp; Start-Sleep 3
  $root = (Get-CimInstance Win32_Process -Filter "Name='Pluma.exe'" | ? { $_.ExecutablePath -eq $env:PLUMA_EXE }).ProcessId
  & "$b\tools\passgen\ventanas.ps1" -Root $root -Watch
  "== PDF"
  Click "127.0.0.1:$pages/" 'a:nth-of-type(1)'; Start-Sleep 3
  Cmd 'pluma-prueba:foto=dl-pdf'
  "engine windows (no engine flyout for a PDF):"; & "$b\tools\passgen\ventanas.ps1" -Root $root
  Cmd 'pluma-prueba:descarga-abrir=1'; Start-Sleep 3
  Targets
  Cmd 'pluma-prueba:foto=dl-pdf-tab'
  "== ZIP"
  & $node "$b\tools\cdp.js" $cdp 'prueba-192.pdf' "location.href='http://127.0.0.1:$pages/?b'; 1" | Out-Null; Start-Sleep 2
  Click "$pages/?b" 'a:nth-of-type(2)'; Start-Sleep 3
  Cmd 'pluma-prueba:descargas'; Start-Sleep 1
  Cmd 'pluma-prueba:foto=dl-zip'
  Cmd 'pluma-prueba:descarga-abrir=1'
  Cmd 'pluma-prueba:descarga-carpeta=1'
  "== slow one, cancelled"
  Click "$pages/?b" 'a:nth-of-type(3)'; Start-Sleep 3
  Cmd 'pluma-prueba:foto=dl-progress'
  Cmd 'pluma-prueba:descarga-cancelar=1'; Start-Sleep 2
  Cmd 'pluma-prueba:descargas'; Start-Sleep 1
  Cmd 'pluma-prueba:foto=dl-cancelled'
  "== a program: the engine's flyout asks to keep it (WebView2 cannot answer that)"
  Click "$pages/?b" 'a:nth-of-type(4)'; Start-Sleep 4
  # (its Keep is in the menu of "More actions", a menu this script cannot reach: it only shows the question is there)
  & $node "$PSScriptRoot\panel-motor.js" $cdp
  Cmd 'pluma-prueba:descargas'; Start-Sleep 1
  Cmd 'pluma-prueba:foto=dl-exe'
  "== log"
  Get-Content "$data\pluma.log" | Select-String 'descarga|motor ha fallado|falla' | % { "  $_" }
  "reports: " + @(Get-ChildItem "$data\webview2\EBWebView\Crashpad\reports" -ErrorAction SilentlyContinue).Count
  Get-ChildItem $dl -Filter '*192*' | % { "  dl: $($_.Name) $($_.Length)" }
  Get-ChildItem $data -Filter 'dl-*.png' | % { Copy-Item $_.FullName "$out\$($_.Name)"; "  shot: $($_.Name)" }
} finally {
  & "$b\tools\passgen\ventanas.ps1" -Stop
  & "$b\tools\prueba.ps1" -Nombre dl -Puerto $cdp -Salir
  if ($srv) { Stop-Process -Id $srv.Id -Force -ErrorAction SilentlyContinue }
  Get-ChildItem $dl | ? { $_.Name -like 'prueba-192*' -or $_.Name -like 'lento-192*' -or $_.Name -like 'Unconfirmed*.crdownload' -and $_.LastWriteTime -gt (Get-Date).AddMinutes(-10) } | % { "borro $($_.Name)"; Remove-Item -LiteralPath $_.FullName }
}
