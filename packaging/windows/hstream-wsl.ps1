[CmdletBinding()]
param(
    [ValidateSet("Install", "Launch", "Unregister", "EnsureWslMachine")]
    [string]$Action = "Install",
    [string]$VersionTag = "",
    [string]$Repository = "cjolivier01/hstream",
    [string]$DistroName = "HStream",
    [string]$DeepStreamDeb = ""
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$WslMsiUri = "https://github.com/microsoft/WSL/releases/download/2.7.11/wsl.2.7.11.0.x64.msi"
$WslMsiSha256 = "A611DDACEE689D2FB1FB5319E58AF7F3998864D86CDCE632EADD8E61614A0F9D"
$MinimumWslVersion = [Version]"2.7.11.0"
$UbuntuRootfsName = "ubuntu-noble-wsl-amd64-wsl.rootfs.tar.gz"
$UbuntuRootfsUri = "https://cloud-images.ubuntu.com/wsl/releases/24.04/20240423/$UbuntuRootfsName"
$UbuntuRootfsSha256 = "8251E27FFFF381A4AF5F41DCB94D867DE3E0D9774A9241908AB34555D99315EA"
$DistroMarkerPath = "/etc/hstream-wsl-distribution"
$DistroMarkerValue = "hstream-wsl-bootstrapper-schema-1"
$WslPrerequisiteExitCode = 1701

function Write-Stage([string]$Message) {
    Write-Host "[HStream] $Message"
}

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$ArgumentList,
        [int[]]$AllowedExitCodes = @(0)
    )
    & $FilePath @ArgumentList
    $code = $LASTEXITCODE
    if ($AllowedExitCodes -notcontains $code) {
        throw "$FilePath exited with code $code"
    }
    return $code
}

function Get-WslDistros {
    $output = & wsl.exe --list --quiet
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to list WSL distributions (exit $LASTEXITCODE)."
    }
    return @($output | ForEach-Object { ($_ -replace [char]0, "").Trim() } | Where-Object { $_ })
}

function Test-WslDistro([string]$Name) {
    return (Get-WslDistros) -contains $Name
}

function Test-HStreamDistroOwnership([string]$Name) {
    if (-not (Test-WslDistro $Name)) {
        return $false
    }
    & wsl.exe --distribution $Name --user root -- "/bin/grep" "-Fxq" $DistroMarkerValue $DistroMarkerPath
    return $LASTEXITCODE -eq 0
}

function Set-HStreamDistroOwnership([string]$Name) {
    Invoke-Checked -FilePath "wsl.exe" -ArgumentList @(
        "--distribution", $Name, "--user", "root", "--", "sh", "-c",
        "printf '%s\n' '$DistroMarkerValue' > '$DistroMarkerPath'"
    ) | Out-Null
}

function Normalize-WindowsPath([string]$Path) {
    $withoutDevicePrefix = $Path -replace '^\\\\[?]\\', ''
    return [IO.Path]::GetFullPath($withoutDevicePrefix).TrimEnd('\')
}

function Get-WslDistroBasePath([string]$Name) {
    $lxssRoot = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Lxss"
    foreach ($registration in Get-ChildItem -LiteralPath $lxssRoot -ErrorAction SilentlyContinue) {
        $properties = Get-ItemProperty -LiteralPath $registration.PSPath
        if ($properties.DistributionName -eq $Name) {
            return [string]$properties.BasePath
        }
    }
    return ""
}

function Write-PendingImportTransaction([string]$Path, [string]$Name, [string]$BasePath) {
    $temporary = "$Path.new"
    [ordered]@{
        Schema = 1
        DistroName = $Name
        BasePath = (Normalize-WindowsPath $BasePath)
        MarkerValue = $DistroMarkerValue
    } | ConvertTo-Json -Compress | Set-Content -LiteralPath $temporary -Encoding UTF8
    Move-Item -Force -LiteralPath $temporary -Destination $Path
}

function Test-PendingImportTransaction([string]$Path, [string]$Name, [string]$BasePath) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $false
    }
    try {
        $transaction = Get-Content -Raw -LiteralPath $Path | ConvertFrom-Json
        $registeredBasePath = Get-WslDistroBasePath $Name
        if (-not $registeredBasePath) {
            return $false
        }
        $expectedBasePath = Normalize-WindowsPath $BasePath
        return $transaction.Schema -eq 1 -and
            $transaction.DistroName -eq $Name -and
            $transaction.MarkerValue -eq $DistroMarkerValue -and
            (Normalize-WindowsPath ([string]$transaction.BasePath)) -eq $expectedBasePath -and
            (Normalize-WindowsPath $registeredBasePath) -eq $expectedBasePath
    } catch {
        return $false
    }
}

function Get-WslPath([string]$WindowsPath) {
    $output = & wsl.exe --distribution $DistroName --user root -- wslpath -a $WindowsPath
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to translate Windows path for WSL: $WindowsPath"
    }
    return ($output | Select-Object -Last 1).Trim()
}

function Save-GitHubReleaseAsset {
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryName,
        [Parameter(Mandatory = $true)][string]$ReleaseTag,
        [Parameter(Mandatory = $true)][string]$AssetName,
        [Parameter(Mandatory = $true)][string]$Destination
    )
    $headers = @{
        "Accept" = "application/vnd.github+json"
        "User-Agent" = "HStream-WSL-Installer/$ReleaseTag"
        "X-GitHub-Api-Version" = "2022-11-28"
    }
    $token = $env:HSTREAM_GITHUB_TOKEN
    if ($token) {
        $headers["Authorization"] = "Bearer $token"
    }
    $releaseUri = "https://api.github.com/repos/$RepositoryName/releases/tags/$ReleaseTag"
    try {
        $release = Invoke-RestMethod -Uri $releaseUri -Headers $headers
    } catch {
        throw "Unable to access GitHub release $RepositoryName $ReleaseTag. For a private repository, provide a token with read-only Contents access. $($_.Exception.Message)"
    }
    $asset = @($release.assets) | Where-Object { $_.name -eq $AssetName } | Select-Object -First 1
    if (-not $asset) {
        throw "GitHub release $RepositoryName $ReleaseTag has no asset named $AssetName."
    }
    $headers["Accept"] = "application/octet-stream"
    $assetUri = "https://api.github.com/repos/$RepositoryName/releases/assets/$($asset.id)"
    Invoke-WebRequest -UseBasicParsing -Uri $assetUri -Headers $headers -OutFile $Destination
}

function Download-VerifiedFile {
    param(
        [Parameter(Mandatory = $true)][string]$Uri,
        [Parameter(Mandatory = $true)][string]$Destination,
        [string]$ChecksumUri = "",
        [string]$ChecksumName = "",
        [string]$ExpectedHash = "",
        [string]$GitHubRepository = "",
        [string]$GitHubReleaseTag = ""
    )
    if ($ExpectedHash) {
        if ($ExpectedHash -notmatch '^[0-9a-fA-F]{64}$') {
            throw "ExpectedHash must be a SHA-256 digest."
        }
        $expectedHash = $ExpectedHash.ToUpperInvariant()
    } else {
        if (-not $ChecksumUri -or -not $ChecksumName) {
            throw "ChecksumUri and ChecksumName are required when ExpectedHash is omitted."
        }
        $checksumPath = "$Destination.SHA256SUMS"
        Write-Stage "Downloading checksums from $ChecksumUri"
        if ($GitHubRepository) {
            Save-GitHubReleaseAsset -RepositoryName $GitHubRepository -ReleaseTag $GitHubReleaseTag `
                -AssetName "SHA256SUMS" -Destination $checksumPath
        } else {
            Invoke-WebRequest -UseBasicParsing -Uri $ChecksumUri -OutFile $checksumPath
        }
        $escapedName = [Regex]::Escape($ChecksumName)
        $checksumLine = Get-Content -LiteralPath $checksumPath | Where-Object {
            $_ -match "^([0-9a-fA-F]{64})\s+\*?(?:[.][\\/])?$escapedName$"
        } | Select-Object -First 1
        if (-not $checksumLine) {
            throw "No SHA-256 entry for $ChecksumName in $ChecksumUri"
        }
        $expectedHash = ([Regex]::Match($checksumLine, "^[0-9a-fA-F]{64}").Value).ToUpperInvariant()
    }
    $download = $true
    if (Test-Path -LiteralPath $Destination -PathType Leaf) {
        $download = (Get-FileHash -Algorithm SHA256 -LiteralPath $Destination).Hash -ne $expectedHash
    }
    if ($download) {
        $temporary = "$Destination.download"
        Remove-Item -Force -ErrorAction SilentlyContinue -LiteralPath $temporary
        Write-Stage "Downloading $Uri"
        if ($GitHubRepository) {
            Save-GitHubReleaseAsset -RepositoryName $GitHubRepository -ReleaseTag $GitHubReleaseTag `
                -AssetName $ChecksumName -Destination $temporary
        } else {
            Invoke-WebRequest -UseBasicParsing -Uri $Uri -OutFile $temporary
        }
        if ((Get-FileHash -Algorithm SHA256 -LiteralPath $temporary).Hash -ne $expectedHash) {
            Remove-Item -Force -LiteralPath $temporary
            throw "SHA-256 verification failed for $Uri"
        }
        Move-Item -Force -LiteralPath $temporary -Destination $Destination
    }
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $Destination).Hash -ne $expectedHash) {
        throw "SHA-256 verification failed for cached file $Destination"
    }
    $verifiedName = if ($ChecksumName) { $ChecksumName } else { [IO.Path]::GetFileName($Destination) }
    Write-Stage "Verified $verifiedName"
}

function Get-WslRuntimeVersion {
    if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
        return $null
    }
    $output = @(& wsl.exe --version 2>$null)
    if ($LASTEXITCODE -ne 0) {
        return $null
    }
    foreach ($line in $output) {
        $normalized = ($line -replace [char]0, "").Trim()
        if ($normalized -match '([0-9]+\.[0-9]+\.[0-9]+(?:\.[0-9]+)?)') {
            try {
                return [Version]$Matches[1]
            } catch {
                return $null
            }
        }
    }
    return $null
}

function Test-WslRuntimeReady {
    $version = Get-WslRuntimeVersion
    if (-not $version -or $version -lt $MinimumWslVersion) {
        return $false
    }
    & wsl.exe --status 2>$null | Out-Null
    return $LASTEXITCODE -eq 0
}

function Install-WslRuntime {
    $stagingDirectory = Join-Path $env:ProgramData ("HStream-WSL-" + [Guid]::NewGuid().ToString("N"))
    $directory = New-Item -ItemType Directory -Path $stagingDirectory
    if (($directory.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Refusing to stage the WSL runtime in a reparse point: $stagingDirectory"
    }
    Invoke-Checked -FilePath "icacls.exe" -ArgumentList @(
        $stagingDirectory, "/inheritance:r", "/grant:r",
        "*S-1-5-18:(OI)(CI)F", "*S-1-5-32-544:(OI)(CI)F"
    ) | Out-Null
    Invoke-Checked -FilePath "icacls.exe" -ArgumentList @(
        $stagingDirectory, "/setowner", "*S-1-5-32-544"
    ) | Out-Null
    $wslMsi = Join-Path $stagingDirectory "wsl.2.7.11.0.x64.msi"

    try {
        $temporary = "$wslMsi.download"
        Write-Stage "Downloading the Microsoft WSL 2.7.11 runtime"
        Invoke-WebRequest -UseBasicParsing -Uri $WslMsiUri -OutFile $temporary
        if ((Get-FileHash -Algorithm SHA256 -LiteralPath $temporary).Hash -ne $WslMsiSha256) {
            Remove-Item -Force -LiteralPath $temporary
            throw "SHA-256 verification failed for $WslMsiUri"
        }
        Move-Item -Force -LiteralPath $temporary -Destination $wslMsi
        if ((Get-FileHash -Algorithm SHA256 -LiteralPath $wslMsi).Hash -ne $WslMsiSha256) {
            throw "SHA-256 verification failed for staged WSL runtime $wslMsi"
        }

        $signature = Get-AuthenticodeSignature -LiteralPath $wslMsi
        if ($signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid -or
            -not $signature.SignerCertificate -or
            $signature.SignerCertificate.Subject -notmatch '(^|,\s*)O=Microsoft Corporation(,|$)') {
            throw "The WSL runtime MSI does not have a valid Microsoft Authenticode signature."
        }
        Write-Stage "Verified the Microsoft WSL 2.7.11 runtime"

        Write-Stage "Installing the Microsoft WSL runtime silently"
        $installCode = Invoke-Checked -FilePath "msiexec.exe" -ArgumentList @(
            "/i", $wslMsi, "/qn", "/norestart"
        ) -AllowedExitCodes @(0, 3010)
        if ($installCode -eq 3010) {
            Write-Stage "Windows must restart before WSL provisioning can continue."
            exit 3010
        }
    } finally {
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue -LiteralPath $stagingDirectory
    }
}

function Ensure-WslMachinePrerequisites {
    $rebootRequired = $false
    foreach ($featureName in @("Microsoft-Windows-Subsystem-Linux", "VirtualMachinePlatform")) {
        $feature = Get-WindowsOptionalFeature -Online -FeatureName $featureName
        if ($feature.State -ne "Enabled") {
            Write-Stage "Enabling Windows feature $featureName"
            Enable-WindowsOptionalFeature -Online -FeatureName $featureName -All -NoRestart | Out-Null
            $rebootRequired = $true
        }
    }
    if ($rebootRequired) {
        Write-Stage "Windows must restart before WSL provisioning can continue."
        exit 3010
    }
    if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
        throw "wsl.exe is unavailable after enabling the required Windows features."
    }
    $version = Get-WslRuntimeVersion
    if (-not $version -or $version -lt $MinimumWslVersion) {
        Install-WslRuntime
    }
    & wsl.exe --status | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "The Microsoft WSL runtime is installed, but wsl.exe --status fails (exit $LASTEXITCODE)."
    }
}

function Ensure-WslPlatform {
    if (-not (Test-WslRuntimeReady)) {
        Write-Stage "Administrator approval is required for Windows WSL prerequisites."
        exit $WslPrerequisiteExitCode
    }
    Invoke-Checked -FilePath "wsl.exe" -ArgumentList @("--set-default-version", "2") | Out-Null
}

function Initialize-HStreamDistro {
    $bootstrap = @'
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends binutils ca-certificates curl gnupg sudo xz-utils zstd
if ! id -u hstream >/dev/null 2>&1; then
  useradd --create-home --shell /bin/bash hstream
fi
usermod --append --groups sudo hstream
printf 'hstream ALL=(ALL) NOPASSWD:ALL\n' >/etc/sudoers.d/hstream
chmod 0440 /etc/sudoers.d/hstream
install -d -o hstream -g hstream /home/hstream/Videos /home/hstream/hstream_output
printf '[user]\ndefault=hstream\n[boot]\nsystemd=true\n' >/etc/wsl.conf
if [ ! -r /usr/lib/wsl/lib/libcuda.so.1 ]; then
  echo 'HStream requires an NVIDIA Windows driver with CUDA support for WSL.' >&2
  exit 20
fi
printf '/usr/lib/wsl/lib\n' >/etc/ld.so.conf.d/00-wsl-libcuda.conf
ldconfig
provider=/tmp/hstream-wsl-libcuda
rm -rf "${provider}"
install -d "${provider}/DEBIAN"
cat >"${provider}/DEBIAN/control" <<'CONTROL'
Package: hstream-wsl-libcuda
Version: 1.0
Architecture: amd64
Provides: libcuda.so.1
Description: Dependency marker for the Windows-provided WSL CUDA driver
 This package owns no driver files. The Windows NVIDIA driver projects
 libcuda.so.1 into /usr/lib/wsl/lib for WSL applications.
CONTROL
dpkg-deb --build --root-owner-group "${provider}" /tmp/hstream-wsl-libcuda.deb >/dev/null
dpkg -i /tmp/hstream-wsl-libcuda.deb >/dev/null
rm -rf "${provider}" /tmp/hstream-wsl-libcuda.deb
'@
    Write-Stage "Preparing Ubuntu and validating Windows-provided CUDA"
    Invoke-Checked -FilePath "wsl.exe" -ArgumentList @(
        "--distribution", $DistroName, "--user", "root", "--", "bash", "-lc", $bootstrap
    ) | Out-Null
}

function Install-HStream {
    if ($VersionTag -notmatch '^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$') {
        throw "A strict release tag such as v0.1.0 is required."
    }
    if ($Repository -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') {
        throw "Repository must be OWNER/REPO."
    }
    if ($DistroName -notmatch '^[A-Za-z0-9._-]+$') {
        throw "DistroName contains unsupported characters."
    }
    $nativeArchitecture = if ($env:PROCESSOR_ARCHITEW6432) {
        $env:PROCESSOR_ARCHITEW6432
    } else {
        $env:PROCESSOR_ARCHITECTURE
    }
    if (-not [Environment]::Is64BitOperatingSystem -or $nativeArchitecture -ne "AMD64") {
        throw "HStream requires 64-bit Windows on an AMD64 processor."
    }
    if (-not (Test-Path -LiteralPath $DeepStreamDeb -PathType Leaf)) {
        throw "Select the local DeepStream 9.1 Debian package."
    }
    Ensure-WslPlatform

    $stateRoot = Join-Path $env:LOCALAPPDATA "HStream"
    $downloads = Join-Path $stateRoot "Downloads"
    $distroRoot = Join-Path $stateRoot "WSL"
    $pendingImport = Join-Path $stateRoot "pending-wsl-import.json"
    New-Item -ItemType Directory -Force -Path $downloads | Out-Null
    $rootfs = Join-Path $downloads $UbuntuRootfsName
    try {
        if (-not (Test-WslDistro $DistroName)) {
            Download-VerifiedFile `
                -Uri $UbuntuRootfsUri `
                -Destination $rootfs `
                -ChecksumName $UbuntuRootfsName `
                -ExpectedHash $UbuntuRootfsSha256
            New-Item -ItemType Directory -Force -Path $distroRoot | Out-Null
            Write-PendingImportTransaction $pendingImport $DistroName $distroRoot
            Write-Stage "Importing the dedicated $DistroName WSL 2 distribution"
            Invoke-Checked -FilePath "wsl.exe" -ArgumentList @(
                "--import", $DistroName, $distroRoot, $rootfs, "--version", "2"
            ) | Out-Null
            Remove-Item -Force -LiteralPath $rootfs
            Set-HStreamDistroOwnership $DistroName
        } elseif (-not (Test-HStreamDistroOwnership $DistroName)) {
            if (-not (Test-PendingImportTransaction $pendingImport $DistroName $distroRoot)) {
                throw "A WSL distribution named $DistroName already exists but is not managed by the HStream installer. Rename it or choose a different Windows account before installing."
            }
            Write-Stage "Recovering an interrupted HStream WSL import"
            Set-HStreamDistroOwnership $DistroName
        }
        if (-not (Test-HStreamDistroOwnership $DistroName)) {
            throw "The $DistroName WSL distribution is missing its HStream ownership marker."
        }

        Write-Stage "Validating the imported Ubuntu distribution"
        Invoke-Checked -FilePath "wsl.exe" -ArgumentList @(
            "--distribution", $DistroName, "--user", "root", "--",
            "grep", "-Eq", '^ID=ubuntu$', "/etc/os-release"
        ) | Out-Null
        Invoke-Checked -FilePath "wsl.exe" -ArgumentList @(
            "--distribution", $DistroName, "--user", "root", "--",
            "grep", "-Eq", '^VERSION_ID=.*24[.]04.*$', "/etc/os-release"
        ) | Out-Null
        Invoke-Checked -FilePath "wsl.exe" -ArgumentList @(
            "--distribution", $DistroName, "--user", "root", "--",
            "/usr/bin/test", "-e", "/lib64/ld-linux-x86-64.so.2"
        ) | Out-Null
        Remove-Item -Force -ErrorAction SilentlyContinue -LiteralPath $pendingImport, $rootfs
    } catch {
        if (Test-PendingImportTransaction $pendingImport $DistroName $distroRoot) {
            Write-Stage "Removing the incomplete HStream WSL import"
            & wsl.exe --unregister $DistroName | Out-Null
            if ($LASTEXITCODE -eq 0) {
                Remove-Item -Force -ErrorAction SilentlyContinue -LiteralPath $pendingImport
            }
        } elseif (-not (Test-WslDistro $DistroName)) {
            Remove-Item -Force -ErrorAction SilentlyContinue -LiteralPath $pendingImport
        }
        throw
    }
    Initialize-HStreamDistro

    $hstreamName = "hstream_${VersionTag}_ubuntu24.04_amd64.deb"
    $hstreamDeb = Join-Path $downloads $hstreamName
    $releaseBase = "https://github.com/$Repository/releases/download/$VersionTag"
    Download-VerifiedFile `
        -Uri "$releaseBase/$hstreamName" `
        -Destination $hstreamDeb `
        -ChecksumUri "$releaseBase/SHA256SUMS" `
        -ChecksumName $hstreamName `
        -GitHubRepository $Repository `
        -GitHubReleaseTag $VersionTag

    $helperWindows = Join-Path $PSScriptRoot "install-hstream-deb"
    if (-not (Test-Path -LiteralPath $helperWindows -PathType Leaf)) {
        throw "Bundled Linux installer helper is missing: $helperWindows"
    }
    $helperLinux = Get-WslPath $helperWindows
    $hstreamLinux = Get-WslPath $hstreamDeb
    $deepstreamLinux = Get-WslPath (Resolve-Path -LiteralPath $DeepStreamDeb).Path
    Write-Stage "Installing DeepStream and HStream inside $DistroName"
    Invoke-Checked -FilePath "wsl.exe" -ArgumentList @(
        "--distribution", $DistroName, "--user", "root", "--", "bash", $helperLinux,
        "--deepstream-deb=$deepstreamLinux", "--hstream-deb=$hstreamLinux"
    ) | Out-Null

    Write-Stage "Validating installed packages and launchers"
    Invoke-Checked -FilePath "wsl.exe" -ArgumentList @(
        "--distribution", $DistroName, "--user", "root", "--", "bash", "-lc",
        "dpkg-query -W -f='`$`{db:Status-Abbrev} `$`{Version}\\n' hstream deepstream-9.1; test -x /usr/bin/hstream-ui"
    ) | Out-Null
    Invoke-Checked -FilePath "wsl.exe" -ArgumentList @("--terminate", $DistroName) | Out-Null
    Write-Stage "HStream $VersionTag is installed. Use the Start menu shortcut to launch it."
}

function Launch-HStream {
    if (-not (Test-WslDistro $DistroName)) {
        throw "The $DistroName WSL distribution is not installed."
    }
    Start-Process -FilePath "wsl.exe" -ArgumentList @(
        "--distribution", $DistroName, "--", "/usr/bin/hstream-ui"
    ) | Out-Null
}

function Unregister-HStream {
    $stateRoot = Join-Path $env:LOCALAPPDATA "HStream"
    $distroRoot = Join-Path $stateRoot "WSL"
    $pendingImport = Join-Path $stateRoot "pending-wsl-import.json"
    if (Test-WslDistro $DistroName) {
        if (-not (Test-HStreamDistroOwnership $DistroName)) {
            if (-not (Test-PendingImportTransaction $pendingImport $DistroName $distroRoot)) {
                throw "Refusing to unregister $DistroName because it is not owned by the HStream installer."
            }
        }
        Write-Stage "Unregistering $DistroName and permanently deleting its WSL filesystem"
        Invoke-Checked -FilePath "wsl.exe" -ArgumentList @("--unregister", $DistroName) | Out-Null
        Remove-Item -Force -ErrorAction SilentlyContinue -LiteralPath $pendingImport
    }
}

$logRoot = Join-Path $env:LOCALAPPDATA "HStream"
New-Item -ItemType Directory -Force -Path $logRoot | Out-Null
$logPath = Join-Path $logRoot "installer.log"
try {
    Start-Transcript -Path $logPath -Append | Out-Null
    switch ($Action) {
        "Install" { Install-HStream }
        "Launch" { Launch-HStream }
        "Unregister" { Unregister-HStream }
        "EnsureWslMachine" { Ensure-WslMachinePrerequisites }
    }
} catch {
    Write-Error $_
    exit 1
} finally {
    try { Stop-Transcript | Out-Null } catch { }
}
