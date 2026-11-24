# xbased changelog

## 1.2.11 (2026-11)

### Fixes
- Frame rate limit: games that keep three frames ready, like Crash Bandicoot
  The Wrath of Cortex, could go over the limit on a busy PC (64-65 fps at
  Up to 60 on a laptop). Right after a frame went through, a second one could
  be let through a millisecond later before the emulated GPU had moved on
  (#411).
- Games that flip in their own interrupt handler, like Star Wars Republic
  Commando, went about one frame a second over the limit (61 fps at Up to 60):
  a frame they flipped outside that handler didn't count against it.

## 1.2.10 (2026-11)

### Fixes
- Glow and blur effects at higher resolutions: a game's blur passes, which
  sample a target at offsets of one pixel of the console's resolution, now
  run at the console's resolution, so above 1x they blur instead of breaking
  into copies. Splinter Cell Pandora Tomorrow's goggles had a grid of green
  dots round them instead of a soft halo (#412, #421).
- SoulCalibur II after loading a state: the picture went softer and slower.
  A frame buffer taller than the screen came back from the state shorter than
  it is, and the game's final filter, which reads all of it, went through
  memory at the console's resolution every frame (#418).
- Dead or Alive 3 kept its 16:9 picture only until a state was loaded, which
  brought it back in 4:3. Its shape is now read from the game at boot as well.

### Controls
- Pressure modifier: a new input under each player's buttons. While it's held,
  A, B, X, Y, Black and White are pressed at the amount set in the controller
  settings (50% by default), for games that read how hard the face buttons
  are pressed, like Metal Gear Solid 2 (#420).
- A trigger assigned to a face button presses it as far as the trigger goes,
  with the triggers' dead zone and full press applied.
- DualShock 3: its buttons' pressure goes to the game when SDL reads the pad
  with its PS3 driver (now on for Windows, with Sony's driver or DsHidMini).

## 1.2.9 (2026-11)

### Fixes
- Crazy Taxi 3: no longer drops to 30 or 20 fps after a few levels (#407).
  Its menus draw into its frame buffers from the CPU, and those buffers kept
  counting as the CPU's for the rest of the session: each time the game
  changed what it draws to (about 150 times a frame in Glitter Oasis) a
  whole frame buffer was copied back to memory, and every frame missed its
  vblank. A buffer now counts as the CPU's only while the CPU keeps writing
  it. Forza Motorsport's buffers did the same after its menus.
- The frame pacing hold (which keeps a game's frames in step with the
  display) lets go of a game it slows down: Crazy Taxi 3 waits for a vblank
  after each frame, and held it missed one more each time (20 fps). Games
  that need the hold keep it, such as GTA Vice City at its own 30 fps and
  Ninja Gaiden Black.

## 1.2.8 (2026-11)

### Fixes
- Armed and Dangerous: a level no longer starts at half its frame rate for
  its first seconds (#365). Each time a game looked for one file by name in
  a folder, the whole folder was listed, and on the hard disk with a lookup
  of every file in it: the game looks up each sound of a level in its cache
  as the level loads, about 16 ms each on Windows with the game held. The
  loading screen is shorter too.
- Grand Theft Auto III: the raindrops on the screen show in the rain
  (#389). A draw that reads the very picture it draws into now reads it
  where it asks to, as the console does: each drop shows the scene behind
  it through the water, and read at its own spot it drew the picture over
  itself, unseen.

## 1.2.7 (2026-11)

### Fixes
- Screenshots no longer freeze the picture (#401). The picture is read,
  encoded and written on a thread of its own (the screen stood still for
  about a third of a second, longer at 4K), and its notice no longer makes
  the screen redraw every refresh while it shows.
- Savestates hold the game only while it is taken (#401). The file is
  written once the game runs again, and the wait for the game's next frame
  is shorter (Republic Commando 680 ms down to 90, San Andreas about 130).
  States saved by earlier versions load as before.
- HRTF filtering off (Audio, or a game's settings): the sounds a game
  places in 3D play at the level they have with the filter, instead of 7 to
  8 dB louder (#410). Max Payne's gunshots and effects covered its dialogue.

## 1.2.6 (2026-11)

### New
- Pause when a controller disconnects (Controls, on by default, #400): if a
  player's controller goes during a game, the in-game menu opens, the game
  waits, and a notice says whose controller it was. A controller connected
  again while the menu is up is taken by the game as it closes.
- Fast forward (toggle) (#404): a second fast forward shortcut, F3 by
  default and none on the controller, that stays on until pressed again.
  While fast forward is on, by either shortcut, two arrows show in the top
  right corner of the picture.
- The in-game menu closes with the shortcut that opens it (#404): Guide on
  the controller, F1 on the keyboard, or whatever they were set to.
- LB and RB jump between the sections of a game's settings (video, audio,
  system) and between mods and cheats (#400).
- A clear warning that frame rate patches and an unlocked frame rate can
  get you banned from Insignia (#400), as one is turned on and under those
  rows, with the advice to turn on Online mode before playing online.

### Fixes
- Star Wars Republic Commando: its legal screen stays on screen while the
  game loads, instead of garbage (#398). The game keeps a copy of that
  screen to show while its graphics are released, and xbased kept showing
  the old frame buffer, which the loader was filling with its data.
- The mouse wheel moves the settings lists whatever row is selected (#400):
  a list didn't move when the selected row would have left it. The wheel
  selects the row under the pointer, and past an end, after a pause, goes
  round to the other end as the controller does (#404).
- The warnings of a setting (fast disc, 128 MB, full screen refresh,
  asynchronous shaders, the frame rate patch tip) go as soon as you leave
  their row or tab, instead of covering the next ones; they show in the
  in-game menu too, and end in a full stop in every language (#400).
- Interface sounds are only under Interface (#400), and a game's own
  controls are called "Game controls" (#404).
- The frame rate unlock no longer suggests the game's frame rate patch when
  one of its patches is on already.

## 1.2.5 (2026-11)

### Fixes
- Stereo: what a game sends to the centre or the rear speakers is heard
  again (#402). The new audio path of 1.2.4 played only the front pair in
  stereo, so Dynasty Warriors 5's officers' voice lines, which it sends to
  the centre speaker even in stereo, were lost. The centre and the rears
  now go into the front speakers as the console mixes them for its analog
  out (at -3 dB, the LFE left out); 5.1 is as it was.

## 1.2.4 (2026-11)

### New
- Sound as the console makes it (#375): the audio processor's own DSP
  program, which many games use for their mix, their reverb and the like
  and their 5.1 output, now runs recompiled to x86-64, and its speaker
  outputs are what you hear. Each game plays its own mix and effects, and
  in 5.1 every speaker gets what the game sends it.
- Jay's Magic Patcher patches (#391): a .JMP file copied into the mods
  folder shows up in its game's Mods & patches. It's offered only on the
  executable it fits (its game, the builds its CRC-32s name, every record's
  bytes there, offsets only on its region's release), and goes in whole or
  not at all.

### Fixes
- Frame pacing in games that show each frame as soon as it's drawn (Star
  Wars Republic Commando, #395): a frame that took a little longer moved
  the limiter's timing, and about once a second two frames came out almost
  together, at Original, Up to 60 and Up to 30 alike. A late frame now
  keeps its place and the next one comes on time.
- Tomb Raider: Legend: no more flashes of older frames while Lara moves.
  The rows of a picture the game wrote with the CPU are uploaded as they
  are, without the rows the GPU had drawn since.
- Box art (#385): titles with trade marks (007: NightFire), names with
  dotted acronyms (Codename: Kids Next Door) and names the libretro list
  writes with a licence's words around them or without the subtitle
  (Lotus Challenge, Wakeboarding Unleashed, Splinter Cell Double Agent,
  NightCaster) find their covers. Download cover says how it went and,
  when no cover matches, opens Choose cover image. The picker's "No covers
  match." no longer covers its From a file row. A game that had a cover
  keeps the same one.
- Screenshots: the shortcut writes a line in xbased.log, and a screenshot
  that can't be saved says so on screen, with the reason in the log,
  instead of nothing (#383).

## 1.2.3 (2026-11)

### Fixes
- Pirates: The Legend of Black Kat: the sea shows its reflections again,
  without the white edges and the colour that changed as the camera moved.
  The NV2A's offset textures (bump environment maps) read the coordinates
  as they come, without dividing them by q, and xbased divided them.
- Graphics cards whose Vulkan 1.3 driver lacks descriptor indexing (Intel
  HD Graphics 5500 on Linux's Mesa hasvk) can start games (#378): xbased
  asked for it even where it draws without it. When a game can't start for
  the graphics driver, it now stops while the message shows, instead of
  playing on with its sound.

## 1.2.2 (2026-11)

### New
- Audio: HRTF filtering (3D sound) can be turned off, globally or per game
  (#374). It's the console's filter for the sounds a game places around you;
  on by default, as before.
- Library: a game folder kept as on a modded Xbox, the game's image with an
  ISO attacher's default.xbe beside it, is added and played as that image
  (#367). Before, xbased ran the attacher, which can't load here.
- Time played leaves out the time a game is paused: the pause key, the
  in-game menu, its window minimized (#379).

### Fixes
- Frame rate: a game no longer runs too fast for a moment after a stutter,
  and its frame rate no longer drops when the PC is busy. The emulated
  console's vblank, its clock, could fall behind on a loaded PC or a laptop
  saving power, then caught up in a burst (#370). Its thread now runs at a
  high priority, it never catches up in a burst, and xbased stays out of
  Windows' power throttling, which on laptops moves programs to the slower
  cores and delays their timers.
- Frame rate limits that are a whole number of the display's refreshes (Up
  to 30 or 60 on a 60 Hz display, Unlocked at the display's refresh rate)
  show every frame for the same time. Before, some frames were shown a
  refresh early or late (Otogi at Up to 30, Crash Bandicoot at Up to 60
  without V-Sync).
- Games that present without waiting for the vblank (Total Immersion Racing
  and others) are shown evenly too: before, about one frame a second was
  shown twice and the next skipped at 60 Hz.
- 50 Hz games (European discs) at Up to 30 or Up to 60 with V-Sync no
  longer get stuck behind after a stutter: once one frame missed its
  refresh, every frame after it was shown a refresh late for the rest of the
  session, almost three times the delay from the game to the screen (Crash
  Bandicoot at Up to 60: 37 ms instead of 13).
- Up to 120 on a 60 Hz display with V-Sync shows each frame 14 ms after the
  game made it instead of 34, and its frames are more even.
- The emulated vblank no longer drifts off the display when Windows' frame
  statistics miss a refresh, which happens now and then: frames were then
  shown a refresh late for a moment.
- Games that present without waiting for the vblank are timed like the
  others: their frames went to the screen as they came, and when they came
  close to the display's refresh some were never shown (Total Immersion
  Racing: 19 in 12 s in one run, none now).
- 50 Hz games run at their real speed on 144 Hz and 240 Hz displays. Their
  vblank kept to the display's refreshes when it shouldn't have, and they
  ran at 48 fps, 4 % slow.
- 120 Hz displays show every frame for the same time: some frames were
  shown a refresh late (Otogi at Up to 30: 19 in 12 s, Crash Bandicoot at
  Up to 30: 4; none now).
- High frame rates are all shown: at Unlocked on a 280 Hz display, Crash
  Bandicoot made 253 frames a second and only 113 reached the screen; now
  208 of its 220.
- With G-Sync or FreeSync, frames under a frame rate limit reach the screen
  at exactly even times. Before, they were up to almost a millisecond off.
- Tomb Raider: Legend no longer crashes on its first screen (#376). It
  happened on the boot that sets up its cache partition on the hard disk (the
  first after playing other games): xbased wrote rows of the picture the
  game never draws over the memory right after its frame buffer, where the
  game keeps its heap.
- Dead or Alive 2 Ultimate: the palace stage's floor no longer shows white
  and blue specks along its step edges (#42). The NV2A's signed blend
  equations are now done as the console does them, with any blend factors.
- Psychonauts no longer stands on its logo on the first boot (#360). That
  boot unpacks its data to the hard disk's cache, and when the PC read the
  disc image slower than it wrote the hard disk, the copy stopped just short
  of its end. Disc and hard disk requests now complete in the order the game
  asked for them, as on the console, where both drives share one channel.

## 1.2.1 (2026-11)

- The About screen includes a link to the website, xbased-emu.com.

## 1.2.0 (2026-11)

### The GPU layer
- xbased draws with 1.0.94's GPU layer again, the last before the exact
  NV2A work of 1.0.95 to 1.1.15, with every game fix and improvement made
  since kept on it and the hybrid shaders' interpreters built for it. Games
  run 9 to 21% faster than in 1.1.15 (Vice City, The Two Thrones, Burnout 3,
  Conker, Dead or Alive 3, Driv3r, Fable, Prince of Persia) (#309).
- With fresh caches the hybrid mode builds new shaders without holding the
  game: a few milliseconds in all where 1.1.15 spent seconds (Forza, Conker,
  Halo 2, The Two Thrones, Dead or Alive 3).
- This update sets the shader mode back to Hybrid once, and the GPU
  precision setting is gone: this layer has one.

### New
- Frame rate: games without a frame cap of their own (Total Immersion
  Racing, Star Wars Republic Commando and others) keep the console's pace at
  Original instead of running at hundreds of frames a second, and the
  limits apply to them too (#351). Up to 30 fps per game, and 90 fps for the
  global limit.
- Controls: Face buttons, Xbox or Nintendo, for controllers with A on the
  right; it applies to the menus and the games (#337).
- Controls: the mouse can move player 1's left or right stick, with its own
  sensitivity, and its buttons can be assigned like keys (#255).
- Controls: Xbox Communicator, a voice headset in player 1's controller
  that uses your PC's microphone and speakers (#254).

### Fixes
- Super Monkey Ball Deluxe: no more Fatal Error as it starts (a folder of
  more than 2048 files).
- StarCraft: Ghost (the v1.256 build): its first mission loads; builds for
  the debug kit get its 128 MB.
- The Nightmare Before Christmas: Oogie's Revenge no longer stays on Now
  Loading now and then.
- Fable: the ripples on rivers and puddles no longer draw over the people
  and things in front of them.
- Ninja Gaiden Black: Ryu's shadow no longer comes and goes (hanging from a
  ledge, on the wall below him).
- Spider-Man 2: the night sky is dark with its stars again, not white
  between the clouds (games that draw in points or lines get them).
- Xbox Music Mixer: no more black screen as it starts; it reaches its menus.
  It runs part of itself in ring 3 and reads the GDT, the page tables and
  the hard disk's serial number, which the emulated console now has (#361).
- Lines keep their width at higher internal resolutions, as points already
  did; before, they stayed one pixel wide and looked thinner at each step up.
  Metal Gear Solid 2: Substance: Otacon's glasses in the codec are drawn as
  on the console again at 2x and above, not as a lattice of thin lines, and
  Ninja Gaiden Black's sparks keep their size (#350).
- Cold Fear: things a few metres from the hero cast their own shadows
  instead of black blocks that moved as he walked (#241).
- Tomb Raider: Legend: the water's reflections follow the scene again;
  after the PDA or walking about they could stick on an old frame (a white
  screen, a blurred Lara) (#185).
- Tomb Raider: Legend: no more crashes a little way into a level. The game
  loads code over its own as it goes, and the emulated CPU now notices.
- Prince of Persia: Warrior Within: the screen no longer turns white and
  stays that way after a bright flash. The game builds its glow out of
  pieces of one image, and each piece is now read from where it was drawn
  last.
- Armed and Dangerous: the second level loads instead of a dirty disc error
  after a long loading screen (#245). The kernel's memory pool now gives back
  pages with nothing in them when a game asks for a large block and finds no
  room.
- Games: hidden games can be shown again from Search (Show hidden games, as
  in Settings > Interface); hiding one says so, and the Games screen counts
  them.
- The main menu no longer comes back cut off after hiding a game (its logo's
  left half and the buttons' hints gone).

## 1.1.15 (2026-11)

### Fixes
- TOCA Race Driver 3 (and other games with DirectSound): no more random
  crashes to the menu in the middle of a race. A sound that had just
  finished could be reported to the game once more after it had let go of
  it.

## 1.1.14 (2026-11)

### Fixes
- Forza Motorsport: the sky no longer smears into white streaks in a corner
  of the screen, since 1.1.0.
- Fable: the pond's water is smooth again, without the fine bright lines
  or the grid of squares it showed since 1.1.0.

## 1.1.13 (2026-11)

### Fixes
- TOCA Race Driver 3: entering World Tour no longer crashes to a black
  screen. The game checks where its data files sit on the disc and, on
  images where they were moved, dropped the event World Tour starts with.

## 1.1.12 (2026-11)

### Fixes
- Fable: the boy's hair came out bald at the crown, a hard white halo stood
  round whoever he could talk to and the light looked washed out, since
  1.1.0. Both are back as 1.0.104 drew them.
- Games that turn on dithering into a 16-bit picture (Fable's sky) no longer
  show the light in rings at resolutions above 1x.

## 1.1.11 (2026-11)

### Interface
- Every option has a short description under the list, in all 11
  languages. In the in-game menu it also says when a change takes effect:
  right away, when you go back to the game, or the next time it starts.
- Long descriptions scroll on their own instead of being cut, and they show
  as plain text under the list, without the dark box.
- A game's settings are in sections (video, audio, system) and have their
  own volume, audio output, background mute, frame rate counter, V-Sync
  and limit for unlocked games.
- About has a Check for updates button.
- The Remove (X) hint only shows on the game folder and file rows.
- With the interface in 4:3, the in-game menu no longer squeezes the game's
  picture.
- Long option names and values get cut less, the tabs fit at 4:3 and in
  longer languages, and Japanese and Chinese text wraps properly.

## 1.1.10 (2026-11)

### Fixes
- OutRun 2006: Coast 2 Coast closed as a race loaded on some AMD GPUs
  (older GCN ones such as the Vega 3, with AMD's driver). The driver itself
  crashes building one of the game's shaders. xbased now remembers a shader
  that crashed the driver and doesn't build it again on that GPU and driver,
  so after one more close the game goes on, maybe missing that one effect.

## 1.1.9 (2026-11)

### Fixes
- The Nightmare Before Christmas: Oogie's Revenge: the game froze after its
  first cutscene, and sometimes on a black screen right at the intro. The
  timers it creates by the thousand were never freed and filled the memory,
  and its level loading took a read still in progress as finished. Both
  fixed: it now reaches the first level.
- Games that create many timers no longer slowly fill the console's memory.

## 1.1.8 (2026-11)

### Fixes
- Project Gotham Racing: the game froze on the loading screen of a New York
  quick race. It asks for a 22.6 MiB block there, and with enough
  memory free it was split by pages xbased never moved. They now move when
  nothing else makes room, once the file copies in progress are done.

## 1.1.7 (2026-11)

### Fixes
- Games wouldn't start while some programs ran in the background (a
  clipboard manager, others that hook every window): "The game's program
  (default.xbe) can't be loaded". Their DLL took the addresses where the
  game's code goes. xbased now holds those addresses from the start, so
  those programs can stay open.
- Shin Sangoku Musou 3 (Japan, Asia): the screen went black as a battle
  started. The game reads a little past the top of its stack there, which
  the console allows.

## 1.1.6 (2026-11)

### New
- **Invert the sticks** (Settings > Controls, and per game in its Controls
  screen): the right or the left stick turned over vertically,
  horizontally or both, for games with a camera that turns the other way
  and no option of their own (Jaws, Red Ninja).
- **xbased's own pointer**: a bigger green pointer in the dashboard's style,
  sharp at any display scale, on the dashboard, the in-game menu and over
  the game.
- **Clear the game cache** (Settings > System, and per game): off by
  default. Games copy files from the disc to the cache partitions (X, Y and
  Z) and keep using that copy, so a translated or modded file could go on
  showing the old one. It can clear them before each start or once now.
  Saves aren't touched.
- With a gamepad, the Settings tabs show LB and RB at their ends (they
  switch tabs), and the Back button is called Back as on the original
  controller.

### Fixes
- The mouse in games: the pointer never showed in the in-game menu and its
  options couldn't be clicked. It now shows as the mouse moves, there and
  over the game, and hides again after 3 still seconds.
- AMD graphics: if the driver crashes while it links a shader from the
  parts xbased builds ahead (an RX 6600 on driver 26.11 at Halo 2's start,
  on fast precision too), xbased builds them whole for that GPU and driver
  from the next start, as on cards without that feature.

## 1.1.5 (2026-11)

### New
- **Cover picker** (a game's Choose cover image): the box art that best
  fits the game from libretro-thumbnails, with a preview of each one before
  you pick it, a search to find another by name (keyboard or the on-screen
  one) and, as before, a picture file of your own.

### Fixes
- AMD graphics: things drawn at the same depth as what's under them went
  missing, such as Dead to Rights II's menu text.
- Covers found automatically: the file's name decides between two
  editions that share a title (Dead or Alive Ultimate's two discs), the
  disc's own region comes first, and alphas, betas and demos come last.

## 1.1.4 (2026-11)

### Fixes
- Lighter texture reads on Intel graphics: every texture read also asked
  for the texture's size, which only a stage that clamps to its edge needs.
  On Intel GPUs that query costs as much as the read itself, so games at
  higher internal resolutions kept the GPU at its limit (Midnight Club 3 at
  3x on Intel UHD, just under its 30 fps). Both precisions.
- Fast precision: cube maps read as in 1.0.104.
- The executable's file properties show its real version again (1.1.3's said
  1.1.2).

## 1.1.3 (2026-11)

### Fixes
- **Fast precision is as light as the older versions again.** Its pixel
  shaders still did per-pixel work added for the exact mode (rounding every
  step to the console's bytes, holding coordinates on texel edges, a pass
  per sample on antialiased surfaces), which made full-screen effects up to
  half again as expensive. At higher internal resolutions on integrated and
  older GPUs that was the difference between full speed and half
  (Midnight Club 3 at 3x). Fast now costs the same as 1.0.104 or less.
- Fast precision: stray lines over Vice City's buildings.
- Log lines and environment variables say xbased (`XBASED_*`; the older
  `HAKUX_*` names still work). For LAN play every machine needs 1.1.3.

## 1.1.2 (2026-11)

### New
- **GPU precision** (Settings › System, or per game): **Exact** draws with
  the console graphics chip's own arithmetic, the closest to a real Xbox and
  the heaviest on the GPU; **Fast** uses plain float shaders, as xbased did
  up to 1.0.94, much lighter on the GPU. **Automatic**, the
  default, picks Fast on AMD Polaris and Vega cards and Vega integrated
  graphics (their driver crashed building the exact shaders), on CPUs of 4
  threads or fewer, and from the next run on a GPU that crashed building a
  shader, took too long to build the exact ones or could not keep up with
  them; Exact everywhere else. The setting's help shows what Automatic
  chose last and why.
- **Cached shaders built before the game** is on by default, in both
  precisions.

### Fixes
- No more shader stutter on GPUs without pipeline libraries (AMD's Windows
  driver, older Intel drivers): the hybrid mode can't run there and fell
  back to synchronous builds, pausing the game for every new shader; it now
  builds them in the background.
- On CPUs with few threads, shader builds no longer take time from the
  game: they run below its threads and only on the threads it leaves free.

## 1.1.1 (2026-11)

### New
- **Online mode**, per game (Mods & patches): the game runs as the console
  does for playing online (Insignia, XLink Kai, System Link), at its
  original frame rate and speed, without patches, mods or cheats, and fast
  forward does nothing. Insignia bans players who use them.
- **Interface in 4:3** (Settings › Interface): the menus in a centered 4:3
  area, for bezels and frame shaders over a 4:3 picture.
- **Import saves from a folder** (Memory, Tab): saves copied from an Xbox or
  from xemu, re-signed with the keys of xemu's own files (and, for saves
  locked to another console, that console's eeprom.bin in the folder). The
  import from xemu also finds more games' signatures.
- **Discs from an NBD server**: `nbd://host:port/export` as the disc, the way
  GDOX serves a physical disc from a supported drive.
- **Command line**: `-h` / `--help`, `-c` / `--config FILE` for another
  settings file, `-i` / `--input FILE` for the disc.
- The game window keeps its place and size for the next game (Windows).

### Fixes
- Original Xbox controllers on Linux: the black and white buttons work.
- Alt+Tab out of a game no longer leaves Alt held (the F keys did nothing
  and Enter alone switched to full screen).
- The CRT TV and arcade filters' scanlines without moiré in bright areas.

## 1.1.0 (2026-11)

### New
- **Hybrid shader compilation**, the new default: a shader a game hasn't
  used before is drawn at once by interpreter shaders while its own is
  built in the background, so games no longer stutter while shaders
  compile, even on a first run with an empty cache. Settings › System ›
  New shaders also offers the asynchronous and synchronous modes. Updating
  switches every install to hybrid once; your choice after that stays.
- **Aspect ratio Auto**: 16:9 for games that render in widescreen, 4:3 for
  the rest, and save states remember each game's shape.
- **Screenshots and save state thumbnails as JPEG**, much smaller files.
- xbased.exe's file properties show the real version.

### Graphics
- Graphics chip accuracy, from the chip test program (nxdk_pgraph_tests):
  2959 of its 3556 pictures now match the console's exactly (1103 in
  1.0.104). One accurate base for every game, Direct3D titles included:
  lighting, vertex programs, fog and depth computed with the chip's own
  arithmetic; lines, points, outlines and smoothed edges; W-buffering;
  texture filtering, DXT, YUV, bump and shadow map sampling; multisampling,
  anisotropic filtering, logic ops and polygon stipple.
- Halo's water shows its reflections, Blade II's shadows are dark again,
  Hitman: Blood Money's shadows and the outline around the character's
  head are fixed, and the Insignia setup assistant no longer stays on a
  black screen.
- Performance on par with the fastest versions while more accurate.

### Frame pacing
- Smoother 30 fps games and driving in GTA: Vice City; Halo's PAL release
  paced at its own 50 Hz.
- Prince of Persia: Warrior Within keeps its own frame rate and no longer
  crashes unlocked; its frame rate setting shows Original (locked), and a
  60 fps patch still works.
- The mouse pointer hides over the game, and the picture crop cuts instead
  of stretching.

### Games
- Maximum Chase no longer crashes when the first chase begins.
- Rainbow Six: Lockdown no longer crashes while loading after the intro.
- Save states keep the screen shape they were made with (load a state
  made with an older version after starting the game once).

## 1.0.104 (2026-11)

### Fixes
- Graphics chip accuracy for homebrew that drives the chip itself, from the
  chip test program (nxdk_pgraph_tests): 1103 of its 3556 pictures now
  match the console's exactly (1091 in 1.0.103). Texture perspective
  correction can be turned off, linear and vertex fog and pass-through
  texture coordinates round as the console does, 16-bit float depth
  clears with tiny values are right, and drawing stops while the program
  takes the graphics context away.

## 1.0.103 (2026-11)

### Fixes
- Graphics chip accuracy, from the chip test program (nxdk_pgraph_tests):
  1091 of its 3556 pictures now match the console's exactly (1017 in
  1.0.102). What games can see of it:
  - V16U16 (R16B16) textures are drawn; they were white.
  - Two-sided fixed-function lighting lights the back faces with the back
    material and lights' colours.
  - With separate specular off, the lights' highlights go into the diffuse
    colour even when specular is disabled, as on the console.
  - Culling both faces draws nothing (it culled the back ones alone).
- Homebrew that drives the chip itself: textures with their own border,
  the signed blend equations, signed texture channels and bump-mapped
  texel edges now come out as on the console, and a texture rewritten
  right after a draw that used it no longer shows up in that draw.

## 1.0.102 (2026-11)

### Fixes
- Depth precision as the console stores it: W-buffered surfaces in the
  16- and 24-bit depth formats, and (homebrew that drives the chip itself)
  16-bit depth buffers, keep the depth truncated to the buffer's steps
  instead of rounded. 1017 of the chip test program's 3556 pictures now
  match the console's exactly (977 in 1.0.101).

## 1.0.101 (2026-11)

### Fixes
- Graphics chip accuracy, from the chip test program (nxdk_pgraph_tests):
  977 of its 3556 pictures now match the console's exactly (803 in 1.0.99).
  What games can see of it:
  - Wireframe and point fill modes, and line widths, are drawn as the
    console draws them (they were always filled, and lines 1 pixel wide).
  - Two-sided lighting: back faces take their own colours (unlit, none).
  - The specular alpha is 1 unless the material provides it, with vertex
    shaders too.
  - Exponential fog takes the fog parameters' bias.
  - 2D blits honour their clip rectangle, the blend with a beta factor and
    the alpha of the X8R8G8B8 surface formats.
  - Pixel shader dot mappings (signed D3D / GL, HILO hemisphere) and the
    specular reflection with a constant eye vector are the console's.
  - W-buffered surfaces with a depth offset (decals) take the console's
    offset slope.
- Homebrew that drives the chip itself: float depth buffers, the window
  clip's eight rectangles (inside or outside), the surface clip, the 2D
  line object and texel edges now behave as on the console.

## 1.0.100 (2026-11)

### Fixes
- GTA San Andreas ran at 20 fps at its original frame rate (and no higher
  than 30 unlocked) since 1.0.98: the fix for Vice City's stutter held back
  a CPU that needs most of each frame. It now only waits for the screen
  when it has time to spare. San Andreas is back to 30 fps, and Vice City
  keeps turning smoothly.

## 1.0.99 (2026-11)

### Fixes
- Fixed-function lighting: the lit colour's base (the scene ambient and the
  emission) is made as the console makes it when the material's colours
  come from the vertices, and each light's ambient takes the vertex colour
  it is set to. Some lit surfaces were too bright or the wrong tint.
- Homebrew that drives the graphics chip itself (no D3D) has its vertices
  placed on the chip's own 1/16-pixel grid: 189 more of the chip test
  program's pictures now match the console's exactly (803 of 3556).

## 1.0.98 (2026-11)

### Fixes
- Stutter in games whose world moved unevenly from one frame to the next
  even with every frame on screen on time: the emulated CPU, much faster
  than the console's, got so far ahead of the display that the game had to
  wait for room partway through a frame, and its frames then began at
  uneven times. GTA Vice City turning the camera judders no more, at the
  original frame rate, at 60 fps and at 120 fps. A game that finishes a
  frame while another is still waiting to be shown now waits for the next
  one to go on screen, at the frame's end, as on the console.
- The "up to 60" frame limit ran at 59 fps: it now gives exactly 60.

## 1.0.97 (2026-11)

### Fixes
- Frame pacing on displays with a fixed refresh rate and V-Sync (60 Hz and
  its multiples, no variable refresh rate): every frame of a 60 fps game is
  now on screen for exactly one refresh. The console's display keeps time
  with the monitor's own refreshes, as the graphics driver reports them,
  and each frame is handed over midway between two of them. Before, a frame
  was now and then shown twice and the next one never (Burnout 3: once
  every 45 seconds), and from a game's first second every frame could reach
  the screen one refresh later than it should.
- Less delay with V-Sync at 60 Hz: a frame is on screen 8 ms after it is
  handed over, where it took 15.

## 1.0.96 (2026-11)

### Fixes
- Frame pacing on Windows: each frame is shown at its own time, a fixed
  moment after the console's display would have changed to it, instead of
  whenever it reached the window. A game's frames went on screen unevenly
  spaced (a 60 fps game's 13 to 20 ms apart where every one should be
  16.7), which a variable refresh rate display shows as it comes: a
  constant fine stutter. They are now evenly spaced to within a few
  hundredths of a millisecond at 60 fps, and within a millisecond at 30 fps
  and at unlocked frame rates.

## 1.0.95 (2026-11)

The graphics chip is now checked against the chip's own test program
(nxdk_pgraph_tests), whose pictures are compared with the ones a console
makes. This update is what it took to run it, and the first rules it
corrected.

### New
- Homebrew that drives the graphics chip itself, without the console's
  graphics library, now runs: programs made with nxdk's pbkit start, draw
  and are shown, and can read back what they drew, the depth buffer
  included.

### Fixes
- Scissor rectangles follow the console's rule: one set in the first slot
  applies to all eight, and one that covers the whole picture means nothing
  is cut. A rectangle left over in another slot could cut away everything
  drawn afterwards.
- A vertex whose w is 0 is placed as the console places it, far off along
  its own direction, instead of where it stood.
- A picture a game writes into a render target reaches it after many
  different target sizes were used in a session (only the first eight
  sizes worked, the rest were skipped).
- A session that goes through more than 8192 different drawing states
  keeps drawing: the draws of any new one were dropped from there on.

## 1.0.94 (2026-11)

This update comes out of a review of the whole core: the processor's
translator, the kernel, the devices, savestates and the graphics chip.

### New
- The graphics chip follows the console more closely in a dozen rules,
  each taken from tests run on a console rather than from one game:
  - A texture's colour key is cut out, as sprites drawn over a flat
    background colour expect.
  - Textures in the L6V5U5 and L16 formats are drawn (they were skipped).
  - A flat texture where a cube map is expected, and a cube map where a
    flat texture is expected, are read as the console reads them instead
    of as white or as the cube's first face.
  - Fog whose distance is infinite or not a number follows the fog mode;
    it could cover the picture with the fog's colour.
  - Fixed-function specular highlights and spotlight cones use the
    console's own curve, and reflection maps follow the infinite viewer
    setting.
  - A draw without a colour or normal array takes the last vertex of the
    draw before it, as the console's registers do.
  - Points drawn through a vertex program have the size the game set.
  - Blending by the destination's alpha into a render target without
    alpha, and the alpha such a target is cleared to.
  - A render state given a value the console rejects keeps the value it
    had.
- Games can catch processor exceptions with their own handlers (access
  violations, divisions by zero, invalid instructions and breakpoints),
  and a group of rare instructions that used to stop a game now run.

### Fixes
- Savestates, disc images, game folders, mods and cheat files are read as
  files that may come from anywhere. A damaged or altered one is refused
  instead of writing outside xbased's folders or over its own memory.
- A savestate made with a patch longer than 256 bytes switched on loads
  again.
- Values a game hands over (vertex data, texture sizes, long paths,
  accesses at the edge of a device's registers) no longer reach past the
  emulator's own buffers.
- A picture the graphics chip drew is no longer copied back over memory
  the game has freed and given another use.
- A long session no longer slows down as the game keeps making textures.
- Vertex data at odd addresses is drawn right on drivers that are strict
  about it, and occlusion tests no longer run out of room in scenes that
  use thousands of them.

### Performance
- Release builds no longer keep internal statistics while a game runs:
  about 3 % more speed where the processor is the limit and less
  processor use everywhere else. The log is shorter for it: the lines of
  statistics every few seconds are gone, the rest is as before.

## 1.0.93 (2026-11)

### New
- Add games has an "Other location..." entry at the end of its places. It
  opens the system's own folder picker (a file picker where one file is
  asked for), where you can type a network path such as `\\server\games`
  or browse anywhere the places don't list. An extracted game's folder
  chosen there goes straight into the library.

## 1.0.92 (2026-11)

### New
- Extracted games play from their folder. A folder holding a game's
  `default.xbe` and the rest of its files is read as the disc it came
  from, and the library finds it like a disc image, with its name, icon
  and cover. Open the folder (or its `default.xbe`) from the command line
  too. Network paths such as `\\server\share\game` work as well.

### Fixes
- Breakdown no longer drops you through the floor of its first room after
  the opening cutscene. The game copies each area's data from the disc to
  the hard disk's cache, and xbased answered its writes to the cache as
  still in progress, which the game took as a failed copy. Only the first
  16 KB of the area made it, and the room loaded empty. Writes through the
  hard disk's cache now finish at once, as on the console.

## 1.0.91 (2026-11)

### Fixes
- Angelic Concert boots instead of staying on a black screen. It gives a
  background thread that measures the CPU a very high priority, and
  xbased let that thread run above everything, so the game itself never
  got to run again. Thread priorities now stay in the range the Xbox
  kernel allows.
- AMF Bowling 2004 no longer hangs on its loading screen the first time it
  loads. While it loads, the game pauses a background thread that copies
  its files to the hard disk, and that thread could be paused while it was
  waiting for a lock. xbased still handed it the lock, so the paused thread
  kept it and the game waited for it for ever. A paused thread now leaves
  the lock for the others until it runs again, as on the console.
- America's Army - Rise of a Soldier no longer closes after its intro. The
  game uses a rare 16-bit form of an instruction that saves the CPU flags
  on the stack, and xbased ran it on its own stack instead of the game's.
  That left xbased's stack slightly out of line, and it crashed soon
  after. Those instructions are now translated properly.

## 1.0.90 (2026-11)

### New
- Controller shortcuts can use Back plus a button (Back + X, Back + RB and
  so on) as well as Guide plus a button, for controllers that don't have a
  Guide button or where it opens something else. Pick them in Settings ›
  Shortcuts. The game still sees Back pressed, but not the button that goes
  with it, and Back + Start keeps opening the menu.

### Fixes
- Aquaman: Battle for Atlantis shows its intro movies instead of a black
  screen. The game writes each movie frame straight into its frame buffers
  and leaves the GPU set up for a small texture it drew before, and that
  small texture is what went on screen. Now a frame buffer the display is
  reading is shown the way the video mode lays it out.

## 1.0.89 (2026-11)

### Fixes
- Oddworld: Stranger's Wrath no longer crashes when you start a new game.
  The first time it runs, the game fills its cache on the hard disk, and
  one small file it writes and closes straight away was still being
  written in the background when the game had already moved on. Finishing
  that write later scribbled over the game's memory. Now a file's pending
  reads and writes finish before it's closed, as on the console. This had
  been broken since 1.0.49.

## 1.0.88 (2026-11)

### New
- Cheats have a section of their own in Mods & patches, under the frame
  rate and the other mods. Their names are cleaned up and written properly
  (abbreviations and misspellings spelled out, stray symbols gone, the
  player and the buttons at the end), and the common ones are translated
  into the interface's language.
- When a game has several cheat files, xbased picks the ones made for your
  disc's exact release and region by itself, and an option that two files
  both give shows only once.
- The date sits under the clock at the top of the dashboard, in the
  interface's language.

### Fixes
- The game's window no longer comes up with its title bar above the top of
  the screen. With Windows' display scaling at 125% or more, the window
  was taller than the screen; now it's made to fit the screen and the
  taskbar, on the monitor the dashboard is on, and coming back from full
  screen (Alt+Enter) keeps the title bar on screen too.
- ReShade works again. Since 1.0.82 the session's window drew the game's
  picture itself, which left ReShade nothing to draw on; with ReShade
  loaded, the game now draws in its own window as it did before.
- A game's settings changed from the in-game menu (Game settings) are no
  longer undone when you play that game again from the dashboard.

## 1.0.87 (2026-11)

### New
- The Games screen shows how long you've played each game and when you
  last played it, under the selected game's details and on its page.
- The game you played last is marked on the Games screen with a thin frame
  and a small "Last played" tag, in every view.
- Nearest neighbour upscaling (Settings > Video > Upscaling) keeps the
  picture's pixels square and sharp.

### Fixes
- Hunter: The Reckoning - Redeemer no longer stops with a disc error at its
  first level, and the soldiers' shadows are dark again instead of yellow.
- Castlevania: Curse of Darkness gets past its title screen into the game.
- Dr. Muto no longer stops right after New Game.
- The in-game menu fades out completely when it's closed, without leaving
  a tinted picture behind.

## 1.0.86 (2026-11)

### Fixes
- G-Sync and FreeSync work again in full screen on Windows, with V-Sync on
  or off. The session's window now presents the game's picture itself with
  D3D12 and a DXGI swapchain, as RetroArch does when it presents Vulkan
  through DXGI: the emulator draws into images the window made for it, and
  the driver sees an ordinary full screen game, with independent flip and
  variable refresh. Frame pacing on screen is a little steadier than in
  1.0.84.
- 1.0.84's way of handing the picture over, which left some PCs (such as
  the ROG Xbox Ally) with a black screen, is gone. If the session's window
  ever goes away, the game now goes back to the dashboard instead of
  staying open where it can't be seen, and the log says why.
- Starting a game from the dashboard and going back to it no longer show
  the desktop or the taskbar for a moment.

## 1.0.85 (2026-11)

### Fixes
- On some PCs, such as the ROG Xbox Ally, 1.0.84 showed no picture when a
  game started. The game is drawn as in 1.0.83 again while that's sorted
  out, so G-Sync and FreeSync in full screen are back to how 1.0.83 had
  them for now.
- The screen saver still doesn't start in the middle of a game played with
  a controller.

## 1.0.84 (2026-11)

### Fixes
- G-Sync and FreeSync work again in full screen on Windows, with V-Sync on
  or off (off, there was tearing). Since 1.0.82 the game's picture was
  drawn in a window inside the session's one, and the display stayed at
  its fixed refresh. The session's window now presents the picture itself,
  so the driver sees a full screen game again: independent flip and
  variable refresh, with the same frame pacing as before.
- The screen saver no longer starts in the middle of a game played with a
  controller. Since 1.0.82 the session's window let it through.
- Starting a game from the dashboard no longer shows the desktop or the
  taskbar for a moment, and neither does going back to the dashboard when
  the game closes.

## 1.0.83 (2026-11)

### Fixes
- Starting a game no longer shows the taskbar for a moment on Windows. The
  game's window asked for the foreground before it was allowed to, so
  Windows flashed its taskbar button instead.

## 1.0.82 (2026-11)

### Changes
- A game that launches another executable or reboots the console, a state
  being loaded, or a restart from the menu no longer swap the window. The
  emulator still starts again underneath, but its window now lives inside
  one window kept for the whole game session: the next run's picture comes
  up inside it, under the last one, and takes over once it has drawn. The
  taskbar button, the focus, the window's place and full screen stay as
  they were. On Windows always; on Linux under X11 (on Wayland a program
  can't hold another one's window, so there it works as before).
  `frame=0` on the command line turns it off.
- Starting a game in full screen no longer shows the desktop for a moment.

## 1.0.81 (2026-11)

### Fixes
- Modified copies of a game (same title ID, version and region) are no
  longer folded into one entry with the original. The library tells two
  dumps of the same disc (an XISO and a full image) from a changed one by
  the game's executable headers and its root folder, and shows each modded
  copy with its file's name to tell them apart. Each can be renamed or
  hidden on its own.
- Overscan now really hides the picture's edges: around a 4:3 picture in a
  wide window they spilled into the black bars instead of being cut off.

### Changes
- Crop: the sides, and the top and bottom, can be cut by a number of
  console pixels each, globally or per game (Settings › Video), for games
  whose borders aren't the same all round. Overscan goes up to 20%.
- Shortcuts can use Ctrl, Alt and Shift (Ctrl+1 and the like; the game
  doesn't get the key while a shortcut has it), and each one can also be on
  the controller: Guide plus a button, the touchpad or Share. The Guide
  button's tap still opens the menu, now when it's let go, and Share takes a
  screenshot by default.
- Uncapped games' limit (Settings › Video): a game set to Uncapped is held
  to the monitor's refresh rate by default, or 60, 120, 144, 240 fps, or no
  limit. Some games have no frame cap of their own and ran at hundreds of
  frames a second.
- Full screen refresh (Settings › Video): full screen can switch the monitor
  to its highest mode that's a multiple of 60 Hz (120 Hz on a 144 or 165 Hz
  monitor, 240 on a 280). The console's 60 Hz then lines up with every
  refresh and frames show for an even time without G-Sync or FreeSync. Off
  by default (the screen goes black for a moment when the mode switches).
- The README explains how to get on Insignia.

## 1.0.80 (2026-11)

### Fixes
- Prince of Persia: Warrior Within could crash when the Prince falls into
  the ship's hold, early in the first level. Its data arriving sooner than
  from a real drive let the game use an object it had already freed. It
  now always reads at the console drive's speed, whatever the disc speed
  setting says, and so do The Sands of Time and The Two Thrones (the same
  engine) and Star Wars: KotOR II. The setting is hidden for these games.
- A game jumping to address 0 closed the whole emulator without a word.
  It's now reported as the game's own crash in the log.

### Changes
- Disc speed is now also a global setting (System), for every game, and
  it has two choices: Like the console, the default, or Fast. Games now
  load at the console drive's speed unless you pick Fast, which shows a
  warning because some games break with it (loading screens get longer
  than in earlier versions). A game's own disc speed setting still wins
  over the global one, and the games that need the console drive's speed
  always get it. The old "Close to the console's" choice is gone: it
  became Like the console.
- Picking 128 MB of console memory shows a warning, as some games can
  crash or not start with a debug kit's memory.

## 1.0.79 (2026-11)

### Changes
- Asynchronous shaders are back as a setting (System, and per game), off by
  default. Turning them on shows a warning: while a shader is being built
  the draws that need it are skipped, so things can flicker or look broken
  for a moment, in exchange for less stutter.

## 1.0.78 (2026-11)

### Fixes
- Now and then, for a single frame, part of a scene was drawn with another
  part's camera or vertices: a view from somewhere else, large black or
  stretched polygons, a piece of a character or of the scenery flung across
  the screen (Prince of Persia: Warrior Within's ship, most often in
  fights). The memory holding a draw's vertices and constants could be
  handed to other data before the GPU drew it, when the emulator had to
  wait for the GPU while it was still preparing that draw.
- Two copies into the same image (glow effects) could run in either order.
- A texture made from a render target could show the image the target had
  before it was recreated, on the frames already being drawn.
- A rare CPU translation error when a game popped a value into memory the
  emulator was watching.

### Changes
- The Windows download is smaller: its executable no longer carries debug
  information.

## 1.0.77 (2026-11)

### Fixes
- At higher internal resolutions, glow effects built from smaller copies of
  the frame no longer draw a bright outline around characters and objects in
  front of lit backgrounds (Prince of Persia: The Sands of Time and others).
  Those small glow images now render at the console's resolution, as their
  blur expects.
- The CRT TV and arcade filters no longer show wavy moire bands at window
  sizes where each scanline covers two or three screen pixels.

## 1.0.76 (2026-11)

### New
- A Vertical sync setting (Settings, Video), on by default. Off, frames are
  shown as soon as they're ready, with tearing.

### Fixes
- The Xbox dashboard (Settings, Online) took no controller or keyboard until
  the in-game menu was opened and closed.
- The library's bottom line shows the game's region and version, and two
  games with the same name show theirs on their tiles, so different releases
  can be told apart without opening them. The game's page lists its title
  ID, version, region, publisher, year and size under the cover, no longer
  running under the menu beside it. The separator between publisher and year
  no longer shows as garbled characters.
- The on-screen keyboard's highlight follows the mouse on mice that report
  small steps. It stayed on the key the pointer had left while the click
  typed the one under it.

## 1.0.75 (2026-11)

### Changes
- The shader caches are cleared the first time a new version starts, so
  each update builds its shaders again and none kept from an older version
  is used.

## 1.0.74 (2026-11)

### Changes
- The asynchronous shaders setting is gone: shaders are always built when
  they are first drawn. Built in the background, the draws waiting for them
  were skipped, which showed as flickering or broken graphics, and it didn't
  stutter less.

## 1.0.73 (2026-11)

### Changes
- Asynchronous shaders and building cached shaders before the game are now
  off by default, and an earlier setting of either is reset once. Timed
  in-game with the shader cache cleared (Fable, Burnout 3, Vice City, Prince
  of Persia: The Two Thrones), building shaders when they are first drawn
  stuttered no more than building them in the background, while
  asynchronous shaders skipped the draws that were waiting for theirs, which
  shows as flickering or missing graphics. Turning asynchronous shaders back
  on shows a warning about that.

## 1.0.72 (2026-11)

### Fixes
- Water and other bump-mapped effects (Fable's pond, Burnout 3's lake and
  many more) are drawn as on the console. Textures whose channels the game
  marks as signed are now read and filtered as signed values, and the bump
  matrix is taken in the order the GPU uses. Before, the water showed a
  streaked or grainy pattern.

## 1.0.71 (2026-11)

### Fixes
- GTA Vice City in 480p widescreen (NTSC consoles, 720 x 480) with Trails
  on drew its night sky as a solid colour, white or yellow, and smeared the
  picture. The trails pass now draws over the frame itself.

## 1.0.70 (2026-11)

### Fixes
- A PC whose graphics card has no Vulkan driver (or one too old) ran every
  game with its sound over a black screen, the reason only in the log. It's
  said at once now, in a message that tells what to update.

## 1.0.69 (2026-11)

### Fixes
- Games that share a title ID and version with another one (prototypes,
  homebrew, modded discs: He-Man with an Aliens vs Predator homebrew) were
  one game to the library: hiding one hid both, and only one of them was
  listed. Two discs are the same game now only if their names match too;
  each one is hidden, renamed or made a favorite on its own. What was set
  before still applies until it's changed.

## 1.0.68 (2026-11)

### Fixes
- Star Wars: Republic Commando took minutes to reach its title screen and
  played its LucasArts logo at about 5 fps: while it loads it shows empty
  frames as fast as it can, thousands a second, and xbased sent every one
  of them to the screen on its own, far behind the game. Frames that come
  faster than any monitor can show, with a newer one already waiting, are
  now left out (the game still draws them all), and the title screen is up
  in about 20 seconds with the movies at their normal speed. Games at a
  normal frame rate keep every frame as before.

## 1.0.67 (2026-11)

### Fixes
- Chase: Hollywood Stunt Driver froze on the last frame of its intro: as
  its menu comes up it resets the processor's time stamp counter (WRMSR),
  an instruction xbased didn't take, and the emulation stopped there. The
  model-specific registers are written and read now, the time stamp
  counter counting from 0 again when zeroed as on the console. The game
  reaches its menu and plays.

## 1.0.66 (2026-11)

### Fixes
- Linux: crackling sound, with the emulator never short of it. xbased asked
  the sound server (PulseAudio, or PipeWire's) for a 512-sample buffer, and
  it ran the stream every 128 samples (2.7 ms): the audio thread a little
  late and the server ran dry for a moment. The buffer is 2048 samples on
  Linux now, which took Forza Motorsport's crackles away on the machine
  that had them. Still crackling: NC_APU_SAMPLES=4096 (more delay).

## 1.0.65 (2026-11)

### Fixes
- A disc for North America and Japan both (a World release) always played
  as on a North American console, whatever the language: Capcom Fighting
  Evolution's World disc stayed Capcom Fighting Evolution in Japanese,
  where a Japanese console has it Capcom Fighting Jam (the game reads the
  console's region, not its language). With the language set to Japanese,
  Korean or Chinese, such a disc now plays as on a Japanese console.

## 1.0.64 (2026-11)

### Fixes
- CHD images made with `chdman createcd` (what most guides use, even for
  DVD games) said the game's file wasn't an Xbox disc image: only
  `createdvd`'s were read, and a CD's CHD keeps each 2048-byte sector in a
  bigger CD frame. Both are read now, from an ISO or a raw .cue/.bin, the
  same bytes as the image they were made from.

## 1.0.63 (2026-11)

### Fixes
- Forza Motorsport stalled while loading from a slow drive (a USB hard
  disk): it sets the size of each of its cache files before filling them,
  and on Windows that wrote every one of the new bytes as zeros, the whole
  console waiting meanwhile (up to 23 seconds at a time, 70 in all). A
  file now takes its new size at once on any drive, its new part reading
  back as zeros as before.

### Other
- The log says which audio driver plays the sound and the period the
  device really runs at (PipeWire can run every app at a shorter period
  than xbased asks for).

## 1.0.62 (2026-11)

### Fixes
- Dave Mirra Freestyle BMX 2 crashed right after its intro, as its title
  screen came up. It reads a little past the top of its stack there (an
  index it never set), which on the Xbox lands in memory the console's
  kernel keeps over every thread's stack. Every thread's stack now has
  that memory over it too: the game reaches its menus and plays.
- The widescreen database's longer patches were cut at 96 bytes, written
  only halfway: Phantom Dust's 16:9 patch left a function half moved and
  the game closed as it started. Patterns of any length the database has
  (up to 337 bytes, 15 games' patches) are written whole now.

## 1.0.61 (2026-11)

### Fixes
- Insignia never got past the first connection on Windows: as soon as a
  game or the Setup Assistant opened a connection to the Internet, the
  network stopped (Windows refused a kind of request the network asked
  for). Insignia's Setup Assistant now registers the console, and the
  Xbox dashboard signs in to Xbox Live through Insignia.
- The gamepad works with xbased's window in the background: the menu (and
  games) take it with another window in front, with no click on xbased
  first.

### New
- Settings › Online › Open the Xbox dashboard (with Insignia chosen): the
  Xbox's own dashboard, for creating or recovering an Xbox Live account on
  Insignia. Its files (xboxdash.xbe and its folders, from your own Xbox)
  go in hdd/Partition2, the hard disk's C: drive. It starts with the tray
  empty.

## 1.0.60 (2026-11)

### Fixes
- Conflict: Desert Storm and Conflict: Desert Storm II crashed before
  showing anything. Both ask for a block of memory before the graphics
  start and write a marker just past its end. xbased handed out the very
  last bytes of the console's memory for it, so that marker landed
  outside it. The Xbox keeps the top 84 KiB of its memory for the
  graphics chip from the moment it boots, and now xbased does too: both
  games boot, reach their menus and play.

## 1.0.59 (2026-11)

### Fixes
- AMD FSR 1 filled the picture with noise and a diagonal seam on AMD
  graphics cards at 1x and 2x internal resolution (the sizes FSR scales
  up from), with FXAA on. Two of the passes that draw the picture into
  the window shared one set of inputs, and AMD's driver read the later
  one for both, so FXAA sampled the image it was drawing into. Each pass
  has its own now (the Vulkan validation layer reported the old way as an
  error, and reports nothing now).

## 1.0.58 (2026-11)

### Fixes
- Games never saw the component (HDTV) cable xbased reports: the value
  Xbox games read for the cable plugged in at boot was left empty, which
  means SCART. So the 480p, 720p and 1080i settings were dropped before
  games saw them, and a game that only goes widescreen in those modes
  stayed 4:3 and got stretched. X-Men Legends now runs at 720p in
  widescreen (or 720x480 widescreen with only 480p on), as on an Xbox,
  and other games now use the HD modes turned on in Settings.

## 1.0.57 (2026-11)

### Fixes
- Star Wars: KotOR II stayed on a black screen at the T3-M4 scene on
  Peragus, after T3's part in the fuel depot. The game needs its disc read
  at the speed of the Xbox's DVD drive there (players hit the same black
  screen on the Xbox when they played it from the hard disk). Every
  edition of KotOR II now reads its disc at the drive's speed by default.

### New
- Disc speed in a game's settings: Fast (as fast as the PC reads, the
  default), Close to the console's, or Like the console, for games that
  break when their loads finish too soon.

## 1.0.56 (2026-11)

### Fixes
- No game booted on a Windows with "Randomize memory allocations
  (Bottom-up ASLR)" turned off in Exploit protection: Windows put its own
  memory where the game's program has to go, and every game said its
  default.xbe couldn't be loaded. xbased now runs its games with that
  protection on for their own process, without changing any setting,
  and if the memory is still taken the message says what to turn on.
- The "Add games" card in the icons view was drawn at the size of a
  cover's, spilling out of its tile. It's laid out like a game's tile now.

## 1.0.55 (2026-11)

### Fixes
- A folder added while the library was still being read was never read
  at all until xbased started again. It's read now, whenever it's added.
  The same went for swapping one folder for another in Settings.
- A game whose file was moved or renamed (and the second copy of a game,
  when the copy that was shown went away) vanished from the library until
  xbased started again. Now the library follows the file.
- Reading a big library is much faster: each game's executable is no
  longer read whole for its name and icon, only the few kilobytes those
  are in. On a hard disk that took minutes for a few hundred games.
- Pressing A on one of the game folders in Settings, Interface took it
  out of the library at once. Taking a folder (or a game file) out now
  asks first.
- Games in subfolders were read once for each folder level they were
  under (a library with a folder a game read every game twice).
- Changing xbased.ini by hand while xbased was open was undone when it
  closed, as it wrote its own copy over the file. It reads the file again
  when it changes now, and a folder added there is scanned at once.

### New
- The library loads in the background and is playable meanwhile: a
  counter in the corner says how many games are in so far, and the
  selection stays on its game as others come in. A game started while it
  loads plays as usual (the library reads gently meanwhile).
- The library is kept: the next time xbased starts, every game shows at
  once, before the folders are checked again. A disk that isn't plugged
  in keeps its games for when it is.
- Search: Y (Tab, F3 or Ctrl+F) on the Games screen. Type the name with
  the keyboard or, with a gamepad, on the on-screen keyboard. It matches
  the game's name and its file's. The same screen filters by publisher,
  year and region, and sets the order and the view. B on the Games screen
  clears the search.
- Order by name (numbers first, then A to Z), by publisher or by year.
  The triggers (Z and C on the keyboard) jump to the next letter,
  publisher or year, which shows big for a moment.
- Three views: large covers (as before), small covers (about four times
  as many on the screen) and icons with names, publishers and years. The
  View button (V) switches between them.
- Favorites: Add to favorites on a game's page. They come first, A to Z,
  with a star.
- The last game played comes first.
- Both can be turned off, in the search screen or Settings, Interface.
- A game's page shows its publisher and year.

## 1.0.54 (2026-11)

### New
- A file browser of xbased's own for adding games, made for the gamepad
  and just as usable with the keyboard or the mouse. Before, adding games
  opened the system's file dialog, which a gamepad can't drive.
  - Open it from the new **Add games** card at the end of the Games grid
    (or with A when the library is empty), or from Settings, Interface.
  - On the left are your places: the library's folders, your home,
    desktop, documents and downloads folders, and every drive with its
    name, file system and free space. On Linux that's the disks your
    desktop mounts (/media, /run/media), anything mounted in /mnt and
    any other real disk, from ext4, Btrfs and XFS to FAT32, exFAT, NTFS
    and network shares.
  - On the right is the folder you're in: its folders first, then its
    disc images. Y shows the other files too. Hidden and system folders
    stay out.
  - A opens a folder, or adds a single game image to the library after
    checking it really is one. X (or the Add this folder button) adds
    the folder you're in. Adding a folder that holds folders or files
    already in the library merges them into it. Everything that's
    already in the library is marked as such.
  - B goes up a folder and keeps your place in the list, left and right
    move between the two sides, LB and RB page through long folders.
    With the mouse, a click opens, the wheel scrolls and a right click
    goes back.
  - Folders are read in the background, so a slow USB disk or network
    share doesn't freeze the dashboard, and the browser opens where you
    left it last time.
- The same browser replaces the other file dialogs too: playing a game
  file without adding it, choosing a game's cover image, and finding
  xemu.toml for importing xemu saves.

## 1.0.53 (2026-11)

### Fixes
- Test Drive no longer hangs on its loading screen before a race. The
  race executable keeps a slot for each of its data archives, and after
  reading an archive's header it closes the file but keeps the handle in
  the slot, then closes that same handle again when it frees the slot.
  xbased always gave out the lowest free handle number, so by then the
  number belonged to the game's loader thread, and the game lost its
  loader and waited for it forever while it filled up memory. Handles
  are now given out in turn around the table, so a number that was just
  closed isn't handed out again straight away.
- Test Drive's loading screen shows its Pong inside the dashboard screen
  again instead of on black. The game draws it over the menu's last frame
  without clearing it, relying on the console keeping its memory when it
  reboots into the race executable. xbased started each executable with
  clean memory; what the last one left in memory is now carried over, and
  a block the next one allocates there gets it back, as on the console.
- The frame a game keeps on screen for its next executable is now also
  saved when the game has shut its GPU channel down before the reboot.
  Thanks to BloodRaynare for the report (#88).

## 1.0.52 (2026-11)

### Fixes
- RalliSport Challenge no longer says the console doesn't have enough
  free blocks when you create a profile. Before saving one, the game
  makes a file named with a single space in its save folder. The
  console's file system takes that name, but Windows drops spaces at the
  end of a name, so the file became the save folder itself, creating it
  failed and the game took that as a full disk. Names Windows can't hold
  (ending in a space or a dot, or a device name like CON or NUL) are now
  stored on the host with those characters written as =XX, and the game
  sees them exactly as it named them. Profiles now get created and saved,
  races start, and the profile is still there after restarting. Thanks to
  Archive82-star for the report (#92).

## 1.0.51 (2026-11)

### Fixes
- FIFA World Cup: Germany 2006 no longer freezes at start on every boot
  after the first. On its first boot the game copies its data to a cache
  drive, and from then on a helper thread checks that cache while the
  game starts. The game pauses that helper by waiting for it to reach a
  safe point, giving up the CPU in the meantime, but the helper runs at a
  lower priority and the emulator only ever handed the CPU to threads of
  the same priority or higher, so it never got to run and the game waited
  forever. Giving up the CPU now lets a lower priority thread run, as on
  the console. Checked over five boots in a row with the cache in place,
  each reaching the intro. Thanks to RiasatSalminSami for the report (#89).

## 1.0.50 (2026-11)

### Fixes
- Steadier frame pacing on high refresh rate and variable refresh rate
  displays (G-Sync, FreeSync). xbased used to send each frame out on the
  first refresh of the display after it was ready, so at 60 fps on a 144
  or 280 Hz display frames went out anywhere from 8 to 26 ms apart. A
  variable refresh rate display refreshes when each frame arrives, so its
  refresh rate swung all over the place and the game stuttered even at a
  locked 60. On these displays frames now go out as soon as they're ready,
  at the game's own pace (16.7 ms apart, give or take half a millisecond),
  and the emulated console's clock no longer tries to follow refreshes
  that aren't the display's real ones. On regular 60 and 120 Hz displays
  nothing changes. Thanks to RiasatSalminSami for the report (#90).

## 1.0.49 (2026-11)

### Fixes
- xbased now copes much better with slow drives, like external USB hard
  disks and SD cards. Everything the emulated hard disk did on the host
  ran on the emulated console's own CPU, so a slow drive held the whole
  console while a game wrote to it. That's what froze Project Gotham
  Racing 2's intro for minutes while it filled its cache. Now writes go
  to a background thread like reads already did, so the game thread that
  writes waits and the rest of the game keeps running. The hard disk also
  has its own thread next to the DVD drive's, each write is a single
  request instead of one per memory page, and wiping a cache drive
  happens in the background. Files on the cache drives (X, Y and Z) are
  also no longer forced to disk one by one as they're closed, since the
  console treats those as disposable anyway. Saves still are. Thanks to
  shadowflash for the report (#83).

## 1.0.48 (2026-11)

### Fixes
- Loading a savestate no longer restarts the game after a few loads.
  Every time xbased restarts itself (to load a state, or when a game
  switches to another of its executables) it handed the new run all of
  its settings again on top of the ones the new run reads anyway, so the
  list kept growing until it was full and the last entries were lost,
  among them the state to load. Some games that switch executables, like
  Chicago Enforcer, could even end up restarting over and over. Now only
  the options given on the command line are passed on, and loading the
  same state five times in a row goes back to it every time. This also
  covers loading from the in-game menu (F1). Thanks to antonio3687 for
  the report (#81).

## 1.0.47 (2026-11)

### New
- Stick anti dead zone, in Settings, Controls, Stick range, and for each
  game in its own Controls. Many Xbox games ignore the first part of a
  stick's travel themselves (they were made for the original controller's
  loose sticks), so even with the dead zone at 0 a small push does
  nothing. In Chicago Enforcer, for example, the view doesn't turn at all
  until the stick is about a third of the way over. The anti dead zone
  makes the smallest push past the dead zone start at that amount, so
  the game reacts straight away. Both stick boxes on the Stick range
  screen draw it as a red disc, next to the dead zone's grey ring.
- A game's Controls now have its own sticks (dead zone, anti dead zone,
  range, shape) and triggers, so each game can be set apart from the
  global settings. Thanks to crashmidnick25 for the report (#84).

## 1.0.46 (2026-11)

### Fixes
- Less audio stutter on Linux in games with a lot of sound going on,
  like Forza Motorsport's races. The emulated sound chip's voices were
  split between the audio thread and a helper thread, and on Linux
  waking the helper and waiting for it 1500 times a second cost more
  than the work it took over. In a Forza race the audio thread was busy
  about three times as much on Linux as on Windows on the same machine,
  and on slower laptops it couldn't keep up. On Linux the audio thread
  now does the voices itself, which takes it from around 20% busy to
  around 7% in that race. Windows keeps the helper, which pays off
  there. Thanks to saultsomy-jpg for the report and the detailed log
  (#82).

## 1.0.45 (2026-11)

### Fixes
- Chicago Enforcer no longer freezes when starting a new game. Its
  loading screen works with events it opens by name, and the emulator
  had no way to open an object by name, so it stopped right there. Named
  events, semaphores and mutants are now supported, and a name goes away
  with the last handle to it, as on the console, since the next loading
  screen waits for exactly that. The game now gets through both loads
  of the first chapter and plays. Thanks to P1Login for the report (#56).

## 1.0.44 (2026-11)

### Fixes
- The updater on Windows no longer gives up on an update that unpacked
  fine. It unpacks the new version with Windows' own tar, and on some
  Windows 10 machines tar reported an error even though every file came
  out right, so the updater stopped and opened the releases page
  instead. Now, when tar reports an error, the updater checks every
  file of the package against the download (its name and size) and
  carries on if they're all there. Thanks to shadowflash for the report
  (#76).

## 1.0.43 (2026-11)

### Fixes
- Crimson Skies: High Road to Revenge no longer goes back to the
  dashboard as it starts. Some games check that the disc is an original
  by reading its security sector, which lives in a part of the disc that
  disc images don't include, and the emulator handed back zeros. The
  emulated DVD drive now makes a security sector that passes the game's
  check (the right time stamp and checksum, signed with a key of
  xbased's own, which the emulated console uses as its public key). The
  game now boots and plays. Thanks to BlueDragon992 for the report (#80).

## 1.0.42 (2026-11)

### Fixes
- The console's language and video settings are now kept in step with
  the console's EEPROM (hdd/eeprom.bin), both ways. Before, the
  emulator wrote its own settings over the EEPROM every time a game
  started, so editing eeprom.bin (as you would in xemu) did nothing.
  Now a change made in eeprom.bin, by hand or by a game saving its
  settings, shows up in Settings, and a change made in Settings is
  written to eeprom.bin. The file also no longer picks up what was only
  meant for one game, like that game's own language. Thanks to joesan09
  and ArhumMK for the reports (#43).
- A game's own language (its Game settings) is now always kept. Games
  whose disc has an English folder but none for the chosen language
  still fall back to English when the language comes from the global
  setting, because some of them break otherwise, but choosing the
  language for the game itself now overrides that.
- In Game settings the language is either the global setting or a
  language of its own. The separate Auto there, which followed the
  interface's language rather than the global setting, is gone. In
  Settings, Auto now says which language it gives.

## 1.0.41 (2026-11)

### Fixes
- Sound no longer stutters and pops now and then, mostly while a game is
  loading. The emulator kept only one block of sound ready ahead of what
  the sound system takes at a time, so if it was a few milliseconds late
  (the computer busy with a game's loading) the sound ran dry for a
  moment and came back with a click. It now keeps two blocks ready, sized
  from what the sound system actually asks for rather than what it was
  asked for at the start. That matters most on Linux, where PulseAudio
  and PipeWire can take bigger blocks than asked: with Burnout 3 on
  PulseAudio taking 43 ms blocks, gaps went from over two thousand in
  three minutes to one or two. Sound now runs about 10 ms further
  behind the picture. Thanks to saultsomy-jpg for the report (#77).

## 1.0.40 (2026-11)

### Fixes
- XIII no longer goes to a black screen after the logos. Its engine
  catches an error in every function it passes through and throws it
  again from there, so one error at boot ends up nine exceptions deep,
  each still being handled when the next starts. The emulator kept at
  most eight per thread and stopped the game past that. It now keeps up
  to 64, and XIII reaches its menus and plays. Savestates made with older
  versions still load. Thanks to P1Login for the report (#64).
- Operation Flashpoint: Elite no longer goes to a black screen at boot.
  It writes its cache files with gathered writes (one write from many
  separate memory pages), a kernel call the emulator didn't have yet.
  It's in now, and the game reaches its menus and missions. Thanks to
  P1Login for the report (#63).
- Total Immersion Racing no longer closes the emulator at its first logo
  video. Reads a game starts in the background (overlapped reads) now
  take a little time, as the console's drive does, instead of finishing
  almost at once. The game counts on its first video read still being
  under way when it first checks, and when it wasn't, the video's sound
  was set up with no channels and the game divided by zero. Background
  reads still run four times faster than the console's DVD drive. The
  game now plays its videos and reaches its races. Thanks to P1Login for
  the report (#65).
- Hitman 2: Silent Assassin no longer goes to a black screen at boot.
  It edits the console's page tables to see its memory at a second
  address, by copying the entries the console's kernel keeps for its
  RAM. The emulator gave it page tables without those entries, so the
  copy came out empty and the game's first use of that memory failed.
  The page tables now start the way the console's do, and the game
  reaches its menus and levels. Thanks to P1Login for the report (#69).
- On Windows, a game dividing by zero now leaves a line in the log
  saying where, instead of the emulator closing without a word.

## 1.0.39 (2026-11)

### Fixes
- Sniper Elite no longer stops on its first screen the first time it
  runs. It copies its sound bank to the cache drive in writes that skip
  the file system's cache, and waits for each one to be reported as
  pending before it moves on. Those writes finished at once here, so the
  game sent the same first block again and again forever. They now pend
  as they do on the console's hard disk, the 135 MB copy finishes in a
  few seconds, and the game plays. Thanks to P1Login for the report (#62).

## 1.0.38 (2026-11)

### The interface over the game
The interface is now drawn by the game's own presenter, over the game's
picture, so the window never changes hands while you play.

- The in-game menu comes up over the paused game, blurred behind a
  translucent veil, and fades in and out. Before, the presenter let go of
  the window and another renderer drew the menu over a still picture.
- Changing the screen shape in the menu shows at once on the picture
  under it, and the picture eases into its new shape instead of jumping.
- Saving a state and taking a screenshot show a short notice over the
  game.

### Overscan
Settings > Video (and each game's settings) have an Overscan row, for any
screen shape, 4:3 as much as 16:9. As on a TV, it's the share of the
picture hidden past the screen's edges: 3% to 15%, where a CRT hid some
5 to 10% (7% and 10% are the action-safe areas). The negative values
underscan, leaving that much border. It shows at once, under the in-game
menu too. Thanks to P1Login for asking (#67).

### Transitions in and out of games
- Starting a game: the dashboard breaks into shards that fly at you, and
  the game comes in out of a white flash. Quitting to the dashboard or
  restarting breaks the game's picture the same way, and the dashboard
  comes back in out of black. Each takes half a second.
- Loading a state, or a game switching to another of its executables:
  the picture stays on screen until the new one is ready, then fades into
  it. The next window opens under the current one, which closes only once
  the new one has drawn, so there's no desktop or blank window between
  them. A window keeps its place and size.
- Settings > Interface > Transitions in and out of games turns the
  effects off.

None of this runs during gameplay. Checked against 1.0.37 on Conker's
same scene: the same frame rate unlocked (840) and at Original (59.9, one
dropped frame in each), and the same presenter timings.

## 1.0.37 (2026-11)

### Patches and cheats only for the build they were made for
A game's releases can share a title ID and still have executables built
apart (another region, a later printing), and a patch or cheat writes to
fixed places of the one it was made for. xbased now tells them apart by
the executable itself: its certificate's regions and its build time.

- The shipped IPS patches (60 FPS, widescreen, 480p) were made and tested
  on the North American releases. They're offered only on an executable
  sold there, and only where each record fits its code. Grand Theft Auto:
  Vice City's European release crashed with its 60 FPS patch, which cut
  an instruction in half; it's no longer offered there. Patches you add
  yourself are offered as before.
- Cheat files: nearly all compare the build they were made for before
  they patch anything. xbased reads those builds from each file (plain or
  XOR-obscured) and lists a file only on its own build. Burnout 3's USA
  disc lists its four trainers and none of the three for the later
  printing. 36 files that patch without checking the build are out of the
  collection, and an update deletes them while they're unchanged.

Checked in game with the frame rate at Original: the 60 FPS patches of
Driv3r, Rainbow Six 3, Prince of Persia: The Sands of Time and The Thing
go from 30 to 60, and the widescreen patches of Conker and The Sands of
Time widen the view.

## 1.0.36 (2026-11)

### Fixes
- Grand Theft Auto: San Andreas (European Classics release) crashed while
  loading with its 60 FPS patch on. The shipped patches (60 FPS,
  widescreen, 480p) were made for the USA releases and write bytes at fixed
  places of the game's executable; given to other releases of the same game
  they landed on other code (in San Andreas, half of a call). They now go
  only to the USA release each one was made for: 45 copies on other
  releases are gone, and an update deletes them from the mods folder at
  start (only while they're unchanged, never a patch of your own).

## 1.0.35 (2026-11)

### Sound
The voice processor now follows raddad772's measurements of the audio chip
on a retail console (xemu pull request #3105):

- 3D sound (HRTF): the filter taps are read as the chip's small floating
  point numbers, so 3D voices no longer come out several times too loud and
  lopsided between ears; the left and right ears go to their own routes, a
  3D voice plays mono, and new HRTF settings blend in across one frame.
- Voices are resampled with the chip's 8-point interpolation instead of a
  linear one: cleaner highs, no aliasing.
- Volume and pitch changes ramp across each frame instead of stepping (no
  clicks or buzz on envelopes), with the chip's volume curve and envelope
  timing.
- The ParaEQ and I3DL2 filters and the voice LFOs (vibrato, tremolo) work;
  the DLS2 filter has no extra input cut or distortion.
- DirectSound submixes (multipass voices) are no longer up to 42 dB too
  quiet, and a voice sharing a page number with an output buffer no longer
  goes silent.
- Savestates from earlier versions load; their voices fade back in over
  one frame.

## 1.0.34 (2026-11)

### Fixes
- Spikeout: Battle Street said its program was damaged and didn't start.
  Its executable has 398 sections and the loader took no more than 256; it
  now takes up to 512.
- That error message said hakuX where it should say xbased.

## 1.0.33 (2026-11)

### Fixes
- Rainbow Six 3 crashed back to the dashboard while loading the first
  campaign mission. At boot it rewrites the C runtime's malloc, calloc and
  free to use its own allocator, and malloc had already been translated, so
  the old version kept handing out the system heap's memory while free sent
  it to the game's allocator. Changing a range's protection (as a game does
  before and after patching its code) now drops the code translated from
  it.

## 1.0.32 (2026-11)

### Fixes
- SNK vs. Capcom: SVC Chaos went back to the dashboard after the Neo Geo
  screen. It makes thousands of small contiguous memory blocks at boot, and
  past 4096 of them every allocation failed even with plenty of memory free,
  so a disc read went into a block it never got. The kernel now holds as
  many blocks as memory has pages, so only running out of memory fails, as
  on the console.

## 1.0.31 (2026-11)

### Fixes
- StarCraft: Ghost's 2003 build froze on its title screen with 128 MB of
  console memory. D3D's push buffer went at the very top of memory, past
  the end of the area the GPU reads commands from (D3D leaves the top 20 KB
  of a development kit's 128 MB to the GPU), so its last commands never
  ran. With 128 MB those 20 KB now stay out of every allocation, as on a
  development kit.
- Development builds that import the debug monitor (xbdm.dll) no longer
  stop at the first call into it: its DmRegisterCommandProcessor is a stub
  that reports success (StarCraft: Ghost's GhostR.xbe stopped at its first
  video). Other xbdm imports are listed in the log.

## 1.0.30 (2026-11)

### Fixes
- WWE WrestleMania 21 stayed on a black screen at boot. It copies its
  1024 x 512 legal screen whole into its 640 x 480 front buffer, and the rows
  past the buffer's end ran over the commands it had just sent the GPU (and
  a page of its own code), so nothing after them ever ran. A copy whose rows
  are wider than the render target it goes into now stops at the target's
  edges, which leaves the target exactly as the console's copy does.
- Prince of Persia: The Two Thrones' menus and gameplay were washed out to
  a bright green and yellow haze. Its glow shrinks the frame in place,
  drawing each smaller copy into the corner of the same target while it
  reads the bigger one, and those reads were taken as each pixel reading
  itself, so nothing shrank and the frame was added over itself. A draw now
  reads its own target that way only where the texture is the target's
  full size.

## 1.0.29 (2026-11)

### Fixes
- Hunter: The Reckoning - Redeemer crashed at the copyright screen and
  stayed black (on every boot once its cache was on the hard disk). Its back
  buffer is a supersampled 640 x 960 one, bound before the video mode is
  set, and the two 640 x 480 front buffers cleared then were taken for
  640 x 960. Writing one back to memory ran 480 rows past its end, over the
  game's sound data. Until the video mode is known a target is now as tall
  as it's drawn, not as tall as that first buffer.

## 1.0.28 (2026-11)

### Changes
- 1.0.27's change for Prince of Persia: The Two Thrones on AMD graphics
  cards is taken back. It wasn't tested on AMD hardware and it isn't what
  draws the blocks, so images read before anything wrote them behave as in
  1.0.26 again. The blocks are still being looked into.

## 1.0.27 (2026-11)

### Fixes
- Prince of Persia: The Two Thrones on AMD graphics cards: the screen
  filled with blocks of other images over the menus and the game. Its glow
  shrinks the frame through targets down to 10x7 and samples them before
  all of them are drawn; an image read before anything wrote it held the
  memory's leftovers, which NVIDIA's drivers clear and AMD's don't. Such an
  image now reads as zeros (a depth buffer as its far plane) on every GPU.

## 1.0.26 (2026-11)

### New
- Console memory, 64 MB (retail) or 128 MB (debug kit), for all games or
  one (Settings › System, or a game's settings): betas, demos and debug
  builds made for a debug kit's 128 MB (StarCraft: Ghost's beta, Halo 2's
  E3 2003 demo and others) get it. A game marked for 64 MB keeps 64, as a
  debug kit runs it (given 128, a retail Halo drew nothing). The GPU
  reports the memory as well. On the command line: `ram=128`.
- Play a game file without adding it to the library: Settings › Interface
  › Play a game file, or drop the disc image on xbased's window.

## 1.0.25 (2026-11)

### Fixes
- 3D sound (Halo, Grand Theft Auto San Andreas and every game with
  positional effects and voices): muffled, smeared and too quiet. The
  console's HRTF filters, the head-related responses DirectSound loads
  for 3D voices, come as sign-and-magnitude bytes; they were read as two's
  complement, so a small negative tap (0x81, -1) became -127 and each
  response turned to noise across its 31 taps. Each was then divided by
  the sum of its taps' sizes, which left 3D voices 8 to 10 dB under the
  rest of the mix (dialogue among them). The taps are read as the
  hardware reads them and weighed as it does: the frontal response passes
  speech at about unity, and the sides show the head's shadow and the
  ear's resonance as they should. Savestates from before are converted as
  they load.

## 1.0.24 (2026-11)

### New
- A game with a frame rate patch of its own (60 or 120 fps, in Mods &
  patches) says so when the frame rate is unlocked: the patch is made for
  the game and keeps its speed right, where lifting the console's pace can
  break it. The frame rate option's help says so too.

## 1.0.23 (2026-11)

### New
- Cheats for 500 games: the Xbox scene's cheat files (.etm and .xbtf,
  the ones EvolutionX, Avalaunch and XBMC4Xbox ran as trainers) ship in
  `cheats/`, about a thousand of them, by title ID. A game's Mods & patches
  lists each file's options as `[cheat]` entries; switched on, the file's
  code runs once before the game's entry point and patches it, as the
  console's loader ran it. `.xbtf` files get the helper routines XBMC4Xbox
  loads for them. Your own files go in `cheats/<TITLE ID>/`.
- Over 160 widescreen, 60 fps and 480p patches ship in `mods/<TITLE ID>/`
  (icup321's and Slippy2k's XboxGraphicalPatches, matched to their games
  through xemu's title database; the 720p ones, which their authors call
  unstable, left out). They were made for the USA releases: a patch made for
  another release of a game can break it.
- Widescreen 16:9 (database) for 90 games: OgXHD's widescreen patterns,
  searched for in the disc's executable and written where they're found
  (their old bytes checked), so they hold across releases where the code is
  the same and do nothing where it isn't.

## 1.0.22 (2026-11)

### New
- CHD disc images (`chdman createdvd`, from an XISO or a full dump): in the
  library and on the command line, like an ISO, read through libchdr.
- Desktop shortcuts (a game's page › Create desktop shortcut): xbased with
  the game's image as its argument, which boots the game straight away, with
  the game's own icon. Windows: a shortcut on the desktop; Linux: a desktop
  entry there and in the applications menu. Steam and other front ends take
  the same command: `xbased "<image>"`.
- A game's page: Name (rename it; left empty, the disc's own name again),
  Choose cover image (a PNG or JPEG of your own as its box art) and Hide
  game. Settings › Interface › Show hidden games brings them back, dimmed.
- Settings › Interface › 12-hour clock (the dashboard's clock and the save
  dates).
- Downloaded content and title updates copied in by hand
  (hdd/Partition1/TDATA/<title id>/$c, $u) are signed for this console as
  the game starts. Signed for another console, or by a tool with another
  key, a game called them damaged.

### Fixes
- The frame rate counter wobbled between 58 and 62 at a steady 60: it
  counted frames in fixed half seconds, where one frame more or less is
  2 fps. It now measures the time between the frames themselves, over a
  second.

## 1.0.21 (2026-11)

### Fixes
- The King of Fighters: Maximum Impact (Maniax): a black screen at boot.
  The game closes each file by waiting on it until it is signalled, a
  file it read nothing from too. A file object's event was signalled only
  once an I/O of its own completed, so a file opened and closed unread
  was waited on forever. As on the console (the open's request completes
  on that event), a file is now signalled from its open: the game boots,
  plays its movies and reaches its menus and fights.

## 1.0.20 (2026-11)

### New
- Frame rate, per game (the game's Mods and patches): Original (the
  console's pace), Up to 60 fps, Up to 120 fps or Unlocked. The limits let
  a game draw faster than the console did but no faster than the chosen
  rate. Anything other than Original can break a game (its speed, physics
  or cutscenes), and the option's help says so. Fast forward lifts the
  limit while held. A game set to the old Unlocked stays Unlocked.

## 1.0.19 (2026-11)

### Fixes
- The Lord of the Rings: The Return of the King: walls and floors turned
  into a smear of another level's textures after playing for a while
  (a savestate loaded there showed them right). A texture is defined
  again when its memory is written, and that was seen through dirty
  page bits any other user of the same memory could take in first (another
  texture over it, a render target's copy). Each texture now also keeps
  its pages' write counts, which only grow, and is defined again when
  they change.

## 1.0.18 (2026-11)

### Fixes
- Disney's The Haunted Mansion: the cutscenes were black with their sound
  playing. The game copies each Bink frame into a frame buffer and draws it
  from there; the frame buffer's image was written back to memory as it was
  unbound, after the game had already written its next frame there (the
  command decoder runs behind the CPU), landing the older image over it.
  The CPU's writes now go into the image first.
- Settings: holding left or right on an on/off option changes it once,
  instead of repeating on to the next value.

### About
- xbased is an original Xbox emulator for Windows and Linux, developed by
  crazyriddler. Based on hakuX. The About screen, the executable's details
  and the Linux menu entry say so.

## 1.0.17 (2026-11)

### Fixes
- AMD graphics: Grand Theft Auto San Andreas' and Vice City's characters
  stretched into spikes. Their vertices are 9 bytes, 16-bit values at odd
  offsets, and AMD's vertex fetch reads a value only at an offset its size
  divides (NVIDIA's reads them anywhere). Such vertices are now copied with
  each value aligned, as they already were for Mali-G57.
- Windows: back from a game to the dashboard (the in-game menu's quit, or
  a game closing itself), the dashboard could still be left without the
  gamepad and keyboard until touched or clicked (an ROG Xbox Ally X). The
  game now brings the dashboard's window to the front itself as it ends,
  while it still has the foreground.

## 1.0.16 (2026-11)

### Fixes
- Windows: back in the dashboard after quitting a game from the in-game
  menu, the dashboard showed but had neither the keyboard nor the gamepad
  until clicked or Alt+Tabbed to. The game hid its window before handing
  the foreground on, and hidden it no longer had one to hand; it now hands
  it on first, and the dashboard takes the foreground as it comes back.

## 1.0.15 (2026-11)

### New
- Trigger calibration (Settings › Controls › Triggers, also from the in-game
  menu's settings): a bar per trigger shows its travel as you
  press it, with the dead zone and the full-press zone, beside what the
  game gets and how far it reached. Trigger dead zone (default 10%) and
  Full press at (default 100%: lower it for a trigger that no longer
  reaches its end). Past the dead zone a trigger now rises smoothly to full
  instead of jumping from nothing to a tenth.

### Fixes
- AMD graphics cards: geometry broken or missing in games with vertex
  programs (Def Jam: Fight for NY's menus black, with a stray band across
  them). AMD's drivers lack the 3-component 8- and 16-bit vertex formats;
  their 4-component forms were used, and the extra component, read past
  the array, became the position's w. It is now 1, as on the console.
- Windows: back from a game to the dashboard, or a game restarting itself
  (Crimson Skies after a mission), the window could end up behind others,
  its taskbar button flashing, as if minimized.

### Shader cache
- The compiled shaders are kept in one file per game (hle_spirv/<title
  id>.pack) instead of a file each: thousands of small files took far more
  disk than their size and were slow to open on a hard disk. The existing
  ones are packed into one file (legacy.pack) the first time, nothing to
  compile again.

## 1.0.14 (2026-11)

### Fixes
- Def Jam: Fight for NY: froze at the intro movie (black screen or a still
  frame, the game never reaching its title). Its 60 Hz input timer
  schedules each tick from the last through D3D's SetTimerCallback, which
  refuses a time already passed; one tick a frame late (the emulation held
  up a moment) and the timer stopped for good, and with it the game's
  updates. Such a tick now comes at once and the game catches up: it
  reaches the menus and plays.
- PTIMER's alarm: at a title's first alarm, its thread could start before
  the alarm was set and raise a stray alarm interrupt flag.

## 1.0.13 (2026-11)

### Fixes
- Prince of Persia: The Sands of Time: a band of stray colour (green,
  purple) down the right of the screen in the menus and in game. The game's
  glow adds four sizes of a blurred picture in one pass, each smaller than
  the image it sits in (the extra columns are never drawn); only the first
  was kept to its own size, and the others' filtering read the stale
  columns. Every texture of such a pass is now kept to its own size.

## 1.0.12 (2026-11)

### New
- Import from xemu (Memory, Y): xemu's game saves (its hard disk's E:
  UDATA and TDATA) copied into xbased and re-signed, so games accept them.
  Copied as they were, a game found them damaged: xemu's console signs
  saves with its own keys (from its BIOS and EEPROM), xbased with its own.
  xemu's settings (xemu.toml) are found where xemu keeps them, or chosen;
  its BIOS, boot ROM, EEPROM and qcow2 or raw hard disk image are only read.
  Saves whose game isn't in the library are copied but not re-signed: add
  the game and import again. xemu_import.log beside xbased lists each file.
- Add a game file (Settings, below the game folders, or Y in an empty
  Games screen): a
  disc image the folder scan doesn't find (another extension, another
  place) added on its own; it stays in the library.

### Fixes
- Halo: Combat Evolved: starting a campaign level closed the game back to
  xbased's menu. Halo lays out its game state and tags from 0x61000 to past
  16 MiB, and D3D's second page of visibility-test results has to be below
  16 MiB: the kernel kept 256 KiB of low memory it doesn't use (the Xbox
  kernel lets its start-up memory go), none was left, and the game read
  through the page it didn't get.
- Grand Theft Auto III and Vice City: a black screen at start. Their timer
  reads save the registers with the 16-bit PUSHA/POPA, which stopped the
  game; they run now.
- Quit from the in-game menu, and a game closing itself (a title opening
  the Xbox dashboard), could leave a black full-screen window that had to
  be closed by force: the process now ends at once, without waiting on the
  graphics, gamepad and audio drivers' shutdown.
- A game opening the Xbox dashboard's Xbox Live account page (creating or
  recovering an account) with no dashboard installed now says so instead
  of just closing.

### Dashboard
- Long messages and questions wrap over several lines instead of being cut.

## 1.0.11 (2026-11)

### Fixes
- Mission: Impossible - Operation Surma: a new game stopped on the "disc is
  dirty or damaged" screen. The game copies its levels to the hard disk's
  cache in the background, keeping each copy's file open and unshared until
  it is complete, and loads a level from the cache only when it can open it
  (from the disc meanwhile). Files on the hard disk now keep the share
  access their handles were opened with, as the Xbox's do: an open of a
  copy still in progress is refused, and the level loads from the disc.

### Picture
- Upscaling with AMD FidelityFX Super Resolution 1 (its EASU edge
  reconstruction and RCAS sharpening) in place of Snapdragon GSR: Settings ›
  Video › Upscaling › AMD FSR 1. A setting that chose GSR (global or a
  game's) now chooses FSR; bilinear stays bilinear.

## 1.0.10 (2026-11)

### New
- PAL games at 60 Hz by default, as a PAL console with its PAL-60 setting
  on runs them (Panzer Dragoon Orta, 50 fps before, plays at 60). Settings ›
  System › PAL games sets 50 Hz instead, for all games or one (its
  settings). A game without a 60 Hz mode stays at 50, as on the console.

### Fixes
- Driv3r: Tanner drawn with flat, skin-coloured legs and a bald head,
  where he has jeans and hair. The game's characters add
  an environment cube map through a texture stage whose coordinates their
  vertex program leaves unwritten; a PC GPU's look-up along no direction
  returned a bright texel, and the game's tint turned it into skin tone over
  the whole body. That look-up now gives black, and Tanner looks as he
  does on the console.
- Save states no longer depend on where xbased's folder is: they kept the
  full path of each file the game had open, so a state stopped loading once
  the folder moved (or reopened the old folder's files), and a Windows
  state didn't load on Linux. They now keep each file's place in the hard
  disk folder; states from earlier versions load too.
- Windows: no console windows flash as xbased starts (the update check and
  the box-art downloads ran curl in one each).
- The mouse pointer hides itself: hidden when xbased opens and during play,
  it shows when the mouse moves, and hides again after a few still seconds
  or when a key or a gamepad is used.

## 1.0.9 (2026-11)

### New
- Stick range, in Settings › Controls (and in the in-game menu's settings):
  a screen that shows both sticks live. A green square is the range the
  games expect (the Xbox controller's); a white outline is the range your
  stick reaches, as the game gets it, with its reach in percent. Rotate the
  sticks once around and adjust while watching it:
  - Range, 50% to 150%: above 100% for a worn stick that doesn't reach its
    edge, below for games that turn too quickly.
  - Shape: Square (Xbox), the default, or Round (the stick's own circle).
  - Dead zone, also here.
  While the screen is open, the stick doesn't move through the menu.

## 1.0.8 (2026-11)

### Fixes
- After a game closed, the dashboard's settings showed their defaults until
  xbased restarted, and a setting changed then wasn't saved. Reloading the
  settings file (a game may change it) lost its path on the way. The games
  themselves kept the right settings all along.
- Analog stick diagonals (Tony Hawk's Pro Skater 2x's tricks, a direction
  and a button, and other games of the time): a modern stick pushed fully
  diagonal reached only two thirds of each axis, where the Xbox controller
  reached nearly all of it, so games read a straight direction, or none. The
  stick now responds like the Xbox controller's: full deflection is full on
  both axes at any angle, and the dead zone is a circle around the centre
  instead of a band along each axis.

## 1.0.7 (2026-11)

### New
- Updates: as the dashboard starts, xbased looks for a newer release on
  GitHub and offers it: Update, Not now, or Don't ask again. Update downloads
  it and installs it over this copy (the Windows folder, or the Linux
  AppImage), then starts it; settings, saves, states and the hard disk stay
  as they are. Settings › Interface › Check for updates at startup turns the
  check back on (or off). A copy that can't replace itself opens the
  release's page instead.
- Audio output, in Settings › Audio: Stereo (the default) or 5.1 surround.
  In 5.1 the console reports Dolby Digital, the games mix for six speakers,
  and xbased plays the six channels (front, center, subwoofer, rear); on
  stereo speakers the system mixes them down. Stereo is now set whatever an
  older console image (EEPROM) had kept.

## 1.0.6 (2026-11)

### Picture
- Internal resolution up to 8x (5120x3840 for a 640x480 game), from 4x:
  5x and 6x for 4K screens, 7x and 8x as supersampling. A target too big for
  the GPU's largest image at the chosen scale renders at the largest one that
  fits. Fable plays at its full 30 fps at 8x.

## 1.0.5 (2026-11)

### Credits
- xbased is crazyriddler's Windows and Linux edition of hakuX by rfandango,
  and now says so everywhere: the About screen (in every language, with
  github.com/crazyriddler/xbased), the README, the program's version
  information on Windows and its desktop entry on Linux.

## 1.0.4 (2026-11)

### Fixes
- Fable: it stopped on a black screen as it booted. Its own memory pager
  leaves each page fault with an IRETD, which the core didn't run. Fable now
  plays.
- Games on a console set to a language their disc doesn't have (Spanish on
  Fable, The Thing, Call of Duty: Finest Hour...): they start in English, as a
  console would have them do. Swapping in the English files under the other
  language's name wasn't enough: Fable saw that language's folder missing,
  loaded half of it and crashed.
- The Thing: it stopped at boot. The kernel now has I/O completion ports
  (CreateIoCompletionPort, GetQueuedCompletionStatus,
  PostQueuedCompletionStatus), with packets from reads and writes of the
  files tied to them. The Thing plays.

## 1.0.3 (2026-11)

### Fixes
- Windows: the "disc is dirty or damaged" message in games that copy disc
  files to their cache on the hard disk (Panzer Dragoon Orta and others).
  Such a game writes each file under a temporary name and renames it while
  it is still open, which Windows refused: the cache stayed broken and the
  game reported a bad disc. Files are now opened so they can be renamed or
  deleted while open, as on the Xbox and on Linux. Panzer Dragoon Orta plays
  its stages.

## 1.0.2 (2026-11)

### Fixes
- Games that stopped forever on a loading screen while one of their threads
  waited for another of the same priority (NFL Fever 2004 before its
  matches): giving up the CPU now takes a step off a thread's wait boost, as
  on the Xbox, so the thread it waits for runs. NFL Fever 2004 plays its
  matches at full speed.

## 1.0.1 (2026-11)

Fixes for games that stopped with their "disc is dirty or damaged" message,
or closed at start.

### Fixes
- Windows: xbased in a folder whose path has an accent, an ñ or letters of
  another alphabet (`C:\Users\José\...`) put its virtual hard disk in another
  folder, or nowhere. Games that write their cache then stopped with the
  dirty-disc message, or closed. xbased now uses UTF-8 paths throughout.
- Windows: when xbased's folder can't be written (Program Files, a read-only
  copy, Windows' controlled folder access on Desktop or Documents), its data
  goes to `%LOCALAPPDATA%\xbased` instead of failing.
- Disc images trimmed too far by an image tool: reads past the image's end
  return zeros, as on the disc, instead of the dirty-disc message.
- Disc images whose directory tree an image tool left damaged: every file is
  still found.
- A disc read that fails is tried again, through a path the emulated
  console's memory protection can't refuse.
- Memory: an allocation too big to fit gives back what it took. Games that
  ask for all the memory they can get (BloodRayne 2) closed at start; they
  run now.

## 1.0 (2026-11)

The first release of xbased, crazyriddler's Windows and Linux edition of
[hakuX](https://github.com/rfandango/hakuX) by rfandango.

### Emulation
- hakuX's core on x86-64: the guest's code translated to native x86-64, its
  return addresses predicted, interrupts delivered at block boundaries.
- The Xbox kernel's thread priority boosts, so games that lean on them
  (NightFire and others) keep their frame rate.
- Frame-rate unlock, per game, that reaches hundreds of frames per second
  where the game allows it, and that sleeps instead of spinning when capped
  (low power on laptops and handhelds).
- Per-game patches as mods (higher frame rates for a set of games so far),
  toggled per game. IPS patches shared online work too: drop them into
  `mods/<TITLE ID>/` (MODS.md).
- Anisotropic filtering up to 16x, alongside FXAA, internal resolution and
  upscaling.
- Games start in the console's language, or in English when the disc doesn't
  have that language's files.
- Save states with thumbnails, 10 slots per game.

### Front end
- A console-style dashboard: animated orb, hexagons and fog, 3D transitions,
  navigation sounds (can be turned off), a clock.
- Game library with box art (libretro-thumbnails) and the discs' own icons.
- Per-game settings, mods and cheats, controls and states.
- Memory manager for the games' own saves, with their icons: export, import,
  delete.
- In-game menu (F1, or Back + Start on a gamepad): resume, states, settings,
  controls, restart, quit.
- Borderless full screen by default, like a console; Exit from the dashboard,
  with a confirmation, so it closes from a gamepad.
- Controls: any key, button or axis per input, 4 players, per-game mappings,
  configurable shortcuts; prompts follow the device in use.
- CRT filters (scanlines, TV, arcade), frame counter, volume and
  mute-in-background.
- Online: Insignia, XLink Kai and System Link.
- 11 languages, the system's by default; the console's language follows.
- Any window size, aspect ratio and DPI.

### Platforms
- Windows 10/11 x64: a portable folder, system DLLs only.
- Linux x86-64: an AppImage (glibc 2.35 or newer: Ubuntu 22.04, Debian 12,
  Fedora 36 and later); settings in `~/.local/share/xbased`.
