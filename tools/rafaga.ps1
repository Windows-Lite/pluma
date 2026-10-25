# A burst of pictures of a test instance taken by Pluma itself (foto-00.png, foto-01.png... in its data folder, with
# the time of each in pluma.log), to see a transition frame by frame. Then, optionally, tools\foto.ps1 actions.
#   tools\rafaga.ps1 [-Nombre yt] [-Fotos 14] [-Cada 35] [-Acciones 'key 0x46'] [-Salida <carpeta>]
param([string]$Nombre = 'yt', [int]$Fotos = 14, [int]$Cada = 35, [string[]]$Acciones = @(), [string]$Salida = '')
Add-Type -Namespace R -Name U -MemberDefinition @'
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, string t);
[DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
'@
$win = [R.U]::FindWindowW("PlumaVentana.$Nombre", [NullString]::Value)
if ($win -eq [IntPtr]::Zero) { throw "no hay ventana PlumaVentana.$Nombre" }
$dir = "$env:LOCALAPPDATA\Pluma\prueba.$Nombre"
Remove-Item "$dir\foto-*.png" -ErrorAction SilentlyContinue
[R.U]::PostMessageW($win, 0x8000 + 91, [IntPtr]$Fotos, [IntPtr]$Cada) | Out-Null
if ($Acciones) { & (Join-Path $PSScriptRoot 'foto.ps1') -Nombre $Nombre -Acciones $Acciones }
$deadline = (Get-Date).AddSeconds(15)
while ((Get-ChildItem "$dir\foto-*.png" -ErrorAction SilentlyContinue).Count -lt $Fotos -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 200 }
if ($Salida) { New-Item -ItemType Directory -Force $Salida | Out-Null; Copy-Item "$dir\foto-*.png" $Salida -Force }
