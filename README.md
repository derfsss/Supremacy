# Supremacy

A native port of **Supremacy: Your Will Be Done** (Probe Software / Virgin
Mastertronic, 1990), the Amiga strategy game, to modern systems:

- **Windows** 10 and 11 (64-bit)
- **Linux** (tested on Ubuntu 24.04)
- **AmigaOS 4.1** Final Edition (PowerPC)

The port is a translation of the original Amiga program into portable C, with
a new platform layer built on [SDL 3](https://www.libsdl.org/). It plays
exactly like the Amiga original as the WHDLoad install (V1.1) runs it: the
same rules, the same random numbers, the same screens, sound and music,
including the intro, the four victory sequences and the defeat sequence.

> **You need your own copy of the game.** This repository contains none of
> the original's code, graphics, sound, music or text (the program icons,
> converted from the game's icon, are the only exception; see
> [Licence](#licence)). The program reads everything else from your copy
> when it starts. See [Getting the game data](#getting-the-game-data).

## Contents

- [Getting the game data](#getting-the-game-data)
- [Playing](#playing)
- [What the port adds for modern systems](#what-the-port-adds-for-modern-systems)
- [Building from source](#building-from-source)
- [Testing](#testing)
- [Project layout](#project-layout)
- [Licence](#licence)
- [Acknowledgements and third-party tools](#acknowledgements-and-third-party-tools)

## Getting the game data

The port needs the twelve data files of the Amiga version (`GAM1`, `GAM2`,
`INT1`, `INT2`, `Main`, `WIN0` to `WIN3`, `WIN.`, `WINM` and `DIEM`). The
launcher can extract them for you from either of these:

- the **WHDLoad install archive** `Supremacy_v1.1_1384.lha` (or a `.zip` of
  the installed folder);
- the **two original disks** as IPF images (SPS release 1384).

Plain ADF disk images of cracked releases do **not** work: their files are
packed or patched.

## Playing

1. Start the program (`supremacy.exe` on Windows, `supremacy` elsewhere). With
   no command-line arguments it opens the **launcher**.
2. The first time, press **EXTRACT FROM WHDLOAD .LHA/.ZIP OR .IPF DISKS** and
   pick your archive (or one or both IPF disks). The files are written to a
   folder called `SupremacyData` next to the program. If you already have the
   files, press **BROWSE** and pick their folder instead.
3. Press **PLAY NEW GAME**.

If your system cannot show a file picker (on Linux this needs a desktop
portal or `zenity`), the launcher says so. Click the **Game data folder**
field and type the full path of your archive or data folder instead; for an
archive, an **EXTRACT THE GAME FILES** button appears.

### The launcher

| Setting | What it does |
|---|---|
| Game data folder | Where the game's files are (BROWSE, EXTRACT). |
| Intro | On: the original title, music and opponent selection. Off: start straight away against the opponent chosen below it. |
| Game speed | Normal (the original speed, 47 game passes a second) up to twice as fast. |
| Window size | Whole multiples of 320 x 200, as large as your screen allows. |
| Full screen | Start in full screen. |
| Replay a session log | Play a recorded session again (see below), then carry on playing. |
| Load saved game | Continue one of the games saved from the in-game disk menu. |

Settings are kept in `supremacy.cfg` next to the program.

### Controls

The mouse does everything, as in the original: the **left button** selects,
the **right button** leaves a screen. Ship and planet names are typed on the
keyboard (letters, digits, space and `- = ; , . /`). **Backspace** or
**Delete** erases, **Enter** accepts and **Esc** cancels. As in the WHDLoad
version, game time stops while you type a name.

| Key | Action |
|---|---|
| F11 or Alt+Enter | Switch between window and full screen |
| Close the window | Quit (save first from the in-game disk menu) |

### Saved games and logs

- The in-game disk menu's four save slots are the files `SSG1` to `SSG4` in a
  folder called `save` next to the program. They use the same format as the
  WHDLoad version's saves.
- Every session writes `supremacy.log` next to the program: the opponent, the
  random seed and every click and key press. **Replay a session log** plays it
  back exactly, which makes a bug easy to reproduce. Please attach the log
  when you report a problem.

### Command line

```
supremacy <data folder> [options]
  --opponent N   0-3: skip the intro and play against this opponent
  --seed S       the game's random seed (skips the intro)
  --no-intro     start the game at once
  --speed P      game passes per second (default 47)
  --scale N      window size: N x 320 by N x 200
  --fullscreen   start in full screen
  --log FILE     where to write the session log
  --replay FILE  play a session log again, then carry on
  --ending N     only play an end sequence: 0-3 a victory, -1 the defeat
```

## What the port adds for modern systems

The game logic, screens and sound are translated routine by routine from the
original 68000 program and checked against the original running in an
emulator. Everything that touched the Amiga hardware directly has been
replaced. These are the additions and changes:

### Game logic

- **A C translation of the original code.** Every game routine (the daily
  simulation, economy, combat, the enemy, events, craft and cargo) is
  translated to portable C. The random-number generator is reproduced
  exactly and every call to it is kept, so a game started with the same seed
  plays out the same way as on an Amiga.
- **The original state layout.** The game state is kept in the original's
  7,000-byte block, big-endian, so saved games are compatible with the
  WHDLoad version.
- **The WHDLoad version's behaviour.** The port matches WHDLoad V1.1,
  including its two fixes (a sound-number clamp and the "can't carry" craft
  class test). Fixes for the original's other bugs are in the code but
  switched off by default (`fix_bugs`), so the game plays as it always did.
- **No original data in the program.** Tables and texts the game needs are
  read from your own copy at start-up rather than built in.

### Display and input

- **SDL 3 platform layer** instead of the Amiga's custom chips: the original
  320 x 200, 32-colour screen is rebuilt in memory and drawn each frame.
- **Window and full-screen modes** with whole-number scaling, so pixels stay
  sharp; F11 or Alt+Enter switches at any time.
- **Mouse mapping** from the window to the original 320 x 200 coordinates,
  limited to the area the original pointer could reach.
- **Quick clicks are never lost.** The original read the mouse once per game
  pass; a press and release between two passes (a fast click, a touchpad tap)
  is now held for one pass so the game always sees it.
- **Keyboard text entry** through SDL's text input, filtered to the
  characters the original accepts.

### Sound

- **A Paula emulation.** The Amiga's four-channel sound chip is emulated
  (sample playback, periods, volumes, looping) and mixed to stereo at
  44.1 kHz.
- **The original music and effects player**, translated from the 68000 code,
  drives the emulated chip, as do the separate players for the intro and the
  end sequences.

### Timing

- **A fixed game-pass clock.** On an Amiga the game's speed depended on the
  machine. The port runs the game at the measured speed of the original
  (47 passes a second) on any computer, and the launcher offers faster speeds
  up to twice that.

### Files and set-up

- **A launcher** for the settings, saved games, replays and the data folder.
- **A built-in extractor** that reads the game files straight from the
  WHDLoad `.lha` (methods `-lh0-` and `-lh5-`, CRC checked), from a `.zip`
  (stored and deflate, CRC checked) or from the original disks as IPF images
  (the disks' custom track format is decoded as the WHDLoad installer reads
  it). No Amiga, emulator or WHDLoad is needed. The extractor has been tested
  with thousands of deliberately damaged files.
- **Saved games as files** (`SSG1` to `SSG4`) instead of a save disk.
- **Session logs and replays** for reproducing problems exactly.
- **A version banner** (`$VER:` string) that names the source commit, shown
  by AmigaOS's `Version` command and written to the session log.

### Platform-specific

- **AmigaOS 4:** file and folder pickers use ASL requesters and errors use
  Intuition requesters; a Workbench icon; built with newlib.
- **Windows:** a program icon; only `SDL3.dll` (and the `libiconv-2.dll` it
  uses) are needed beside the program.
- **Linux:** builds with the system compiler against SDL 3.

## Building from source

You need a C compiler, [CMake](https://cmake.org/) 3.20 or later and SDL 3.
The steps below start from nothing on each system. Type the commands in a
terminal, one line at a time.

First get the source code:

```sh
git clone https://github.com/derfsss/Supremacy.git
cd Supremacy
```

### Windows (MSYS2)

1. Install [MSYS2](https://www.msys2.org/) and open the **MSYS2 UCRT64**
   terminal from the Start menu.
2. Update MSYS2 (the terminal may ask you to close it and open it again; if so,
   run the command a second time):

   ```sh
   pacman -Syu
   ```

3. Install the compiler, the build tools, Git and SDL 3:

   ```sh
   pacman -S --needed git mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake \
     mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-sdl3
   ```

4. Build (from the `Supremacy` folder):

   ```sh
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
   cmake --build build
   ```

5. The program is `build/supremacy.exe`. To run it outside the MSYS2
   terminal, copy `SDL3.dll` and `libiconv-2.dll` from `C:\msys64\ucrt64\bin`
   into the same folder.

### Linux (Ubuntu and Debian)

Ubuntu 25.04 and later, and Debian 13 and later, have an SDL 3 package:

```sh
sudo apt update
sudo apt install build-essential cmake ninja-build git pkg-config libsdl3-dev zenity
```

On older releases such as Ubuntu 24.04, build SDL 3 from source first. These
commands install the libraries SDL needs for graphics (X11 and Wayland) and
sound (ALSA, PulseAudio and PipeWire), and `zenity` for the launcher's
file pickers, then build and install SDL 3:

```sh
sudo apt update
sudo apt install build-essential cmake ninja-build git pkg-config \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev \
  libxfixes-dev libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev \
  libegl1-mesa-dev libgl1-mesa-dev libasound2-dev libpulse-dev \
  libpipewire-0.3-dev libdbus-1-dev libudev-dev zenity

git clone --depth 1 --branch release-3.4.18 https://github.com/libsdl-org/SDL.git
cmake -S SDL -B SDL/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build SDL/build
sudo cmake --install SDL/build
```

Then build the game (from the `Supremacy` folder):

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/supremacy
```

If the program cannot find `libSDL3.so.0` when it starts, run
`sudo ldconfig` once and try again.

### AmigaOS 4 (cross-compiled)

The AmigaOS 4 version is built on Windows, macOS or Linux with a
cross-compiler in [Docker](https://www.docker.com/). The script uses the
[`walkero/amigagccondocker:os4-gcc15`](https://hub.docker.com/r/walkero/amigagccondocker)
image, which contains GCC 15 for AmigaOS 4, newlib and SDL 3.

1. Install Docker and start it.
2. From the `Supremacy` folder, in a Bash terminal (on Windows: MSYS2 or Git
   Bash):

   ```sh
   bash tools/build_os4.sh
   ```

3. The result is `build-os4/supremacy.stripped`. Copy it to your Amiga as
   `supremacy`, together with `build-os4/supremacy.info` (the icon), and make
   it executable (`Protect supremacy +e`).

## Testing

`tools/fuzz` holds tests that build without SDL and run under
AddressSanitizer and UndefinedBehaviorSanitizer (Linux or WSL with GCC):

| Program | What it checks |
|---|---|
| `fuzz_game` | Plays whole games with random input and reports freezes and memory errors. |
| `test_evwait` | The one in-game event that waits for a YES/NO answer, from every screen it can start on. |
| `test_shows` | The intro, the five end sequences and their music. |
| `fuzz_extract` | The extractor, fed thousands of damaged archives and disk images. |

```sh
sh tools/fuzz/build_fuzz.sh
mkdir -p build-fuzz/logs
./build-fuzz/fuzz_game path/to/SupremacyData build-fuzz/logs 10 100000 1
```

Each program describes its arguments at the top of its source file.

## Project layout

| Path | Contents |
|---|---|
| `src/core` | The game logic: state, daily simulation, economy, craft, combat, enemy, events. No platform code. |
| `src/ui` | The screens, text and graphics routines, the intro and the end sequences. |
| `src/sound` | The music and effects player and the Paula emulation. |
| `src/assets` | Loading and unpacking the original data files. |
| `src/platform` | The SDL 3 front end, the launcher, the extractor and the AmigaOS 4 requesters. |
| `cmake` | The AmigaOS 4 toolchain file and the version-banner script. |
| `assets/icons` | The Windows and Workbench icons. |
| `tools` | The AmigaOS 4 build script and the tests. |

Comments in the code give the original's 68000 addresses (for example
`$4748`) for the routines and data they correspond to.

## Licence

The source code in this repository is released under the
[MIT Licence](LICENSE).

Supremacy itself (its code, graphics, sound, music and text) remains the
property of its copyright holders and is **not** covered by this licence.
This project is not affiliated with or endorsed by them. The icons in
`assets/icons` are converted from the game icon of the WHDLoad install and
are likewise not covered by the MIT Licence.

## Acknowledgements and third-party tools

**The original game**

- *Supremacy: Your Will Be Done* © 1990 Virgin Mastertronic, published by
  Melbourne House. System software by David Perry, graphics by Nick Bruty,
  design and concept by Probe Software.
- The Supremacy WHDLoad install (V1.1) by Dark Angel, whose behaviour this
  port follows, and [WHDLoad](https://www.whdload.de/) by Bert Jahn.
- The [Software Preservation Society](http://www.softpres.org/), whose IPF
  disk images preserve the original disks.

**Libraries used by the program**

- [SDL 3](https://www.libsdl.org/) (zlib licence), for graphics, sound and
  input on every platform.
- [GNU libiconv](https://www.gnu.org/software/libiconv/) (LGPL), used by
  the Windows build of SDL 3.
- The LZSS method used to unpack the game files is Haruhiko Okumura's; the
  `-lh5-` method of LHA is Haruyasu Yoshizaki's.

**Tools used to build, analyse and test the port**

- [GCC](https://gcc.gnu.org/), [CMake](https://cmake.org/) and
  [Ninja](https://ninja-build.org/).
- [MSYS2](https://www.msys2.org/), for the Windows build.
- The [adtools](https://github.com/sba1/adtools) GCC cross-compiler for
  AmigaOS 4, via the
  [walkero/amigagccondocker](https://github.com/walkero-gr/amigagccondocker)
  Docker image, and [Docker](https://www.docker.com/).
- [Ghidra](https://ghidra-sre.org/) (National Security Agency), for
  analysing the original program.
- [Capstone](https://www.capstone-engine.org/), for disassembling 68000
  code.
- [machine68k](https://github.com/cnvogelg/machine68k) by Christian
  Vogelgsang, built on the [Musashi](https://github.com/kstenerud/Musashi)
  68000 emulator by Karl Stenerud, for running the original program to check
  the port against it.
- [Python](https://www.python.org/) and [Pillow](https://python-pillow.org/),
  for the analysis and checking scripts.
- [QEMU](https://www.qemu.org/), for testing on AmigaOS 4.1 (Hyperion
  Entertainment).
- [WSL](https://learn.microsoft.com/windows/wsl/) with Ubuntu, for testing
  the Linux build.
- [Claude Code](https://www.anthropic.com/claude-code) (Anthropic), the AI
  assistant used throughout the development of this port.
