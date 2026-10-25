# Baja el motor de vídeo de Pluma (libmpv, la compilación oficial de mpv para Windows de shinchiro) a motor-video\,
# junto a Pluma.exe. Es lo que reproduce los vídeos de YouTube en el reproductor propio de Pluma. Versión fijada y
# comprobada con su SHA-256: si el archivo no es exactamente ese, no se instala.
#   tools\motor-video.ps1 [-Destino <carpeta>]
param([string]$Destino = (Join-Path (Split-Path $PSScriptRoot) 'motor-video'))
$ErrorActionPreference = 'Stop'
$url = 'https://github.com/shinchiro/mpv-winbuild-cmake/releases/download/20261021/mpv-dev-x86_64-20261021-git-e471496c56.7z'
$sha = 'efc719eff3dc5954b4a5efc800632808bcec01cc9034940dd14e5eb7fe9ea633'
$dll = Join-Path $Destino 'libmpv-2.dll'
if (Test-Path $dll) { Write-Host "El motor de vídeo ya está en $Destino"; return }
$tmp = Join-Path $env:TEMP ('pluma-motor-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $tmp | Out-Null
try {
    Write-Host 'Bajando el motor de vídeo (30 MB)...'
    $ProgressPreference = 'SilentlyContinue'
    Invoke-WebRequest -UseBasicParsing $url -OutFile "$tmp\mpv-dev.7z"
    if ((Get-FileHash "$tmp\mpv-dev.7z" -Algorithm SHA256).Hash.ToLower() -ne $sha) { throw 'El archivo bajado no es el esperado (SHA-256 distinto): no se instala.' }
    tar -xf "$tmp\mpv-dev.7z" -C $tmp libmpv-2.dll
    if (-not (Test-Path "$tmp\libmpv-2.dll")) { throw 'No se pudo sacar libmpv-2.dll del archivo.' }
    New-Item -ItemType Directory -Force $Destino | Out-Null
    Move-Item -Force "$tmp\libmpv-2.dll" $dll
    Write-Host "Motor de vídeo listo en $Destino"
} finally { Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue }
