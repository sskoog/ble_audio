param(
    [Parameter(Mandatory=$false)]
    [string]$Port = "COM20",

    [Parameter(Mandatory=$false)]
    [int]$Baud = 115200,

    [Parameter(Mandatory=$false)]
    [int]$DurationSeconds = 10,

    [Parameter(Mandatory=$false)]
    [int]$TimeoutMs = 1500
)

Write-Host "Opening $Port @ $Baud baud for $DurationSeconds seconds..." -ForegroundColor Cyan

$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, None, 8, one
$sp.ReadTimeout = $TimeoutMs

try {
    $sp.Open()
} catch {
    Write-Error "Failed to open $Port : $_"
    exit 1
}

$startTime = [DateTime]::UtcNow
try {
    while (([DateTime]::UtcNow - $startTime).TotalSeconds -lt $DurationSeconds) {
        try {
            $line = $sp.ReadLine()
            Write-Output $line
        } catch [System.TimeoutException] {
            # Continue listening
        }
    }
} finally {
    $sp.Close()
    Write-Host "`nClosed $Port." -ForegroundColor DarkGray
}
