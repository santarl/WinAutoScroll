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

function Get-Remote-Stats {
    try {
        $jsonRaw = curl.exe -s -k -X GET $Url
        if ($LASTEXITCODE -eq 0 -and $jsonRaw) {
            return ($jsonRaw | ConvertFrom-Json)
        }
    } catch {}
    
    return $null
}

function Upload-Data {
    Write-Host "`n[Submitting Data...]" -ForegroundColor Cyan
    
    $remote = Get-Remote-Stats
    
    $currentPixels = 0; $currentKm = 0.0
    if ($remote) {
        if ($remote.pixels) { $currentPixels = [long]$remote.pixels }
        if ($remote.kilometres) { $currentKm = [double]$remote.kilometres }
    }

    $addPixels = [long]$script:LocalPending
    $addKm     = $addPixels * 0.000000264583
    
    $newPixels = $currentPixels + $addPixels
    $newKm     = $currentKm + $addKm

    $body = @{ pixels = $newPixels; kilometres = $newKm } | ConvertTo-Json -Compress
    $tempFile = [System.IO.Path]::GetTempFileName()
    $body | Set-Content $tempFile -Encoding ASCII

    try {
        $cmdArgs = @("-s", "-k", "-X", "POST", $Url, "-H", "Content-Type: application/json", "-d", "@$tempFile")
        $output = & curl.exe $cmdArgs
        
        if ($LASTEXITCODE -eq 0) {
            Set-IniValue -Path $IniPath -Key "Unuploaded" -Value "0"
            
            Write-Host "`n[SUCCESS]" -ForegroundColor Green
            Write-Host "Uploaded       : $addPixels pixels"
            Write-Host "New Global Total: $newPixels pixels"
            Write-Host "New Global Dist : $(($newKm).ToString('F2')) km"
            Write-Host "`nNOTE: Right-Click Tray Icon -> 'Reload Config' to reset your local pending counter." -ForegroundColor Yellow
            
            $script:LocalPending = 0
        } else {
            throw "Curl returned exit code $LASTEXITCODE"
        }
    } catch {
        Write-Error "Upload Failed."
        Write-Host "Details: $($_.Exception.Message)" -ForegroundColor Gray
    } finally {
        if (Test-Path $tempFile) { Remove-Item $tempFile }
    }
    Pause
}

function Pause { Read-Host "`nPress Enter to continue..." }

# --- Main Loop ---

# PATH DETECTION LOGIC FIXED
if (-not $IniPath) {
    # 1. Check Local Variable (Passed from One-Liner)
    if ($WASPath) { $IniPath = $WASPath }
    # 2. Check Global Variable (Backup)
    elseif ($global:WASPath) { $IniPath = $global:WASPath }
    # 3. Check Script Root (Only works if script is downloaded, not irm|iex)
    elseif ($PSScriptRoot) { $IniPath = "$PSScriptRoot\..\stats.ini" }
    # 4. Fallback to current directory
    else { $IniPath = ".\stats.ini" }
}

# Sanity Check
if (-not (Test-Path $IniPath)) {
    Write-Error "stats.ini not found at: $IniPath"
    Write-Host "If you are running this manually, provide the path to stats.ini."
    Pause
    exit
}

while ($true) {
    Clear-Host
    Write-Host "=== WinAutoScroll Global Stats ===" -ForegroundColor Cyan
    
    $script:LocalPending = Get-IniValue -Path $IniPath -Key "Unuploaded"
    $remote = Get-Remote-Stats
    
    $pColor = "Gray"
    if ([long]$script:LocalPending -gt 0) { $pColor = "Yellow" }

    Write-Host "`n[YOUR STATS]"
    Write-Host "Pending Upload : $script:LocalPending pixels" -ForegroundColor $pColor
    
    Write-Host "`n[GLOBAL STATS]"
    if ($remote) {
        Write-Host "Total Pixels   : $($remote.pixels)"
        Write-Host "Total Distance : $([math]::Round([double]$remote.kilometres, 2)) km"
    } else {
        Write-Host "Remote Status  : OFFLINE (Connection Blocked)" -ForegroundColor Red
    }

    Write-Host "`n[ACTIONS]"
    Write-Host "1. Upload Data"
    Write-Host "2. Refresh Data"
    Write-Host "3. Exit"
    
    $choice = Read-Host "`nSelect Option"
    
    switch ($choice) {
        '1' { if ([long]$script:LocalPending -gt 0) { Upload-Data } else { Write-Warning "Nothing to upload."; Pause } }
        '2' { continue }
        '3' { exit }
    }
}