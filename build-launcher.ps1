$ErrorActionPreference = 'Stop'
$compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$framework = Split-Path $compiler
$wpf = Join-Path $framework 'WPF'
& $compiler /nologo /target:winexe /platform:x64 /optimize+ "/win32icon:$PSScriptRoot\assets\branding\mushroom-maker-recomp.ico" "/win32manifest:$PSScriptRoot\Launcher.manifest" "/out:$PSScriptRoot\MushroomMakerRecomp.exe" "/reference:$framework\System.Xaml.dll" "/reference:$wpf\WindowsBase.dll" "/reference:$wpf\PresentationCore.dll" "/reference:$wpf\PresentationFramework.dll" /reference:System.Windows.Forms.dll /reference:System.Web.Extensions.dll "$PSScriptRoot\Launcher.cs"
if ($LASTEXITCODE -ne 0) { throw 'Launcher compilation failed.' }
