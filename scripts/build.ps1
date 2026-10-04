param([switch]$Run, [string]$BuildDirectory = 'build', [string]$DistributionName = 'QuestCompleter')
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskVersion = [regex]::Match((Get-Content -LiteralPath (Join-Path $taskRoot 'CMakeLists.txt') -Raw), 'project\(QuestCompleter VERSION ([0-9.]+)').Groups[1].Value
if (-not $taskVersion) { throw 'Could not read the project version.' }
if ($BuildDirectory -notmatch '^[a-zA-Z0-9_-]+$') { throw 'BuildDirectory must be a folder name within the project.' }
if ($DistributionName -notmatch '^[a-zA-Z0-9_-]+$') { throw 'DistributionName must be a folder name within dist.' }
$taskBuildRoot = Join-Path $taskRoot $BuildDirectory

function Invoke-QuestBuildTool {
    param([string]$Tool, [string[]]$ToolArguments)
    $taskInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $taskInfo.FileName = (Get-Command $Tool -ErrorAction Stop).Source
    $taskInfo.UseShellExecute = $false
    $taskInfo.WorkingDirectory = $taskRoot
    # Normalize duplicate Path/PATH keys inherited from some Windows launchers.
    if ($null -ne $taskInfo.Environment) {
        $taskPathValue = [Environment]::GetEnvironmentVariable('PATH')
        @($taskInfo.Environment.Keys) | Where-Object { $_ -ieq 'PATH' } | ForEach-Object {
            $taskInfo.Environment.Remove($_) | Out-Null
        }
        $taskInfo.Environment['PATH'] = $taskPathValue
    }
    $taskInfo.Arguments = ($ToolArguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $taskProcess = [System.Diagnostics.Process]::Start($taskInfo)
    $taskProcess.WaitForExit()
    if ($taskProcess.ExitCode -ne 0) { throw "$Tool exited with code $($taskProcess.ExitCode)." }
}

Invoke-QuestBuildTool -Tool cmake -ToolArguments @('-S', $taskRoot, '-B', $taskBuildRoot, '-G', 'Visual Studio 17 2022', '-A', 'x64')
Invoke-QuestBuildTool -Tool cmake -ToolArguments @('--build', $taskBuildRoot, '--config', 'Release', '--parallel')
Invoke-QuestBuildTool -Tool ctest -ToolArguments @('--test-dir', $taskBuildRoot, '-C', 'Release', '--output-on-failure')

$taskDistribution = Join-Path (Join-Path $taskRoot 'dist') $DistributionName
New-Item -ItemType Directory -Force -Path (Join-Path $taskDistribution 'licenses') | Out-Null
Copy-Item -LiteralPath (Join-Path $taskBuildRoot 'Release\QuestCompleter.exe') -Destination (Join-Path $taskDistribution 'QuestCompleter.exe') -Force
foreach ($taskLicense in Get-ChildItem -LiteralPath (Join-Path $taskRoot 'assets\licenses') -File) {
    Copy-Item -LiteralPath $taskLicense.FullName -Destination (Join-Path $taskDistribution 'licenses') -Force
}
Copy-Item -LiteralPath (Join-Path $taskRoot 'LICENSE') -Destination (Join-Path $taskDistribution 'LICENSE') -Force
$taskNotices = (Get-Content -LiteralPath (Join-Path $taskRoot 'THIRD_PARTY_NOTICES.md') -Raw).Replace('(assets/licenses/', '(licenses/')
[IO.File]::WriteAllText((Join-Path $taskDistribution 'THIRD_PARTY_NOTICES.md'), $taskNotices, [Text.UTF8Encoding]::new($false))
@'
Quest Completer [VERSION] — Windows 10/11 x64

Extract the archive and run QuestCompleter.exe.
Open Discord, select a game, inspect Info, choose Standard or RPC, and start.
Stop ends the session. Closing the app stops its helpers; minimizing keeps them alive.

Dashboard counts local sessions, not Discord rewards. RPC acknowledgements confirm
activity publication only. Check actual Quest progress and claim rewards in Discord.

FC 27 is experimental. The latest five-minute trial after a full Discord restart
stayed at 0%. Its temporary Steam placement refuses existing installations and
cleans up owned files. Without a usable Steam library, it uses a local helper folder.

Preferences, caches and reports: %LOCALAPPDATA%\QuestCompleter
Source, documentation and issues: https://github.com/osk4rrv/discord-quest-completer

Ctrl+F — search
Tab / Shift+Tab — keyboard navigation
Enter / Space — activate a control
Esc — clear search
Ctrl+A / Ctrl+V — select all / paste in text fields
Arrow keys / Home / End — move the text cursor
Mouse wheel / Page Up / Page Down — scroll games

Original code: MIT (LICENSE). Font and JSON licenses: licenses folder.
'@.Replace('[VERSION]', $taskVersion) | Set-Content -LiteralPath (Join-Path $taskDistribution 'START.txt') -Encoding UTF8
$taskPackageFiles = @(
    (Join-Path $taskDistribution 'QuestCompleter.exe'),
    (Join-Path $taskDistribution 'licenses'),
    (Join-Path $taskDistribution 'START.txt'),
    (Join-Path $taskDistribution 'LICENSE'),
    (Join-Path $taskDistribution 'THIRD_PARTY_NOTICES.md')
)
$taskZip = Join-Path (Join-Path $taskRoot 'dist') "$DistributionName-Windows-x64.zip"
Compress-Archive -LiteralPath $taskPackageFiles -DestinationPath $taskZip -Force
$taskHash = (Get-FileHash -LiteralPath $taskZip -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText("$taskZip.sha256", "$taskHash  $([IO.Path]::GetFileName($taskZip))`n", [Text.UTF8Encoding]::new($false))
Write-Output "Application ready: $taskDistribution\QuestCompleter.exe"
if ($Run) {
    Start-Process -FilePath (Join-Path $taskDistribution 'QuestCompleter.exe') -WindowStyle Hidden
}
