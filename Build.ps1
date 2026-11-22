param(
    [Parameter(Mandatory = $true)]
    [string] $Output,
    [string] $Compiler
)

$ErrorActionPreference = 'Stop'
if (!$Compiler) {
    $command = Get-Command x86_64-w64-mingw32-g++ -ErrorAction SilentlyContinue
    if (!$command) { throw 'Pass -Compiler with the path to an x64 MinGW g++ executable.' }
    $Compiler = $command.Source
}

$desktopCompiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if (!(Test-Path -LiteralPath $desktopCompiler)) { throw '.NET Framework 4.x compiler not found.' }

$Output = [IO.Path]::GetFullPath($Output)
[void][IO.Directory]::CreateDirectory($Output)
$nativeArgs = @('-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-static', '-s',
    '-shared', '-o', (Join-Path $Output 'VolumeLeveler.dll'), (Join-Path $PSScriptRoot 'VolumeLeveler.cpp'))
& $Compiler @nativeArgs
if ($LASTEXITCODE -ne 0) { throw 'Plugin build failed.' }

$desktopArgs = @('/nologo', '/target:winexe', '/platform:x64', '/optimize+', '/unsafe+', '/codepage:65001',
    '/reference:System.Windows.Forms.dll', '/reference:System.Drawing.dll',
    ('/out:' + (Join-Path $Output 'VolumeLevelerTray.exe')), (Join-Path $PSScriptRoot 'VolumeLevelerTray.cs'))
& $desktopCompiler @desktopArgs
if ($LASTEXITCODE -ne 0) { throw 'Tray build failed.' }

$settings = Join-Path $Output 'VolumeLeveler.settings'
if (!(Test-Path -LiteralPath $settings)) {
    [IO.File]::WriteAllBytes($settings, [BitConverter]::GetBytes([int]24))
}
