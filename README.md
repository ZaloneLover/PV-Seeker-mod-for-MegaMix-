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
