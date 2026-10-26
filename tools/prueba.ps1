# Test instance of Pluma, off the screen and with its own data (never the user's):
#   tools\prueba.ps1 [-Nombre t1] [-Url https://...] [-Puerto 9401] [-Limpio]     starts it (closing a previous one)
#   tools\prueba.ps1 -Salir                                                         closes it
param([string]$Nombre = 't1', [string]$Url = '', [int]$Puerto = 9401, [switch]$Limpio, [switch]$Salir, [switch]$Oculto)
# PLUMA_EXE: a build elsewhere (never test with the exe the user's own Pluma is running from)
$exe = if ($env:PLUMA_EXE) { $env:PLUMA_EXE } else { Join-Path (Split-Path $PSScriptRoot) 'Pluma.exe' }
$env:PLUMA_INSTANCIA = $Nombre
$env:PLUMA_DEPURAR = "$Puerto"
$mutex = "PlumaNavegadorUnico.$Nombre"
# (the handle is closed at once: an open one keeps the mutex alive after Pluma is gone, so this waited the whole 10 s
# and a Pluma started next found it "running" and ended without a word)
function Running { $m = $null; $r = [System.Threading.Mutex]::TryOpenExisting($mutex, [ref]$m); if ($m) { $m.Dispose() }; return $r }
if (Running) {
    $before = @(Get-Process Pluma -ErrorAction SilentlyContinue | ForEach-Object Id)
    & $exe --salir
    for ($i = 0; $i -lt 100 -and (Running); $i++) { Start-Sleep -Milliseconds 100 }
    # the process lets go of the mutex a moment before it is gone (and lets go of Pluma.exe)
    for ($i = 0; $i -lt 30; $i++) {
        $now = @(Get-Process Pluma -ErrorAction SilentlyContinue | ForEach-Object Id)
        if ($now.Count -lt $before.Count -or $now.Count -eq 0) { break }
        Start-Sleep -Milliseconds 100
    }
}
if ($Salir) { return }
if ($Limpio) { Remove-Item -Recurse -Force "$env:LOCALAPPDATA\Pluma\prueba.$Nombre" -ErrorAction SilentlyContinue }
$a = @(); if ($Oculto) { $a += '--oculto' }; if ($Url) { $a += $Url }
if ($a.Count) { Start-Process $exe -ArgumentList $a } else { Start-Process $exe }
