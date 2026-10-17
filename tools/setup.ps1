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

# 5. T5AI platform patch: when a board moves the log to another UART
#    (BOARD_LOG_UART_PORT), move the crash dump there too, otherwise exceptions
#    are printed on UART1 and a single-cable setup never sees them.
#    The platform repo is downloaded by the first `tos.py build`; re-run this
#    script after that.
$PlatformScript = Join-Path $Sdk 'platform\T5AI\build_example.py'
if (Test-Path $PlatformScript) {
    $py = [IO.File]::ReadAllText($PlatformScript)
    $orig = '            f.write(f"CONFIG_UART_PRINT_PORT={port}\n")'
    $dump = '            f.write(f"CONFIG_DUMP_UART_PRINT_PORT={port}\n")'
    if (-not $py.Contains($dump)) {
        if (-not $py.Contains($orig)) { throw "Log-port line not found in $PlatformScript, platform layout changed?" }
        $nl = if ($py.Contains("`r`n")) { "`r`n" } else { "`n" }
        [IO.File]::WriteAllText($PlatformScript, $py.Replace($orig, $orig + $nl + $dump))
        # force the generated sdkconfig to be rebuilt with the new fragment
        Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $Sdk 'platform\T5AI\t5_os\build\.tuya_logport_intent')
        Write-Host "[setup] Patched $PlatformScript (crash dump follows BOARD_LOG_UART_PORT)"
    }
} else {
    Write-Host '[setup] T5AI platform not downloaded yet: run `tos.py build` once, then re-run this script.'
}

# 6. T5AI platform patch: the SoftAP DHCP server starts a DNS responder that answers
#    every query with REFUSED on the AP interface. Phones then never see the captive
#    portal; the application runs its own captive DNS (apps/xiaozhi_toy xz_dns.c).
$DhcpMain = Join-Path $Sdk 'platform\T5AI\t5_os\ap\components\lwip_intf_v2_1\dhcpd\dhcp-server-main.c'
if (Test-Path $DhcpMain) {
    $c = [IO.File]::ReadAllText($DhcpMain)
    $orig = "`tdhcp_enable_nack_dns_server();"
    $patched = "`t/* KW002: captive DNS is served by the application */ /* dhcp_enable_nack_dns_server(); */"
    if (-not $c.Contains($patched)) {
        if (-not $c.Contains($orig)) { throw "nack DNS call not found in $DhcpMain, platform layout changed?" }
        [IO.File]::WriteAllText($DhcpMain, $c.Replace($orig, $patched))
        Write-Host "[setup] Patched $DhcpMain (no REFUSED DNS responder on the SoftAP)"
    }
}

Write-Host '[setup] Done. TuyaOpen\boards\T5AI\Kconfig is modified locally on purpose (not committed).'
