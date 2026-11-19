# Installs an MSI silently, starts the installed TermX, then uninstalls it.
# Used by CI for the unsigned build and again for the signed one.
param([Parameter(Mandatory)][string]$Msi, [string]$LogDir = $env:RUNNER_TEMP)
$ErrorActionPreference = 'Stop'
$msi = (Resolve-Path $Msi).Path
$log = Join-Path $LogDir 'install.log'
$p = Start-Process msiexec -ArgumentList "/i `"$msi`" /qn /l*v `"$log`"" -Wait -PassThru
if ($p.ExitCode -ne 0) { Get-Content $log -Tail 60; throw "install failed: $($p.ExitCode)" }
$exe = "$env:ProgramFiles\TermX\TermX.exe"
if (-not (Test-Path $exe)) { throw "TermX.exe not installed" }
(Get-Item $exe).VersionInfo | Format-List FileVersion, ProductVersion, CompanyName, FileDescription
$app = Start-Process -FilePath $exe -ArgumentList "--data-dir", (Join-Path $LogDir 'termx-msi') -PassThru
Start-Sleep -Seconds 8
if ($app.HasExited) { throw "Installed TermX exited early with code $($app.ExitCode)" }
Stop-Process -Id $app.Id -Force
Start-Sleep -Seconds 2
$p = Start-Process msiexec -ArgumentList "/x `"$msi`" /qn" -Wait -PassThru
if ($p.ExitCode -ne 0) { throw "uninstall failed: $($p.ExitCode)" }
if (Test-Path $exe) { throw "TermX.exe still present after uninstall" }
