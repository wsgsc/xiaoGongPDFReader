param(
	[Parameter(Mandatory = $true)][string]$RedistRoot,
	[Parameter(Mandatory = $true)][ValidateSet('x86', 'x64')][string]$Architecture,
	[Parameter(Mandatory = $true)][string]$TargetPath
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $RedistRoot -PathType Container)) {
	throw "Visual C++ redistributable directory not found: $RedistRoot"
}

$versions = Get-ChildItem -LiteralPath $RedistRoot -Directory |
	Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } |
	Sort-Object { [version]$_.Name } -Descending

$selected = $null
foreach ($version in $versions) {
	$archDir = Join-Path $version.FullName $Architecture
	$crtDir = Join-Path $archDir 'Microsoft.VC143.CRT'
	$mfcDir = Join-Path $archDir 'Microsoft.VC143.MFC'
	if ((Test-Path -LiteralPath $crtDir -PathType Container) -and
		(Test-Path -LiteralPath $mfcDir -PathType Container)) {
		$selected = @($crtDir, $mfcDir)
		break
	}
}

if (-not $selected) {
	throw "Visual C++ v143 CRT and MFC redistributable DLLs for $Architecture were not found under $RedistRoot"
}

$destination = Split-Path -Parent $TargetPath
foreach ($directory in $selected) {
	Get-ChildItem -LiteralPath $directory -Filter '*.dll' -File |
		Copy-Item -Destination $destination -Force
}

Write-Host "Copied v143 CRT and MFC DLLs to $destination"
