# Saved passwords (vault.cpp) in a test instance, never the user's Pluma: run in the interactive session with
# PLUMA_EXE = a build elsewhere. A login saved by the engine itself (its prompt's Guardar, pressed as the person, via
# tools\passgen) and invented ones (pluma-prueba:sembrar, in at the next start); then Configuración > Contraseñas and
# the key panel. The verification is answered by PLUMA_PRUEBA_VERIFICAR (si / no / lento): the security checks are a
# reveal refused without it, a page that went away getting nothing, and no web page able to reach Pluma. Clipboard
# (saved and put back), log without any of the invented data, photos light and dark in <exe folder>\out-claves.
# -Hello: only the real Windows prompt (Hello or the account's password): shown, photographed, closed with the instance.
param([switch]$Hello)
$ErrorActionPreference = 'Continue'
$L = $env:LOCALAPPDATA
$src = Split-Path (Split-Path $PSScriptRoot)
$pg = "$src\tools\passgen"
$dir = Split-Path $env:PLUMA_EXE
$out = "$dir\out-claves"
New-Item -ItemType Directory -Force $out, "$out\passgen" | Out-Null
$node = "$L\fp-tools\node\node.exe"
$py = (Get-ChildItem "$L\Programs\Python" -Recurse -Filter python.exe | Where-Object { $_.FullName -notmatch 'venv' } | Select-Object -First 1).FullName
$env:PLUMA_SIN_SONIDO = '1'
$env:PLUMA_PRUEBA_PORTAPAPELES_S = '3'
$data = "$L\Pluma\prueba.pw"
$pages = 9513; $cdp = 9503
$srv = $null
$fails = 0
function Check($name, $ok) { if ($ok) { "ok    $name" } else { "FALLA $name"; $script:fails++ } }
function SetTheme($t) {
  New-Item -ItemType Directory -Force $data | Out-Null
  $ini = "$data\ajustes.ini"
  $lines = @(if (Test-Path $ini) { Get-Content $ini | Where-Object { $_ -notmatch '^(tema|ajustes_v|idioma)=' -and $_ -ne '[' } } else { '[pluma]' })
  if (-not ($lines -contains '[pluma]')) { $lines = @('[pluma]') + $lines }
  $i = [array]::IndexOf($lines, '[pluma]')
  $new = @($lines[0..$i]) + @("tema=$t", 'ajustes_v=2', 'idioma=es') + @(if ($i + 1 -lt $lines.Count) { $lines[($i + 1)..($lines.Count - 1)] })
  Set-Content $ini $new -Encoding ASCII
}
function Logins {
  Copy-Item "$data\webview2\EBWebView\Default\Login Data" "$env:TEMP\ld-claves.db" -Force
  & $py -c "import sqlite3;c=sqlite3.connect(r'$env:TEMP\ld-claves.db');print(c.execute('select username_value from logins').fetchall())"
  Remove-Item "$env:TEMP\ld-claves.db"
}
function PlumaPid {
  $p = @(Get-CimInstance Win32_Process -Filter "Name='Pluma.exe'" | Where-Object { $_.ExecutablePath -eq $env:PLUMA_EXE } | Sort-Object CreationDate)
  if ($p.Count) { [int]$p[0].ProcessId } else { 0 }
}
function Quit {
  & "$src\tools\prueba.ps1" -Nombre pw -Puerto $cdp -Salir
  for ($i = 0; $i -lt 60; $i++) { $m = $null; if (-not [System.Threading.Mutex]::TryOpenExisting('PlumaNavegadorUnico.pw', [ref]$m)) { break }; $m.Dispose(); Start-Sleep -Milliseconds 500 }
  for ($i = 0; $i -lt 40; $i++) { try { Invoke-RestMethod "http://127.0.0.1:$cdp/json/version" -TimeoutSec 1 | Out-Null; Start-Sleep -Milliseconds 500 } catch { break } }
}
function WaitCdp {
  for ($i = 0; $i -lt 60; $i++) {
    try { if (@(Invoke-RestMethod "http://127.0.0.1:$cdp/json/list" -TimeoutSec 1 | Where-Object { $_.type -eq 'page' -and $_.url -like 'http*' }).Count) { return $true } } catch {}
    Start-Sleep -Milliseconds 500
  }
  return $false
}
function Start-Pluma($verify) {
  if ($verify) { $env:PLUMA_PRUEBA_VERIFICAR = $verify } else { Remove-Item Env:PLUMA_PRUEBA_VERIFICAR -ErrorAction SilentlyContinue }
  Remove-Item "$data\sesion.tsv" -ErrorAction SilentlyContinue
  for ($k = 0; $k -lt 2; $k++) {
    & "$src\tools\prueba.ps1" -Nombre pw -Puerto $cdp -Url "http://127.0.0.1:$pages/login.html"
    if (WaitCdp) { Start-Sleep 2; & "$pg\ventanas.ps1" -Stop; & "$pg\ventanas.ps1" -Root (PlumaPid) -Watch; return }
    Quit
  }
}
function Order($what, $ms = 700) { & $env:PLUMA_EXE "pluma-prueba:$what"; Start-Sleep -Milliseconds $ms }
function Ev($part, $expr) { (& $node "$src\tools\cdp.js" $cdp $part $expr) -join '' }
function Settings {
  & $env:PLUMA_EXE 'https://pluma.local/settings#contrasenas'
  for ($i = 0; $i -lt 30; $i++) { Start-Sleep -Milliseconds 500; if ((Ev 'pluma.local' 'PW ? PW.rows.length : -9') -match '^\d+$') { break } }
}
function IdOf($user) { Ev 'pluma.local' "PW.rows.find(r => r.user == '$user').id" }
function Send($cmd, $id) { Ev 'pluma.local' "chrome.webview.postMessage({op:'pw',cmd:'$cmd',id:$id}),1" | Out-Null }
function Panel($name) { Order 'claves' 1200; Order "foto=$name" 1500; if (Test-Path "$data\$name.png") { Copy-Item "$data\$name.png" "$out\$name.png" -Force } }
function ClipText { for ($i = 0; $i -lt 10; $i++) { try { return [System.Windows.Forms.Clipboard]::GetText() } catch { Start-Sleep -Milliseconds 200 } }; '' }
function Shot($name) { & "$src\tools\captura.exe" PlumaVentana.pw "$out\$name.png" | Out-Null }
$secret = 'Prueba-Uno-123|Otra\.Clave-456|Tienda_789|Local-Prueba-321|ana\.prueba|beto@|carla|usuario\.prueba|ejemplo-pluma|ana@example'
try {
  $srv = Start-Process $py -ArgumentList "`"$pg\serve.py`" $pages" -WindowStyle Hidden -PassThru
  Quit
  if ($Hello) {
    # the real prompt (Hello, or without it the account's password): nothing answered for it. Found and photographed
    # by itself (PrintWindow: never the screen, which may show anything); then the instance quits, which closes it
    # (pluma-prueba:cancelar cancels Hello's; the password's one does not take a close from outside)
    Add-Type -AssemblyName System.Drawing
    Add-Type -ReferencedAssemblies System.Drawing @"
using System; using System.Runtime.InteropServices; using System.Text; using System.Drawing;
public static class H {
  public delegate bool EP(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EP f, IntPtr l);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  public struct RECT { public int L, T, R, B; }
  public static IntPtr Find(string cls) { IntPtr f = IntPtr.Zero; EnumWindows((h, x) => { var s = new StringBuilder(256); GetClassName(h, s, 256);
    if (IsWindowVisible(h) && s.ToString() == cls) { f = h; return false; } return true; }, IntPtr.Zero); return f; }
  public static void Shot(IntPtr h, string f) { RECT r; GetWindowRect(h, out r); var b = new Bitmap(Math.Max(1, r.R - r.L), Math.Max(1, r.B - r.T));
    using (var g = Graphics.FromImage(b)) { var dc = g.GetHdc(); PrintWindow(h, dc, 2); g.ReleaseHdc(dc); } b.Save(f); }
}
"@
    $cls = 'Credential Dialog Xaml Host'
    if ([H]::Find($cls) -ne [IntPtr]::Zero) { 'ya hay una pregunta de Windows en pantalla: no se prueba'; return }
    SetTheme 'claro'
    Start-Pluma ''
    Order 'claves' 1500
    $v0 = @(Get-Content "$data\pluma.log" | Select-String 'contraseñas: verificado').Count
    Order 'clave-ver=1' 500
    $h = [IntPtr]::Zero
    for ($i = 0; $i -lt 20 -and $h -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 250; $h = [H]::Find($cls) }
    Check 'la pregunta de Windows sale' ($h -ne [IntPtr]::Zero)
    if ($h -ne [IntPtr]::Zero) { Start-Sleep 2; [H]::Shot($h, "$out\windows-pregunta.png") }
    Order 'cancelar' 1000
    Get-Content "$data\pluma.log" -Tail 4 | Select-String 'contraseñas:'
    Check 'Pluma no revela nada mientras tanto' (@(Get-Content "$data\pluma.log" | Select-String 'contraseñas: verificado').Count -eq $v0)
    Quit
    Start-Sleep 1
    Check 'se va con la instancia' ([H]::Find($cls) -ne $h -or $h -eq [IntPtr]::Zero)
    return
  }
  if (Test-Path $data) { Remove-Item -Recurse -Force $data }
  SetTheme 'claro'
  Start-Pluma 'si'
  "== una guardada por el motor (su pregunta, Guardar pulsado como la persona)"
  & $node "$src\tools\passgen-e2e.js" $cdp $pages gen "$src\tools\captura.exe" PlumaVentana.pw "$out\passgen" | Select-String 'FALLA|error' 
  Start-Sleep 2
  & "$pg\ventanas.ps1" -Root (PlumaPid) -Invoke 'Guardar|Save' | Select-String 'pulsado'
  Start-Sleep 4
  Check 'el motor la tiene' ((Logins) -match 'ana@example.com')
  Order 'sembrar'
  Quit
  Start-Pluma 'si'
  Check 'las inventadas, dentro al arrancar' ((Logins) -match 'beto@')

  "== Configuración > Contraseñas"
  Settings
  Check 'la lista: 5' ((Ev 'pluma.local' 'PW.rows.length') -eq '5')
  Check 'del fichero vivo' ((Ev 'pluma.local' 'PW.from') -eq '0')
  $uno = IdOf 'ana.prueba'; $ana = IdOf 'ana@example.com'; $carla = IdOf 'carla'
  Check 'nada revelado sin pedirlo' ((Ev 'pluma.local' 'Object.keys(pwShown).length') -eq '0')
  Send 'reveal' $uno; Start-Sleep 1
  Check 'mostrar una de Pluma (v10)' ((Ev 'pluma.local' "pwShown[$uno]") -eq '"Prueba-Uno-123"')
  Send 'reveal' $ana; Start-Sleep 1
  $made = Get-Content "$out\passgen\guardada.txt" -Raw -ErrorAction SilentlyContinue
  Check 'mostrar la que guardó el motor (la sugerida, la misma)' ($made -and (Ev 'pluma.local' "pwShown[$ana]") -eq ('"' + $made.Trim() + '"'))
  Check 'buscar en ajustes: password' ((Ev 'pluma.local' "(document.getElementById('q').value='password',filter(),!document.getElementById('contrasenas').classList.contains('hidden'))") -eq 'true')
  Ev 'pluma.local' "(document.getElementById('q').value='',filter(),document.getElementById('contrasenas').scrollIntoView(),1)" | Out-Null
  Start-Sleep 1
  Shot 'ajustes-claro'
  "== copiar (el portapapeles se guarda y se devuelve)"
  Add-Type -AssemblyName System.Windows.Forms
  $keep = ClipText
  try {
    Send 'copy' $carla; Start-Sleep 1
    $f = @(try { [System.Windows.Forms.Clipboard]::GetDataObject().GetFormats() } catch { })
    Check 'copiada' ((ClipText) -eq 'Tienda_789-xyz')
    Check 'fuera del historial y de la nube' (($f -contains 'CanIncludeInClipboardHistory') -and ($f -contains 'CanUploadToCloudClipboard') -and ($f -contains 'ExcludeClipboardContentFromMonitorProcessing'))
    Start-Sleep 3
    Check 'borrada del portapapeles a los 3 s' (-not (ClipText))
  } finally {
    # back as it was, unless something new is there
    # (another program may hold the clipboard a moment: a few tries, never an error that stops the rest)
    for ($i = 0; $keep -and $i -lt 10; $i++) {
      try { if (-not [System.Windows.Forms.Clipboard]::ContainsText()) { [System.Windows.Forms.Clipboard]::SetText($keep) }; break } catch { Start-Sleep -Milliseconds 200 }
    }
    $keep = $null
  }
  "== quitar (al reiniciar)"
  Send 'delete' $carla; Start-Sleep 1.5
  Check 'marcada para quitar' ((Ev 'pluma.local' "PW.rows.find(r=>r.id==$carla).del") -eq 'true')
  "== el panel de la llave"
  & $env:PLUMA_EXE "http://127.0.0.1:$pages/login.html?web=1"
  Start-Sleep 3
  # (its own address: the first tab, out of sight, is frozen and would not answer). A web page asks for a copy and
  # the list as the settings page would (and the verification here says yes at once): nothing may happen
  $n0 = @(Get-Content "$data\pluma.log" | Select-String 'contraseñas: (verificado|copiada|lista)').Count
  $web = Ev "login.html?web=1" "(() => { try { chrome.webview.postMessage({op:'pw',cmd:'copy',id:$uno}); chrome.webview.postMessage({op:'pw',cmd:'list'}); return 'enviado' } catch (e) { return 'sin canal' } })()"
  Start-Sleep 2
  $n1 = @(Get-Content "$data\pluma.log" | Select-String 'contraseñas: (verificado|copiada|lista)').Count
  Check "una web no consigue nada de Pluma ($web)" ($n1 -eq $n0 -and -not ((ClipText) -match 'Prueba-Uno'))
  Shot 'barra-llave-claro'
  Panel 'panel-claro'
  Order 'clave-ver=1' 1500
  Order 'foto=panel-visto-claro' 1500
  Copy-Item "$data\panel-visto-claro.png" "$out\" -Force -ErrorAction SilentlyContinue
  Quit

  "== sin verificar: no se manda nada"
  Start-Pluma 'no'
  Check 'la quitada ya no está al arrancar otra vez' (-not ((Logins) -match 'carla'))
  Settings
  $uno = IdOf 'ana.prueba'
  Send 'reveal' $uno; Start-Sleep 1.5
  Check 'rechazada' (((Ev 'pluma.local' "pwShown[$uno] === undefined && JSON.stringify(pwFlash)") -match 'comprobar'))
  Quit
  "== la página se va mientras Windows pregunta: no recibe nada"
  Start-Pluma 'lento'
  Settings
  $uno = IdOf 'ana.prueba'
  Send 'reveal' $uno
  Ev 'pluma.local' "location.href='http://127.0.0.1:$pages/login.html',1" | Out-Null
  Start-Sleep 3.5
  Check 'descartada' (@(Get-Content "$data\pluma.log" -Tail 20 | Select-String 'la página ya no está').Count -ge 1)
  Quit
  "== oscuro"
  SetTheme 'oscuro'
  Start-Pluma 'si'
  Settings
  $uno = IdOf 'ana.prueba'
  Send 'reveal' $uno; Start-Sleep 1
  Ev 'pluma.local' "document.getElementById('contrasenas').scrollIntoView(),1" | Out-Null
  Start-Sleep 1
  Shot 'ajustes-oscuro'
  & $env:PLUMA_EXE "http://127.0.0.1:$pages/login.html"
  Start-Sleep 3
  Shot 'barra-llave-oscuro'
  Panel 'panel-oscuro'
  Order 'clave-ver=2' 1500
  Order 'foto=panel-visto-oscuro' 1500
  Copy-Item "$data\panel-visto-oscuro.png" "$out\" -Force -ErrorAction SilentlyContinue
  "== el registro no tiene nada de esto"
  $hits = @(Get-Content "$data\pluma.log" | Select-String $secret)
  Check 'pluma.log sin contraseñas, usuarios ni sitios inventados' ($hits.Count -eq 0)
  $hits | Select-Object -First 3
  Get-Content "$data\pluma.log" | Select-String 'contraseñas|prueba:' | Select-Object -Last 15
} finally {
  & "$pg\ventanas.ps1" -Stop
  Quit
  if ($srv) { Stop-Process -Id $srv.Id -Force -ErrorAction SilentlyContinue }
}
Get-ChildItem $out -File | ForEach-Object { $_.Name + ' ' + $_.Length }
if ($fails) { "$fails fallos en contrasenas\prueba.ps1" } else { 'contrasenas\prueba.ps1: todo bien' }
