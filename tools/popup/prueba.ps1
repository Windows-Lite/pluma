# Extension popups and their dialogs, in a test instance (never the user's Pluma): run in the interactive session with
# PLUMA_EXE = a build elsewhere. Bug of 26/10: Authenticator asks for the clipboard when a code is clicked; the engine's
# prompt, owned by the popup, disables the popup and the Pluma window; it took the activation, Pluma closed the popup,
# the prompt went with it and the window stayed disabled: the page took no clicks.
# What is checked is the window state (enabled, the page's window under the point), not a click through DevTools (that
# reaches the page even with the window disabled). -Authenticator <its unpacked folder>: that one too.
# -Botones: where the bar's extension buttons are (window pixels; the default is 1400x900 at 125 %, the extensions in
# their order: Authenticator, Pluma popup test, uBlock).
param([string]$Authenticator = '', [string]$Botones = '1231,75,1273,75', [string]$Salida = '')
$ErrorActionPreference = 'Continue'
$L = $env:LOCALAPPDATA
$src = Split-Path (Split-Path $PSScriptRoot)
$out = if ($Salida) { $Salida } else { "$(Split-Path $env:PLUMA_EXE)\out" }
New-Item -ItemType Directory -Force $out | Out-Null
$env:PLUMA_SIN_SONIDO = '1'
$node = "$L\fp-tools\node\node.exe"
$cap = "$src\tools\captura.exe"
$name = 'pop'; $cls = "PlumaVentana.$name"
# a DevTools port nobody else has (another test instance on it would get the clicks)
$cdp = 9431; while ($cdp -lt 9499) { try { Invoke-RestMethod "http://127.0.0.1:$cdp/json/version" -TimeoutSec 1 | Out-Null; $cdp++ } catch { break } }
$data = "$L\Pluma\prueba.$name"
$b = $Botones.Split(',') | ForEach-Object { [int]$_ }
$t = if ($Authenticator) { 2 } else { 0 }   # the test extension's button (after Authenticator's, by name)
$tx = $b[$t]; $ty = $b[$t + 1]
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices; using System.Collections.Generic;
public static class P {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct PT { public int x, y; }
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
  [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint c);
  [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(PT p);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  public delegate bool EP(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EP f, IntPtr l);
  public static IntPtr PopOf(uint pid) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, x) => { uint p; GetWindowThreadProcessId(h, out p); if (p == pid && Cls(h) == "PlumaExtension") { found = h; return false; } return true; }, IntPtr.Zero);
    return found;
  }
  public static string Cls(IntPtr h) { var s = new StringBuilder(256); GetClassName(h, s, 256); return s.ToString(); }
  // a visible window owned (at any depth) by w
  public static IntPtr Owned(IntPtr w) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, x) => { if (h != w && IsWindowVisible(h)) for (var o = GetWindow(h, 4); o != IntPtr.Zero; o = GetWindow(o, 4)) if (o == w) { found = h; return false; } return true; }, IntPtr.Zero);
    return found;
  }
}
"@
function Main { [P]::FindWindow($cls, [NullString]::Value) }
# the popup of this instance (never another Pluma's)
function Pop { [P]::PopOf([uint32]$script:root) }
# the page's own window under the middle of the page (a disabled Pluma window gets the point instead)
function PageHit {
  $r = New-Object P+RECT; [void][P]::GetWindowRect((Main), [ref]$r)
  $p = New-Object P+PT; $p.x = [int](($r.L + $r.R) / 2); $p.y = [int](($r.T + $r.B) / 2 + 60)
  [P]::Cls([P]::WindowFromPoint($p))
}
$fails = 0
function Check($what, $ok) { if ($ok) { "ok    $what" } else { "FALLA $what"; $script:fails++ } }
function WaitFor($sb, $ms = 8000) { for ($i = 0; $i -lt $ms / 50; $i++) { if (& $sb) { return $true }; Start-Sleep -Milliseconds 50 }; return $false }
function Shot($f) { & $cap $cls "$out\$f" | Out-Null }
function Answer($root, $shot) {
  # the prompt's button, as the person would (UI Automation: no focus taken; the engine enables it a moment after)
  Start-Sleep -Milliseconds 1000
  try { & "$src\tools\passgen\ventanas.ps1" -Root $root -Shot "$out\$shot" -Invoke 'Permitir|Allow|Aceptar|OK' | Where-Object { $_ -match '^pulsado' } } catch { "no se pudo pulsar: $_" }
}
function Ready($what) {
  Check "${what}: ventana de Pluma activa" ([P]::IsWindowEnabled((Main)))
  Check "${what}: la página recibe el clic" ((PageHit) -eq 'Chrome_RenderWidgetHostHWND')
}

& "$src\tools\prueba.ps1" -Nombre $name -Puerto $cdp -Salir
Start-Sleep 2
Remove-Item -Recurse -Force $data -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$data\extensiones" | Out-Null
Copy-Item -Recurse "$PSScriptRoot\extension" "$data\extensiones\pluma-popup-test"
if ($Authenticator) { Copy-Item -Recurse $Authenticator "$data\extensiones\authenticator" }
$page = 'file:///' + ("$PSScriptRoot\pagina.html" -replace '\\', '/' -replace ' ', '%20')
# the user's clipboard: kept, and put back at the end
$saved = New-Object Windows.Forms.DataObject
$cb = [Windows.Forms.Clipboard]::GetDataObject()
if ($cb) { foreach ($f in $cb.GetFormats($false)) { try { $d = $cb.GetData($f, $false); if ($d -ne $null) { $saved.SetData($f, $false, $d) } } catch {} } }
try {
  & "$src\tools\prueba.ps1" -Nombre $name -Puerto $cdp -Url $page
  [void](WaitFor { try { @(Invoke-RestMethod "http://127.0.0.1:$cdp/json/list" -TimeoutSec 1 | Where-Object { $_.url -like 'file*' }).Count } catch { $false } } 30000)
  $root = @(Get-CimInstance Win32_Process -Filter "Name='Pluma.exe'" | Where-Object { $_.ExecutablePath -eq $env:PLUMA_EXE } | Sort-Object CreationDate)[0].ProcessId
  # the engine puts its prompt on the nearest screen: off it at once
  & "$src\tools\passgen\ventanas.ps1" -Root $root -Watch
  Start-Sleep 4
  Ready 'al empezar'

  if ($Authenticator) {
    "-- Authenticator: clic en un código (pide el portapapeles)"
    & $cap $cls "$out\x.png" click $($b[0]) $($b[1]) | Out-Null
    [void](WaitFor { (Pop) -ne [IntPtr]::Zero })
    Start-Sleep 2
    $seed = "(async () => { const e = { dataType: 'OTPStorage', account: 'test@pluma', issuer: 'Pluma', secret: 'JBSWY3DPEHPK3PXP', type: 'totp', encrypted: false, index: 0, counter: 0, period: 30, digits: 6, algorithm: 'SHA1', pinned: false, hash: 'pluma0' }; await chrome.storage.sync.set({ pluma0: e }); await chrome.storage.local.set({ pluma0: e }); setTimeout(() => location.reload(), 100); return 'ok'; })()"
    & $node "$src\tools\cdp.js" $cdp 'view/popup.html' $seed | Out-Null
    Start-Sleep 3
    & $cap 'PlumaExtension|Authenticator' "$out\authenticator-1-popup.png" | Out-Null
    & $node "$src\tools\clic.js" $cdp 'view/popup.html' '.entry' | Out-Null
    $dlg = WaitFor { (Pop) -ne [IntPtr]::Zero -and [P]::Owned((Pop)) -ne [IntPtr]::Zero }
    Check 'su aviso sale y la ventanita sigue' $dlg
    Start-Sleep 1
    Check 'la ventanita sigue con el aviso arriba' ((Pop) -ne [IntPtr]::Zero)
    Answer $root 'authenticator-aviso'
    [void](WaitFor { (Pop) -ne [IntPtr]::Zero -and [P]::Owned((Pop)) -eq [IntPtr]::Zero })
    Start-Sleep 1
    $clip = [Windows.Forms.Clipboard]::GetText()
    Check 'código copiado' ($clip -match '^\d{6}$')
    Check 'la ventanita sigue tras contestar' ((Pop) -ne [IntPtr]::Zero)
    if ((Pop) -ne [IntPtr]::Zero) { & $cap 'PlumaExtension|Authenticator' "$out\authenticator-2-copiado.png" | Out-Null }
    Ready 'tras contestar'
    # a click on the page: the popup loses the activation
    $p = Pop; if ($p -ne [IntPtr]::Zero) { [void][P]::SendMessage($p, 6, [IntPtr]0, [IntPtr]::Zero) }
    Check 'clic fuera: se cierra' (WaitFor { (Pop) -eq [IntPtr]::Zero })
    Ready 'Authenticator cerrada'
    Shot 'authenticator-3-pagina.png'
  }

  "-- ventanita de prueba: se cierra con su aviso arriba (window.close o Esc)"
  & $cap $cls "$out\x.png" click $tx $ty | Out-Null
  [void](WaitFor { (Pop) -ne [IntPtr]::Zero })
  Start-Sleep 2
  & $node "$src\tools\clic.js" $cdp 'popup.html' '#copy' | Out-Null
  Check 'su aviso sale' (WaitFor { (Pop) -ne [IntPtr]::Zero -and [P]::Owned((Pop)) -ne [IntPtr]::Zero })
  [void][P]::SendMessage((Pop), 0x10, [IntPtr]::Zero, [IntPtr]::Zero)   # WM_CLOSE, as window.close() and Esc end
  Check 'cerrada' (WaitFor { (Pop) -eq [IntPtr]::Zero })
  Start-Sleep 1
  Ready 'cerrada con el aviso arriba'

  "-- ventanita de prueba: copiar y window.close()"
  & $cap $cls "$out\x.png" click $tx $ty | Out-Null
  [void](WaitFor { (Pop) -ne [IntPtr]::Zero })
  Start-Sleep 2
  & $cap 'PlumaExtension|Pluma popup test' "$out\prueba-1-popup.png" | Out-Null
  & $node "$src\tools\clic.js" $cdp 'popup.html' '#copyclose' | Out-Null
  if (WaitFor { (Pop) -ne [IntPtr]::Zero -and [P]::Owned((Pop)) -ne [IntPtr]::Zero } 3000) { Answer $root 'prueba-aviso' }
  Check 'window.close(): se cierra' (WaitFor { (Pop) -eq [IntPtr]::Zero })
  Check 'copiado' ([Windows.Forms.Clipboard]::GetText() -eq '123456')
  Start-Sleep 1
  Ready 'tras window.close()'
  Shot 'prueba-2-pagina.png'
  $done = $true
} finally {
  & "$src\tools\passgen\ventanas.ps1" -Stop
  & "$src\tools\prueba.ps1" -Nombre $name -Puerto $cdp -Salir
  try { if (@($saved.GetFormats()).Count) { [Windows.Forms.Clipboard]::SetDataObject($saved, $true) } else { [Windows.Forms.Clipboard]::Clear() } } catch { "no se pudo devolver el portapapeles: $_" }
  Remove-Item "$out\x.png" -ErrorAction SilentlyContinue
}
if (-not $done) { "FALLA la prueba no llegó al final"; $fails++ }
"fallos: $fails"
