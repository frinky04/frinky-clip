# Shared SDK pin: the local tool manifest must match this version.
$clipVeloVersion = '1.2.0'
$clipVeloHash = '547262ED7A1AB1FF62F580AA53851EDE2F1A451AC61B8974EB7BC01117488835'
$clipVeloRoot = Join-Path (Split-Path -Parent $PSScriptRoot) ".deps/velopack-$clipVeloVersion"
$clipTool = Get-Content (Join-Path $PSScriptRoot '../.config/dotnet-tools.json') -Raw | ConvertFrom-Json
if ($clipTool.tools.vpk.version -ne $clipVeloVersion) { throw 'Velopack SDK and packaging tool versions must match.' }
if (-not (Test-Path "$clipVeloRoot/include/Velopack.hpp")) {
    $clipVeloZip = "$clipVeloRoot.zip"
    New-Item -ItemType Directory -Force (Split-Path -Parent $clipVeloRoot) | Out-Null
    if (-not (Test-Path $clipVeloZip)) {
        Invoke-WebRequest "https://github.com/velopack/velopack/releases/download/$clipVeloVersion/velopack_libc_$clipVeloVersion.zip" -OutFile $clipVeloZip
    }
    if ((Get-FileHash $clipVeloZip -Algorithm SHA256).Hash -ne $clipVeloHash) { throw 'Velopack SDK checksum mismatch.' }
    Expand-Archive -LiteralPath $clipVeloZip -DestinationPath $clipVeloRoot -Force
}
