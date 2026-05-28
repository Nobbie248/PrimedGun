## Prerequisites
- [Visual Studio 2022](https://visualstudio.microsoft.com/vs/)
  - You can use any edition of VS (Community Edition is free)
  - Easiest to just choose "Desktop development with C++" workload
  - However, minimum required to get source and build are:
    - Git for Windows (Note: [The standalone git installer](https://git-scm.com/downloads) is more recent and is compatible with VS)
    - C++ core features
    - Latest MSVC C++ build tools (x64/x86 and/or ARM64)
    - Latest Windows SDK
    - C++ CMake tools for Windows (if using cmake)
  - Recommended for development / debugging (included in workload)
    - Just-In-Time debugger
    - C++ profiling tools (pulls in graphics debugging features)
    - Test Adapter for Google Test

To install all of the above on the command line, you can use something like:
`vs_Community.exe --passive --add Microsoft.VisualStudio.Workload.NativeDesktop;includeRecommended --add Microsoft.VisualStudio.Component.Git --add Microsoft.VisualStudio.Component.VC.Tools.ARM64`

## Clone Dolphin
* Building on Windows requires pulling in some submodules, so use `git clone --recursive` to clone dolphin from the main repo or your fork.
* If you have already cloned the directory *without* pulling in the submodules run ``git submodule update --init`` to initialize the submodules.

## Building
Dolphin can be built with msbuild or cmake. Both msbuild and cmake are supported from Visual Studio (or other IDEs, like VS Code) as well as command line. The following assume Dolphin was cloned into `c:\src\dolphin`.

### Build in Visual Studio from GUI with msbuild (recommended)
1. Open `Source\dolphin-emu.sln` with Visual Studio
1. Configure build target (e.g. x64 / Release)
1. Press F7 or choose `Build > Build Solution` from the menu bar

### Build in Visual Studio from GUI with cmake
1. Open the folder Dolphin was cloned into with Visual Studio
    * From the cmdline: `c:\src\dolphin>devenv .`
1. Configure build target (e.g. Release)
1. Select ``Dolphin.exe`` as startup item
1. Press F7 or choose `Build > Build All` from the menu bar

### Build in VS Code with cmake
1. Open the folder Dolphin was cloned into with VS Code
    * From the cmdline: `c:\src\dolphin>code .`
1. Install and use the [CMake Tools](https://marketplace.visualstudio.com/items?itemName=ms-vscode.cmake-tools) extension.

### Command line
First, bring msvc tools into your environment.
* For example: `c:\src\dolphin>"c:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64`

#### Build with msbuild
```
c:\src\dolphin>msbuild -v:m -m -p:Platform=x64,Configuration=Release Source\dolphin-emu.sln
```

#### Build with cmake
```
cd c:\src\dolphin
mkdir build && cd build
cmake .. -GNinja
ninja
```