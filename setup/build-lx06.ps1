param([ValidateRange(1, 16)][int]$Jobs = 4)

$ErrorActionPreference = 'Stop'
$source = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\xiaoai-plus')).Path
$git = (Get-Command git.exe -ErrorAction Stop).Source
$bash = Join-Path (Split-Path (Split-Path $git)) 'bin\bash.exe'
if (-not (Test-Path -LiteralPath $bash)) { throw 'Git for Windows Bash is required.' }

$engine = docker info --format '{{.OSType}}'
if ($LASTEXITCODE -ne 0 -or $engine -ne 'linux') {
    throw 'Start Docker Desktop with Linux containers enabled.'
}

$previousJobs = $env:BUILD_JOBS
Push-Location -LiteralPath $source
try {
    $env:BUILD_JOBS = "$Jobs"
    & $bash toolchain/build-sherpa-static.sh
    if ($LASTEXITCODE -ne 0) { throw 'sherpa-onnx build failed.' }
    docker run --rm --platform linux/amd64 --mount "type=bind,source=$source,target=/workspace" `
        -w /workspace -e SYSROOT=/opt/sysroot -e "BUILD_JOBS=$Jobs" xiaoai-plus-toolchain:dev `
        bash -ec 'cmake -S . -B build-armv7 -DCMAKE_TOOLCHAIN_FILE=armv7-linux-gnueabihf.cmake -DCMAKE_BUILD_TYPE=Release -DSHERPA_ONNX_STATIC_ROOT=/workspace/assets/sherpa_onnx_static && cmake --build build-armv7 --parallel "$BUILD_JOBS" --target xiaoai_plus_speaker && arm-linux-gnueabihf-strip --strip-unneeded build-armv7/xiaoai_plus_speaker'
    if ($LASTEXITCODE -ne 0) { throw 'ARM speaker build failed.' }
    Write-Output (Join-Path $source 'build-armv7\xiaoai_plus_speaker')
} finally {
    $env:BUILD_JOBS = $previousJobs
    Pop-Location
}
