# RAM of a Pluma instance, as the Task Manager counts it (private working set), by kind of process.
#   tools\medir.ps1 [-Nombre t1] [-Detalle]       a test instance; -Nombre '' measures the user's own Pluma
param([string]$Nombre = 't1', [switch]$Detalle)
$folder = if ($Nombre) { "Pluma\prueba.$Nombre\webview2" } else { 'Pluma\webview2' }
$wv = Get-CimInstance Win32_Process -Filter "Name='msedgewebview2.exe'" | Where-Object { $_.CommandLine -like "*$folder*" -and ($Nombre -or $_.CommandLine -notlike '*prueba.*') }
$hostp = Get-CimInstance Win32_Process -Filter "Name='Pluma.exe'"
$ids = @($wv.ProcessId) + @($hostp.ProcessId)
$perf = Get-CimInstance Win32_PerfFormattedData_PerfProc_Process | Where-Object { $ids -contains [int]$_.IDProcess }
$rows = foreach ($p in $perf) {
    $id = [int]$p.IDProcess
    $w = $wv | Where-Object ProcessId -eq $id
    $k = if (-not $w) { 'Pluma (interfaz)' } elseif ($w.CommandLine -match '--type=([a-z-]+)') { $Matches[1] } else { 'motor (navegador)' }
    if ($w -and $w.CommandLine -match '--utility-sub-type=([a-zA-Z.]+)') { $k = 'utilidad ' + ($Matches[1] -split '\.')[-1] }
    if ($w -and $w.CommandLine -match '--extension-process') { $k = 'extensión' }
    if ($k -eq 'renderer') { $k = 'páginas' }
    if ($k -eq 'gpu-process') { $k = 'gráfica' }
    [pscustomobject]@{ Tipo = $k; MB = [math]::Round($p.WorkingSetPrivate / 1MB) }
}
$total = ($rows | Measure-Object MB -Sum).Sum
"TOTAL: $total MB en $(@($rows).Count) procesos"
$rows | Group-Object Tipo | ForEach-Object { '  {0,-22} {1,5} MB  ({2})' -f $_.Name, ($_.Group | Measure-Object MB -Sum).Sum, $_.Count } | Sort-Object
if ($Detalle) { $rows | Sort-Object MB -Descending | Format-Table -AutoSize }
