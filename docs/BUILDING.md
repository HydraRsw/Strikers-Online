# Building this fork

The game builds exactly like the original port: follow the
[Development page](https://github.com/new-coke/strikers/wiki/Development) of its wiki (Windows: Visual
Studio 2022 with the clang-cl toolset, CMake, and the dependencies it lists). This page only adds
what is specific to the online mode.

## The relay server

`smstrikers-port/CMakeLists.txt` builds `server/strikers_relay.cpp` as the target `strikers_relay`
whenever the `server` folder sits next to `smstrikers-port`, as it does in this repository:

```bat
cmake --build build --target strikers strikers_relay
```

On its own, without the game's build:

```sh
g++ -O2 -std=c++17 -o strikers_relay server/strikers_relay.cpp          # Linux / macOS
cl /O2 /EHsc server\strikers_relay.cpp ws2_32.lib                       # Windows, VS prompt
```

## Making the Windows release zip

After a build, `tools/windows/MAKE-RELEASE.bat` puts together the folder players download:

```bat
tools\windows\MAKE-RELEASE.bat path\to\build path\to\settings-app-folder
```

- The first argument is the build folder holding `strikers.exe` and `strikers_relay.exe`.
- The second, optional, is a folder holding `strikers-settings.exe` and its Qt files, such as the
  original port's Windows release. Without it the zip has no settings app.

It writes `release\SuperMarioStrikersOnline-windows-x86_64.zip`.

## Rules for code that runs during an online match

Both PCs must compute exactly the same thing from the same inputs. In practice:

- Gameplay code runs only in the fixed 50 Hz step (`FixedUpdateTask`). Anything that runs once per
  rendered frame (rendering, the camera, the HUD, sound) must not change gameplay state, and must
  not draw from gameplay's random numbers.
- State that gameplay reads must be in the save state (`gamestate_serialize.cpp`), or a desync
  repair cannot put it back. Bump `SAVESTATE_VERSION` when the format changes.
- Bump `NETPLAY_PROTOCOL_VERSION` (`include/port/netplay/netplay.h`) whenever packets change; builds
  with different versions refuse to connect.
- When the games go out of step, both write `desync-p1-frameN.bin` / `desync-p2-frameN.bin` and a
  field list; comparing the two shows which part of the state differs first.
