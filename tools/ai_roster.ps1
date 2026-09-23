<#
.SYNOPSIS
  把教學平台上的中文姓名，自動配對到下載後的英文資料夾名稱，寫進 名單.csv。

.DESCRIPTION
  教學平台下載作業時，會把學生的顯示名稱轉成拼音當資料夾名稱：
      張敦品            -> zhang_dun_pin
      YiZhi Hu (胡議支) -> yizhi_hu_hu_yi_zhi
      d11230735錢宜沛    -> d11230735qian_yi_pei
  1. 老師把網站上的名單整段複製 (可以含序號、繳交時間)，存成文字檔 (-List)
  2. 英文名稱直接比對 (Lucas -> lucas)
  3. 剩下的中文姓名交給本機 AI (claude / codex / gemini) 依拼音配對
  4. 結果寫進 名單.csv (原本已填好的姓名不會被覆蓋)，老師用 Excel 打開檢查即可

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File ai_roster.ps1 -Students D:\下載\作業1 -List 網站名單.txt -Roster ..\名單.csv
#>
param(
    [Parameter(Mandatory = $true)][string]$Students,
    [Parameter(Mandatory = $true)][string]$List,
    [Parameter(Mandatory = $true)][string]$Roster,
    [ValidateSet('auto', 'claude', 'codex', 'gemini')][string]$Tool = 'auto',
    [switch]$Pause
)

$ErrorActionPreference = 'Stop'

# 從批改工具開啟時 (-Pause)，結束前停一下讓老師看得到訊息
function Finish([int]$code) {
    if ($Pause) { Write-Host ''; Read-Host '按 Enter 關閉這個視窗' | Out-Null }
    exit $code
}
$Commands = [ordered]@{
    claude = 'claude -p --output-format text'
    codex  = 'codex exec --skip-git-repo-check -'
    gemini = 'gemini -p "請完成標準輸入中的任務"'
}
$Utf8NoBom = New-Object Text.UTF8Encoding($false)
$Utf8Bom = New-Object Text.UTF8Encoding($true)

function Get-Slug([string]$s) {
    # 英文與數字轉小寫，其他字元都變成底線 (與教學平台的資料夾名稱規則相同)
    return (($s.ToLower() -replace '[^a-z0-9]+', '_').Trim('_'))
}

function Split-CsvLine([string]$line) {
    $fields = @(); $cur = ''; $quoted = $false
    for ($i = 0; $i -lt $line.Length; $i++) {
        $c = $line[$i]
        if ($quoted) {
            if ($c -eq '"' -and $i + 1 -lt $line.Length -and $line[$i + 1] -eq '"') { $cur += '"'; $i++ }
            elseif ($c -eq '"') { $quoted = $false }
            else { $cur += $c }
        } elseif ($c -eq '"') { $quoted = $true }
        elseif ($c -eq ',') { $fields += $cur; $cur = '' }
        else { $cur += $c }
    }
    $fields += $cur
    return $fields
}

function Format-CsvField([string]$s) {
    if ($s -match '[,"\r\n]') { return '"' + ($s -replace '"', '""') + '"' }
    return $s
}

# ---------- 1. 學生資料夾 ----------
$ignored = @('reference', 'testcase', 'temp', 'reports', '__MACOSX')
$folders = @(Get-ChildItem -LiteralPath $Students -Directory | Where-Object { $_.Name[0] -ne '.' -and $ignored -notcontains $_.Name } | ForEach-Object { $_.Name })
$folders += @(Get-ChildItem -LiteralPath $Students -File -Filter *.c | ForEach-Object { $_.Name })
if ($folders.Count -eq 0) { Write-Host '學生資料夾裡沒有任何學生。' -ForegroundColor Red; Finish 1 }

# ---------- 2. 網站名單：每行一位，去掉序號與繳交時間 ----------
$names = @()
foreach ($line in [IO.File]::ReadAllLines($List, [Text.Encoding]::UTF8)) {
    $n = $line -replace '^\s*\d{1,4}[\s\.、\)]+', ''                     # 開頭的序號
    $n = $n -replace '\s+\d{4}[/-]\d{1,2}[/-]\d{1,2}.*$', ''             # 後面的繳交時間
    $n = $n.Trim()
    if ($n -and $names -notcontains $n) { $names += $n }
}
if ($names.Count -eq 0) { Write-Host '貼上的名單是空的。' -ForegroundColor Red; Finish 1 }

# ---------- 3. 讀取現有名單 ----------
$existing = [ordered]@{}   # folder -> @(name, sid)
if (Test-Path -LiteralPath $Roster) {
    $first = $true
    foreach ($line in [IO.File]::ReadAllLines($Roster, [Text.Encoding]::UTF8)) {
        if (-not $line.Trim()) { continue }
        $f = Split-CsvLine $line
        if ($first -and $f[0] -eq '資料夾名稱') { $first = $false; continue }
        $first = $false
        $existing[$f[0]] = @(($(if ($f.Count -gt 1) { $f[1] } else { '' })), ($(if ($f.Count -gt 2) { $f[2] } else { '' })))
    }
}

# ---------- 4. 英文名稱直接比對 ----------
$match = @{}          # folder -> 顯示名稱
$usedNames = @{}
foreach ($name in $names) {
    $slug = Get-Slug $name
    foreach ($folder in $folders) {
        $fslug = Get-Slug ([IO.Path]::GetFileNameWithoutExtension($folder))
        if ($slug -and $slug -eq $fslug -and -not $match.ContainsKey($folder)) {
            $match[$folder] = $name; $usedNames[$name] = $true; break
        }
    }
}
Write-Host "英文名稱直接配對：$($match.Count) 位"

# ---------- 5. 其餘交給 AI 依拼音配對 ----------
$restFolders = @($folders | Where-Object { -not $match.ContainsKey($_) })
$restNames = @($names | Where-Object { -not $usedNames.ContainsKey($_) })
if ($restFolders.Count -gt 0 -and $restNames.Count -gt 0) {
    $chosen = $null
    if ($Tool -eq 'auto') {
        foreach ($t in $Commands.Keys) { if (Get-Command $t -ErrorAction SilentlyContinue) { $chosen = $t; break } }
    } elseif (Get-Command $Tool -ErrorAction SilentlyContinue) { $chosen = $Tool }

    if (-not $chosen) {
        Write-Host "找不到可用的 AI 工具 ($Tool)，中文姓名需要手動填寫。" -ForegroundColor Yellow
    } else {
        $prompt = @"
教學平台下載學生作業時，會把「學生在網站上的顯示名稱」轉成漢語拼音 (小寫、用底線分隔、中文字逐字轉拼音、英文與數字保留) 當作資料夾名稱。
例如：張敦品 -> zhang_dun_pin；YiZhi Hu (胡議支) -> yizhi_hu_hu_yi_zhi；d11230735錢宜沛 -> d11230735qian_yi_pei。
請把下列每個「資料夾名稱」配對到「網站名稱」中的一個。拼音可以對應到多個同音字時，選名單裡實際存在的那一個。
沒有把握的就不要輸出那一行。只輸出 CSV，每行「資料夾名稱,網站名稱」，不要輸出其他任何文字或 Markdown。

資料夾名稱：
$($restFolders -join "`n")

網站名稱：
$($restNames -join "`n")
"@
        $work = Join-Path ([IO.Path]::GetTempPath()) ("c-grader-roster-" + [Guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $work | Out-Null
        $promptFile = Join-Path $work 'prompt.txt'; $rawFile = Join-Path $work 'answer.txt'; $errFile = Join-Path $work 'error.txt'
        [IO.File]::WriteAllText($promptFile, $prompt, $Utf8NoBom)
        Remove-Item Env:CLAUDECODE -ErrorAction SilentlyContinue

        Write-Host "使用 $chosen 依拼音配對其餘 $($restFolders.Count) 位…" -ForegroundColor Cyan
        $cmdLine = "$($Commands[$chosen]) < `"$promptFile`" > `"$rawFile`" 2> `"$errFile`""
        $proc = Start-Process -FilePath 'cmd.exe' -ArgumentList '/d', '/s', '/c', "`"$cmdLine`"" -NoNewWindow -Wait -PassThru
        $raw = if (Test-Path $rawFile) { [IO.File]::ReadAllText($rawFile, [Text.Encoding]::UTF8) } else { '' }
        if ($proc.ExitCode -ne 0 -or -not $raw.Trim()) {
            Write-Host "$chosen 執行失敗，中文姓名需要手動填寫。" -ForegroundColor Yellow
            if (Test-Path $errFile) { Get-Content $errFile -Encoding UTF8 | Select-Object -First 10 | Write-Host }
        } else {
            $ai = 0
            foreach ($line in ($raw -split "`r?`n")) {
                $f = Split-CsvLine $line.Trim()
                if ($f.Count -lt 2) { continue }
                $folder = $f[0].Trim(); $name = $f[1].Trim()
                # 只接受真的存在的資料夾與名單上真的有的名字 (AI 亂編的一律不用)
                if ($restFolders -contains $folder -and $restNames -contains $name -and -not $match.ContainsKey($folder) -and -not $usedNames.ContainsKey($name)) {
                    $match[$folder] = $name; $usedNames[$name] = $true; $ai++
                }
            }
            Write-Host "AI 依拼音配對：$ai 位"
        }
        Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# ---------- 6. 寫回 名單.csv (已填好的姓名不覆蓋) ----------
$sb = New-Object Text.StringBuilder
[void]$sb.Append("資料夾名稱,姓名,學號`r`n")
$allFolders = @($folders) + @($existing.Keys | Where-Object { $folders -notcontains $_ })
foreach ($folder in $allFolders) {
    $name = ''; $sid = ''
    if ($existing.Contains($folder)) { $name = $existing[$folder][0]; $sid = $existing[$folder][1] }
    if (-not $name -and $match.ContainsKey($folder)) { $name = $match[$folder] }
    if (-not $sid -and $folder -match '^([A-Za-z]?\d{6,})') { $sid = $Matches[1] }
    [void]$sb.Append("$(Format-CsvField $folder),$(Format-CsvField $name),$(Format-CsvField $sid)`r`n")
}
[IO.File]::WriteAllText($Roster, $sb.ToString(), $Utf8Bom)

# ---------- 7. 報告 ----------
$unmatchedFolders = @($folders | Where-Object { -not $match.ContainsKey($_) -and -not ($existing.Contains($_) -and $existing[$_][0]) })
$unusedNames = @($names | Where-Object { -not $usedNames.ContainsKey($_) })
Write-Host ''
Write-Host "已更新 $Roster" -ForegroundColor Green
if ($unmatchedFolders.Count -gt 0) { Write-Host "還沒有姓名的資料夾 ($($unmatchedFolders.Count))：$($unmatchedFolders -join '、')" -ForegroundColor Yellow }
if ($unusedNames.Count -gt 0) { Write-Host "名單上沒配到資料夾的人 ($($unusedNames.Count)，可能沒交作業)：$($unusedNames -join '、')" -ForegroundColor Yellow }
Write-Host '請用 Excel 打開 名單.csv 快速檢查一次。'
Finish 0
