# The 7-tab scenario used to measure Pluma's RAM (as the user's Firefox had 7 tabs: 3,100 MB):
# a clean test profile with the given settings, seven real sites opened one after another (YouTube paused), the
# last one on screen, and the RAM measured once the others had time to fall asleep.
#   tools\escenario.ps1 -Nombre x [-Ajustes 'descargar_s=60','reserva=0'] [-Espera 90] [-Puerto 9430]
param([string]$Nombre = 'esc', [string[]]$Ajustes = @(), [int]$Espera = 90, [int]$Puerto = 9430, [switch]$Abierto)
$T = $PSScriptRoot
# PLUMA_EXE: a build elsewhere (never test with the exe the user's own Pluma is running from)
$exe = if ($env:PLUMA_EXE) { $env:PLUMA_EXE } else { Join-Path (Split-Path $PSScriptRoot) 'Pluma.exe' }
$urls = @('https://www.youtube.com/watch?v=kJQP7kiw5Fk', 'https://www.marca.com/', 'https://elpais.com/', 'https://www.reddit.com/r/spain/',
          'https://es.wikipedia.org/wiki/Madrid', 'https://github.com/microsoft/WebView2Feedback', 'https://www.google.com/search?q=tiempo+madrid')
& "$T\prueba.ps1" -Limpio -Nombre $Nombre -Puerto $Puerto | Out-Null
Start-Sleep 3
& "$T\prueba.ps1" -Nombre $Nombre -Salir | Out-Null
$ini = "$env:LOCALAPPDATA\Pluma\prueba.$Nombre\ajustes.ini"
(@('[pluma]') + $Ajustes) -join "`r`n" | Set-Content $ini -Encoding Unicode
& "$T\prueba.ps1" -Nombre $Nombre -Puerto $Puerto | Out-Null
Start-Sleep 5
$env:PLUMA_INSTANCIA = $Nombre; $env:PLUMA_DEPURAR = "$Puerto"
& $exe $urls[0]; Start-Sleep 7
node "$T\cdp.js" $Puerto youtube.com "(() => { const v = document.querySelector('video'); if (v) v.pause(); return 1; })()" | Out-Null
foreach ($u in $urls[1..6]) { & $exe $u; Start-Sleep -Milliseconds 1800 }
Start-Sleep $Espera
"=== $Nombre  ($($Ajustes -join ', '))"
& "$T\medir.ps1" -Nombre $Nombre
if (-not $Abierto) { & "$T\prueba.ps1" -Nombre $Nombre -Salir | Out-Null }
