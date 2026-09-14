# CI build input: extract the official runtime instead of installing OBS.
param([string]$Destination = (Join-Path $PSScriptRoot '../.deps/obs-runtime-32.2.2'))
$ErrorActionPreference = 'Stop'
$clipObsZip = Join-Path (Split-Path -Parent $Destination) 'OBS-Studio-32.2.2-Windows-x64.zip'
New-Item -ItemType Directory -Force (Split-Path -Parent $Destination) | Out-Null
if (-not (Test-Path $clipObsZip)) {
    Invoke-WebRequest 'https://github.com/obsproject/obs-studio/releases/download/32.2.2/OBS-Studio-32.2.2-Windows-x64.zip' -OutFile $clipObsZip
}
if ((Get-FileHash $clipObsZip -Algorithm SHA256).Hash -ne '4D6E40E3AB155F56B30DE517380566A206D74B63CDF5AD49AA596924768F97E1') { throw 'OBS runtime checksum mismatch.' }
if (-not (Test-Path "$Destination/bin/64bit/obs.dll")) { Expand-Archive $clipObsZip $Destination -Force }
if ((Get-Item "$Destination/bin/64bit/obs.dll").VersionInfo.ProductVersion -ne '32.2.2') { throw 'Unexpected OBS runtime version.' }
