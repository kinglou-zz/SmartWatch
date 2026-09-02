# Create a VMR voice reminder: "提醒你，该吃药了" scheduled +30s
# Usage:
#   powershell -File scripts/create_medicine_reminder.ps1
# Optional:
#   -FamilyId 1  -DelaySec 30  -WavPath .\test_medicine_reminder.wav
# Token: $env:USER_TOKEN, else STAGING_DEVICE_JWT from main/provision.h

param(
    [string]$Base = "http://***",
    [int]$FamilyId = 1,
    [int]$DelaySec = 30,
    [string]$WavPath = ".\test_medicine_reminder.wav",
    [string]$UserToken = $env:USER_TOKEN
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

if ([string]::IsNullOrWhiteSpace($UserToken)) {
    $prov = Get-Content ".\main\provision.h" -Raw
    if ($prov -match 'STAGING_DEVICE_JWT\s*=\s*"([^"]+)"') {
        $UserToken = $Matches[1]
        Write-Host "Using JWT from provision.h"
    }
}
if ([string]::IsNullOrWhiteSpace($UserToken)) {
    Write-Error "No token. Set USER_TOKEN or ensure provision.h has STAGING_DEVICE_JWT."
    exit 1
}
if (-not (Test-Path $WavPath)) {
    Write-Error "WAV not found: $WavPath"
    exit 1
}

$scheduledAt = (Get-Date).ToUniversalTime().AddSeconds($DelaySec).ToString("yyyy-MM-ddTHH:mm:ssZ")
Write-Host "Uploading reminder, scheduled_at=$scheduledAt (UTC, +${DelaySec}s)"

$uploadRaw = curl.exe -sS -w "`n__HTTP__=%{http_code}" -X POST "$Base/families/$FamilyId/messages" `
  -H "Authorization: Bearer $UserToken" `
  -F "file=@$WavPath" `
  -F "scheduled_at=$scheduledAt"
Write-Host "upload: $uploadRaw"

$uploadJson = ($uploadRaw -split "`n" | Where-Object { $_.Trim().StartsWith("{") } | Select-Object -First 1)
if (-not $uploadJson) {
    Write-Error "Upload failed (no JSON body)."
    exit 1
}
$msg = $uploadJson | ConvertFrom-Json
if (-not $msg.message_id) {
    Write-Error "Upload failed — this JWT may be device-only; App user token required for POST /families/.../messages."
    exit 1
}

$scheduleBody = "{`"mode`":`"schedule`",`"scheduled_at`":`"$scheduledAt`"}"
$scheduleRaw = curl.exe -sS -w "`n__HTTP__=%{http_code}" -X POST "$Base/messages/$($msg.message_id)/schedule" `
  -H "Authorization: Bearer $UserToken" `
  -H "Content-Type: application/json" `
  -d $scheduleBody
Write-Host "schedule: $scheduleRaw"
Write-Host "OK message_id=$($msg.message_id) — device should see it around $scheduledAt"
