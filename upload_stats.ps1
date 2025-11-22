param (
    [string]$IniPath
)

# --- Configuration ---
$PantryID = "780d7b02-555b-4678-98e4-d438ea0c9397"
$Basket   = "WinAutoScroll"
$Url      = "https://getpantry.cloud/apiv1/pantry/$PantryID/basket/$Basket"

# --- THE FIX: Make PowerShell Ignore SSL Errors ---
# 1. Enable all modern TLS versions
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 -bor [Net.SecurityProtocolType]::Tls11 -bor [Net.SecurityProtocolType]::Tls

# 2. Inject a "Trust Everything" callback to bypass certificate errors (Same as curl -k)
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
    # Method A: Try PowerShell Native (Preferred)
    try {
        return Invoke-RestMethod -Uri $Url -Method Get -ErrorAction Stop
    } catch {
        # Method B: Fallback to Curl if PS fails (e.g. specific socket errors)
        try {
            $jsonRaw = curl.exe -s -k -X GET $Url
            if ($LASTEXITCODE -eq 0 -and $jsonRaw) {
                return ($jsonRaw | ConvertFrom-Json)
            }
        } catch {}
    }
    return $null
}

function Upload-Data {
    Write-Host "`n[Submitting Data...]" -ForegroundColor Cyan
    
    # 1. Re-Fetch Remote
    $remote = Get-Remote-Stats
    
    $currentPixels = 0; $currentKm = 0.0
    if ($remote) {
        if ($remote.pixels) { $currentPixels = [long]$remote.pixels }
        if ($remote.kilometres) { $currentKm = [double]$remote.kilometres }
    }

    # 2. Calculate New Totals
    $addPixels = [long]$script:LocalPending
    $addKm     = $addPixels * 0.000000264583
    
    $newPixels = $currentPixels + $addPixels
    $newKm     = $currentKm + $addKm

    # 3. Prepare Payload (Save to file to be safe for both methods)
    $bodyObj = @{ pixels = $newPixels; kilometres = $newKm }
    $jsonStr = $bodyObj | ConvertTo-Json -Compress
    
    # We use a temp file to avoid CLI quoting issues with curl
    $tempFile = [System.IO.Path]::GetTempFileName()
    $jsonStr | Set-Content $tempFile -Encoding ASCII

    $uploaded = $false

    # --- ATTEMPT 1: PowerShell Native ---
    try {
        Invoke-RestMethod -Uri $Url -Method Post -Body $bodyObj -ContentType "application/json" -ErrorAction Stop
        $uploaded = $true
    } catch {
        Write-Warning "PowerShell upload failed ($($_.Exception.Message)). Trying curl..."
        
        # --- ATTEMPT 2: Curl Fallback ---
        try {
            $cmdArgs = @("-s", "-k", "-X", "POST", $Url, "-H", "Content-Type: application/json", "-d", "@$tempFile")
            $output = & curl.exe $cmdArgs
            
            if ($LASTEXITCODE -eq 0) {
                $uploaded = $true
            } else {
                Write-Error "Curl also failed (Exit Code $LASTEXITCODE)."
            }
        } catch {
            Write-Error "Curl execution failed."
        }
    } finally {
        if (Test-Path $tempFile) { Remove-Item $tempFile }
    }

    # 5. Reset Local INI on success
    if ($uploaded) {
        Set-IniValue -Path $IniPath -Key "Unuploaded" -Value "0"
        
        Write-Host "`n[SUCCESS]" -ForegroundColor Green
        Write-Host "Uploaded       : $addPixels pixels"
        Write-Host "New Global Total: $newPixels pixels"
        Write-Host "New Global Dist : $(($newKm).ToString('F2')) km"
        Write-Host "`nNOTE: Right-Click Tray Icon -> 'Reload Config' to reset your local pending counter." -ForegroundColor Yellow
        
        $script:LocalPending = 0
    } 
    
    Pause
}

function Pause { Read-Host "`nPress Enter to continue..." }

# --- Main Loop ---

if (-not $IniPath) { $IniPath = "$PSScriptRoot\..\stats.ini" }

if (-not (Test-Path $IniPath)) {
    Write-Error "stats.ini not found at: $IniPath"
    Pause; exit
}

while ($true) {
    Clear-Host
    Write-Host "=== WinAutoScroll Global Stats ===" -ForegroundColor Cyan
    
    # Load Data
    $script:LocalPending = Get-IniValue -Path $IniPath -Key "Unuploaded"
    $remote = Get-Remote-Stats
    
    # Display Dashboard
    Write-Host "`n[YOUR STATS]"
    Write-Host "Pending Upload : $script:LocalPending pixels" -ForegroundColor ($script:LocalPending -gt 0 ? "Yellow" : "Gray")
    
    Write-Host "`n[GLOBAL STATS]"
    if ($remote) {
        Write-Host "Total Pixels   : $($remote.pixels)"
        Write-Host "Total Distance : $([math]::Round([double]$remote.kilometres, 2)) km"
    } else {
        Write-Host "Remote Status  : OFFLINE" -ForegroundColor Red
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