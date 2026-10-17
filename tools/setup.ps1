<#
    Hooks this repo's boards into the TuyaOpen submodule without committing to it.

    Usage (from anywhere):  powershell -ExecutionPolicy Bypass -File tools\setup.ps1

    For every folder in boards\ this script:
      * creates a directory junction TuyaOpen\boards\T5AI\<BOARD> -> boards\<BOARD>,
      * registers the board in TuyaOpen\boards\T5AI\Kconfig (idempotent),
      * copies boards\<BOARD>\<BOARD>.config to TuyaOpen\boards\T5AI\config\ so
        `tos.py config choice -c <BOARD>.config` can find it,
      * adds the generated paths to the submodule's info/exclude.
    Re-run it after cloning, after `git submodule update`, or after adding a board.
#>

$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $PSScriptRoot
$Sdk  = Join-Path $Root 'TuyaOpen'

if (-not (Test-Path (Join-Path $Sdk 'tos.py'))) {
    Write-Host '[setup] Initializing TuyaOpen submodule...'
    git -C $Root submodule update --init --depth 1 TuyaOpen
    if ($LASTEXITCODE -ne 0) { throw 'git submodule update failed' }
}

$PlatformDir = Join-Path $Sdk 'boards\T5AI'
$Kconfig     = Join-Path $PlatformDir 'Kconfig'
$Anchor      = '    default BOARD_CHOICE_TUYA_T5AI_BOARD'

$ExcludeRel  = (git -C $Sdk rev-parse --git-path info/exclude).Trim()
$ExcludeFile = if ([IO.Path]::IsPathRooted($ExcludeRel)) { $ExcludeRel } else { Join-Path $Sdk $ExcludeRel }

foreach ($board in Get-ChildItem (Join-Path $Root 'boards') -Directory) {
    $name = $board.Name
    $dst  = Join-Path $PlatformDir $name

    # 1. junction
    if (Test-Path $dst) {
        $item = Get-Item $dst -Force
        if ($item.LinkType -ne 'Junction') { throw "$dst exists and is not a junction, refusing to touch it" }
    } else {
        New-Item -ItemType Junction -Path $dst -Target $board.FullName | Out-Null
        Write-Host "[setup] Linked $dst -> $($board.FullName)"
    }

    # 2. Kconfig board choice
    $text = [IO.File]::ReadAllText($Kconfig)
    if ($text -notmatch "BOARD_CHOICE_$name\b") {
        if (-not $text.Contains($Anchor)) { throw "Anchor line not found in $Kconfig, TuyaOpen layout changed?" }
        $nl    = if ($text.Contains("`r`n")) { "`r`n" } else { "`n" }
        $block = "$nl$nl    config BOARD_CHOICE_$name$nl        bool `"$name`"$nl        if (BOARD_CHOICE_$name)$nl            rsource `"./$name/Kconfig`"$nl        endif"
        $text  = $text.Replace($Anchor, $Anchor + $block)
        [IO.File]::WriteAllText($Kconfig, $text)
        Write-Host "[setup] Registered BOARD_CHOICE_$name in $Kconfig"
    }

    # 3. board config for `tos.py config choice`
    $cfg = Join-Path $board.FullName "$name.config"
    if (Test-Path $cfg) {
        Copy-Item $cfg (Join-Path $PlatformDir "config\$name.config") -Force
    }

    # 4. keep the submodule status readable
    $patterns = @("boards/T5AI/$name", "boards/T5AI/config/$name.config")
    $existing = if (Test-Path $ExcludeFile) { Get-Content $ExcludeFile } else { @() }
    foreach ($p in $patterns) {
        if ($existing -notcontains $p) { Add-Content -Path $ExcludeFile -Value $p }
    }
}

Write-Host '[setup] Done. TuyaOpen\boards\T5AI\Kconfig is modified locally on purpose (not committed).'
