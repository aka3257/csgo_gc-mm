# csgo_gc-mm

> [!CAUTION]
> This project is incomplete and not ready for general use.

## What is this?
In Valve games, the Game Coordinator (GC) is a backend service most notably responsible for matchmaking and inventory management (like loadouts and skins). This project redirects the GC traffic to a custom, in-process implementation.

## Why would you want this?
While it's still possible to connect CS:GO to CS2's GC by spoofing the version number, this may break in the future if Valve updates the GC protocol. This project aims to restore most GC-related functionality.

## Current features
- nothing (i'll do more as soon as i fix forwarder)

## Planned features
- Rest of the core features (trade ups, souvenirs, storage units, StatTrak swaps...)
- Matchmaking

## Installation
- Download [CS:GO from Steam](steam://install/4465480)
- Download the latest release for your platform from the [releases page](https://github.com/aka3257/csgo_gc-mm/releases/tag/Release)
- Navigate to the game's installation directory
- Back up your existing launcher executables as they'll be overwritten (i.e. csgo.exe, srcds.exe, csgo_linux64, etc.)
- Navigate to `/csgo/panorama/`, open terminal in that folder, and execute command `pbin.exe patch_panorama` 
- Extract the contents of the downloaded archive to your game directory, replace the executables when prompted
- Launch the game. If you get the annoying VAC message box, launch the game with the -steam argument

## Building
Requirements:
- Git
- CMake 3.20 or newer
- C++ compiler with C++17 support (VS 2017 or later, Clang 5 or later, GCC 7 or later)

The game is 32-bit on Windows so you need to build as 32-bit:

`cmake -A Win32 -B build`

Linux dedicated servers are also 32-bit:

`cmake -DCMAKE_C_FLAGS=-m32 -DCMAKE_CXX_FLAGS=-m32 -DCMAKE_ASM_FLAGS=-m32 -B build`

On macOS, you need to build for x86_64 instead of arm64:

`cmake -DCMAKE_OSX_ARCHITECTURES=x86_64 -DFUNCHOOK_CPU=x86 -B build`

For Linux clients you don't have to specify any additional options.

## Configuration
if you want to set up your own GC: 
1. download [CSGO-GC-Replacement](https://github.com/aka3257/CSGO-GC-Replacement)
2. run `Server_v2.js` to generate config file
3. change ip to your internal ip in `config.json`
4. download source code of csgo_gc-mm from Releases page
5. navigate to `csgo_gc-continuous/csgo_gc`
6. open `external_forwarder.cpp` and change ip to your external ip at `static std::string g_serverUrl = "http://your.ip.goes.here/gc";`
7. build folder `csgo_gc-continuous`

## License
This project is licensed under the 2-Clause BSD License. See [LICENSE.md](LICENSE.md) for details.

## Credits
* **kandr** - Fork author
* **Mikko Kokko** - Original author
* **Theeto** - Code reused from the predecessor project, unusual loot lists

## Third party dependencies
- [Crypto++](https://github.com/weidai11/cryptopp) ([Boost Software License](https://github.com/weidai11/cryptopp/blob/master/License.txt))
- [funchook](https://github.com/kubo/funchook) ([GPL v2 with Classpath Exception](https://github.com/kubo/funchook/blob/master/LICENSE))
- [diStorm3](https://github.com/gdabah/distorm) ([3-Clause BSD License](https://github.com/gdabah/distorm/blob/master/COPYING))
- [protobuf](https://github.com/protocolbuffers/protobuf) ([3-Clause BSD License](https://github.com/protocolbuffers/protobuf/blob/main/LICENSE))
