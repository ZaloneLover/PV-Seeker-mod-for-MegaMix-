# PV Seek (DivaModLoader DLL for Mega Mix+)

Jump around inside a PV while you watch it in the PV viewer, and jump to an exact time
such as `1:23.456`.

Everything the mod finds out is written to `pvseek.log` next to the DLL. If something
does not work, that file is the first thing to look at (and to send me).

## Controls

| Input | What it does |
|---|---|
| `[` / `]` | back / forward 5 seconds |
| `Shift` + `[` / `]` | back / forward 30 seconds |
| Small "PV Seek" window | slider, exact time box (`83.5`, `1:23.5`, `1:23.456`), -30s / -5s / +5s / +30s |
| `F8` | show / hide the small window |

Step sizes are in `pvseek.ini`.

## Build and install

Type all commands in **"Developer PowerShell for VS 2022"** (Start menu). You need
[Git](https://git-scm.com/) and CMake installed too.

1. Install Visual Studio 2022 with the "Desktop development with C++" workload.
2. Get Detours (Microsoft's hooking library, MIT license). Pick one:

   **A) vcpkg (recommended).** Set it up once:
   ```
   git clone https://github.com/microsoft/vcpkg C:\vcpkg
   C:\vcpkg\bootstrap-vcpkg.bat
   C:\vcpkg\vcpkg install detours:x64-windows-static
   ```
   Then go to the PvSeek folder (the one with this README) and run:
   ```
   cd C:\path\to\PvSeek
   cmake -B build -A x64
   ```
   No other flag is needed. The build finds Detours in `C:\vcpkg\installed\...` by itself.
   If your vcpkg is somewhere else, set the `VCPKG_ROOT` environment variable, or add
   `"-DVCPKG_DIR=D:/tools/vcpkg"` (keep the quotes in PowerShell).

   **B) Prebuilt copy from DivaDllMods.** Download the
   [DivaDllMods](https://github.com/RagdollClash/DivaDllMods) repo (green "Code" button,
   "Download ZIP") and unzip it. Copy its folder `Dependencies\Detours` to
   `PvSeek\third_party\Detours`, so that `PvSeek\third_party\Detours\include\detours.h` and
   `PvSeek\third_party\Detours\lib\x64\detours.lib` exist. Then, in the PvSeek folder:
   ```
   cmake -B build -A x64
   ```
   (This is a prebuilt binary from someone else's repo. If you would rather not use that,
   use A, or build [microsoft/Detours](https://github.com/microsoft/Detours) yourself.)
3. Build:
   ```
   cmake --build build --config Release
   ```

If CMake says **"Detours was not found"**:
- The message lists every folder it looked in. Compare that with where Detours really is.
- **Delete the `build` folder and run the `cmake -B build -A x64` line again.** CMake
  remembers old results, so a second run in the same `build` folder can repeat the old error.
  (PowerShell: `Remove-Item -Recurse -Force build`)
- For option A, check that `C:\vcpkg\installed\x64-windows-static\lib\detours.lib` exists.
  If not, the `vcpkg install` line failed: run it again and read its output.
- Put the whole `-D` option in quotes when you use one in PowerShell, for example
  `"-DDETOURS_DIR=C:/path/to/Detours"`. There are reports of PowerShell dropping unquoted
  `-DCMAKE_TOOLCHAIN_FILE=...` options.
4. The result is in `build\Release\`: `PvSeek.dll`, `config.toml` and `pvseek.ini`. Copy
   these three files to `<game folder>\mods\PvSeek\`.
5. Start the game, open a PV in the PV viewer, close the game, then read
   `mods\PvSeek\pvseek.log`.

## Configure the mod

DivaModLoader must already be installed next to `DivaMegaMix.exe` (`dinput8.dll`,
`config.toml` and a `mods` folder). `mods\PvSeek\config.toml` already says
`dll = ["PvSeek.dll"]`, so the loader picks the mod up without any change.

The mod's own settings are in `mods\PvSeek\pvseek.ini`. It is plain text: open it with
Notepad and restart the game after a change.

1. Run once with the defaults (step 5 above) and open `pvseek.log`.
2. Read what the log says and fix that one thing:
   - `TickAddress does not look like the start of a function` - the default addresses do
     not fit your exe. Find your own (section "Finding the addresses" below).
   - `pv_game::restart not found` - set `RestartAddress`.
   - `MusicPlayAddress not set` - the picture will seek but the music will not. Set
     `MusicPlayAddress`.
   - `PV Viewer check failed closed` - confirm `ModeAddress` for this game build. Seeking
     is allowed only when that address reads `ViewerMode` exactly.
3. Write addresses as Ghidra shows them, in hex with `0x`, for example
   `RestartAddress=0x140123450` (an example, not a real address).
4. The other settings are the step sizes for `[` and `]` and `ShowWindow`.

## How it works

The game plays a PV by running a script (DSC) command by command. In ReDIVA you can see
that the commands that start a motion, a camera cut, a stage change or a 2D animation
already handle running "late": they work out the right start frame from
`current time - command time`. The PV data also has a time offset that is added to the PV
clock. A seek uses both:

1. Call the game's own `pv_game::restart()`. The game resets the PV to time 0.
2. Write the target time into the PV clock offset (`field_2BF38`).
3. On the next frame the game runs every command up to the target time in one go, with
   its own late-start logic. Motions, camera and 2D end up on the right frame.
4. The MUSIC_PLAY command runs again during that frame. A hook on `pv_game_music::play`
   adds `target time - time of that command` to the music start, so the audio matches.

I replay from the start on purpose. The script only moves forward and every command
changes the scene a bit, so rebuilding the state with the game's own code is safer than
moving the script pointer by hand.

## Finding the addresses

Open `DivaMegaMix.exe` in Ghidra or IDA. Ghidra's default image base is `0x140000000`,
which is what the ini expects. Put what you find into `pvseek.ini`.

**1. `TickAddress`** (the hook point). It must be a function with exactly this shape:
**takes only the `pv_game` pointer (rcx), returns a bool/int, called once per frame**.
The hook in `PvSeek.cpp` is declared as `bool f(pv_game*)`. Do **not** pick a function that
takes a float or other arguments (for example `pv_game::ctrl(float, int64)` or
`play_data_ctrl(float)`): the hook would drop those arguments and break the game. If you
need such a function, change `TickFn` in the code to match it.

ReDIVA's candidate is `pv_game::sub_14010EF00`. How to find it in your exe:
- Search for the scalar `0x2BF38` in instructions. It is used in `pv_game_pv_data::ctrl`
  together with `0x2BF30` and `0x2BF40` (`curr_time = now + field_2BF38 - field_2BF30`).
- The caller of that function is `pv_game::ctrl` (it appears twice in it).
- In `pv_game::ctrl`, look for a small call that has no arguments except `this` and whose
  result decides `if (...) { x = sub_14010F930(); end(true, true); }`. That is the one.
  It reads one setting (`give_up`) and normally returns false.

The mod checks on the first call that it really got a `pv_game` (layout check), so a wrong
pick of the right shape is caught; a wrong shape is not.

**2. `RestartAddress`** (`pv_game::restart`). In `pv_game_parent::ctrl` (outer state 2) the
game does `if (mode >= 4) restart(); else unload();`. `unload` is a very large function
and `restart` is the other branch. `restart` stops the music, reloads it (an AIX or an OGG
branch) and loops over the movie tasks. The mod tries to find it alone from `AnchorA/B`
(the one function that calls both); if that fails, the log lists the candidates it saw.

**3. `MusicPlayAddress`** (`pv_game_music::play`). Find the script interpreter
(`pv_game_pv_data::dsc_ctrl`): a huge function with a switch of about 100 cases. Case 25
(MUSIC_PLAY), in the OGG branch, calls `play(music, 4, songName, false, 0.0, 0.0, 0.0, 3.0, false)`. The
function must take 9 arguments in total (this, int, string, bool, then four floats and a
bool). If the signature differs in your exe, the hook in `PvSeek.cpp` must be changed.

**4. `ModeAddress` / `ViewerMode`.** This build checks the first `int32` of the current
`pv_game_data` global at `0x140DAE930`. The game log reported `3` while the user was in
PV Viewer, so the INI allows exactly `ViewerMode=3`. Every other value and unreadable
memory is denied. Do not use a lower-bound check here: it would also allow unrelated modes.

**5. Struct offsets.** They are hard-coded from ReDIVA's `field_XXXX` names and checked at
start-up. If the check fails, the log says "could not find pv_game_pv_data" and the mod
turns itself off. The offsets then have to be re-derived for your exe.

## Known risks and limits

- When using Future Tone UI, do not click the mod window while seeking. The game pauses when it loses focus and the
  restart's white fade cannot finish (white screen). Use the keys.
- `ForwardWithoutRestart=1` / `BackwardWithoutRestart=1` move only the PV clock (no restart, no
  white flash). The music does not follow in these modes. Forward works well. Backward is
  experimental: the script cannot run backwards, so motions, camera and 2D end up out of sync.

- `restart()` is called from inside the game's per-frame update, which is different from
  where the game calls it itself. This is the riskiest part. Use the hook point that runs
  earliest in the frame, if you have the choice.
- Particles (Glitter) and hair/cloth start fresh at the seek time instead of being warmed up.
- PVs with a video background: the script restarts the video, it may not match the new time.
- A seek causes a short hitch (restart reloads the music and the movies).
- Unpause the game before you seek. `restart` resumes the PV clock.
- Seeking forward or backwards, then pausing, then resuming causes the pv to be seeked in considerable length forward automatically, depending on how long you had it paused, ranging from tens of seconds to even minutes.

## Files

- `src/PvSeek.cpp` - the whole mod
- `CMakeLists.txt` - build
