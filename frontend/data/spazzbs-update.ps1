# SPAZZbs updater
#
# Downloads the newest SPAZZbs build from the project's GitHub releases and
# puts it into -AppDir (the folder that holds bin\, data\ and core\), then
# starts it. Your scenes and settings live in %APPDATA%\spazzbs and are
# never touched.
#
#   -AppDir    where SPAZZbs lives (created if it doesn't exist yet)
#   -WaitPid   SPAZZbs process to wait for before replacing its files
#   -Shortcut  also put a SPAZZbs shortcut on the desktop
#   -NoLaunch  don't start SPAZZbs afterwards

param(
	[Parameter(Mandatory = $true)][string]$AppDir,
	[int]$WaitPid = 0,
	[switch]$Shortcut,
	[switch]$NoLaunch
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$Repo = 'PeachTempura/SPAZZbs'
$AssetName = 'SPAZZbs-portable-windows-x64.zip'

function Say([string]$Text, [string]$Color = 'Gray') { Write-Host $Text -ForegroundColor $Color }

try { $Host.UI.RawUI.WindowTitle = 'Updating SPAZZbs' } catch {}

try {
	$AppDir = [IO.Path]::GetFullPath($AppDir.Trim('"').TrimEnd('\', '.'))
	$Exe = Join-Path $AppDir 'bin\64bit\SPAZZbs.exe'

	Say ''
	Say '  SPAZZbs updater' Magenta
	Say "  $AppDir"
	Say ''

	New-Item -ItemType Directory -Force -Path $AppDir | Out-Null

	# Program Files and similar need admin rights: ask once and run again
	$Probe = Join-Path $AppDir '.spazzbs-write-test'
	try {
		[IO.File]::WriteAllText($Probe, 'x')
		Remove-Item $Probe -Force
	} catch {
		Say 'This folder needs administrator permission, asking Windows...' Yellow
		$Again = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"",
			'-AppDir', "`"$AppDir`"", '-WaitPid', "$WaitPid")
		if ($Shortcut) { $Again += '-Shortcut' }
		if ($NoLaunch) { $Again += '-NoLaunch' }
		Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList $Again
		exit
	}

	$Work = Join-Path $env:TEMP 'spazzbs-update'
	Remove-Item $Work -Recurse -Force -ErrorAction SilentlyContinue
	New-Item -ItemType Directory -Force -Path $Work | Out-Null
	$Zip = Join-Path $Work 'SPAZZbs.zip'
	$Stage = Join-Path $Work 'files'

	Say 'Looking for the newest build...'
	[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
	$Release = Invoke-RestMethod -UseBasicParsing -Headers @{ 'User-Agent' = 'SPAZZbs-updater' } `
		-Uri "https://api.github.com/repos/$Repo/releases/tags/latest"
	$Asset = $Release.assets | Where-Object { $_.name -eq $AssetName } | Select-Object -First 1
	if (-not $Asset) { throw 'The latest release has no portable build yet. Try again in a few minutes.' }

	Say ('Downloading SPAZZbs ({0:N0} MB)...' -f ($Asset.size / 1MB))
	& curl.exe -L --fail --retry 3 --progress-bar -o $Zip $Asset.browser_download_url
	if ($LASTEXITCODE -ne 0) {
		Say 'curl had trouble, trying again another way...' Yellow
		Invoke-WebRequest -UseBasicParsing -Uri $Asset.browser_download_url -OutFile $Zip
	}
	if ((Get-Item $Zip).Length -ne $Asset.size) { throw 'The download was incomplete. Check your connection and try again.' }

	Say 'Unpacking...'
	New-Item -ItemType Directory -Force -Path $Stage | Out-Null
	& tar.exe -xf $Zip -C $Stage
	if ($LASTEXITCODE -ne 0 -or -not (Test-Path (Join-Path $Stage 'bin\64bit\SPAZZbs.exe'))) {
		Remove-Item $Stage -Recurse -Force -ErrorAction SilentlyContinue
		Expand-Archive -Path $Zip -DestinationPath $Stage -Force
	}
	if (-not (Test-Path (Join-Path $Stage 'bin\64bit\SPAZZbs.exe'))) { throw "The download doesn't look like SPAZZbs." }

	# the running copy has to close before its files can be replaced
	if ($WaitPid -gt 0) {
		Say 'Waiting for SPAZZbs to close...'
		Wait-Process -Id $WaitPid -Timeout 120 -ErrorAction SilentlyContinue
	}
	$Warned = $false
	while (Get-Process -Name 'SPAZZbs' -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $Exe }) {
		if (-not $Warned) { Say 'Close SPAZZbs to finish the update (waiting)...' Yellow; $Warned = $true }
		Start-Sleep -Seconds 2
	}

	Say 'Installing...'
	& robocopy.exe $Stage $AppDir /E /R:5 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Null
	if ($LASTEXITCODE -ge 8) { throw "Copying the new files failed (robocopy code $LASTEXITCODE). Is SPAZZbs still open?" }

	# leftovers from older builds that shipped debug symbols
	Get-ChildItem -Path $AppDir -Recurse -Filter '*.pdb' -ErrorAction SilentlyContinue |
		Remove-Item -Force -ErrorAction SilentlyContinue

	if ($Shortcut) {
		$Shell = New-Object -ComObject WScript.Shell
		$Link = $Shell.CreateShortcut((Join-Path ([Environment]::GetFolderPath('Desktop')) 'SPAZZbs.lnk'))
		$Link.TargetPath = $Exe
		$Link.WorkingDirectory = Split-Path $Exe
		$Link.IconLocation = "$Exe,0"
		$Link.Description = 'SPAZZbs'
		$Link.Save()
		Say 'Desktop shortcut updated.'
	}

	Remove-Item $Work -Recurse -Force -ErrorAction SilentlyContinue

	Say ''
	Say '  SPAZZbs is up to date.' Green
	if (-not $NoLaunch) {
		Start-Process -FilePath $Exe -WorkingDirectory (Split-Path $Exe)
	}
	Start-Sleep -Seconds 3
} catch {
	Say ''
	Say "  Update failed: $($_.Exception.Message)" Red
	Say '  Your current SPAZZbs was left as it was.'
	Read-Host '  Press Enter to close'
	exit 1
}
