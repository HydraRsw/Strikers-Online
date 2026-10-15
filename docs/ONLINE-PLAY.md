# Playing online

Two players, two PCs, one match. You need:

- The same release of the game on both PCs (the game checks: different builds will not connect).
- Your own copy of Super Mario Strikers as a disc image (ISO, GCM, CISO or GCZ) or an extracted
  `files` folder.
- The address of a relay server. Someone you know may run one ([how](HOSTING-A-SERVER.md)); it looks
  like `name.ply.gg:12345`.

## First time

1. Unzip the release anywhere, for example `C:\Games\Strikers Online`.
2. Open **Mario Strikers Settings**. On **Game Files**, choose your disc image, then press **Save**.
   (Without the settings app: open `strikers.ini` in Notepad and set `data = C:/path/to/your.iso`,
   with forward slashes.)
3. Open `strikers.ini` in Notepad. In the `[netplay]` section, set the server you were given:

   ```ini
   [netplay]
   server = name.ply.gg:12345
   ```

4. Start `strikers.exe` and go past the title screen to the main menu.

## Playing a match

One of you creates the room, the other joins. It does not matter who.

**Creating the room**

1. Press **F1** and open **Play Online**.
2. Press **Create a room**. A 4-digit code appears; send it to your friend.

**Joining**

1. Press **F1**, open **Play Online**.
2. Type the code under **Join a game** and press **Join**.

When both games are in the room, the **Choose your team** screen opens by itself:

- Each player picks a captain and sidekicks. Two players cannot pick the same captain.
- The player who created the room (the host) picks the stadium and the **match rules**: length,
  computer teammates' skill, items, Super Strikes, Bowser attacks and the cheats. The other player
  sees them as they change.
- Both press **READY!**. The match starts on both PCs a second later.

Both games must be on the main menu (past the title screen) for the match to start.

## During the match

- **Goals, pauses and replays stop both games.** A countdown starts them again together.
- **Skipping a replay:** press **A** or **B** (or START). It skips for both players.
- If your friend's game goes quiet, a message says so; after 10 seconds without them you go back to
  the menu.
- **Leaving:** F1 > Play Online > **Leave the room**.

## Controls and sound

F1 > **Controls**: choose what you play with (a controller, or the keyboard) and change which button
does what. F1 > **Sound**: music, effects, commentary and overall volume. Both are remembered on your
PC; online, each player sets their own.

Keyboard keys, stick deadzones, rumble and graphics are in **Mario Strikers Settings**.

## Things to know

- **Input delay.** The game waits a few frames (20 ms each) before using a button press, so that
  both PCs have it in time. It is picked from the ping when the match starts: about 3-4 frames at
  100 ms. `delay = 2` in `strikers.ini` is the minimum.
- **Lockstep.** `rollback = 0` (the default) never guesses the other player's input: if a packet is
  late, both games wait a moment instead. Rollback (`rollback = 1` to `10`) is experimental.
- **If the games ever go out of step,** they repair themselves: the host's game is copied over in a
  fraction of a second. You may see a short hitch.

## When something goes wrong

| What you see | What to do |
| --- | --- |
| "Could not connect" | Check the `server =` address, and that the relay and its Playit tunnel are running on the host's PC. |
| Stuck on "Joining room..." | Wrong room code, or the room was closed. Ask for a new code. |
| Version mismatch in the log | Both players need the same release. Download it again. |
| The match does not start | Both games must be on the main menu, past the title screen. |
| The game crashes or misbehaves | Send `strikers-log.txt` from the game folder (before starting the game again) when you report it. |

The game writes everything that happens online to `strikers-log.txt` in its folder. If the two
games go out of step it also writes `desync-*.bin` files there; include them in a bug report.
