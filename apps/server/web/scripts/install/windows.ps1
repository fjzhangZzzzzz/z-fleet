param(
  [Parameter(Mandatory=$true)][string]$ServerUrl,
  [Parameter(Mandatory=$true)][string]$ControlUrl,
  [Parameter(Mandatory=$true)][string]$Token,
  [string]$Channel = "stable",
  [ValidateSet("debug","release")][string]$BuildType = "release",
  [string]$Root = "$env:ProgramData\z-fleet"
)
$ErrorActionPreference = "Stop"
$arch = if ([System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture -eq "X64") { "x86_64" } elseif ([System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture -eq "Arm64") { "arm64" } else { throw "unsupported architecture" }
$options = Invoke-RestMethod "$ServerUrl/api/v1/install/options?platform=windows&arch=$arch&channel=$Channel&build_type=$BuildType"
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("zfleet-" + [Guid]::NewGuid())
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
  $installerZip = Join-Path $tmp "installer.zip"; $agentZip = Join-Path $tmp "agent.zip"
  Invoke-WebRequest ($ServerUrl + $options.installer.download_url) -OutFile $installerZip
  Invoke-WebRequest ($ServerUrl + $options.agent.download_url) -OutFile $agentZip
  if ((Get-FileHash $installerZip -Algorithm SHA256).Hash.ToLower() -ne $options.installer.sha256) { throw "installer checksum mismatch" }
  if ((Get-FileHash $agentZip -Algorithm SHA256).Hash.ToLower() -ne $options.agent.sha256) { throw "agent checksum mismatch" }
  $expanded = Join-Path $tmp "installer"; Expand-Archive $installerZip -DestinationPath $expanded
  & (Join-Path $expanded "payload\bin\zfleet_installer.exe") apply --root $Root --package $installerZip
  $installer = Join-Path $Root "installer\bin\zfleet_installer.exe"
  & $installer apply --root $Root --package $agentZip
  $agent = Join-Path $Root "agent\bin\zfleet_agent.exe"
  Start-Process $agent -ArgumentList @("--control-url", $ControlUrl, "--registration-token", $Token) -WorkingDirectory (Split-Path $agent)
} finally { Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue }
