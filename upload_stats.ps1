param (
    [string]$IniPath
)

# --- Configuration ---
$PantryID = "780d7b02-555b-4678-98e4-d438ea0c9397"
$Basket   = "WinAutoScroll"
$Url      = "https://getpantry.cloud/apiv1/pantry/$PantryID/basket/$Basket"

# --- Network Fixes ---
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
if ([System.Net.ServicePointManager]::ServerCertificateValidationCallback -eq $null) {
    [System.Net.ServicePointManager]::ServerCertificateValidationCallback = {$true}
}

# --- Functions ---

function Get-IniValue {
    param($Path, $Key)
    if (-not (Test-Path $Path)) { return 0 }
    $content = Get-Content $Path
    foreach ($line in $content) {
        if ($line -match "^$Key\s*=\s*(.*)$") { return $matches[1] }
    }
    return 0
}

function Set-IniValue {
    param($Path, $Key, $Value)
    if (-not (Test-Path $Path)) { return }
    $content = Get-Content $Path
    $newContent = @()
    $found = $false
    foreach ($line in $content) {
        if ($line -match "^$Key\s*=") {
            $newContent += "$Key=$Value"
            $found = $true
        } else {
            $newContent += $line
        }
    }
    if (-not $found) { $newContent += "$Key=$Value" }
    $newContent | Set-Content $Path
}

function Get-Remote-Stats-With-Retry {
    $attempt = 1
    while ($true) {
        Write-Host "Fetching global stats (Attempt $attempt)..." -NoNewline
        try {
            $jsonRaw = curl.exe -s -k -X GET $Url
            if ($LASTEXITCODE -eq 0 -and $jsonRaw) {
                if ($jsonRaw.Trim().StartsWith("{")) {
                    Write-Host " OK" -ForegroundColor Green
                    return ($jsonRaw | ConvertFrom-Json)
                }
            }
        } catch {}
        try {
            $r = Invoke-RestMethod -Uri $Url -Method Get -ErrorAction Stop
            Write-Host " OK" -ForegroundColor Green
            return $r
        } catch {}
        
        Write-Host " Failed." -ForegroundColor Red
        Write-Host "   [!] Retrying in 3 seconds... (Ctrl+C to cancel)" -ForegroundColor Gray
        Start-Sleep -Seconds 3
        $attempt++
    }
}

function Restart-App {
    Write-Host "`n[Step 3: Finalizing]" -ForegroundColor Cyan
    
    # 1. Kill the App (Force stop prevents it from saving old stats on exit)
    Write-Host "Stopping WinAutoScroll..." -NoNewline
    Stop-Process -Name "WinAutoScroll" -Force -ErrorAction SilentlyContinue
    Write-Host " OK" -ForegroundColor Green

    # 2. Reset INI
    Write-Host "Resetting local counter..." -NoNewline
    Set-IniValue -Path $IniPath -Key "Unuploaded" -Value "0"
    Write-Host " OK" -ForegroundColor Green

    # 3. Restart App
    $ExeDir = Split-Path $IniPath -Parent
    $ExePath = Join-Path $ExeDir "WinAutoScroll.exe"
    
    if (Test-Path $ExePath) {
        Write-Host "Restarting Application..." -NoNewline
        Start-Process $ExePath
        Write-Host " OK" -ForegroundColor Green
    } else {
        Write-Warning "`nCould not find WinAutoScroll.exe to restart automatically."
        Write-Warning "Please start it manually."
    }
    
    # Clear cached view
    $script:LocalPending = 0
}

function Upload-Data {
    Write-Host "`n[Step 1: Syncing with Server]" -ForegroundColor Cyan
    
    $remote = Get-Remote-Stats-With-Retry
    
    if (-not $remote -or -not $remote.PSObject.Properties['pixels']) {
        Write-Error "Critical Error: Data fetched but format is invalid."
        Pause
        return
    }

    $currentPixels = [long]$remote.pixels
    $currentKm     = [double]$remote.kilometres

    $addPixels = [long]$script:LocalPending
    $addKm     = $addPixels * 0.000000264583
    
    $newPixels = $currentPixels + $addPixels
    $newKm     = $currentKm + $addKm
    $newKmRounded = [math]::Round($newKm, 2)

    $body = @{ pixels = $newPixels; kilometres = $newKmRounded } | ConvertTo-Json -Compress
    $tempFile = [System.IO.Path]::GetTempFileName()
    $body | Set-Content $tempFile -Encoding ASCII

    Write-Host "`n[Step 2: Uploading Data]" -ForegroundColor Cyan
    $attempt = 1
    
    while ($true) {
        Write-Host "Uploading ($addPixels new pixels) - Attempt $attempt..." -NoNewline
        
        try {
            $cmdArgs = @("-s", "-k", "-X", "POST", $Url, "-H", "Content-Type: application/json", "-d", "@$tempFile")
            $output = & curl.exe $cmdArgs
            
            if ($LASTEXITCODE -eq 0) {
                Write-Host " OK" -ForegroundColor Green
                
                # --- CALL RESTART LOGIC HERE ---
                Restart-App
                
                Write-Host "`n[SUCCESS] Global Stats Updated!" -ForegroundColor Green
                break
            }
        } catch {}

        Write-Host " Failed." -ForegroundColor Red
        Write-Host "   [!] Upload failed. Retrying in 3 seconds..." -ForegroundColor Gray
        Start-Sleep -Seconds 3
        $attempt++
    }

    if (Test-Path $tempFile) { Remove-Item $tempFile }
    Pause
}

function Pause { Read-Host "`nPress Enter to continue..." }

# --- Main Loop ---

if (-not $IniPath) {
    if ($WASPath) { $IniPath = $WASPath }
    elseif ($global:WASPath) { $IniPath = $global:WASPath }
    elseif ($PSScriptRoot) { $IniPath = "$PSScriptRoot\..\stats.ini" }
    else { $IniPath = ".\stats.ini" }
}

if (-not (Test-Path $IniPath)) {
    Write-Error "stats.ini not found at: $IniPath"
    Pause; exit
}

while ($true) {
    Clear-Host
    Write-Host "=== WinAutoScroll Global Stats ===" -ForegroundColor Cyan
    
    $script:LocalPending = Get-IniValue -Path $IniPath -Key "Unuploaded"
    $pendingPx = [long]$script:LocalPending
    $pendingKm = $pendingPx * 0.000000264583

    try {
        $jsonRaw = curl.exe -s -k -X GET $Url
        if ($LASTEXITCODE -eq 0) { $remote = $jsonRaw | ConvertFrom-Json } else { $remote = $null }
    } catch { $remote = $null }
    
    $pColor = "Gray"
    if ($pendingPx -gt 0) { $pColor = "Yellow" }

    Write-Host "`n[YOUR STATS]"
    Write-Host "Pending Pixels : $pendingPx" -ForegroundColor $pColor
    Write-Host "Pending Dist   : $( $pendingKm.ToString('F4') ) km" -ForegroundColor $pColor
    
    Write-Host "`n[GLOBAL STATS]"
    if ($remote -and $remote.PSObject.Properties['pixels']) {
        Write-Host "Total Pixels   : $($remote.pixels)"
        Write-Host "Total Distance : $([math]::Round([double]$remote.kilometres, 2)) km"
    } else {
        Write-Host "Remote Status  : OFFLINE (Will retry on upload)" -ForegroundColor Red
    }

    Write-Host "`n[ACTIONS]"
    Write-Host "1. Upload Data"
    Write-Host "2. Refresh Data"
    Write-Host "3. Exit"
    
    $choice = Read-Host "`nSelect Option"
    
    switch ($choice) {
        '1' { if ($pendingPx -gt 0) { Upload-Data } else { Write-Warning "Nothing to upload."; Pause } }
        '2' { continue }
        '3' { exit }
    }
}