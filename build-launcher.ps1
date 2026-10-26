param([string]$OutputDirectory = (Join-Path $PSScriptRoot 'build/launcher'))
$ErrorActionPreference = 'Stop'
$source = Join-Path $PSScriptRoot 'tools/launcher'
if (!(Test-Path -LiteralPath $source)) { $source = $PSScriptRoot }
$compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$framework = Split-Path $compiler
$wpf = Join-Path $framework 'WPF'
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$output = Join-Path $OutputDirectory 'MushroomMakerRecomp.exe'
& $compiler /nologo /target:winexe /platform:x64 /optimize+ "/win32icon:$PSScriptRoot\assets\branding\mushroom-maker-recomp.ico" "/win32manifest:$source\Launcher.manifest" "/out:$output" "/reference:$framework\System.Xaml.dll" "/reference:$wpf\WindowsBase.dll" "/reference:$wpf\PresentationCore.dll" "/reference:$wpf\PresentationFramework.dll" /reference:System.Windows.Forms.dll /reference:System.Web.Extensions.dll "$source\Launcher.cs"
if ($LASTEXITCODE -ne 0) { throw 'Launcher compilation failed.' }
Write-Output "Built $output"
