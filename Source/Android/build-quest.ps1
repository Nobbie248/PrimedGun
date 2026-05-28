$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$javaHome = "C:\Program Files\Java\jdk-17"
$androidRoot = "C:\Users\Christophe\AppData\Local\Android"
$androidHome = Join-Path $androidRoot "Sdk"
$ndkHome = Join-Path $androidRoot "android-ndk-r29"
$vsCmakeBin = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
$vsNinjaBin = "C:\Program Files\Microsoft Visual Studio\2022\Community\CommonExtensions\Microsoft\CMake\Ninja"

if (-not (Test-Path (Join-Path $javaHome "bin\java.exe"))) {
    throw "JDK 17 not found at $javaHome"
}

if (-not (Test-Path $androidHome)) {
    throw "Android SDK root not found at $androidHome"
}

if (-not (Test-Path $ndkHome)) {
    throw "Android NDK not found at $ndkHome"
}

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
    & ".\gradlew.bat" "app:assembleQuestDebug"
}
finally {
    Pop-Location
}
