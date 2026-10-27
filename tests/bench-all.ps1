# The load comparisons of the server, started by hand (DesDoc, section 13).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests\bench-all.ps1
#   ... -Only commit,hddthreads -Work C:\bench      (the same on another disk)
#   ... -DryRun                                     (lists the runs, starts nothing)
#
# Needs build\CalendarServer.exe and build\CalendarServerBench.exe
# (build-server.bat). Everything goes to the work folder, build\bench by
# default: the made-up base (base\), the server of version 1.2.0 built from
# its commit for comparison (old-build\), the table of results (results.md)
# and the progress (bench.log), one file per run in runs\. The disk of the work
# folder is half of what is measured: a run on another disk is another run.
# Every server of the benchmark listens on 127.0.0.1 and has a folder of its
# own: the server of this machine on 8765 is not touched. The processor is busy
# all the time.
#
# Groups: versions (1.2.0 against now), queues (the four ways of dispatch),
# threads (2 to 16 threads), commit (group commit 0..256 with 4 and 16
# threads, and without fsync), hddthreads (2 to 32 threads with group commit),
# resources (one core, 25 % of the processor, big bodies, 256 and 128 MB of
# memory), tune (CalendarServerBench tune twice: is its advice stable?).
#
# The file is UTF-8 with BOM: PowerShell 5.1 reads it as ANSI without one.

param(
    [string[]]$Only = @('versions', 'queues', 'threads', 'commit', 'hddthreads', 'resources', 'tune'),
    [int]$Seconds = 20,
    [int]$Clients = 64,
    # Where the base, the runs and the results go; its disk is measured.
    [string]$Work = '',
    # The commit of version 1.2.0, the last single threaded server.
    [string]$BaselineCommit = '6b28d21',
    [switch]$SkipBaseline,
    # Only lists the runs, starts nothing: a check of the script itself.
    [switch]$DryRun
)

# Native programs report on stderr too, and with 'Stop' PowerShell 5.1 would
# take the first such line for a failure; exit codes are checked instead.
$ErrorActionPreference = 'Continue'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8

# "powershell -File" hands "-Only commit,hddthreads" over as one string, not a
# list: no group matched it, and the stand finished at once having run nothing.
$Groups = @('versions', 'queues', 'threads', 'commit', 'hddthreads', 'resources', 'tune')
$Only = @($Only | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim().ToLower() } | Where-Object { $_ })
$unknown = @($Only | Where-Object { $Groups -notcontains $_ })
if ($unknown.Count -gt 0 -or $Only.Count -eq 0) {
    Write-Host "Неизвестные группы: $($unknown -join ', '). Есть: $($Groups -join ', ')"
    exit 2
}

$Root = Split-Path -Parent $PSScriptRoot
$Out = if ($Work) { $Work } else { Join-Path $Root 'build\bench' }
$Runs = Join-Path $Out 'runs'
$Log = Join-Path $Out 'bench.log'
$Results = Join-Path $Out 'results.md'
$New = Join-Path $Root 'build\CalendarServer.exe'
$Bench = Join-Path $Root 'build\CalendarServerBench.exe'
$Old = Join-Path $Out 'old-build\CalendarServer.exe'

# The same tools build-env.bat points at; set SERVER_QT and the rest to override.
$Qt = if ($env:SERVER_QT) { $env:SERVER_QT } else { 'D:\Qt\6.5.3\mingw_64' }
$QtTools = if ($env:SERVER_QT_TOOLS) { $env:SERVER_QT_TOOLS } else { 'D:\Qt\Tools' }
$Mingw = if ($env:SERVER_MINGW) { $env:SERVER_MINGW } else { Join-Path $QtTools 'mingw1120_64' }
$Ninja = if ($env:SERVER_NINJA) { $env:SERVER_NINJA } else { Join-Path $QtTools 'Ninja' }
$Cmake = if ($env:SERVER_CMAKE) { $env:SERVER_CMAKE } else { Join-Path $QtTools 'CMake_64\bin\cmake.exe' }
$env:PATH = "$Qt\bin;$Mingw\bin;$Ninja;$env:PATH"

New-Item -ItemType Directory -Force -Path $Out, $Runs | Out-Null

function Say([string]$text) {
    $line = '{0:yyyy-MM-dd HH:mm:ss} {1}' -f (Get-Date), $text
    Write-Host $line
    Add-Content -Path $Log -Value $line -Encoding UTF8
}

function Fail([string]$text) {
    Say "ОШИБКА: $text"
    exit 1
}

Say "=== Нагрузочный стенд: группы $($Only -join ', '), по $Seconds с, клиентов $Clients, папка $Out"

foreach ($exe in @($New, $Bench)) {
    if (-not (Test-Path $exe)) { Fail "нет $exe - сначала build-server.bat" }
}
$running = Get-Process CalendarServerBench -ErrorAction SilentlyContinue
if ($running) { Fail "CalendarServerBench уже запущен (процесс $($running.Id -join ', '))" }

# ---- the server of version 1.2.0, for the comparison --------------------------
$needOld = ($Only -contains 'versions') -or ($Only -contains 'resources')
if ($needOld -and -not $SkipBaseline -and -not $DryRun -and -not (Test-Path $Old)) {
    Say "Сборка сервера 1.2.0 из коммита $BaselineCommit"
    $zip = Join-Path $Out 'old-src.zip'
    $src = Join-Path $Out 'old-src'
    if (Test-Path $src) { Remove-Item -Recurse -Force $src }
    & git -C $Root archive --format=zip -o $zip $BaselineCommit
    if ($LASTEXITCODE -ne 0) { Fail "git archive $BaselineCommit" }
    Expand-Archive -Path $zip -DestinationPath $src
    Remove-Item $zip
    & $Cmake -S $src -B (Join-Path $Out 'old-build') -G Ninja -DCMAKE_BUILD_TYPE=Release `
        "-DCMAKE_PREFIX_PATH=$Qt" -DSERVER_BUILD_TESTS=OFF -DSERVER_BUILD_ADMIN=OFF *> (Join-Path $Out 'old-config.txt')
    if ($LASTEXITCODE -ne 0) { Fail "настройка сборки 1.2.0, см. old-config.txt" }
    & $Cmake --build (Join-Path $Out 'old-build') --target CalendarServer *> (Join-Path $Out 'old-build.txt')
    if ($LASTEXITCODE -ne 0) { Fail "сборка 1.2.0, см. old-build.txt" }
}
$haveOld = (-not $SkipBaseline) -and ((Test-Path $Old) -or $DryRun)

# ---- the made-up base: 200 accounts of 500 records ------------------------------
$Base = Join-Path $Out 'base'
if (-not $DryRun) {
    Say 'База: 200 аккаунтов по 500 записей (готовая остаётся)'
    & $Bench prepare --dir $Base --accounts 200 --records 500 | ForEach-Object { Say "$_" }
    if ($LASTEXITCODE -ne 0) { Fail 'prepare' }
}

# ---- the runs ------------------------------------------------------------------
$table = @(
    '| Сервер | Сценарий | Запр./с | p50, мс | p95, мс | p99, мс | Лёгкие p50/p99 | 503 | Ошибки | ЦП | Пик памяти | Разброс потоков | За сброс | Жив |',
    '|---|---|---|---|---|---|---|---|---|---|---|---|---|---|'
)
if (-not $DryRun) { Set-Content -Path $Results -Value $table -Encoding UTF8 }

$script:Number = 0
$started = Get-Date

function Run([string]$label, [string]$server, [string]$scenario, [string[]]$extra = @()) {
    if ($server -eq $Old -and -not $haveOld) { return }
    $script:Number++
    $output = Join-Path $Runs ('{0:00}.txt' -f $script:Number)
    $arguments = @('run', '--server', $server, '--base', $Base, '--work', $Runs, '--scenario', $scenario,
                   '--clients', $Clients, '--seconds', $Seconds, '--label', "`"$label`"",
                   '--out', $Results) + $extra
    Say ("[{0:00}] {1}, {2} {3}" -f $script:Number, $label, $scenario, ($extra -join ' '))
    if ($DryRun) {
        Say "     $Bench $($arguments -join ' ')"
        return
    }
    $process = Start-Process -FilePath $Bench -ArgumentList $arguments -NoNewWindow -PassThru `
        -RedirectStandardOutput $output -RedirectStandardError "$output.err"
    $null = $process.Handle   # without it ExitCode stays empty after the exit
    # A run takes its seconds plus about fifteen; one that hangs is stopped.
    if (-not $process.WaitForExit(($Seconds + 120) * 1000)) {
        Stop-Process -Id $process.Id -Force
        Say "     завис и остановлен"
        Add-Content -Path $Results -Value "| $label | $scenario | завис |" -Encoding UTF8
        return
    }
    Get-Content -Path $output -Encoding UTF8 | ForEach-Object { Say "     $_" }
    if ($process.ExitCode -ne 0) { Say "     код выхода $($process.ExitCode)" }
}

# Not "Group": an alias of Group-Object, and an alias wins over a function.
function Start-Section([string]$name, [string]$title) {
    if ($Only -notcontains $name) { return $false }
    if (-not $DryRun) { Add-Content -Path $Results -Value "**$title**" -Encoding UTF8 }
    Say "--- $title"
    return $true
}

$scenarios = @('pull', 'push', 'mixed', 'skewed')
# The settings of 1.3 for each run of "the new one", spelled out: the defaults may change.
$now = @('--threads', '4', '--dispatch', 'fair', '--group-commit', '64', '--sync-mode', 'full')

if (Start-Section 'versions' 'Версии') {
    foreach ($s in $scenarios) { Run '1.2.0, один поток' $Old $s }
    foreach ($s in $scenarios) { Run 'новый, fair ×4, запись по 64' $New $s $now }
}
if (Start-Section 'queues' 'Очереди, 4 потока') {
    foreach ($d in @('fifo', 'round_robin', 'least_loaded')) {
        foreach ($s in $scenarios) {
            Run "новый, $d ×4" $New $s @('--threads', '4', '--dispatch', $d, '--group-commit', '64')
        }
    }
}
if (Start-Section 'threads' 'Число потоков, fair, запись по 64') {
    foreach ($t in @(2, 8, 16)) {
        foreach ($s in @('pull', 'mixed')) {
            Run "новый, fair ×$t" $New $s @('--threads', "$t", '--dispatch', 'fair', '--group-commit', '64')
        }
    }
}
if (Start-Section 'commit' 'Групповая запись') {
    foreach ($t in @(4, 16)) {
        foreach ($g in @(0, 8, 64, 256)) {
            $name = if ($g -eq 0) { 'без групповой записи' } else { "запись по $g" }
            foreach ($s in @('push', 'mixed')) {
                Run "новый ×$t, $name" $New $s @('--threads', "$t", '--group-commit', "$g")
            }
        }
        Run "новый ×$t, запись по 64 без fsync" $New 'push' @('--threads', "$t", '--group-commit', '64', '--sync-mode', 'normal')
    }
}
if (Start-Section 'hddthreads' 'Число потоков при групповой записи') {
    foreach ($t in @(2, 4, 8, 16, 32)) {
        Run "новый ×$t, запись по 64" $New 'mixed' @('--threads', "$t", '--group-commit', '64')
    }
}
if (Start-Section 'resources' 'Ограниченные ресурсы') {
    Run '1.2.0, одно ядро' $Old 'mixed' @('--affinity', '1')
    Run 'новый, одно ядро' $New 'mixed' ($now + @('--affinity', '1'))
    Run '1.2.0, ЦП 25 %' $Old 'mixed' @('--cpu-rate', '25')
    Run 'новый, ЦП 25 %' $New 'mixed' ($now + @('--cpu-rate', '25'))
    Run '1.2.0, большие тела' $Old 'bulk' @('--max-body-mb', '8')
    Run 'новый, большие тела' $New 'bulk' ($now + @('--max-body-mb', '8'))
    foreach ($mb in @(256, 128)) {
        Run "1.2.0, большие тела, память $mb МБ" $Old 'bulk' @('--max-body-mb', '8', '--memory-mb', "$mb")
        Run "новый, большие тела, память $mb МБ" $New 'bulk' ($now + @('--max-body-mb', '8', '--memory-mb', "$mb"))
    }
}
if (Start-Section 'tune' 'Подбор настроек (дважды)') {
    foreach ($attempt in @(1, 2)) {
        $output = Join-Path $Runs ('tune-{0}.txt' -f $attempt)
        Say "[tune $attempt] CalendarServerBench tune, папка $Out\tune"
        if ($DryRun) { continue }
        $process = Start-Process -FilePath $Bench -NoNewWindow -PassThru `
            -ArgumentList @('tune', '--data-dir', (Join-Path $Out 'tune-data'), '--work', (Join-Path $Out 'tune'), '--force') `
            -RedirectStandardOutput $output -RedirectStandardError "$output.err"
        $null = $process.Handle
        if (-not $process.WaitForExit(30 * 60 * 1000)) {
            Stop-Process -Id $process.Id -Force
            Say "     завис и остановлен"
            continue
        }
        Get-Content -Path $output -Encoding UTF8 | Where-Object { $_ -match '===|запросов/с против|Рекомендация|^\s{4}\S' } |
            ForEach-Object { Say "     $_" }
        Copy-Item (Join-Path $Out 'tune\result.json') (Join-Path $Out ('tune-result-{0}.json' -f $attempt)) -ErrorAction SilentlyContinue
    }
}

Say ("=== Готово за {0:N0} мин. Таблица: {1}" -f ((Get-Date) - $started).TotalMinutes, $Results)
