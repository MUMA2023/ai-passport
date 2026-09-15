<p align="right">
  <a href="playability-validation-at-player-granularity.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Validate Playability at the Player's Input Granularity

Captured while building the **Mini Golf** play on the `feature-mini-golf-game`
branch (baseline `a5c30d3`). These lessons apply to any AI Passport play whose
difficulty lives in *aiming a shot* or *hitting a timing window* — that is, any
play where the player's achievable inputs are a coarse, discrete set.

## "Geometrically solvable" is not "playable"

A course layout can look correct on screen, render correctly, pass every physics
test, and still be unwinnable for a human holding the device. Mini Golf's third
hole was exactly that. It was drawn as two water bands with a gap between them;
a solver sweeping angles at 1° resolution found clean lines through the gap and
reported the hole as solvable. But the aim control moves in
`GOLF_AIM_STEP_DEG` = 4° steps, so the player has only 90 selectable directions,
not 360. At that resolution the hole offered four usable angles, and 333 of 735
tee shots found water — roughly 45%. The hole was solvable in theory and a
coin flip in practice.

The root cause is a measurement bug, not a physics bug: **the verification swept
finer than the input device can express**, so it validated a game nobody can
play. Fixing it meant throwing away the layout and rebuilding it: widening the
gap 52 to 72 units still left 41% of shots wet; a two-sided water design was
worse; the version that finally shipped is a single pond guarding one side of
the green, and the hole was renamed to **Pond Side** to match.

## The same mistake over-reports danger, too

After the rebuild, a fine-grained sweep of Pond Side reports 53 wet tee shots
out of 685 (about 7.7%). That number is real but useless: those angles fall
between the 4° steps, so the player cannot select them. The 4°-step scan reports
**0 of 90 angles finding water**. Both measurements come from the same layout
and neither is wrong — they answer different questions.

The rule that follows: pick the granularity to match the *player*, and be
suspicious of any verdict computed at a resolution the input device cannot
produce. Fine resolution invents solutions the player cannot aim and hazards the
player cannot hit.

## Scan the default aim line as a first-class check

Mini Golf's second hole shipped briefly with a default aim angle of 330° that
pointed straight at a vertical wall. The player's very first press of OK was a
guaranteed wall hit, and nothing in the physics tests noticed, because each
individual shot behaved correctly.

The permanent guard is now a host test that walks the tee-to-cup segment for
every hole and asserts that the default line is clean:

```c
assert(golf_model_terrain_at(&model, x, y) != GOLF_TERRAIN_WATER);
// Collision expands walls by the ball radius, so the ball center must
// stay at least GOLF_BALL_RADIUS away from any wall pixel.
for (int oy = -GOLF_BALL_RADIUS; oy <= GOLF_BALL_RADIUS; oy++)
    for (int ox = -GOLF_BALL_RADIUS; ox <= GOLF_BALL_RADIUS; ox++)
        assert(golf_model_scene_pixel(&model, x + ox, y + oy) != GOLF_PX_WALL);
```

Two details make it work. The wall check samples a `GOLF_BALL_RADIUS`-sized
neighborhood rather than the single center point, because collision expands
walls outward by the ball radius — a center-only check passes while the ball is
still clipping. And the assertion is per hole, over all holes, so a new layout
cannot be added without satisfying it.

## Measure the tolerance window, not just reachability

A shot that *can* reach the green is not the same as a shot a human can time.
The power input is a meter that sweeps out and back in
`GOLF_SWING_SWEEP_MS` = 600 ms, so the fair question is how wide the good window
is, counted in the discrete notches the player can actually land on. The
measured windows after the final layout:

| Hole | On the green | Within 60 units of the cup |
| --- | --- | --- |
| 1 Side Step | 4 notches, 89%–99% | 10 notches, 69%–99% |
| 2 Dogleg | 5 notches, 85%–99% | 11 notches, 66%–99% |
| 3 Pond Side | 4 notches, 89%–99% | 10 notches, 69%–99% |

Four to five notches on the green, ten to eleven for a short putt, is a timing
game with real margin. A single-notch window would have passed every
"reachability" check and been miserable to play.

## Keep the tooling, and keep the checks

Four throwaway host programs did the work, all built against the same
dependency-free model the firmware uses:

- `aim_grid.c` — sweeps every selectable angle at the real 4° step and reports
  safe / reachable / on-green / ace / water counts per hole.
- `power_window.c` — sweeps the power meter at its real notch resolution and
  reports the on-green and near-cup windows.
- `solve_play.c` — fine-grained sweep for par reachability, used to *find*
  candidate lines before the coarse scan judges them.
- `render_preview.c` + `ppm2png.py` — renders the course to an image so the
  layout can be eyeballed.

The throwaway tools found the problems; the assertions kept them fixed. Both
matter: a tool that only runs once lets the next edit reintroduce the hole.

## Generalization for the next app

- State the player's input resolution explicitly (step size, notch count) and
  validate only at that resolution. Anything finer is a different question.
- Treat a coarse scan and a fine scan as complementary: the fine scan proposes
  candidate solutions, the coarse scan decides whether a human can execute them.
- Make "the default state is safe" an assertion. The player's first action
  should never be a forced mistake.
- Sample collision checks over the moving body's extent, not its center point.
- Measure tolerance windows in player-selectable increments, and set a floor you
  are willing to defend before shipping.
- Keep the analysis tools in the repo alongside the model, so the next layout
  change can be judged the same way.

## Related documents

- `main/golf_model.h` and `main/golf_model.c` — the dependency-free model, the
  three course layouts, and the pixel-semantics renderer the tools read.
- `tests/test_golf_model.c` — `test_default_aim_line_is_clear` and the rest of
  the layout regressions.
- `main/golf_input.h` — `GOLF_AIM_STEP_DEG` and `GOLF_AIM_COARSE_DEG`, the
  definitions of what the player can actually select.
- `docs/CHANGELOG.md` — the Unreleased Mini Golf entry, including the hole
  rebuilds.
