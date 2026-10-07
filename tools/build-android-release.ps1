param(
    [string]$JavaHome = 'C:\Program Files\Java\jdk-19'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$android = Join-Path $root 'android-source'
$sdk = Join-Path $root 'sdk'
$gradle = Join-Path $root 'gradle\gradle-9.8.0\bin\gradle.bat'
$keytool = Join-Path $JavaHome 'bin\keytool.exe'
$signingDir = Join-Path $root 'home\release-signing'
$keyFile = Join-Path $signingDir 'audiohub-release.p12'
$passwordFile = Join-Path $signingDir 'password.txt'

if (-not (Test-Path $keytool)) { throw "JDK not found: $JavaHome" }
New-Item -ItemType Directory -Force $signingDir | Out-Null
if (-not (Test-Path $keyFile)) {
    if (Test-Path $passwordFile) { throw 'Release password exists but keystore is missing' }
    $bytes = New-Object byte[] 48
    $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    try { $rng.GetBytes($bytes) } finally { $rng.Dispose() }
    $password = [Convert]::ToBase64String($bytes)
    [System.IO.File]::WriteAllText($passwordFile, $password)
    $env:AUDIOHUB_RELEASE_PASSWORD = $password
    & $keytool -genkeypair -noprompt -alias audiohub -keyalg RSA -keysize 3072 `
        -validity 10000 -dname 'CN=AudioHub, OU=Release, O=AudioHub' `
        -storetype PKCS12 -keystore $keyFile `
        -storepass:env AUDIOHUB_RELEASE_PASSWORD -keypass:env AUDIOHUB_RELEASE_PASSWORD
    if ($LASTEXITCODE -ne 0) { throw 'Release key generation failed' }
} else {
    if (-not (Test-Path $passwordFile)) { throw 'Release keystore exists but password is missing' }
    $env:AUDIOHUB_RELEASE_PASSWORD = [System.IO.File]::ReadAllText($passwordFile).Trim()
}

$env:AUDIOHUB_RELEASE_KEYSTORE = $keyFile
$env:JAVA_HOME = $JavaHome
$env:PATH = "$JavaHome\bin;$env:PATH"
$env:ANDROID_HOME = $sdk
$env:ANDROID_SDK_ROOT = $sdk
$env:ANDROID_USER_HOME = Join-Path $root 'home\.android'
$env:GRADLE_USER_HOME = Join-Path $root 'home\.gradle'
$env:TEMP = Join-Path $root 'home\tmp'
$env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

# Some restricted Windows environments cannot launch aapt2 from the workspace.
$userTemp = Join-Path $env:LOCALAPPDATA 'Temp'
$aapt2Source = Join-Path $sdk 'build-tools\36.0.0\aapt2.exe'
$aapt2Temp = Join-Path $userTemp 'aapt2.exe'
New-Item -ItemType Directory -Force $userTemp | Out-Null
Copy-Item $aapt2Source $aapt2Temp -Force

$gradleProps = Join-Path $android 'gradle.properties'
$originalGradleProps = [System.IO.File]::ReadAllText($gradleProps)
Push-Location $android
try {
    if ($originalGradleProps -notmatch '(?m)^android\.aapt2FromMavenOverride=') {
        [System.IO.File]::AppendAllText($gradleProps, "`nandroid.aapt2FromMavenOverride=$($aapt2Temp.Replace('\', '/'))`n")
    }
    & $gradle --no-daemon --console=plain :app:assembleRelease
    if ($LASTEXITCODE -ne 0) { throw 'Release build failed' }
} finally {
    [System.IO.File]::WriteAllText($gradleProps, $originalGradleProps)
    Pop-Location
    Remove-Item Env:AUDIOHUB_RELEASE_PASSWORD -ErrorAction SilentlyContinue
    Remove-Item Env:AUDIOHUB_RELEASE_KEYSTORE -ErrorAction SilentlyContinue
}

$apk = Join-Path $android 'app\build\outputs\apk\release\app-release.apk'
$dist = Join-Path $root 'dist\AudioHub-v1.0.apk'
Copy-Item $apk $dist -Force
$apksigner = Join-Path $sdk 'build-tools\36.0.0\apksigner.bat'
& $apksigner verify --verbose $dist
if ($LASTEXITCODE -ne 0) { throw 'APK signature verification failed' }
Write-Host "APK: $dist"
Write-Host "Release signing key: $keyFile (back up with password.txt)"
