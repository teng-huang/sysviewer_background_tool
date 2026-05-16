$ErrorActionPreference = "Stop"

function Get-RequiredVersion {
  param(
    [string] $Path,
    [string] $Pattern,
    [string] $Name
  )

  $text = Get-Content -LiteralPath $Path -Raw
  $match = [regex]::Match($text, $Pattern)
  if (-not $match.Success) {
    throw "Could not find $Name version in $Path."
  }
  return $match.Groups[1].Value
}

$appVersion = Get-RequiredVersion `
  -Path "version_info.h" `
  -Pattern '#define\s+SYSMON_APP_VERSION\s+"([^"]+)"' `
  -Name "app"

$installerVersion = Get-RequiredVersion `
  -Path "SysMonitor.iss" `
  -Pattern '#define\s+MyAppVersion\s+"([^"]+)"' `
  -Name "installer"

$versions = [ordered]@{
  "version_info.h" = $appVersion
  "SysMonitor.iss" = $installerVersion
}

$readmeText = Get-Content -LiteralPath "README.md" -Raw
$readmeMatches = [regex]::Matches($readmeText, 'SysMonitor_Setup_([0-9]+\.[0-9]+\.[0-9]+)\.exe')
for ($i = 0; $i -lt $readmeMatches.Count; $i++) {
  $versions["README.md#$($i + 1)"] = $readmeMatches[$i].Groups[1].Value
}

$uniqueVersions = @($versions.Values | Sort-Object -Unique)
if ($uniqueVersions.Count -ne 1) {
  $lines = foreach ($entry in $versions.GetEnumerator()) {
    "  $($entry.Key): $($entry.Value)"
  }
  throw "Version mismatch:`n$($lines -join "`n")"
}

Write-Host "Version consistency OK: $appVersion"
