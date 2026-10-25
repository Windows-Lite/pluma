# Scrolling stutters on pages with many pictures, for a given setting: each page is opened in a clean test instance,
# left to load, then scrolled down and back up for 12 s while every frame is timed (tools/tirones.js).
#   tools\scroll.ps1 -Nombre x [-Ajustes 'motor=...'] [-Puerto 9550]
param([string]$Nombre = 'scr', [string[]]$Ajustes = @(), [int]$Puerto = 9550)
$T = $PSScriptRoot
$pages = @('https://es.wikipedia.org/wiki/Espa%C3%B1a', 'https://www.marca.com/', 'https://www.reddit.com/r/EarthPorn/')
& "$T\prueba.ps1" -Limpio -Nombre $Nombre -Puerto $Puerto | Out-Null
Start-Sleep 4
& "$T\prueba.ps1" -Nombre $Nombre -Salir | Out-Null
(@('[pluma]') + $Ajustes) -join "`r`n" | Set-Content "$env:LOCALAPPDATA\Pluma\prueba.$Nombre\ajustes.ini" -Encoding Unicode
"=== $Nombre  ($($Ajustes -join ', '))"
foreach ($p in $pages) {
    & "$T\prueba.ps1" -Nombre $Nombre -Puerto $Puerto -Url $p | Out-Null
    Start-Sleep 12
    $host_ = ([uri]$p).Host
    $r = node "$T\tirones.js" $Puerto $host_.Replace('www.', '') 12
    "  $($host_.PadRight(20)) $r"
    & "$T\prueba.ps1" -Nombre $Nombre -Salir | Out-Null
}
