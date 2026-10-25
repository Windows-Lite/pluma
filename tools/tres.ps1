# Three tabs (Wikipedia, GitHub, Google on screen): what two hidden tabs cost, for a given setting.
#   tools\tres.ps1 -Nombre x [-Ajustes 'recientes=0'] [-Puerto 9450] [-Espera 75] [-Abierto]
param([string]$Nombre = 'tres', [string[]]$Ajustes = @(), [int]$Puerto = 9450, [int]$Espera = 75, [switch]$Abierto)
$T = $PSScriptRoot
$exe = if ($env:PLUMA_EXE) { $env:PLUMA_EXE } else { Join-Path (Split-Path $T) 'Pluma.exe' }
& "$T\prueba.ps1" -Limpio -Nombre $Nombre -Puerto $Puerto | Out-Null
Start-Sleep 3
& "$T\prueba.ps1" -Nombre $Nombre -Salir | Out-Null
(@('[pluma]') + $Ajustes) -join "`r`n" | Set-Content "$env:LOCALAPPDATA\Pluma\prueba.$Nombre\ajustes.ini" -Encoding Unicode
& "$T\prueba.ps1" -Nombre $Nombre -Puerto $Puerto | Out-Null
Start-Sleep 5
$env:PLUMA_INSTANCIA = $Nombre; $env:PLUMA_DEPURAR = "$Puerto"
foreach ($u in 'https://es.wikipedia.org/wiki/Madrid', 'https://github.com/microsoft/WebView2Feedback', 'https://www.google.com/search?q=tiempo+madrid') { & $exe $u; Start-Sleep 3 }
Start-Sleep $Espera
"=== $Nombre  ($($Ajustes -join ', '))"
& "$T\medir.ps1" -Nombre $Nombre
if (-not $Abierto) { & "$T\prueba.ps1" -Nombre $Nombre -Salir | Out-Null }
