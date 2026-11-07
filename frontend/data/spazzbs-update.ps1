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

# Windows won't rename a folder that's any program's "current folder", and
# older SPAZZbs builds started this updater from inside bin\64bit. Step out.
try {
	Set-Location -LiteralPath $env:TEMP
	[IO.Directory]::SetCurrentDirectory($env:TEMP)
} catch {}

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

	# Swap SPAZZbs's own folders instead of copying over them: set the old
	# ones aside first (that fails cleanly if anything still has a file
	# open), copy the new build in, and only then delete the old ones. If
	# anything goes wrong the old folders go back, so you're never left
	# with half an update. Plugins you added elsewhere are left alone.
	$Owned = @('bin', 'core', 'data\libobs', 'data\obs-scripting', 'data\obs-studio')
	$Tag = 'spazzbs-old'
	foreach ($Rel in $Owned) {
		$Old = Join-Path $AppDir "$Rel.$Tag"
		if (Test-Path $Old) { Remove-Item $Old -Recurse -Force -ErrorAction SilentlyContinue }
	}

	function Restore-Old([string[]]$Rels) {
		foreach ($Rel in $Rels) {
			$Cur = Join-Path $AppDir $Rel
			if (Test-Path $Cur) { Remove-Item $Cur -Recurse -Force -ErrorAction SilentlyContinue }
			Rename-Item -Path (Join-Path $AppDir "$Rel.$Tag") -NewName (Split-Path $Rel -Leaf) -ErrorAction SilentlyContinue
		}
	}

	Say 'Installing...'
	# old copies of files that were in use during an earlier update
	Get-ChildItem -Path $AppDir -Recurse -Filter '*.spazzbs-old-*' -ErrorAction SilentlyContinue |
		Remove-Item -Force -ErrorAction SilentlyContinue
	# anything left of SPAZZbs's browser helper would hold files open
	Get-Process -Name 'obs-browser-page' -ErrorAction SilentlyContinue |
		Where-Object { $_.Path -and $_.Path.StartsWith($AppDir, [StringComparison]::OrdinalIgnoreCase) } |
		Stop-Process -Force -ErrorAction SilentlyContinue

	# set the old folders aside; antivirus or Windows Search can hold a file
	# for a moment, so try a few times
	$SetAside = @()
	$Swapped = $true
	foreach ($Rel in $Owned) {
		$Cur = Join-Path $AppDir $Rel
		if (-not (Test-Path $Cur)) { continue }
		$Done = $false
		for ($Try = 0; $Try -lt 10 -and -not $Done; $Try++) {
			try {
				Rename-Item -Path $Cur -NewName ((Split-Path $Rel -Leaf) + ".$Tag") -ErrorAction Stop
				$Done = $true
			} catch { Start-Sleep -Milliseconds 700 }
		}
		if (-not $Done) { $Swapped = $false; break }
		$SetAside += $Rel
	}

	if (-not $Swapped) {
		# a folder is still busy (an Explorer window open in it, say): put
		# everything back and copy over the files instead, like before
		Restore-Old $SetAside
		$SetAside = @()
		Say 'A SPAZZbs folder is in use (an Explorer window, or an app using the virtual camera?); copying over it instead...' Yellow
	}

	& robocopy.exe $Stage $AppDir /E /R:2 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Null
	if ($LASTEXITCODE -ge 8) {
		if ($Swapped) {
			$Code = $LASTEXITCODE
			Restore-Old $SetAside
			throw "Copying the new files failed (robocopy code $Code)."
		}
		# Some files are open in another program. A file that's in use can
		# still be renamed, so move each of those aside and put the new one
		# in its place; the old copies are cleared up next time.
		Say 'Some files are in use; replacing them one by one...' Yellow
		$Busy = @()
		$StageFull = (Resolve-Path $Stage).Path.TrimEnd('\')
		foreach ($File in Get-ChildItem -Path $Stage -Recurse -File) {
			$Rel = $File.FullName.Substring($StageFull.Length + 1)
			$Dest = Join-Path $AppDir $Rel
			New-Item -ItemType Directory -Force -Path (Split-Path $Dest) | Out-Null
			try {
				Copy-Item -LiteralPath $File.FullName -Destination $Dest -Force -ErrorAction Stop
			} catch {
				try {
					Rename-Item -LiteralPath $Dest -NewName ((Split-Path $Dest -Leaf) + '.spazzbs-old-' +
						[Guid]::NewGuid().ToString('N').Substring(0, 6)) -ErrorAction Stop
					Copy-Item -LiteralPath $File.FullName -Destination $Dest -Force -ErrorAction Stop
				} catch {
					$Busy += $Rel
				}
			}
		}
		if ($Busy.Count -gt 0) {
			throw ("These files couldn't be replaced because another program has them open:`n    " +
				($Busy -join "`n    ") + "`n  Close that program (or restart Windows) and run the update again.")
		}
	}
	foreach ($Rel in $SetAside) {
		Remove-Item (Join-Path $AppDir "$Rel.$Tag") -Recurse -Force -ErrorAction SilentlyContinue
	}

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
