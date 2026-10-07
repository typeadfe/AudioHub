param(
    [string]$JavaHome = 'C:\Program Files\Java\jdk-19'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$android = Join-Path $root 'android-source'
$sdk = Join-Path $root 'sdk'
$gradle = Join-Path $root 'gradle\gradle-9.8.0\bin\gradle.bat'

if (-not (Test-Path (Join-Path $JavaHome 'bin\java.exe'))) {
    throw "JDK not found: $JavaHome"
}

$env:JAVA_HOME = $JavaHome
$env:PATH = "$JavaHome\bin;$env:PATH"
$env:ANDROID_HOME = $sdk
$env:ANDROID_SDK_ROOT = $sdk
$env:ANDROID_USER_HOME = Join-Path $root 'home\.android'
$env:GRADLE_USER_HOME = Join-Path $root 'home\.gradle'
$env:TEMP = Join-Path $root 'home\tmp'
$env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

# Some restricted Windows execution environments cannot start aapt2 from the workspace.
# Put a copy in the normal user temp directory when needed.
$userTemp = Join-Path $env:LOCALAPPDATA 'Temp'
$aapt2Source = Join-Path $sdk 'build-tools\36.0.0\aapt2.exe'
$aapt2Temp = Join-Path $userTemp 'aapt2.exe'
New-Item -ItemType Directory -Force $userTemp | Out-Null
Copy-Item $aapt2Source $aapt2Temp -Force

Push-Location $android
$gradleProps = Join-Path $android 'gradle.properties'
$originalGradleProps = Get-Content $gradleProps -Raw
if ($originalGradleProps -notmatch '(?m)^android\.aapt2FromMavenOverride=') {
    Add-Content $gradleProps "android.aapt2FromMavenOverride=$($aapt2Temp.Replace('\', '/'))"
}
try {
    & $gradle --no-daemon --console=plain :app:assembleDebug
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} finally {
    Set-Content $gradleProps $originalGradleProps -Encoding UTF8
    Pop-Location
}

$apk = Join-Path $android 'app\build\outputs\apk\debug\app-debug.apk'
$dist = Join-Path $root 'dist\AudioHub-v1.0-debug.apk'
Copy-Item $apk $dist -Force
Write-Host "APK: $dist"

