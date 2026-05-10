param(
    [string]$Message
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Write-Step($text)  { Write-Host "  -> $text" -ForegroundColor Cyan }
function Write-Ok($text)    { Write-Host "  OK $text" -ForegroundColor Green }
function Write-Err($text)   { Write-Host "  !! $text" -ForegroundColor Red }

$status = git status --porcelain
if (-not $status) {
    Write-Ok "Nothing to commit - working tree clean."
    exit 0
}

Write-Host ""
Write-Host "Changes to commit:" -ForegroundColor Yellow
git status --short
Write-Host ""

if (-not $Message -or -not $Message.Trim()) {
    $Message = "auto-commit $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
}

Write-Step "Staging all changes..."
git add .
if ($LASTEXITCODE -ne 0) {
    Write-Err "git add failed."
    exit 1
}
Write-Ok "All changes staged."

Write-Step "Committing: $Message"
git commit -m $Message
if ($LASTEXITCODE -ne 0) {
    Write-Err "git commit failed."
    exit 1
}
Write-Ok "Committed."

Write-Step "Pushing to remote..."
git push
if ($LASTEXITCODE -ne 0) {
    Write-Err "git push failed. You may need to pull first."
    exit 1
}

Write-Host ""
Write-Ok "Done! All changes committed and pushed."
Write-Host ""
