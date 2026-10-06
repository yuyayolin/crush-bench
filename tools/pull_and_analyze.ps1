# 极光跑分 —— 从真机拉取日志并做自洽性校验
#
# 用法:  powershell -ExecutionPolicy Bypass -File tools\pull_and_analyze.ps1
# 作用:
#   1) 用 hdc 把 App 沙箱里的四份文件拉到本地 D:\gb7logs\
#   2) 解析 runlog.jsonl, 逐项检查 metric x ms/1000 是否等于该负载写死的分子
#      (metric 与 ms 在代码里是同一个 wallMs 算出来的, 所以这条必须严格成立;
#       不成立就说明那一行数据不是同一次运行产生的, 或是计时被截断)
#   3) 打印每项的: 耗时 / 吞吐 / 分数 / 所在核 / 是否在大核簇内 / note 摘要

$ErrorActionPreference = 'Continue'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$hdc  = 'D:\ohos-tools\sdk\default\openharmony\toolchains\hdc.exe'
$base = '/data/app/el2/100/base/com.aurora.bench/haps/entry/files'
$out  = 'D:\gb7logs'
New-Item -ItemType Directory -Force -Path $out | Out-Null

Write-Output '=== 1) 拉取 ==='
foreach ($f in 'runlog.jsonl','runhistory.jsonl','native_crash.txt','native_hang.txt') {
    $r = & $hdc file recv "$base/$f" "$out\$f" 2>&1
    $line = ($r | Select-Object -Last 1)
    if ($line -match 'permission denied') { Write-Output "  $f  -> 权限不足(需 0660 取证修复后的版本)" }
    elseif ($line -match 'No such file') { Write-Output "  $f  -> 不存在" }
    else { Write-Output "  $f  -> 已拉取" }
}

# 每个负载写死的分子(与 C++ 源码里的口径一一对应; 改规模时同步这里)
$num = @{
    'File Compression'      = 24.0      # MB      (3 语料 x 4 MiB, x2 压缩+解压)
    'Navigation'            = 24.0      # 条路线
    'Text Processing'       = 192.0     # 页      (192 页 x 1 MiB)
    'Asset Compression'     = 64.0      # MB      (4096^2 RGBA)
    'Photo Library'         = 7.0       # 张
    'Photo Editor'          = 24.0      # Mpx     (3000x2000 x 4 张)
    'HDR'                   = 11.0592   # Mpx     (2560x1440x3)
    'Ray Tracer'            = 8.2575    # Msample (768x768x14)
    'Game Physics'          = 1.556     # Mbody-steps (8192 x 190, 规模已重标定)
    'PDF Viewer'            = 33.66     # Mpx     (2 页 x 1275x1650 x 8 超采样)
    'HTML5 Browser'         = 56.0      # 页
    'Audio Encoder'         = 0.192     # MB      (1 秒 48kHz 立体声 16bit)
    'Video Encoder'         = 3.6864    # Mpx     (4 帧 x 921600)
    'Video Decoder'         = 0.0       # 随"真正解出的帧数"变化, 不在此表校验
}

Write-Output ''
Write-Output '=== 2) 逐项自洽性校验 (metric x ms/1000 是否等于分子) ==='
if (-not (Test-Path "$out\runlog.jsonl")) { Write-Output '  runlog.jsonl 不存在, 跳过'; exit 0 }

$rows = @()
foreach ($l in (Get-Content "$out\runlog.jsonl" -Encoding UTF8)) {
    try { $o = $l | ConvertFrom-Json } catch { continue }
    if ($o.kind -eq 'item' -and $o.phase -like 'GB7*') { $rows += $o }
}
Write-Output ("  GB7 逐项行数: " + $rows.Count)
Write-Output ''
Write-Output ('  {0,-22} {1,-6} {2,8} {3,8} {4,9} {5,9} {6,7} {7}' -f 'item','phase','ms','metric','product','numerator','dev%','cpu note')
foreach ($r in $rows) {
    $name = [string]$r.item
    $prod = [double]$r.metric * ([double]$r.ms / 1000.0)
    $n = $null; if ($num.ContainsKey($name)) { $n = [double]$num[$name] }
    $dev = ''; $mark = ''
    if ($n -ne $null -and $n -gt 0) {
        $d = ($prod - $n) / $n * 100.0
        $dev = ('{0,8:F1}' -f $d)
        if ([Math]::Abs($d) -lt 3) { $mark = 'OK' } else { $mark = 'MISMATCH' }
    } else { $dev = '     n/a'; $mark = '' }
    $note = [string]$r.note
    $cpuPart = ''
    if ($note -match 'cpu=(-?\d+)') { $cpuPart = 'cpu=' + $matches[1] }
    if ($note -match '不在快簇内') { $cpuPart += ' !!OUTSIDE' }
    elseif ($note -match '在快簇内|大核簇已生效|已绑定') { $cpuPart += ' in-cluster' }
    if ($note -match '未绑定') { $cpuPart += ' UNBOUND' }
    Write-Output ('  {0,-22} {1,-6} {2,8:F0} {3,8} {4,9:F2} {5,9} {6,7} {7} {8}' -f $name, $r.phase, $r.ms, $r.metric, $prod, $(if($n){[string]$n}else{'-'}), $dev, $mark, $cpuPart)
}

Write-Output ''
Write-Output '=== 3) 每轮汇总 ==='
foreach ($l in (Get-Content "$out\runlog.jsonl" -Encoding UTF8)) {
    try { $o = $l | ConvertFrom-Json } catch { continue }
    if ($o.kind -eq 'run_summary') {
        Write-Output ('  {0,-10} 综合={1,7} 计分={2} 失败={3} 跳过={4} 用时={5}s' -f $o.phase, $o.composite, $o.scored, $o.failed, $o.skipped, [Math]::Round([double]$o.durationMs/1000,1))
    }
    if ($o.kind -eq 'crash') { Write-Output ('  [崩溃] 最后启动的项: ' + $o.item) }
}

Write-Output ''
Write-Output '=== 4) 卡死/崩溃证据 ==='
foreach ($f in 'native_hang.txt','native_crash.txt') {
    $p = "$out\$f"
    if ((Test-Path $p) -and (Get-Item $p).Length -gt 0) {
        Write-Output ("  --- $f (" + (Get-Item $p).Length + "B) ---")
        Get-Content $p -TotalCount 40 | ForEach-Object { '    ' + $_.Substring(0, [Math]::Min(180, $_.Length)) }
    } else { Write-Output ("  " + $f + " : 空或不存在") }
}