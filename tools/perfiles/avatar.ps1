# Profile avatars in a test instance, never the user's Pluma: run in the interactive session with PLUMA_EXE = a build
# elsewhere. A picture set from a file (pluma-prueba:avatar-imagen: the same path as «Elegir imagen...» after the
# picker), cropped to the centred square at 128 px; a built-in one; back to the initial; and files that are not
# images (random bytes, a PNG cut in half) refused without writing anything. Then the settings page gets it from Pluma
# (a data URL), and photos light and dark of the bar, its panel, Configuración > Perfiles and the taskbar badge in
# <exe folder>\out-avatar. -Selector: only shows the real file picker (photographed and closed; never driven).
param([switch]$Selector)
$ErrorActionPreference = 'Continue'
$L = $env:LOCALAPPDATA
$src = Split-Path (Split-Path $PSScriptRoot)
$dir = Split-Path $env:PLUMA_EXE
$out = "$dir\out-avatar"
$img = "$dir\t223img"
New-Item -ItemType Directory -Force $out, $img | Out-Null
$node = "$L\fp-tools\node\node.exe"
$env:PLUMA_SIN_SONIDO = '1'
$name = 'av'
$data = "$L\Pluma\prueba.$name"
$cdp = 9523
$fails = 0
Add-Type -AssemblyName System.Drawing
function Check($what, $ok) { if ($ok) { "ok    $what" } else { "FALLA $what"; $script:fails++ } }
function SetTheme($t) {
  $ini = "$data\ajustes.ini"
  $lines = @(if (Test-Path $ini) { Get-Content $ini | Where-Object { $_ -notmatch '^(tema|ajustes_v|idioma)=' } } else { '[pluma]' })
  if (-not ($lines -contains '[pluma]')) { $lines = @('[pluma]') + $lines }
  $i = [array]::IndexOf($lines, '[pluma]')
  $new = @($lines[0..$i]) + @("tema=$t", 'ajustes_v=2', 'idioma=es') + @(if ($i + 1 -lt $lines.Count) { $lines[($i + 1)..($lines.Count - 1)] })
  Set-Content $ini $new -Encoding ASCII
}
function Quit {
  & "$src\tools\prueba.ps1" -Nombre $name -Puerto $cdp -Salir
  for ($i = 0; $i -lt 60; $i++) { $m = $null; if (-not [System.Threading.Mutex]::TryOpenExisting("PlumaNavegadorUnico.$name", [ref]$m)) { break }; $m.Dispose(); Start-Sleep -Milliseconds 500 }
}
function Ev($expr) { (& $node "$src\tools\cdp.js" $cdp 'pluma.local' $expr) -join '' }
function Start-Pluma {
  Remove-Item "$data\sesion.tsv" -ErrorAction SilentlyContinue
  & "$src\tools\prueba.ps1" -Nombre $name -Puerto $cdp -Url 'https://pluma.local/settings#perfiles'
  for ($i = 0; $i -lt 60; $i++) { Start-Sleep -Milliseconds 500; if ((Ev 'S && S.profiles ? S.profiles.list.length : -1') -match '^[1-9]') { break } }
  Start-Sleep 1
}
function Order($what, $ms = 900) { & $env:PLUMA_EXE "pluma-prueba:$what"; Start-Sleep -Milliseconds $ms }
function Tsv { (Get-Content "$data\perfiles.tsv" -Raw -Encoding UTF8) }
function LogHas($pat) { @(Get-Content "$data\pluma.log" -Encoding UTF8 | Select-String $pat).Count }
function Px($file, $x, $y) {
  $b = [Drawing.Bitmap]::FromFile($file); try { $c = $b.GetPixel($x, $y); @{ w = $b.Width; h = $b.Height; r = $c.R; g = $c.G; b = $c.B; a = $c.A } } finally { $b.Dispose() }
}
function Shot($n) { & "$src\tools\captura.exe" "PlumaVentana.$name" "$out\$n.png" | Out-Null }
function Panel($n) { Order 'perfil' 1300; Order "foto=$n" 1600; if (Test-Path "$data\$n.png") { Move-Item "$data\$n.png" "$out\$n.png" -Force } }

# the test files: a wide photo (red | green | blue thirds: the centred square starts in the red and ends in the blue),
# random bytes named .jpg, and a PNG cut in half
$bm = New-Object Drawing.Bitmap 1200, 800
$g = [Drawing.Graphics]::FromImage($bm)
$g.FillRectangle([Drawing.Brushes]::Red, 0, 0, 400, 800); $g.FillRectangle([Drawing.Brushes]::Lime, 400, 0, 400, 800); $g.FillRectangle([Drawing.Brushes]::Blue, 800, 0, 400, 800)
$g.FillEllipse([Drawing.Brushes]::White, 520, 320, 160, 160)
$g.Dispose(); $bm.Save("$img\foto.jpg", [Drawing.Imaging.ImageFormat]::Jpeg)
$bm.Save("$img\entera.png", [Drawing.Imaging.ImageFormat]::Png); $bm.Dispose()
$bytes = [IO.File]::ReadAllBytes("$img\entera.png")
[IO.File]::WriteAllBytes("$img\cortada.png", $bytes[0..([int]($bytes.Length / 2))])
$bytes = [IO.File]::ReadAllBytes("$img\foto.jpg")
[IO.File]::WriteAllBytes("$img\cortada.jpg", $bytes[0..([int]($bytes.Length / 2))])
$rnd = New-Object byte[] 20000; (New-Object Random 7).NextBytes($rnd); [IO.File]::WriteAllBytes("$img\roto.jpg", $rnd)

try {
  Quit
  if (Test-Path $data) { Remove-Item -Recurse -Force $data }
  New-Item -ItemType Directory -Force "$data\perfiles\ptrabajo1" | Out-Null
  Set-Content "$data\perfiles.tsv" "default`tPersonal`t2f6fed`t`nptrabajo1`tTrabajo`t0f9d58`t`n" -NoNewline -Encoding UTF8
  SetTheme 'claro'
  Start-Pluma
  if ($Selector) {
    # the real picker: shown by Pluma (off the screen with its window), photographed, closed; nothing chosen
    Add-Type -ReferencedAssemblies System.Drawing @"
using System; using System.Runtime.InteropServices; using System.Text; using System.Drawing;
public static class H {
  public delegate bool EP(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EP f, IntPtr l);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  public struct RECT { public int L, T, R, B; }
  public static IntPtr Find(uint pid) { IntPtr f = IntPtr.Zero; EnumWindows((h, x) => { var s = new StringBuilder(256); GetClassName(h, s, 256); uint p;
    GetWindowThreadProcessId(h, out p); if (p == pid && IsWindowVisible(h) && s.ToString() == "#32770") { f = h; return false; } return true; }, IntPtr.Zero); return f; }
  public static void Shot(IntPtr h, string f) { RECT r; GetWindowRect(h, out r); var b = new Bitmap(Math.Max(1, r.R - r.L), Math.Max(1, r.B - r.T));
    using (var g = Graphics.FromImage(b)) { var dc = g.GetHdc(); PrintWindow(h, dc, 2); g.ReleaseHdc(dc); } b.Save(f); }
}
"@
    $p = @(Get-CimInstance Win32_Process -Filter "Name='Pluma.exe'" | Where-Object { $_.ExecutablePath -eq $env:PLUMA_EXE } | Sort-Object CreationDate)[0]
    Start-Process $env:PLUMA_EXE 'pluma-prueba:elegir-imagen=default'
    $h = [IntPtr]::Zero
    for ($i = 0; $i -lt 40 -and $h -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 250; $h = [H]::Find([uint32]$p.ProcessId) }
    Check 'sale el selector de archivos' ($h -ne [IntPtr]::Zero)
    if ($h -ne [IntPtr]::Zero) { Start-Sleep 2; [H]::Shot($h, "$out\selector.png"); [H]::PostMessage($h, 0x10, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null; Start-Sleep 1 }
    Check 'se cierra sin elegir nada' ([H]::Find([uint32]$p.ProcessId) -eq [IntPtr]::Zero -and -not (Test-Path "$data\avatar.png"))
    return
  }
  # a picture for Personal: the centred square, 128 px, round edges are drawn, not stored
  Order "avatar-imagen=default|$img\foto.jpg" 2000
  Check 'el registro dice imagen puesta' ((LogHas 'perfil default: imagen puesta') -eq 1)
  Check 'avatar.png en la carpeta del perfil' (Test-Path "$data\avatar.png")
  if (Test-Path "$data\avatar.png") {
    $l = Px "$data\avatar.png" 4 64; $c = Px "$data\avatar.png" 64 20; $r = Px "$data\avatar.png" 123 64
    Check "128x128 ($($l.w)x$($l.h))" ($l.w -eq 128 -and $l.h -eq 128)
    Check "cuadrado del centro: rojo, verde, azul ($($l.r),$($c.g),$($r.b))" ($l.r -gt 200 -and $l.g -lt 60 -and $c.g -gt 200 -and $c.r -lt 60 -and $r.b -gt 200 -and $r.r -lt 60)
  }
  Check 'perfiles.tsv: img' ((Tsv) -match "default`tPersonal`t2f6fed`timg")
  # a built-in one for the other profile, written from this window
  Order 'avatar-icono=ptrabajo1,5' 900
  Check 'perfiles.tsv: g5' ((Tsv) -match "ptrabajo1`tTrabajo`t0f9d58`tg5")
  # a JPEG cut in half: WIC decodes what is there (as browsers show part of one); only said, not required
  Order "avatar-imagen=ptrabajo1|$img\cortada.jpg" 2000
  "jpg cortado: " + @(Get-Content "$data\pluma.log" -Encoding UTF8 | Select-String 'ptrabajo1: imagen')[-1]
  Order 'avatar-icono=ptrabajo1,5' 900
  # files that are not images: refused, nothing written, the list as it was
  $before = Tsv
  foreach ($f in 'roto.jpg', 'cortada.png') {
    $n0 = LogHas 'perfil ptrabajo1: imagen no válida'
    Order "avatar-imagen=ptrabajo1|$img\$f" 2000
    Check "$f rechazado" ((LogHas 'perfil ptrabajo1: imagen no válida') -eq $n0 + 1)
    Check "$f no deja nada" (-not (Test-Path "$data\perfiles\ptrabajo1\avatar.png") -and -not (Test-Path "$data\perfiles\ptrabajo1\avatar.png.tmp") -and (Tsv) -eq $before)
  }
  Get-Content "$data\pluma.log" -Encoding UTF8 | Select-String 'imagen' | Select-Object -Last 4
  # the settings page gets it from Pluma, over its message (no pluma.local address serves it)
  Start-Sleep 1
  Check 'la página tiene la imagen (data URL)' ((Ev "S.profiles.list[0].picture && S.profiles.list[0].picture.startsWith('data:image/png;base64,')") -eq 'true')
  Check 'la página tiene los 12 de serie' ((Ev 'S.profiles.glyphs.filter(g => g.length > 100).length') -eq '12')
  Check 'ninguna dirección de pluma.local la sirve' ((Ev "fetch('https://pluma.local/avatar.png').then(r => r.status)") -eq '404')
  Order 'insignia=insignia-claro' 600; if (Test-Path "$data\insignia-claro.png") { Move-Item "$data\insignia-claro.png" "$out\insignia-claro.png" -Force }
  Check 'la insignia de la barra de tareas lleva la imagen' ((Test-Path "$out\insignia-claro.png") -and ((Px "$out\insignia-claro.png" 6 16).r -gt 150))
  foreach ($t in 'claro', 'oscuro') {
    if ($t -eq 'oscuro') { Quit; SetTheme 'oscuro'; Start-Pluma }
    Ev "picking = ''; renderProfiles(); document.getElementById('perfiles').scrollIntoView(); 1" | Out-Null
    Start-Sleep -Milliseconds 800
    Shot "configuracion-$t"
    Panel "panel-$t"
  }
  # back to the initial: the file goes
  Order 'avatar-quitar=default' 900
  Check 'Quitar imagen borra avatar.png' (-not (Test-Path "$data\avatar.png") -and (Tsv) -match "default`tPersonal`t2f6fed`t`r?`n")
  "fotos en $out"
} finally {
  Quit
  "fallos: $fails"
}
