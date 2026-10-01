# Building BabbleForge (JUCE app, CLI player)

The core library and tests build without JUCE (`BF_BUILD_APP=OFF`, the default). The app
(`babbleforge-cli`, `BabbleForge` GUI) needs `-DBF_BUILD_APP=ON`, which fetches JUCE 8.0.x
(shallow) into the build directory.

## Windows (Developer PowerShell for VS 2022)

Open "Developer PowerShell for VS 2022" (Visual Studio 2022 with the "Desktop development with
C++" workload installed), then paste the whole block:

```powershell
winget install --id Kitware.CMake -e --accept-source-agreements --accept-package-agreements
winget install --id Ninja-build.Ninja -e --accept-source-agreements --accept-package-agreements
winget install --id Git.Git -e --accept-source-agreements --accept-package-agreements
# (or: choco install -y cmake ninja git)
# Restart the shell once so PATH picks up the new tools, then:
git clone https://github.com/<owner>/babbleforge.git
cd babbleforge
git checkout claude/babbleforge-v1-engine-spec-2ivcgh
cmake -S . -B build-app -G Ninja -DCMAKE_BUILD_TYPE=Release -DBF_BUILD_APP=ON -DBF_BUILD_TESTS=OFF
cmake --build build-app --target babbleforge-cli BabbleForge
.\build-app\app\cli\babbleforge-cli_artefacts\Release\babbleforge-cli.exe --list-devices
```

Play (device ids are `<type>:<name>` exactly as printed by `--list-devices`):

```powershell
.\build-app\app\cli\babbleforge-cli_artefacts\Release\babbleforge-cli.exe play --device "Windows Audio:Speakers (Realtek)" --preset preset.json --corpus C:\corpus --seconds 60
```

Drivers offered: Windows Audio (WASAPI shared), "Windows Audio (Exclusive Mode)", "Windows Audio
(Low Latency Mode)" and ASIO when enabled. DirectSound is disabled. The backend never switches to
another device on its own.

### ASIO (optional)

The Steinberg ASIO SDK cannot be redistributed. Download it from
https://www.steinberg.net/developers/ , unzip it, and add
`-DBF_ASIO_SDK_DIR=C:\SDKs\asiosdk` (the folder containing `common\iasiodrv.h`) to the configure
command. Without it, no ASIO driver type exists.

## Linux (Debian/Ubuntu)

```bash
sudo apt-get update && sudo apt-get install -y build-essential cmake ninja-build git pkg-config \
  libasound2-dev libfreetype-dev libfontconfig1-dev libx11-dev libxext-dev libxrandr-dev \
  libxinerama-dev libxcursor-dev libgl-dev libsqlite3-dev
git clone https://github.com/<owner>/babbleforge.git && cd babbleforge
git checkout claude/babbleforge-v1-engine-spec-2ivcgh
cmake -S . -B build-app -G Ninja -DCMAKE_BUILD_TYPE=Release -DBF_BUILD_APP=ON \
  -DBF_SQLITE_USE_SYSTEM=ON -DBF_BUILD_TESTS=OFF
cmake --build build-app --target babbleforge-cli BabbleForge
./build-app/app/cli/babbleforge-cli_artefacts/Release/babbleforge-cli --list-devices
```

Audio-only build (no X11/freetype needed for the GUI): add `-DBF_BUILD_GUI=OFF` and build only
`babbleforge-cli`.
