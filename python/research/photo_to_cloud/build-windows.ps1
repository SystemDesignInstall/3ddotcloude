[CmdletBinding()]
param(
    [switch]$SkipTests
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ProjectRoot

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)] [string]$FilePath,
        [Parameter(Mandatory = $false)] [string[]]$ArgumentList = @()
    )
    & $FilePath @ArgumentList
    if ($LASTEXITCODE -ne 0) {
        throw "Команда завершилась с кодом $LASTEXITCODE`: $FilePath $($ArgumentList -join ' ')"
    }
}

$launcher = Get-Command py -ErrorAction SilentlyContinue
if ($null -eq $launcher) {
    throw "Не найден Python Launcher (py). Установите 64-битный Python 3.11–3.14."
}

$venv = Join-Path $ProjectRoot ".venv-build"
$python = Join-Path $venv "Scripts\python.exe"
if (-not (Test-Path $python)) {
    $created = $false
    foreach ($version in @("3.14", "3.13", "3.12", "3.11")) {
        & $launcher.Source "-$version" -m venv $venv
        if ($LASTEXITCODE -eq 0) {
            $created = $true
            break
        }
    }
    if (-not $created) {
        throw "Не удалось создать виртуальное окружение Python 3.11–3.14."
    }
}

Write-Host "Установка зависимостей сборки..." -ForegroundColor Cyan
Invoke-Checked $python @("-m", "pip", "install", "--upgrade", "pip")
Invoke-Checked $python @("-m", "pip", "install", "-r", "requirements.txt", "pyinstaller==6.22.3")

$dist = Join-Path $ProjectRoot "dist"
$build = Join-Path $ProjectRoot "build"
if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
if (Test-Path $build) { Remove-Item -Recurse -Force $build }

Write-Host "Сборка SpatialPhotoCloud.exe..." -ForegroundColor Cyan
$pyinstallerArgs = @(
    "-m", "PyInstaller", "--noconfirm", "--clean", "--onedir", "--console",
    "--name", "SpatialPhotoCloud",
    "--add-data", "full_viewer.html;.",
    "--collect-all", "pycolmap",
    "--collect-all", "PIL",
    "--collect-all", "pyopencl",
    "--collect-all", "numpy",
    "--collect-all", "rawpy",
    "--paths", $ProjectRoot,
    "app.py"
)
Invoke-Checked $python $pyinstallerArgs

$executable = Join-Path $dist "SpatialPhotoCloud\SpatialPhotoCloud.exe"
if (-not (Test-Path $executable)) {
    throw "PyInstaller не создал $executable"
}

if (-not $SkipTests) {
    Write-Host "Запуск модульных проверок..." -ForegroundColor Cyan
    Invoke-Checked $python @("-m", "unittest", "discover", "-s", ".", "-p", "test_*.py", "-v")
    Write-Host "Проверка собранного приложения..." -ForegroundColor Cyan
    Invoke-Checked $executable @("--self-test")
    Invoke-Checked $executable @("--viewer-self-test")
    Invoke-Checked $executable @("--ui-smoke-test")
    Invoke-Checked $executable @("--hardware-opencl-probe", "--inventory-only")
}

Copy-Item "WINDOWS-TEST.txt" (Join-Path $dist "SpatialPhotoCloud\README.txt") -Force
Copy-Item "README.md" (Join-Path $dist "SpatialPhotoCloud\README-SOURCE.md") -Force

Write-Host "Готово: $executable" -ForegroundColor Green
Write-Host "В папку dist\SpatialPhotoCloud скопированы README.txt и тестовые инструкции."
