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

# 6. T5AI platform patch: captive portal DNS. Wi-Fi, the SoftAP DHCP server and its DNS
#    responder run on the CP core (t5_os\cp\..., CONFIG_WIFI_ENABLE / CONFIG_DHCP), not
#    on the AP core that runs the application. DHCP gives the phone the AP address as DNS
#    server, but that responder answers every query with REFUSED, so phones never find
#    the provisioning page. Make it answer A queries with the AP address instead.
$CpDhcpSrv = Join-Path $Sdk 'platform\T5AI\t5_os\cp\components\lwip_intf_v2_1\dhcpd\dhcp-server.c'
if (Test-Path $CpDhcpSrv) {
    $c = [IO.File]::ReadAllText($CpDhcpSrv)
    if (-not $c.Contains('KW002: captive portal DNS')) {
        $pattern = '(?s)\t/\* make the header represent a response \*/.*?\tSEND_RESPONSE\(dhcps\.dnssock, \(struct sockaddr \*\)fromaddr,\s*msg, endp - msg\);'
        if ($c -notmatch $pattern) { throw "DNS responder not found in $CpDhcpSrv, platform layout changed?" }
        $captive = @'
	/* KW002: captive portal DNS - answer A queries with the AP address, anything else
	 * with an empty NOERROR answer (was: REFUSED for everything) */
	{
		char *q = msg + sizeof(struct dns_header);
		int qtype;

		while (q < endp && *q)
			q += (unsigned char)*q + 1; /* QNAME labels */
		q += 1;
		if (q + 4 > endp)
			return -1;
		qtype = ((unsigned char)q[0] << 8) | (unsigned char)q[1];
		q += 4; /* QTYPE, QCLASS */
		msg[2] = 0x81; /* response, recursion desired */
		msg[3] = 0x80; /* recursion available, NOERROR */
		hdr->num_questions = htons(0x01);
		hdr->answer_rrs = 0;
		hdr->authority_rrs = 0;
		hdr->additional_rrs = 0;
		if (qtype == 1 && q + 16 <= msg + SERVER_BUFFER_SIZE) {
			hdr->answer_rrs = htons(0x01);
			*q++ = 0xC0; *q++ = 0x0C;                  /* name: pointer to the question */
			*q++ = 0x00; *q++ = 0x01;                  /* type A */
			*q++ = 0x00; *q++ = 0x01;                  /* class IN */
			*q++ = 0x00; *q++ = 0x00; *q++ = 0x00; *q++ = 0x3C; /* TTL 60 s */
			*q++ = 0x00; *q++ = 0x04;                  /* RDLENGTH */
			write_u32(q, dhcps.my_ip);                 /* network order */
			q += 4;
		}
		dhcp_d("captive dns reply, qtype %d\r\n", qtype);
		SEND_RESPONSE(dhcps.dnssock, (struct sockaddr *)fromaddr, msg, q - msg);
	}
'@
        $c = [regex]::Replace($c, $pattern, { param($m) $captive.TrimEnd("`r", "`n") })
        [IO.File]::WriteAllText($CpDhcpSrv, $c)
        Write-Host "[setup] Patched $CpDhcpSrv (captive portal DNS on the SoftAP)"
    }
}

Write-Host '[setup] Done. TuyaOpen\boards\T5AI\Kconfig is modified locally on purpose (not committed).'
