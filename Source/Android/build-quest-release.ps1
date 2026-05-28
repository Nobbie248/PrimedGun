param(
    [string]$Keystore = $env:DOLPHIN_ANDROID_KEYSTORE,
    [string]$StorePass = $env:DOLPHIN_ANDROID_STOREPASS,
    [string]$KeyAlias = $env:DOLPHIN_ANDROID_KEYALIAS,
    [string]$KeyPass = $env:DOLPHIN_ANDROID_KEYPASS
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$javaHome = "C:\Program Files\Java\jdk-17"
$androidRoot = "C:\Users\Christophe\AppData\Local\Android"
$androidHome = Join-Path $androidRoot "Sdk"
$ndkHome = Join-Path $androidRoot "android-ndk-r29"
$vsCmakeBin = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
$vsNinjaBin = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"

if (-not (Test-Path (Join-Path $javaHome "bin\java.exe"))) {
    throw "JDK 17 not found at $javaHome"
}

if (-not (Test-Path $androidHome)) {
    throw "Android SDK root not found at $androidHome"
}

if (-not (Test-Path $ndkHome)) {
    throw "Android NDK not found at $ndkHome"
}

$signingValues = @{
    keystore = if ($Keystore) { $Keystore } else { $env:ORG_GRADLE_PROJECT_keystore }
    storepass = if ($StorePass) { $StorePass } else { $env:ORG_GRADLE_PROJECT_storepass }
    keyalias = if ($KeyAlias) { $KeyAlias } else { $env:ORG_GRADLE_PROJECT_keyalias }
    keypass = if ($KeyPass) { $KeyPass } else { $env:ORG_GRADLE_PROJECT_keypass }
}

$missingSigningKeys = $signingValues.GetEnumerator() |
    Where-Object { [string]::IsNullOrWhiteSpace($_.Value) } |
    Select-Object -ExpandProperty Key

if ($missingSigningKeys.Count -gt 0) {
    throw @"
Missing release signing settings: $($missingSigningKeys -join ", ")

Provide them either as parameters:
  .\build-quest-release.ps1 -Keystore C:\path\release.jks -StorePass ... -KeyAlias ... -KeyPass ...

Or as environment variables:
  DOLPHIN_ANDROID_KEYSTORE
  DOLPHIN_ANDROID_STOREPASS
  DOLPHIN_ANDROID_KEYALIAS
  DOLPHIN_ANDROID_KEYPASS
"@
}

$resolvedKeystore = $signingValues.keystore
if (-not [System.IO.Path]::IsPathRooted($resolvedKeystore)) {
    $resolvedKeystore = Join-Path $scriptDir $resolvedKeystore
}
if (-not (Test-Path $resolvedKeystore)) {
    throw "Release keystore not found at $resolvedKeystore"
}
$signingValues.keystore = (Resolve-Path $resolvedKeystore).Path

$env:JAVA_HOME = $javaHome
$env:ANDROID_HOME = $androidHome
$env:ANDROID_SDK_ROOT = $androidHome
$env:ANDROID_NDK_HOME = $ndkHome
$pathPrefixes = @("$javaHome\bin", "$androidHome\platform-tools")
if (Test-Path (Join-Path $vsCmakeBin "cmake.exe")) {
    $pathPrefixes += $vsCmakeBin
}
if (Test-Path (Join-Path $vsNinjaBin "ninja.exe")) {
    $pathPrefixes += $vsNinjaBin
}
$env:Path = (($pathPrefixes -join ";") + ";" + $env:Path)

Push-Location $scriptDir
try {
    $oldGradleSigningEnv = @{}
    foreach ($key in $signingValues.Keys) {
        $envName = "ORG_GRADLE_PROJECT_$key"
        $oldGradleSigningEnv[$envName] = [Environment]::GetEnvironmentVariable($envName, "Process")
        [Environment]::SetEnvironmentVariable($envName, $signingValues[$key], "Process")
    }

    & ".\gradlew.bat" "app:assembleQuestRelease"
}
finally {
    foreach ($entry in $oldGradleSigningEnv.GetEnumerator()) {
        [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, "Process")
    }
    Pop-Location
}
