<p align="right">
  <a href="button-event-model-and-input-translation.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Button Event Model and Input Translation

Captured while building the **Mini Golf** play on the `feature/mini-golf`
branch (baseline `a5c30d3`). These lessons apply to any AI Passport app that
reads the three hardware keys directly instead of leaning on the menu's
click-only navigation.

## One physical press delivers up to two events, at two different times

`components/bsp/src/bsp_button.c` registers four callbacks per key:
`BUTTON_PRESS_DOWN`, `BUTTON_SINGLE_CLICK`, `BUTTON_DOUBLE_CLICK`, and
`BUTTON_LONG_PRESS_START`, forwarded to the app as `BSP_BTN_PRESS`,
`BSP_BTN_CLICK`, `BSP_BTN_DOUBLE`, and `BSP_BTN_LONG`. They do not arrive
together and they are not alternatives:

- `PRESS` fires the moment the key goes down.
- `CLICK` fires on release, if the press was short.
- `LONG` fires when the long-press threshold is reached, which is *before*
  release.

So a single long press produces `PRESS` and then `LONG`; a single short press
produces `PRESS` and then `CLICK`. Acting on `PRESS` **and** on the follow-up
event makes one press count twice. This has a consequence worth stating plainly:
for the same key, **"respond the instant the key goes down" and "tell a short
press apart from a long press" are mutually exclusive**. By the time you know it
was a long press, you have already committed to the press.

Mini Golf hit all three shapes of this trap:

- **A long aim press rotated 36°, not 32°.** `PRESS` applied the 4° fine step
  and `LONG` applied the 32° coarse step, so the total was 4 + 32. Fix: `LONG`
  only tops up the difference, `GOLF_AIM_COARSE_DEG - GOLF_AIM_STEP_DEG` = 28°,
  making one long press exactly one coarse step.
- **A long OK press flashed a power step before the framework's "leave the
  page" gesture fired.** The swing meter sweeps a 600 ms round trip and the
  on-green window is only tens of milliseconds wide, so striking must happen on
  `PRESS` — waiting for `CLICK` makes the timing game unplayable. But `PRESS`
  arrives before `LONG_PRESS_START`, so the OK long-press that the menu-era
  `main.c` consumed was briefly read as a strike.
- **The trailing `CLICK` advanced the hole.** After a strike, the release still
  emitted `SINGLE_CLICK`, which the page read as "the ball is sunk, show the next
  hole".

## Write the translation rules down as a pure-logic layer

The fix was not three local patches. It was extracting every key rule into
`main/golf_input.{h,c}` — no ESP-IDF, no LVGL, only integer state — so host
tests can drive event sequences directly and assert on the resulting model
state. The rules, stated once:

| Key | Event | Action |
| --- | --- | --- |
| UP / DOWN | `PRESS` | rotate `±GOLF_AIM_STEP_DEG` (4°) |
| UP / DOWN | `LONG` | rotate `±(GOLF_AIM_COARSE_DEG - GOLF_AIM_STEP_DEG)` (±28°) |
| OK | `PRESS` in the power phase | strike |
| OK | `CLICK` in any other phase | phase action, but drop a trailing click |

Two mechanisms make this work. `accept_once()` collapses `PRESS` + trailing
`CLICK` into one action: `PRESS` is recorded and acted on, and a `CLICK` that
lands within `GOLF_PRESS_CLICK_WINDOW_MS` (400 ms) of a recorded `PRESS` is
discarded. `click_is_trailing()` answers the same question for phases that act
on `CLICK`, so the release after a strike cannot advance the hole. Both are
deliberately conservative: a `CLICK` with no preceding `PRESS` still works, so
a component version that only reports clicks degrades to a usable app instead
of a dead page.

## Per-key bookkeeping is mandatory, and the tests will catch you forgetting

The state is two small arrays indexed by key: `press_seen[]` and
`last_press_ms[]`. Every branch that acts on a `PRESS` must also write both,
including branches that do not otherwise need them.

The tests caught exactly this omission within minutes of the extraction. The
`PRESS` branch of `handle_aim_key()` rotated the aim but did not record
`press_seen`, so the `CLICK` that arrived on release fell into the
"no `PRESS` was seen, so this must be a click-only component" fallback and
stepped a second notch. `test_short_press_steps_once` failed on the first run.
Without the host-testable layer this would have shipped as an intermittent
"sometimes the aim jumps two steps" report.

## OK long-press: claimed by the menu, then dropped along with it

In a menu-based firmware `main/main.c` intercepts `BSP_BTN_OK` + `BSP_BTN_LONG`
globally to leave a page and return to the menu, so a page never sees that
event. That was the case while Mini Golf was a menu entry, and it is why the
play never claimed OK long-press.

The play later shipped as a single-play firmware with no menu at all, so
`main.c` no longer intercepts anything and OK long-press is simply unclaimed.
The play still does not use it: the gesture has to behave the same whether or
not a menu exists, and "cancel this shot" already sits on UP long-press in the
power phase, which is the gesture that is naturally free there. Treat a
framework-level gesture as unavailable even after the framework stops claiming
it — otherwise the page's behavior silently depends on which entry point it was
built with.

## Generalization for the next app

- `PRESS` always arrives first; exactly one of `CLICK` or `LONG` follows. Never
  act on both `PRESS` and its follower without an explicit de-duplication rule.
- Decide per key, per phase, whether you want immediacy or discrimination. You
  cannot have both on one key.
- Keep a "seen the press" flag per key and write it on every `PRESS` branch,
  even the ones that look self-contained.
- Assume the release may emit a stray `CLICK` after a state-changing press, and
  filter it by a time window rather than by hoping it does not come.
- Treat `OK` long-press as unavailable: a menu-based entry point claims it, a
  single-play entry point does not. Budget the other gestures accordingly, and
  consider what happens when a key must do two jobs in different phases.
- Put the whole translation in a dependency-free module with host tests. The
  bugs here are timing bugs, and timing bugs are only cheap to find in tests.

## Related documents

- `components/bsp/src/bsp_button.c` — the four registered callbacks and their
  event mapping.
- `main/golf_input.h` and `main/golf_input.c` — the translation layer and its
  rule comment block.
- `main/demo_golf.c` — the thin `map_key()` adapter from `bsp_btn_t` /
  `bsp_btn_ev_t` to the model's key vocabulary.
- `tests/test_golf_input.c` — nine event-sequence tests, including the
  single-step and trailing-click regressions.
- `main/main.c` — the entry point: it intercepts OK long-press while a menu
  exists, and enters the play directly once the menu is dropped.
