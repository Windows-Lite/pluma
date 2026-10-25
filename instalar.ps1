# Instala Pluma para el usuario actual (sin permisos de administrador):
#   - accesos directos en el Escritorio y en Inicio
#   - Pluma como navegador en Windows (sale en Configuración > Aplicaciones predeterminadas; el predeterminado lo
#     eliges tú allí, Windows no deja que lo cambie un programa)
#   - el script del botón MP3 de YT-MP3, si YT-MP3 está en Documentos
#   -ArrancarConWindows   Pluma arranca escondido al encender, para que se abra al instante
#   -Quitar               deshace todo lo anterior (no borra tus datos de %LOCALAPPDATA%\Pluma)
param([switch]$ArrancarConWindows, [switch]$Quitar)
$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$exe = Join-Path $dir 'Pluma.exe'
$desktop = [Environment]::GetFolderPath('Desktop')
$start = Join-Path ([Environment]::GetFolderPath('StartMenu')) 'Programs'
$links = @((Join-Path $desktop 'Pluma.lnk'), (Join-Path $start 'Pluma.lnk'))
$cap = 'HKCU:\Software\Clients\StartMenuInternet\Pluma'
$progid = 'HKCU:\Software\Classes\PlumaHTML'
$run = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'

if ($Quitar) {
    & $exe --salir 2>$null
    foreach ($l in $links) { Remove-Item $l -ErrorAction SilentlyContinue }
    Remove-Item $cap, $progid -Recurse -ErrorAction SilentlyContinue
    Remove-ItemProperty 'HKCU:\Software\RegisteredApplications' -Name 'Pluma' -ErrorAction SilentlyContinue
    Remove-ItemProperty $run -Name 'Pluma' -ErrorAction SilentlyContinue
    Write-Host 'Pluma quitado (tus datos siguen en %LOCALAPPDATA%\Pluma).'
    return
}
if (-not (Test-Path $exe)) { throw "No encuentro $exe" }
Get-ChildItem $dir -Recurse -File | Unblock-File

# accesos directos
$sh = New-Object -ComObject WScript.Shell
foreach ($l in $links) {
    $s = $sh.CreateShortcut($l)
    $s.TargetPath = $exe
    $s.WorkingDirectory = $dir
    $s.IconLocation = "$exe,0"
    $s.Description = 'Pluma, el navegador ligero'
    $s.Save()
}

# Pluma como navegador: ProgID para las páginas, y sus capacidades
function Set-Key($path, $name, $value) {
    if (-not (Test-Path $path)) { New-Item $path -Force | Out-Null }
    if ($name -eq '') { Set-Item $path -Value $value } else { New-ItemProperty $path -Name $name -Value $value -PropertyType String -Force | Out-Null }
}
$open = "`"$exe`" `"%1`""
Set-Key $progid '' 'Página web (Pluma)'
Set-Key "$progid\DefaultIcon" '' "$exe,0"
Set-Key "$progid\shell\open\command" '' $open
Set-Key $cap '' 'Pluma'
Set-Key "$cap\DefaultIcon" '' "$exe,0"
Set-Key "$cap\shell\open\command" '' "`"$exe`""
Set-Key "$cap\Capabilities" 'ApplicationName' 'Pluma'
Set-Key "$cap\Capabilities" 'ApplicationDescription' 'Navegador ligero y rápido'
Set-Key "$cap\Capabilities" 'ApplicationIcon' "$exe,0"
foreach ($e in '.htm', '.html', '.shtml', '.xht', '.xhtml', '.svg', '.webp', '.pdf') { Set-Key "$cap\Capabilities\FileAssociations" $e 'PlumaHTML' }
foreach ($p in 'http', 'https') { Set-Key "$cap\Capabilities\URLAssociations" $p 'PlumaHTML' }
Set-Key "$cap\Capabilities\StartMenu" 'StartMenuInternet' 'Pluma'
Set-Key 'HKCU:\Software\RegisteredApplications' 'Pluma' 'Software\Clients\StartMenuInternet\Pluma\Capabilities'

if ($ArrancarConWindows) { Set-Key $run 'Pluma' "`"$exe`" --oculto" }

# el botón MP3 de YT-MP3, ya instalado en Pluma (sin Violentmonkey)
$ytmp3 = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'YT-MP3\boton-firefox.html'
$scripts = Join-Path $env:LOCALAPPDATA 'Pluma\scripts'
if (Test-Path $ytmp3) {
    $html = [IO.File]::ReadAllText($ytmp3, [Text.Encoding]::UTF8)
    $m = [regex]::Match($html, '<textarea[^>]*id="script"[^>]*>([\s\S]*?)</textarea>')
    if ($m.Success) {
        New-Item -ItemType Directory -Force $scripts | Out-Null
        $code = [System.Net.WebUtility]::HtmlDecode($m.Groups[1].Value).Trim() + "`n"
        [IO.File]::WriteAllText((Join-Path $scripts 'YT-MP3.user.js'), $code, (New-Object Text.UTF8Encoding $false))
        Write-Host 'Botón MP3 de YT-MP3 instalado en Pluma.'
    }
}

& $exe --salir 2>$null
Start-Sleep -Milliseconds 800
if ($ArrancarConWindows) { Start-Process $exe -ArgumentList '--oculto' }
Write-Host 'Pluma instalado. Para que sea tu navegador predeterminado: Configuración > Aplicaciones > Aplicaciones predeterminadas > Pluma.'
