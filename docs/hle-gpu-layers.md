# The GPU layer: the base and the fixes on top of it

The GPU layer (`hw/xbox/hle`) is built in three layers. Keeping them apart is
what lets the base be exact for every title, Direct3D games included, and lets
a game's needs be met without touching it.

| Layer | What it is | Where | Asks which title runs? |
|---|---|---|---|
| **Base** | The NV2A: what the chip does, measured on a console | everything under `hw/xbox/hle` except the next two | never |
| **Title fixes** | What the titles that use Direct3D, or one title, need on top of the exact chip | `hw/xbox/hle/fixes/` | only here |
| **Host quirks** | What a Vulkan driver gets wrong | `hw/xbox/hle/vk/quirks.*` | no, it asks the driver |

## The base

Every rule in the base is a behaviour of the chip. The reference for it is
nxdk_pgraph_tests run on a console (the `pgraph` harness kept beside the repository: its runner, the console's
pictures and `RESULTS.md`, the log of what each rule was fitted from). The goal
is every one of its pictures identical to the console's, and since the chip is
the same under every title the rules apply to all of them: Direct3D games are
what the project is for, the test programs are only how the chip is measured.
Homebrew is not a goal and nothing is added for it.

## The rules

1. **A finding goes in the base, for every title.** A rule found from a pgraph
   picture is not conditioned on the title, on whether it links Direct3D, or on
   which build of it. If it looks like it must be (a cost, a game that
   disagrees), see 5 and 6: the answer is a fix row, not a condition in the base.
2. **Nothing outside `fixes/` asks which title runs.** No `hle_xbe_d3d_lib`,
   no title id or library build compared, no "bare title" flag, no
   `if (some title)`. `tools/native/check-layers.sh` enforces it, comments
   included (a comment that says a rule is for "titles without D3D" is how a
   rule gets gated again), and `tools/native/host-tests.sh` runs it. The
   title's id may still name files (the pipeline caches) and be logged.
3. **Describe a rule by what the chip does**, and by the console picture that
   shows it, never by who is running.
4. **Two pixel paths are not two kinds of title.** A draw with the register
   combiners programmed (`HleCmdState.psh`) goes through the chip-exact pixel
   stage (`shader/psh.c`); one without, through the fixed-function stage that
   rebuilds Direct3D's texture-stage states (`shader/ff_gen.c`). The choice is
   the draw's state, the same for every title. A rule that belongs to one path
   is conditioned on that state (the NV2A's own fog tables, for one, on `psh`).
5. **A game's problem is the base's first.** Ask whether the base is wrong. If
   a pgraph picture shows the chip does otherwise, fix the base, for everyone.
   Only when the exact behaviour cannot be what the game needs (the host cannot
   do it, or the game's content relies on a Direct3D convention the chip does
   not share) does it become a fix.
6. **A fix is a row in `fixes/fixes.c`, a bit in `fixes/fixes.h`, and a test of
   `hle_fix()` at the one place it changes what the base does.** Off is the
   chip: the code path of every other title is not edited. Each row says why
   (the game or picture that needs it), what it changes, what it costs the
   base (measured on pgraph where it can be), and when it can go. Fixes are
   named `d3d_*` (every title with the D3D library) or `title_*` (one title's id).
7. **A strategy is not a rule.** `d3d_lazy_*` are not about what is drawn: they
   skip work that Direct3D makes unnecessary because its locks and fences order
   the CPU's reads against the chip. They are valid only under Direct3D, so
   pgraph (which reads memory straight from the chip) cannot confirm them: they
   are the strategies Direct3D titles ran before the split, kept as they were.
8. **Cost is an engineering problem, not a reason to exclude.** If a base rule
   costs too much on a game's draw volume, make it cheaper with the same result
   (as the fog carry, which runs a vertex only when a draw reads it), or give
   the games a measured `d3d_*` fix that states the cost. Never skip it quietly.

## The fixes today

| Fix | On for | Reason | Costs the base |
|---|---|---|---|
| `d3d_snap_bias` | D3D | The chip truncates vertex positions to 1/16 pixel; D3D's own screen offset (0.53125) is that truncation's rounding bias. With the truncation, a decal and the surface under it (other vertices, truncated apart) tilt against each other on the host and fight (Burnout 3's road stains, Dead or Alive 2's eaves shadows). Here every position moves by the bias alone. | About 1000 pgraph pictures are exact only with the truncation (W_buffering, Shade_model, 3D_primitive, ...): the largest deviation. To go once the depth is made as the console's. |
| `d3d_lazy_targets` | D3D | The base keeps every colour target's memory for a program that reads it once the chip is idle; D3D reads through a lock or after a fence, so the front buffer's path serves the frame buffers and small or CPU-used targets are written back at unbind. | Not measurable on pgraph (it needs the chip-direct memory model). |
| `d3d_lazy_pushes` | D3D | The base waits at each PUT until the decoder has read up to it (a program writes the memory of a draw it just pushed). D3D's fences order those writes. | As above. |
| `d3d_lazy_reports` | D3D | The base waits for a visibility report where the chip wrote it. D3D polls the status word. | As above. |
| `d3d_method_runs` | D3D build 4928 | Its push buffer carries long non-incrementing runs the decoder takes whole. | None: an optimisation. |
| `d3d_lazy_zalias` | D3D | A colour target over a depth target's memory, the depth drawn after it, takes the depth's words before its first partial write in the base (a landing and a wait for the GPU). D3D places a target there once it is done with the depth and draws it anew: Fable's 128 x 128 target inside its 256 x 256 shadow map, every frame, ran at 0.586 of 1.0.104's fps. | None on pgraph (Color_zeta_overlap runs the base). To go once the copy is made on the GPU (a remapping ZREAD pass). |

`XBASED_HLE_FIXES` at start, or the live property `debug.xbased.fixes`, turns
any of them off or on for A/B: `-d3d_snap_bias`, `+all`, `-all`. A title with
no D3D library has none on, and the log says so (`title fixes: -`).

## Adding a fix

1. A bit in `fixes.h`, with the reason written as the other rows': the game or
   picture, what it changes, when it can go.
2. A row in `fixes.c` with the predicate for the titles it is on for (`d3d`, or
   a title id compare for a `title_*` row), and a line in
   `tests/hle/test_fixes.c`.
3. `hle_fix(HLE_FIX_...)` at the one place the base branches. The fix-off side
   is the base's own code, unedited.
4. A row in the table above.
5. Run `tools/native/host-tests.sh`; a title comparison anywhere else fails it.

## What the Direct3D titles took when the base stopped asking

Until this split many base rules were conditioned on "no D3D library in the
title", which kept the Direct3D games out of the pgraph work. They now run the
chip-exact rules, and none of them has been exercised on a game yet. This is
the list to regress, by area, with where to look if a game changes:

- **Vertex programs and lighting:** the transform unit's own arithmetic
  (`xflib.h`, `HLE_VSH_XF`: NaN colours, products, reciprocals), the
  lighting unit's 22-bit floats for fixed-function lighting. Cost on weak GPUs.
- **Fog:** on the combiners' pixel stage, the chip's own table modes, the
  fog value as the lighting unit makes it, truncation of the factor to a byte,
  radial fog. A program that writes no oFog takes the fog the vertex six
  before left (`pb/pbdraw.c g_fogc`, run lazily): **Halo's Bink movies** are
  drawn that way and were unfogged on purpose; check them first.
- **Points, lines, polygons:** point sizes and culling past the clip range,
  one-pixel and wide lines, smoothed lines and polygons, antialiased surfaces
  drawn per sample, polygon stipple, flat and smooth shading by the chip's
  vertex weights, quad and polygon outlines.
- **Colour and textures:** the colour byte rounded to nearest, pass-through
  texture mode, texture perspective off, signed channels over 127.5, texel
  ties and edges, textures with their own border (`BORDER_SOURCE` clear), a
  target sampled as texels of another size reading its memory.
- **Depth:** float depth buffers hold the chip's words, integer depth
  truncated to its steps, shadow compares point filtered when every stage is
  point filtered, the clip range's cull and clamp, depth memory landing at
  the guest's first access.
- **Surfaces:** the X8/X1 variants with zero spare bits keep alpha 0,
  signed blend equations, a draw into a surface with no clip width or height
  draws nothing, clears inside the surface clip, window clip rectangles, a
  target drawn over a vertex array landing before it is read, 2D blits
  dropping rows past a PGRAPH tile, `NV09F BLEND_AND`.
- **Methods:** `NV_PGRAPH_CTX_SWITCH1` written 0 after a bind drops the
  methods until it returns.

A game that regresses is not patched in the base. First decide whether the base
or the game's setup is wrong (a pgraph picture or a console capture decides);
if the base, fix it there for every title; if the exact behaviour cannot be
what the game needs, add a fix by the steps above, with the reason.
