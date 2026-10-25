# What Pluma uses while a YouTube video plays (the same video, the window at 1920x1040 so YouTube picks 1080p,
# playing muted for 60 s), for a given setting: total by process, codec, buffer, the page's JS heap.
#   tools\video.ps1 -Nombre x [-Ajustes 'motor=...'] [-Puerto 9610] [-Url ...] [-Script archivo.user.js]
param([string]$Nombre = 'vid', [string[]]$Ajustes = @(), [int]$Puerto = 9610, [string]$Url = 'https://www.youtube.com/watch?v=kJQP7kiw5Fk',
      [string]$Script = '', [int]$Espera = 60)
$T = $PSScriptRoot
$env:PLUMA_TAMANO = '1920x1040'
& "$T\prueba.ps1" -Limpio -Nombre $Nombre -Puerto $Puerto | Out-Null
Start-Sleep 5
& "$T\prueba.ps1" -Nombre $Nombre -Salir | Out-Null
$dir = "$env:LOCALAPPDATA\Pluma\prueba.$Nombre"
(@('[pluma]') + $Ajustes) -join "`r`n" | Set-Content "$dir\ajustes.ini" -Encoding Unicode
if ($Script) { New-Item -ItemType Directory -Force "$dir\scripts" | Out-Null; Copy-Item $Script "$dir\scripts\" }
& "$T\prueba.ps1" -Nombre $Nombre -Puerto $Puerto -Url $Url | Out-Null
Start-Sleep 10
# a clean profile gets YouTube's cookie question first, and the video does not start until it is answered
node "$T\cdp.js" $Puerto 'youtube.com/watch' "(() => { const b = [...document.querySelectorAll('button')].find(x => /Rechazar todo|Reject all/i.test(x.innerText)); if (b) b.click(); return !!b; })()" | Out-Null
Start-Sleep 5
# started with YouTube's own player, muted, and checked: a test whose video never played is no test
for ($k = 0; $k -lt 4; $k++) {
    $st = node "$T\cdp.js" $Puerto 'youtube.com/watch' "(() => { const p = document.getElementById('movie_player'); if (!p || !p.playVideo) return -9; p.mute(); p.playVideo(); return p.getPlayerState(); })()"
    Start-Sleep 3
    $st = node "$T\cdp.js" $Puerto 'youtube.com/watch' "(() => { const p = document.getElementById('movie_player'); return p && p.getPlayerState ? p.getPlayerState() : -9; })()"
    if ("$st".Trim() -eq '1') { break }
}
if ("$st".Trim() -ne '1') { "  (el vídeo no arrancó: estado $st)" }
Start-Sleep $Espera
$os = Get-CimInstance Win32_OperatingSystem
$load = [math]::Round(100 - $os.FreePhysicalMemory * 100.0 / $os.TotalVisibleMemorySize)
"=== $Nombre  ($($Ajustes -join ', ')$(if ($Script) { ' + ' + (Split-Path $Script -Leaf) }))   memoria del PC: $load %$(if ($load -ge 80) { ' (NO VALE)' })"
"  " + (node "$T\yt.js" $Puerto)
& "$T\medir.ps1" -Nombre $Nombre
& "$T\prueba.ps1" -Nombre $Nombre -Salir | Out-Null
