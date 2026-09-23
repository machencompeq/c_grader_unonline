<#
.SYNOPSIS
  用本機的 AI 命令列工具 (Claude Code / Codex / Gemini) 依照參考答案產生測資。

.DESCRIPTION
  1. 自動找本機有哪一個工具：claude -> codex -> gemini (也可以用 -Tool 指定)
  2. 把參考答案原始碼與要求交給 AI
  3. AI 回傳「測資總表」格式，存成文字檔 (批改工具的「編輯測資」可以直接貼上 / 匯入)

  AI 只負責設計「輸入」；標準答案一律由參考答案實際執行產生，所以不會因為 AI 算錯答案而誤判學生。

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File ai_testcases.ps1 -Reference D:\hw\hw1.c -Count 8
  powershell -ExecutionPolicy Bypass -File ai_testcases.ps1 -Reference D:\hw\hw1.c -Tool gemini -Note "姓名要有英文也要有中文"
#>
param(
    [Parameter(Mandatory = $true)][string]$Reference,
    [int]$Count = 8,
    [ValidateSet('auto', 'claude', 'codex', 'gemini')][string]$Tool = 'auto',
    [switch]$Pause,
    [string]$Note = '',
    [string]$Out = ''
)

$ErrorActionPreference = 'Stop'

# 從批改工具開啟時 (-Pause)，結束前停一下讓老師看得到訊息
function Finish([int]$code) {
    if ($Pause) { Write-Host ''; Read-Host '按 Enter 關閉這個視窗' | Out-Null }
    exit $code
}

# 各工具的「非互動模式」指令：提示詞從標準輸入送進去，結果從標準輸出拿回來。
# 如果你的版本參數不同，改這裡即可。
$Commands = [ordered]@{
    claude = 'claude -p --output-format text'
    codex  = 'codex exec --skip-git-repo-check -'
    gemini = 'gemini -p "請完成標準輸入中的任務"'
}

function Write-Utf8File([string]$Path, [string]$Text) {
    [IO.File]::WriteAllText($Path, $Text, (New-Object Text.UTF8Encoding($false)))
}

function Read-SourceText([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    try {
        return (New-Object Text.UTF8Encoding($false, $true)).GetString($bytes)   # 嚴格 UTF-8
    } catch {
        return [Text.Encoding]::GetEncoding(950).GetString($bytes)               # 否則當 Big5
    }
}

# ---------- 1. 找工具 ----------
$chosen = $null
if ($Tool -eq 'auto') {
    foreach ($name in $Commands.Keys) {
        if (Get-Command $name -ErrorAction SilentlyContinue) { $chosen = $name; break }
    }
    if (-not $chosen) {
        Write-Host '找不到 claude / codex / gemini 任何一個命令列工具。' -ForegroundColor Red
        Write-Host '請先安裝其中一個 (例如 npm install -g @anthropic-ai/claude-code) 並登入。'
        Finish 2
    }
} elseif (Get-Command $Tool -ErrorAction SilentlyContinue) {
    $chosen = $Tool
} else {
    Write-Host "找不到 $Tool 命令列工具。" -ForegroundColor Red
    Finish 2
}

$Reference = (Resolve-Path -LiteralPath $Reference).Path
if (-not $Out) {
    $stem = [IO.Path]::Combine([IO.Path]::GetDirectoryName($Reference), [IO.Path]::GetFileNameWithoutExtension($Reference))
    $dataDir = "$($stem)_測資"
    New-Item -ItemType Directory -Force -Path $dataDir | Out-Null
    $Out = Join-Path $dataDir 'AI測資.txt'
}

# ---------- 2. 提示詞 ----------
$source = Read-SourceText $Reference
$fileName = [IO.Path]::GetFileName($Reference)
$extra = if ($Note) { "老師的額外要求：$Note`n" } else { '' }

$prompt = @"
你是 C 語言課程的助教，要為下面這份「參考答案」程式設計測試資料 (stdin 的輸入內容)。
請產生 $Count 組測資，涵蓋：一般情況、邊界情況 (依題目而定，例如最小值、最大值、0、負數、英文與中文混合)、容易寫錯的情況。

規則：
1. 只設計「輸入」，不要寫輸出 (輸出會由參考答案實際執行產生)。
2. 輸入必須符合這支程式預期的格式與合理範圍；不要故意使用會讓一般正確程式緩衝區溢位的超長字串或不合理資料。
3. 大約三分之一比較刁鑽的測資標為「隱藏」，其餘標為「可見」。
4. 每組附一句簡短的中文說明。
5. 嚴格只輸出以下格式，不要輸出任何其他文字，也不要使用 Markdown 程式碼區塊：
===== test01 | 可見 | 一句話說明 =====
第 1 組輸入的內容 (可以多行)
===== test02 | 隱藏 | 一句話說明 =====
第 2 組輸入的內容
$extra
參考答案 ($fileName)：
$source
"@

# ---------- 3. 呼叫 AI ----------
$work = Join-Path ([IO.Path]::GetTempPath()) ("c-grader-ai-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
$promptFile = Join-Path $work 'prompt.txt'
$rawFile = Join-Path $work 'answer.txt'
$errFile = Join-Path $work 'error.txt'
Write-Utf8File $promptFile $prompt

# 如果是在 Claude Code 裡面開的終端機，巢狀呼叫會被擋，先拿掉這個環境變數
Remove-Item Env:CLAUDECODE -ErrorAction SilentlyContinue

Write-Host "使用 $chosen 產生 $Count 組測資 (約需 30 秒到數分鐘)…" -ForegroundColor Cyan
# 用 cmd 做重新導向：位元組原封不動，不會被 PowerShell 5 的編碼轉換弄壞中文
$cmdLine = "$($Commands[$chosen]) < `"$promptFile`" > `"$rawFile`" 2> `"$errFile`""
$proc = Start-Process -FilePath 'cmd.exe' -ArgumentList '/d', '/s', '/c', "`"$cmdLine`"" -NoNewWindow -Wait -PassThru

$raw = if (Test-Path $rawFile) { [IO.File]::ReadAllText($rawFile, [Text.Encoding]::UTF8) } else { '' }
if ($proc.ExitCode -ne 0 -or -not $raw.Trim()) {
    Write-Host "$chosen 執行失敗 (結束碼 $($proc.ExitCode))：" -ForegroundColor Red
    if (Test-Path $errFile) { Get-Content $errFile -Encoding UTF8 | Select-Object -First 20 | Write-Host }
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
    Finish 1
}

# ---------- 4. 整理成測資總表 ----------
$lines = $raw -split "`r?`n" | Where-Object { $_ -notmatch '^\s*```' }   # 去掉 Markdown 程式碼區塊
$start = -1
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match '^===== .* =====\s*$') { $start = $i; break }
}
if ($start -lt 0) {
    Write-Host 'AI 的回答不是測資總表格式，原始回答如下：' -ForegroundColor Red
    Write-Host $raw
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
    Finish 1
}
$result = ($lines[$start..($lines.Count - 1)] -join "`n").TrimEnd() + "`n"
Write-Utf8File $Out $result
Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue

$n = ($lines | Where-Object { $_ -match '^===== .* =====\s*$' }).Count
Write-Host "完成：$n 組測資已存到 $Out" -ForegroundColor Green
Write-Host '請在批改工具的「編輯測資」檢查內容；標準答案會用參考答案自動產生。'
Finish 0
