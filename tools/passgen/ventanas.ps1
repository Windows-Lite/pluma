# The visible top-level windows of a test Pluma's engine (its process tree): listed, photographed (PrintWindow) and,
# with -Invoke, a button of theirs pressed through UI Automation (no focus taken).
# -Watch: from then on, in this PowerShell, every window of the engine that shows up on a screen goes off it at once
# (its "save password" prompt and its autofill list are placed by the engine on the nearest screen, not by the test
# window, which is off the screen); -Stop ends that. Shots and UI Automation work off the screen.
param([int]$Root, [string]$Shot = '', [string]$Invoke = '', [switch]$Watch, [switch]$Stop)
Add-Type -ReferencedAssemblies System.Drawing @"
using System; using System.Text; using System.Runtime.InteropServices; using System.Collections.Generic; using System.Drawing; using System.Threading;
public static class W {
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint f);
  static volatile bool run; public static int Moved;
  public static void StartWatch(uint[] p) {
    var set = new HashSet<uint>(p); run = true;
    var t = new Thread(() => {
      while (run) {
        foreach (var h in All()) {
          uint q; GetWindowThreadProcessId(h, out q);
          if (!set.Contains(q) || !IsWindowVisible(h)) continue;
          RECT r; GetWindowRect(h, out r);
          if (r.L > -10000) { SetWindowPos(h, IntPtr.Zero, -20000, -20000, 0, 0, 0x0010 | 0x0004 | 0x0001); Moved++; }   // NOACTIVATE, NOZORDER, NOSIZE
        }
        Thread.Sleep(15);
      }
    });
    t.IsBackground = true; t.Start();
  }
  public static void StopWatch() { run = false; }
  public delegate bool EP(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EP f, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint c);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  public struct RECT { public int L, T, R, B; }
  public static List<IntPtr> All() { var l = new List<IntPtr>(); EnumWindows((h, x) => { l.Add(h); return true; }, IntPtr.Zero); return l; }
  public static void Shot(IntPtr h, string f) { RECT r; GetWindowRect(h, out r); var b = new Bitmap(Math.Max(1, r.R - r.L), Math.Max(1, r.B - r.T)); using (var g = Graphics.FromImage(b)) { var dc = g.GetHdc(); PrintWindow(h, dc, 2); g.ReleaseHdc(dc); } b.Save(f); }
}
"@
if ($Stop) { [W]::StopWatch(); "ventanas del motor apartadas: $([W]::Moved)"; return }
# (no root, no tree: with 0 the tree would take in every process started by a service, other tests' too)
if ($Root -le 0 -or -not (Get-Process -Id $Root -ErrorAction SilentlyContinue)) { "sin raiz: nada"; return }
$procs = Get-CimInstance Win32_Process | Select-Object ProcessId, ParentProcessId, Name
$tree = @{ $Root = $true }; $grew = $true
while ($grew) { $grew = $false; foreach ($p in $procs) { if ($tree.ContainsKey([int]$p.ParentProcessId) -and -not $tree.ContainsKey([int]$p.ProcessId)) { $tree[[int]$p.ProcessId] = $true; $grew = $true } } }
if ($Watch) { $tree.Remove($Root); [W]::StartWatch([uint32[]]@($tree.Keys | ForEach-Object { [uint32]$_ })); return }
$found = @()
foreach ($h in [W]::All()) {
  $pid2 = 0; [void][W]::GetWindowThreadProcessId($h, [ref]$pid2)
  if (-not $tree.ContainsKey([int]$pid2) -or -not [W]::IsWindowVisible($h)) { continue }
  $c = New-Object Text.StringBuilder 256; [void][W]::GetClassName($h, $c, 256)
  $t = New-Object Text.StringBuilder 256; [void][W]::GetWindowText($h, $t, 256)
  $r = New-Object W+RECT; [void][W]::GetWindowRect($h, [ref]$r)
  $o = [W]::GetWindow($h, 4)
  "win $h pid=$pid2 class=$c title='$t' rect=$($r.L),$($r.T),$($r.R),$($r.B) owner=$o"
  if ("$c" -ne 'PlumaVentana.pw' -and "$c" -notmatch 'Pluma') { $found += $h }
}
if ($Shot -and $found.Count) { $i = 0; foreach ($h in $found) { [W]::Shot($h, "$Shot-$i.png"); $i++ } }
if ($Invoke) {
  Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
  foreach ($h in $found) {
    $e = [Windows.Automation.AutomationElement]::FromHandle($h)
    $btns = $e.FindAll([Windows.Automation.TreeScope]::Descendants, (New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::ControlTypeProperty, [Windows.Automation.ControlType]::Button)))
    foreach ($b in $btns) {
      $n = $b.Current.Name; "boton: $n"
      if ($Invoke.Split('|') -contains $n) { ($b.GetCurrentPattern([Windows.Automation.InvokePattern]::Pattern)).Invoke(); "pulsado: $n" }
    }
  }
}
