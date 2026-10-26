# Picture of a test instance taken by Pluma itself, with the video player's controls blended in as the screen shows
# them (captura.exe gets the layered windows opaque). Before it, actions on the player, at points of the window's
# client area: they go as window messages to whatever is there (a control layer or the player's window), since the
# page's DevTools input does not reach Pluma's own player.
#   tools\foto.ps1 [-Nombre yt] [-Salida x.png] [-Acciones 'move 700,500','click 30,860','wheel 400,300,-120','key 0x4B','char <','wait 500']
param([string]$Nombre = 'yt', [string]$Salida = '', [string[]]$Acciones = @(), [string]$Raton = '', [int]$Espera = 300)
Add-Type -Namespace W -Name U -MemberDefinition @'
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, string t);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr p, IntPtr a, string c, string t);
[DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
public struct POINT { public int X, Y; }
public struct RECT { public int Left, Top, Right, Bottom; }
'@
$win = [W.U]::FindWindowW("PlumaVentana.$Nombre", [NullString]::Value)
if ($win -eq [IntPtr]::Zero) { throw "no hay ventana PlumaVentana.$Nombre" }
function Target($x, $y) {
    # the window under a client point: a visible control layer of this window first, else the player's window
    $o = New-Object W.U+POINT; [W.U]::ClientToScreen($win, [ref]$o) | Out-Null
    $sx = $o.X + $x; $sy = $o.Y + $y
    $h = [IntPtr]::Zero
    while (($h = [W.U]::FindWindowExW([IntPtr]::Zero, $h, 'PlumaControlesVideo', [NullString]::Value)) -ne [IntPtr]::Zero) {
        if ([W.U]::GetWindow($h, 4) -ne $win -or -not [W.U]::IsWindowVisible($h)) { continue }
        $r = New-Object W.U+RECT; [W.U]::GetWindowRect($h, [ref]$r) | Out-Null
        if ($sx -ge $r.Left -and $sx -lt $r.Right -and $sy -ge $r.Top -and $sy -lt $r.Bottom) { return @($h, ($sx - $r.Left), ($sy - $r.Top)) }
    }
    $v = [IntPtr]::Zero
    do { $v = [W.U]::FindWindowExW($win, $v, 'PlumaVideo', [NullString]::Value) } while ($v -ne [IntPtr]::Zero -and -not [W.U]::IsWindowVisible($v))
    if ($v -eq [IntPtr]::Zero) { return $null }
    # the player is in the video's place in the page: around it, the photo of the page
    $r = New-Object W.U+RECT; [W.U]::GetWindowRect($v, [ref]$r) | Out-Null
    $f = [W.U]::FindWindowExW($win, [IntPtr]::Zero, 'PlumaFotoPagina', [NullString]::Value)
    if (-not ($sx -ge $r.Left -and $sx -lt $r.Right -and $sy -ge $r.Top -and $sy -lt $r.Bottom) -and $f -ne [IntPtr]::Zero -and [W.U]::IsWindowVisible($f)) { $v = $f }
    $p = New-Object W.U+POINT; [W.U]::ClientToScreen($v, [ref]$p) | Out-Null
    return @($v, ($sx - $p.X), ($sy - $p.Y))
}
function Player { $v = [IntPtr]::Zero; do { $v = [W.U]::FindWindowExW($win, $v, 'PlumaVideo', [NullString]::Value) } while ($v -ne [IntPtr]::Zero -and -not [W.U]::IsWindowVisible($v)); $v }
if ($Raton) { $Acciones = @("move $Raton", "wait $Espera") + $Acciones }
foreach ($a in $Acciones) {
    $verb, $arg = $a -split ' ', 2
    switch ($verb) {
        { $_ -in 'move', 'click', 'dblclick' } {
            $x, $y = $arg -split ',' | ForEach-Object { [int]$_ }
            $t = Target $x $y
            if (-not $t) { "nada en $arg"; continue }
            $lp = [IntPtr]([int]$t[2] * 65536 + [int]$t[1])
            [W.U]::SendMessageW($t[0], 0x0200, [IntPtr]::Zero, $lp) | Out-Null
            if ($verb -eq 'click' -or $verb -eq 'dblclick') {
                [W.U]::SendMessageW($t[0], 0x0201, [IntPtr]1, $lp) | Out-Null; Start-Sleep -Milliseconds 30
                [W.U]::SendMessageW($t[0], 0x0202, [IntPtr]::Zero, $lp) | Out-Null
            }
            if ($verb -eq 'dblclick') {
                [W.U]::SendMessageW($t[0], 0x0203, [IntPtr]1, $lp) | Out-Null; Start-Sleep -Milliseconds 30
                [W.U]::SendMessageW($t[0], 0x0202, [IntPtr]::Zero, $lp) | Out-Null
            }
        }
        'wheel' {
            # 'wheel x,y,delta': the wheel over that point (-120 one notch down)
            $x, $y, $d = $arg -split ',' | ForEach-Object { [int]$_ }
            $t = Target $x $y
            if (-not $t) { "nada en $arg"; continue }
            [W.U]::SendMessageW($t[0], 0x020A, [IntPtr]([int64]$d * 65536), [IntPtr]::Zero) | Out-Null
        }
        'key' { $p = Player; [W.U]::SendMessageW($p, 0x0100, [IntPtr][int]$arg, [IntPtr]1) | Out-Null; [W.U]::SendMessageW($p, 0x0101, [IntPtr][int]$arg, [IntPtr]0xC0000001) | Out-Null }
        'char' { $p = Player; foreach ($c in $arg.ToCharArray()) { [W.U]::SendMessageW($p, 0x0102, [IntPtr][int]$c, [IntPtr]1) | Out-Null } }
        'wait' { Start-Sleep -Milliseconds ([int]$arg) }
    }
}
if (-not $Salida) { return }
Start-Sleep -Milliseconds 60
$png = "$env:LOCALAPPDATA\Pluma\prueba.$Nombre\foto.png"
Remove-Item $png -ErrorAction SilentlyContinue
[W.U]::SendMessageW($win, 0x8000 + 90, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
Copy-Item $png $Salida -Force
