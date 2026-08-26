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
$MinimumWslVersion = [Version]"2.0.0.0"
$GitHubCliVersion = "2.98.0"
$GitHubCliArchiveName = "gh_2.98.0_windows_amd64.zip"
$GitHubCliUri = "https://github.com/cli/cli/releases/download/v$GitHubCliVersion/$GitHubCliArchiveName"
$GitHubCliSha256 = "C28C7B3B584967A05B74D9EAF7481BFF24DDC34930BF2D6E442C148236561EB1"
$GitHubDeviceLoginUri = "https://github.com/login/device"
$UbuntuRootfsName = "ubuntu-noble-wsl-amd64-wsl.rootfs.tar.gz"
$UbuntuRootfsUri = "https://cloud-images.ubuntu.com/wsl/releases/24.04/20240423/$UbuntuRootfsName"
$UbuntuRootfsSha256 = "8251E27FFFF381A4AF5F41DCB94D867DE3E0D9774A9241908AB34555D99315EA"
$DistroMarkerPath = "/etc/hstream-wsl-distribution"
$DistroMarkerValue = "hstream-wsl-bootstrapper-schema-1"
$WslPrerequisiteExitCode = 1701
$WindowsSystemDirectory = [Environment]::SystemDirectory
$WindowsProgramDataDirectory = [Environment]::GetFolderPath(
    [Environment+SpecialFolder]::CommonApplicationData
)
$WslExecutable = Join-Path $WindowsSystemDirectory "wsl.exe"
$IcaclsExecutable = Join-Path $WindowsSystemDirectory "icacls.exe"
$MsiExecExecutable = Join-Path $WindowsSystemDirectory "msiexec.exe"
$WindowsPowerShellModules = Join-Path $WindowsSystemDirectory "WindowsPowerShell\v1.0\Modules"
$DismModule = Join-Path $WindowsPowerShellModules "Dism\Dism.psd1"
$GitHubCliPath = ""
$GitHubCliConfigDirectory = ""
$GitHubCliStagingDirectory = ""

if ($Action -eq "EnsureWslMachine") {
    # The elevated helper must not resolve executables or modules from paths
    # controlled by the invoking user.
    $env:PATH = $WindowsSystemDirectory
    $env:PSModulePath = $WindowsPowerShellModules
}

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

function Invoke-WslBashScript {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Script,
        [string[]]$ArgumentList = @()
    )
    # Windows PowerShell 5 reconstructs native command lines and can corrupt
    # quotes inside a multi-line Bash script. Feed the script over stdin so
    # Bash receives it as data instead of as a command-line argument. A final
    # comment absorbs the CR that PowerShell 5 appends to native stdin.
    $scriptWithTerminator = $Script + "`n# hstream-wsl-stdin-terminator"
    $scriptWithTerminator | `
        & $WslExecutable --distribution $Name --user root -- bash -s -- @ArgumentList
    $code = $LASTEXITCODE
    if ($code -ne 0) {
        throw "$WslExecutable Bash bootstrap exited with code $code"
    }
}

function Get-WslPackageState {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Package
    )
    $output = @(& $WslExecutable --distribution $Name --user root -- `
        dpkg-query --status $Package)
    $code = $LASTEXITCODE
    if ($code -ne 0) {
        throw "Unable to query package $Package inside $Name (exit $code)."
    }
    $status = ""
    $version = ""
    foreach ($line in $output) {
        $normalized = ($line -replace [char]0, "").Trim()
        if ($normalized -match '^Status:\s*(.+)$') {
            $status = $Matches[1]
        } elseif ($normalized -match '^Version:\s*(.+)$') {
            $version = $Matches[1]
        }
    }
    if (-not $status -or -not $version) {
        throw "Package $Package inside $Name has incomplete dpkg status output."
    }
    return [PSCustomObject]@{ Status = $status; Version = $version }
}

function Get-WslDistros {
    $output = & $WslExecutable --list --quiet
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
    & $WslExecutable --distribution $Name --user root -- "/bin/grep" "-Fxq" $DistroMarkerValue $DistroMarkerPath
    return $LASTEXITCODE -eq 0
}

function Set-HStreamDistroOwnership([string]$Name) {
    Invoke-Checked -FilePath $WslExecutable -ArgumentList @(
        "--distribution", $Name, "--user", "root", "--", "sh", "-c",
        "printf '%s\n' '$DistroMarkerValue' > '$DistroMarkerPath'"
    ) | Out-Null
}

function Normalize-WindowsPath([string]$Path) {
    $withoutDevicePrefix = $Path -replace '^\\\\[?]\\', ''
    return [IO.Path]::GetFullPath($withoutDevicePrefix).TrimEnd('\')
}

function Get-WslDistroRegistration([string]$Name) {
    $lxssRoot = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Lxss"
    foreach ($registration in Get-ChildItem -LiteralPath $lxssRoot -ErrorAction SilentlyContinue) {
        $properties = Get-ItemProperty -LiteralPath $registration.PSPath
        if ($properties.DistributionName -eq $Name) {
            try {
                $registrationId = ([Guid]$registration.PSChildName).ToString("D")
            } catch {
                continue
            }
            return [PSCustomObject]@{
                RegistrationId = $registrationId
                BasePath = [string]$properties.BasePath
            }
        }
    }
    return $null
}

function Write-WslRegistrationRecord(
    [string]$Path,
    [string]$Name,
    [string]$BasePath,
    [string]$State,
    [string]$RegistrationId
) {
    $temporary = "$Path.new"
    [ordered]@{
        Schema = 2
        DistroName = $Name
        BasePath = (Normalize-WindowsPath $BasePath)
        RegistrationId = $RegistrationId
        MarkerValue = $DistroMarkerValue
        State = $State
    } | ConvertTo-Json -Compress | Set-Content -LiteralPath $temporary -Encoding UTF8
    Move-Item -Force -LiteralPath $temporary -Destination $Path
}

function Read-WslRegistrationRecord([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $null
    }
    try {
        return Get-Content -Raw -LiteralPath $Path | ConvertFrom-Json
    } catch {
        return $null
    }
}

function Test-WslRegistrationRecord([string]$Path, [string]$Name, [string]$State) {
    try {
        $record = Read-WslRegistrationRecord $Path
        $registration = Get-WslDistroRegistration $Name
        if (-not $record -or -not $registration) {
            return $false
        }
        return $record.Schema -eq 2 -and
            $record.DistroName -eq $Name -and
            $record.MarkerValue -eq $DistroMarkerValue -and
            $record.State -eq $State -and
            $record.RegistrationId -eq $registration.RegistrationId -and
            (Normalize-WindowsPath ([string]$record.BasePath)) -eq
                (Normalize-WindowsPath $registration.BasePath)
    } catch {
        return $false
    }
}

function Test-WslBasePathRegistered([string]$BasePath) {
    $expectedBasePath = Normalize-WindowsPath $BasePath
    $lxssRoot = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Lxss"
    foreach ($registration in Get-ChildItem -LiteralPath $lxssRoot -ErrorAction SilentlyContinue) {
        $properties = Get-ItemProperty -LiteralPath $registration.PSPath
        if ($properties.BasePath -and
            (Normalize-WindowsPath ([string]$properties.BasePath)) -eq $expectedBasePath) {
            return $true
        }
    }
    return $false
}

function Remove-AbandonedWslImport([string]$RecordPath, [string]$Name, [string]$StateRoot) {
    try {
        $record = Read-WslRegistrationRecord $RecordPath
        if (-not $record) {
            return $false
        }
        $recordBasePath = Normalize-WindowsPath ([string]$record.BasePath)
        $directoryName = Split-Path -Leaf $recordBasePath
        $expectedBasePath = Normalize-WindowsPath (Join-Path $StateRoot $directoryName)
        if ($record.Schema -ne 2 -or $record.DistroName -ne $Name -or
            $record.MarkerValue -ne $DistroMarkerValue -or $record.State -ne "pending" -or
            $directoryName -notmatch '^WSL-[0-9a-f]{32}$' -or $recordBasePath -ne $expectedBasePath -or
            (Test-WslBasePathRegistered $expectedBasePath)) {
            return $false
        }
        if (Test-Path -LiteralPath $expectedBasePath) {
            $directory = Get-Item -Force -LiteralPath $expectedBasePath
            if (($directory.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Refusing to remove an abandoned WSL import through a reparse point: $expectedBasePath"
            }
            Write-Stage "Removing the abandoned HStream WSL import directory"
            Remove-Item -Recurse -Force -LiteralPath $expectedBasePath
        }
        Remove-Item -Force -LiteralPath $RecordPath
        return $true
    } catch {
        if ($_.Exception.Message -like "Refusing to remove an abandoned WSL import*") {
            throw
        }
        return $false
    }
}

function Bind-PendingWslRegistration([string]$RecordPath, [string]$Name, [string]$StateRoot) {
    try {
        $record = Read-WslRegistrationRecord $RecordPath
        $registration = Get-WslDistroRegistration $Name
        if (-not $record -or -not $registration -or $record.Schema -ne 2 -or
            $record.DistroName -ne $Name -or $record.MarkerValue -ne $DistroMarkerValue -or
            $record.State -ne "pending" -or ([string]$record.RegistrationId)) {
            return $false
        }
        $recordBasePath = Normalize-WindowsPath ([string]$record.BasePath)
        $directoryName = Split-Path -Leaf $recordBasePath
        $expectedBasePath = Normalize-WindowsPath (Join-Path $StateRoot $directoryName)
        if ($directoryName -notmatch '^WSL-[0-9a-f]{32}$' -or $recordBasePath -ne $expectedBasePath -or
            (Normalize-WindowsPath $registration.BasePath) -ne $expectedBasePath) {
            return $false
        }
        Write-WslRegistrationRecord `
            $RecordPath $Name $registration.BasePath "pending" $registration.RegistrationId
        return $true
    } catch {
        return $false
    }
}

function Get-WslPath([string]$WindowsPath) {
    # wsl.exe treats backslashes in argv as escapes when invoked from Windows
    # PowerShell 5. wslpath accepts drive paths with forward slashes and then
    # receives the path intact (including any spaces).
    $portablePath = $WindowsPath.Replace([char]92, [char]47)
    $output = & $WslExecutable --distribution $DistroName --user root -- wslpath -a $portablePath
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to translate Windows path for WSL: $WindowsPath"
    }
    return ($output | Select-Object -Last 1).Trim()
}

function Sync-WindowsRootCertificates {
    $now = Get-Date
    $certificates = @{}
    foreach ($store in @("Cert:\CurrentUser\Root", "Cert:\LocalMachine\Root")) {
        foreach ($certificate in Get-ChildItem -Path $store -ErrorAction SilentlyContinue) {
            $thumbprint = ([string]$certificate.Thumbprint).ToUpperInvariant()
            if ($thumbprint -match '^[0-9A-F]{40}$' -and
                $certificate.NotBefore -le $now -and $certificate.NotAfter -gt $now) {
                $certificates[$thumbprint] = $certificate
            }
        }
    }
    if ($certificates.Count -eq 0) {
        throw "Windows has no currently valid trusted root certificates to synchronize."
    }

    $stagingDirectory = Join-Path `
        ([IO.Path]::GetTempPath()) ("HStream-WindowsRoots-" + [Guid]::NewGuid().ToString("N"))
    try {
        New-Item -ItemType Directory -Path $stagingDirectory | Out-Null
        $utf8WithoutBom = New-Object Text.UTF8Encoding($false)
        foreach ($entry in $certificates.GetEnumerator()) {
            $base64 = [Convert]::ToBase64String(
                $entry.Value.RawData,
                [Base64FormattingOptions]::InsertLineBreaks
            )
            $pem = "-----BEGIN CERTIFICATE-----`r`n$base64`r`n-----END CERTIFICATE-----`r`n"
            [IO.File]::WriteAllText((Join-Path $stagingDirectory "$($entry.Key).crt"), $pem, $utf8WithoutBom)
        }
        $linuxStagingDirectory = Get-WslPath $stagingDirectory
        $syncScript = @'
set -euo pipefail
source_directory=$1
destination=/usr/local/share/ca-certificates/hstream-windows
rm -rf "${destination}"
install -d -m 0755 "${destination}"
for certificate in "${source_directory}"/*.crt; do
  test -f "${certificate}"
  install -m 0644 "${certificate}" "${destination}/$(basename "${certificate}")"
done
update-ca-certificates
'@
        Write-Stage "Synchronizing $($certificates.Count) Windows trusted roots with Ubuntu"
        Invoke-WslBashScript `
            -Name $DistroName -Script $syncScript -ArgumentList @($linuxStagingDirectory)
    } finally {
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue -LiteralPath $stagingDirectory
    }
}

function Invoke-GitHubCli {
    param(
        [Parameter(Mandatory = $true)][string[]]$ArgumentList,
        [switch]$UseTemporaryConfig
    )
    $previousConfigDirectory = $env:GH_CONFIG_DIR
    $previousGhToken = $env:GH_TOKEN
    $previousGitHubToken = $env:GITHUB_TOKEN
    $previousPromptDisabled = $env:GH_PROMPT_DISABLED
    $previousErrorActionPreference = $ErrorActionPreference
    try {
        if ($UseTemporaryConfig) {
            $env:GH_CONFIG_DIR = $script:GitHubCliConfigDirectory
            $env:GH_PROMPT_DISABLED = "1"
            Remove-Item Env:GH_TOKEN, Env:GITHUB_TOKEN -ErrorAction SilentlyContinue
        }
        # Windows PowerShell 5 wraps a native program's stderr as an error
        # record. gh uses stderr for ordinary status and device-flow messages,
        # so inspect only its process exit code here.
        $ErrorActionPreference = "Continue"
        & $script:GitHubCliPath @ArgumentList
        $code = $LASTEXITCODE
        if ($code -ne 0) {
            throw "GitHub CLI exited with code $code."
        }
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
        if ($null -eq $previousConfigDirectory) {
            Remove-Item Env:GH_CONFIG_DIR -ErrorAction SilentlyContinue
        } else {
            $env:GH_CONFIG_DIR = $previousConfigDirectory
        }
        if ($null -eq $previousGhToken) {
            Remove-Item Env:GH_TOKEN -ErrorAction SilentlyContinue
        } else {
            $env:GH_TOKEN = $previousGhToken
        }
        if ($null -eq $previousGitHubToken) {
            Remove-Item Env:GITHUB_TOKEN -ErrorAction SilentlyContinue
        } else {
            $env:GITHUB_TOKEN = $previousGitHubToken
        }
        if ($null -eq $previousPromptDisabled) {
            Remove-Item Env:GH_PROMPT_DISABLED -ErrorAction SilentlyContinue
        } else {
            $env:GH_PROMPT_DISABLED = $previousPromptDisabled
        }
    }
}

function Initialize-GitHubCli {
    if ($script:GitHubCliPath) {
        return
    }

    $script:GitHubCliStagingDirectory = Join-Path `
        ([IO.Path]::GetTempPath()) ("HStream-GitHubCli-" + [Guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $script:GitHubCliStagingDirectory | Out-Null
    $archive = Join-Path $script:GitHubCliStagingDirectory $GitHubCliArchiveName
    Write-Stage "Downloading GitHub CLI $GitHubCliVersion for browser authentication"
    Invoke-WebRequest -UseBasicParsing -Uri $GitHubCliUri -OutFile $archive
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash -ne $GitHubCliSha256) {
        throw "SHA-256 verification failed for $GitHubCliUri"
    }
    Expand-Archive -LiteralPath $archive -DestinationPath $script:GitHubCliStagingDirectory
    $script:GitHubCliPath = Join-Path $script:GitHubCliStagingDirectory "bin\gh.exe"
    if (-not (Test-Path -LiteralPath $script:GitHubCliPath -PathType Leaf)) {
        throw "The verified GitHub CLI archive did not contain gh.exe."
    }
    Write-Stage "Verified GitHub CLI $GitHubCliVersion"

    $previousErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "SilentlyContinue"
        & $script:GitHubCliPath auth status --hostname github.com *> $null
        $authStatus = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    if ($authStatus -eq 0) {
        Write-Stage "Using the existing GitHub CLI login"
        return
    }

    $script:GitHubCliConfigDirectory = Join-Path `
        ([IO.Path]::GetTempPath()) ("HStream-GitHubAuth-" + [Guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $script:GitHubCliConfigDirectory | Out-Null
    Write-Stage "GitHub requires a sign-in for this release; opening the device login in your browser"
    # NSIS does not give the child process an interactive terminal, so gh
    # prints its device URL instead of opening it even when --web is present.
    # Open the fixed GitHub device page here; gh copies and displays the
    # one-time code below, then waits for the browser authorization to finish.
    Start-Process $GitHubDeviceLoginUri | Out-Null
    Invoke-GitHubCli -UseTemporaryConfig -ArgumentList @(
        "auth", "login", "--hostname", "github.com", "--git-protocol", "https",
        "--web", "--clipboard", "--scopes", "repo", "--insecure-storage"
    )
}

function Remove-GitHubCliStaging {
    foreach ($directory in @($script:GitHubCliConfigDirectory, $script:GitHubCliStagingDirectory)) {
        if ($directory) {
            Remove-Item -Recurse -Force -ErrorAction SilentlyContinue -LiteralPath $directory
        }
    }
}

function Save-GitHubReleaseAssetWithCli {
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryName,
        [Parameter(Mandatory = $true)][string]$ReleaseTag,
        [Parameter(Mandatory = $true)][string]$AssetName,
        [Parameter(Mandatory = $true)][string]$Destination
    )
    Initialize-GitHubCli
    $arguments = @(
        "release", "download", $ReleaseTag, "--repo", $RepositoryName,
        "--pattern", $AssetName, "--output", $Destination, "--clobber"
    )
    Invoke-GitHubCli -UseTemporaryConfig:([bool]$script:GitHubCliConfigDirectory) -ArgumentList $arguments
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
        $statusCode = if ($_.Exception.Response) { [int]$_.Exception.Response.StatusCode } else { 0 }
        if (@(401, 403, 404) -notcontains $statusCode) {
            throw "Unable to access GitHub release $RepositoryName $ReleaseTag. $($_.Exception.Message)"
        }
        Save-GitHubReleaseAssetWithCli -RepositoryName $RepositoryName -ReleaseTag $ReleaseTag `
            -AssetName $AssetName -Destination $Destination
        return
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
    if (-not (Test-Path -LiteralPath $WslExecutable -PathType Leaf)) {
        return $null
    }
    $output = @(& $WslExecutable --version 2>$null)
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
    & $WslExecutable --status 2>$null | Out-Null
    return $LASTEXITCODE -eq 0
}

function Install-WslRuntime {
    $stagingDirectory = Join-Path $WindowsProgramDataDirectory ("HStream-WSL-" + [Guid]::NewGuid().ToString("N"))
    $directory = New-Item -ItemType Directory -Path $stagingDirectory
    if (($directory.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Refusing to stage the WSL runtime in a reparse point: $stagingDirectory"
    }
    Invoke-Checked -FilePath $IcaclsExecutable -ArgumentList @(
        $stagingDirectory, "/inheritance:r", "/grant:r",
        "*S-1-5-18:(OI)(CI)F", "*S-1-5-32-544:(OI)(CI)F"
    ) | Out-Null
    Invoke-Checked -FilePath $IcaclsExecutable -ArgumentList @(
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
        $installCode = Invoke-Checked -FilePath $MsiExecExecutable -ArgumentList @(
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
    Import-Module -Name $DismModule -Force -ErrorAction Stop
    $rebootRequired = $false
    foreach ($featureName in @("Microsoft-Windows-Subsystem-Linux", "VirtualMachinePlatform")) {
        $feature = Dism\Get-WindowsOptionalFeature -Online -FeatureName $featureName
        if ($feature.State -ne "Enabled") {
            Write-Stage "Enabling Windows feature $featureName"
            Dism\Enable-WindowsOptionalFeature -Online -FeatureName $featureName -All -NoRestart | Out-Null
            $rebootRequired = $true
        }
    }
    if ($rebootRequired) {
        Write-Stage "Windows must restart before WSL provisioning can continue."
        exit 3010
    }
    if (-not (Test-Path -LiteralPath $WslExecutable -PathType Leaf)) {
        throw "wsl.exe is unavailable after enabling the required Windows features."
    }
    $version = Get-WslRuntimeVersion
    if (-not $version -or $version -lt $MinimumWslVersion) {
        Install-WslRuntime
    }
    & $WslExecutable --status | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "The Microsoft WSL runtime is installed, but wsl.exe --status fails (exit $LASTEXITCODE)."
    }
}

function Ensure-WslPlatform {
    if (-not (Test-WslRuntimeReady)) {
        Write-Stage "Administrator approval is required for Windows WSL prerequisites."
        exit $WslPrerequisiteExitCode
    }
    Invoke-Checked -FilePath $WslExecutable -ArgumentList @("--set-default-version", "2") | Out-Null
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
Maintainer: HStream <noreply@hstream.invalid>
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
    Invoke-WslBashScript -Name $DistroName -Script $bootstrap
    Sync-WindowsRootCertificates
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
    $pendingImport = Join-Path $stateRoot "pending-wsl-import.json"
    $installationRecord = Join-Path $stateRoot "wsl-installation.json"
    New-Item -ItemType Directory -Force -Path $downloads | Out-Null
    $rootfs = Join-Path $downloads $UbuntuRootfsName
    try {
        if (-not (Test-WslDistro $DistroName)) {
            if (Test-Path -LiteralPath $pendingImport) {
                $removedAbandonedImport = Remove-AbandonedWslImport $pendingImport $DistroName $stateRoot
                if (-not $removedAbandonedImport) {
                    throw "Refusing to overwrite an unverified pending WSL import record: $pendingImport"
                }
            }
            Download-VerifiedFile `
                -Uri $UbuntuRootfsUri `
                -Destination $rootfs `
                -ChecksumName $UbuntuRootfsName `
                -ExpectedHash $UbuntuRootfsSha256
            $distroRoot = Join-Path $stateRoot ("WSL-" + [Guid]::NewGuid().ToString("N"))
            Write-WslRegistrationRecord $pendingImport $DistroName $distroRoot "pending" ""
            New-Item -ItemType Directory -Path $distroRoot | Out-Null
            Write-Stage "Importing the dedicated $DistroName WSL 2 distribution"
            Invoke-Checked -FilePath $WslExecutable -ArgumentList @(
                "--import", $DistroName, $distroRoot, $rootfs, "--version", "2"
            ) | Out-Null
            $registration = Get-WslDistroRegistration $DistroName
            if (-not $registration -or
                (Normalize-WindowsPath $registration.BasePath) -ne (Normalize-WindowsPath $distroRoot)) {
                throw "The imported $DistroName registration does not use the expected WSL directory."
            }
            Write-WslRegistrationRecord `
                $pendingImport $DistroName $registration.BasePath "pending" $registration.RegistrationId
            Remove-Item -Force -LiteralPath $rootfs
            Set-HStreamDistroOwnership $DistroName
        } elseif (-not (Test-HStreamDistroOwnership $DistroName)) {
            Bind-PendingWslRegistration $pendingImport $DistroName $stateRoot | Out-Null
            if (-not (Test-WslRegistrationRecord $pendingImport $DistroName "pending")) {
                throw "A WSL distribution named $DistroName already exists but is not managed by the HStream installer. Rename it or choose a different Windows account before installing."
            }
            Write-Stage "Recovering an interrupted HStream WSL import"
            Set-HStreamDistroOwnership $DistroName
        }
        if (-not (Test-HStreamDistroOwnership $DistroName)) {
            throw "The $DistroName WSL distribution is missing its HStream ownership marker."
        }

        Write-Stage "Validating the imported Ubuntu distribution"
        Invoke-Checked -FilePath $WslExecutable -ArgumentList @(
            "--distribution", $DistroName, "--user", "root", "--",
            "grep", "-Eq", '^ID=ubuntu$', "/etc/os-release"
        ) | Out-Null
        Invoke-Checked -FilePath $WslExecutable -ArgumentList @(
            "--distribution", $DistroName, "--user", "root", "--",
            "grep", "-Eq", '^VERSION_ID=.*24[.]04.*$', "/etc/os-release"
        ) | Out-Null
        Invoke-Checked -FilePath $WslExecutable -ArgumentList @(
            "--distribution", $DistroName, "--user", "root", "--",
            "/usr/bin/test", "-e", "/lib64/ld-linux-x86-64.so.2"
        ) | Out-Null
        $registration = Get-WslDistroRegistration $DistroName
        if (-not $registration) {
            throw "The $DistroName WSL registration disappeared during validation."
        }
        Write-WslRegistrationRecord `
            $installationRecord $DistroName $registration.BasePath "complete" $registration.RegistrationId
        Remove-Item -Force -ErrorAction SilentlyContinue -LiteralPath $pendingImport, $rootfs
    } catch {
        if (Test-WslRegistrationRecord $pendingImport $DistroName "pending") {
            Write-Stage "Removing the incomplete HStream WSL import"
            & $WslExecutable --unregister $DistroName | Out-Null
            if ($LASTEXITCODE -eq 0) {
                Remove-Item -Force -ErrorAction SilentlyContinue -LiteralPath $pendingImport
            }
        } elseif (-not (Test-WslDistro $DistroName)) {
            Remove-AbandonedWslImport $pendingImport $DistroName $stateRoot | Out-Null
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
    Invoke-Checked -FilePath $WslExecutable -ArgumentList @(
        "--distribution", $DistroName, "--user", "root", "--", "bash", $helperLinux,
        "--deepstream-deb=$deepstreamLinux", "--hstream-deb=$hstreamLinux"
    ) | Out-Null

    Write-Stage "Validating installed packages and launchers"
    $expectedHStreamVersion = $VersionTag.Substring(1)
    $hstreamState = Get-WslPackageState -Name $DistroName -Package "hstream"
    if ($hstreamState.Status -ne "install ok installed" -or
        $hstreamState.Version -ne $expectedHStreamVersion) {
        throw "Unexpected installed HStream package state: $($hstreamState.Status) $($hstreamState.Version)"
    }
    $deepStreamState = Get-WslPackageState -Name $DistroName -Package "deepstream-9.1"
    if ($deepStreamState.Status -ne "install ok installed") {
        throw "Unexpected installed DeepStream package state: $($deepStreamState.Status) $($deepStreamState.Version)"
    }
    Invoke-Checked -FilePath $WslExecutable -ArgumentList @(
        "--distribution", $DistroName, "--user", "root", "--",
        "/usr/bin/test", "-x", "/usr/bin/hstream-ui"
    ) | Out-Null
    Invoke-Checked -FilePath $WslExecutable -ArgumentList @("--terminate", $DistroName) | Out-Null
    Write-Stage "HStream $VersionTag is installed. Use the Start menu shortcut to launch it."
}

function Launch-HStream {
    if (-not (Test-WslDistro $DistroName)) {
        throw "The $DistroName WSL distribution is not installed."
    }
    Start-Process -FilePath $WslExecutable -ArgumentList @(
        "--distribution", $DistroName, "--", "/usr/bin/hstream-ui"
    ) | Out-Null
}

function Unregister-HStream {
    $stateRoot = Join-Path $env:LOCALAPPDATA "HStream"
    $pendingImport = Join-Path $stateRoot "pending-wsl-import.json"
    $installationRecord = Join-Path $stateRoot "wsl-installation.json"
    if (Test-WslDistro $DistroName) {
        if (-not (Test-HStreamDistroOwnership $DistroName)) {
            $hasPendingRecord = Test-WslRegistrationRecord $pendingImport $DistroName "pending"
            $hasInstallationRecord = Test-WslRegistrationRecord $installationRecord $DistroName "complete"
            if (-not $hasPendingRecord -and -not $hasInstallationRecord) {
                throw "Refusing to unregister $DistroName because it is not owned by the HStream installer."
            }
        }
        Write-Stage "Unregistering $DistroName and permanently deleting its WSL filesystem"
        Invoke-Checked -FilePath $WslExecutable -ArgumentList @("--unregister", $DistroName) | Out-Null
    } else {
        if (Test-Path -LiteralPath $pendingImport) {
            $removedAbandonedImport = Remove-AbandonedWslImport $pendingImport $DistroName $stateRoot
            if (-not $removedAbandonedImport) {
                throw "Refusing to discard an unverified pending WSL import record: $pendingImport"
            }
        }
    }
    Remove-Item -Force -ErrorAction SilentlyContinue -LiteralPath $pendingImport, $installationRecord
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
    Remove-GitHubCliStaging
    try { Stop-Transcript | Out-Null } catch { }
}
