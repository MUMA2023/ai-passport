# Desk Pal — a companion that lives on your desk

English | [简体中文](README.zh_CN.md)

Desk Pal is a firmware application for the FoloToy AI Passport. It turns the
device into a small companion that sits on your desk: it breathes, wanders a
little, blinks, gets hungry, gets sleepy, and keeps track of how long you two
have been together.

The device boots straight into the pet — there is no launcher and no menu. The
on-screen interface is in Simplified Chinese.

## What it does

- **A companion, not a menu.** Power on and the pet is already there.
- **It has a mood.** The face, the status line and the sound all follow the
  current mood, so you can tell at a glance whether it is content, curious,
  happy, excited, hungry, sleepy, lonely, or asleep.
- **Three things you can do.** Pet it, feed it, or play with it. Each one is
  rewarded differently, and each one can be refused — the pet says why.
- **It remembers.** Bond points, counters, hunger, energy, mood-relevant timers
  and the clock are written to NVS, so unplugging the device does not reset your
  relationship. Saves are double-buffered and CRC-checked, so a power cut during
  a write cannot corrupt the record.
- **It gets tired on its own.** Energy drains while it is awake. When energy runs
  low the pet falls asleep, recovers faster than it drains, and wakes up by
  itself. Hunger decays while awake and pauses while asleep.
- **A clock and a battery gauge.** The clock sits at the top left and the battery
  gauge at the top right. When the battery gauge cannot be read, it is hidden
  rather than showing an invented number.
- **Sound feedback.** Boot, petting, feeding, playing, getting hungry, falling
  asleep, waking up, cursor moves, confirmations and refusals each have their own
  short jingle. The tones are synthesized in code, so the firmware ships no audio
  asset files.

## Controls

| Key | Press | Effect |
| --- | --- | --- |
| Up | Short | Pet it. Adds bond, but only once every 5 seconds. |
| Down | Short | Feed it. Restores hunger; refused when it is already full. |
| OK | Short | Play with it. Costs energy and a little hunger; refused when too tired or too hungry. |
| OK | Long | Open the status page. |

| Page | Key | Effect |
| --- | --- | --- |
| Status | OK short | Open the clock page. |
| Status | OK long | Return to the pet. |
| Clock | Up / Down short | Move the clock by one minute. |
| Clock | OK long | Save and return to the pet. |

## How the pet behaves

The behaviour is a pure state machine in
[`main/pet_model.c`](main/pet_model.c) that never touches hardware, so it can be
tested on a host machine. The numbers below are the tunables, all named in
[`main/pet_model.h`](main/pet_model.h).

| Quantity | Value |
| --- | --- |
| Starting hunger / energy | 70 / 80 |
| Hunger decay | 1 point per 90 s while awake |
| Energy drain | 1 point per 120 s while awake |
| Energy recovery | 1 point per 20 s while asleep |
| Falls asleep at | energy 15 or below |
| Wakes up at | energy 90 or above |
| Feels lonely after | 600 s without interaction |
| Feeding | hunger +35, refused at hunger 85 or above |
| Playing | energy -18, hunger -5, needs energy 25+ and hunger 20+ |
| Bond per pet / feed / play | +3 / +2 / +5 |
| Bond levels | stranger 0, familiar 20, close 60, best 120 |

Interactions are refused while the pet is asleep, so the pet cannot be woken by
mashing buttons — it wakes on its own schedule.

## Persistence

State lives in the NVS namespace `desk_pal` as two alternating slots, `slot_a`
and `slot_b`. Every save writes the other slot with an incremented sequence
number and a CRC32 over the record. On boot the firmware reads both slots and
keeps the valid one with the higher sequence number, so an interrupted write
always leaves a usable copy behind.

Saves happen after every interaction and once every 30 seconds. Writing runs on a
background task fed by a one-deep queue, so the button handler and the interface
never block on flash.

One honest limitation: the device has no real-time clock. The clock counts while
the firmware runs and resumes from the last saved value on the next boot, so it
keeps drifting unless you correct it on the clock page.

## Building

Desk Pal needs **ESP-IDF 5.5.3**, the version the device baseline is pinned to.
See [environment setup](docs/development/engineering/environment-setup.md) for
the toolchain and [build and test](docs/development/engineering/build-and-test.md)
for the full workflow.

```bash
./tools/validate.sh --static     # repository checks + host tests
./tools/validate.sh --firmware   # ESP-IDF build + merged 0x0 image
```

The firmware gate produces `build/FoloToy-AI-Passport-full.bin`, the complete
image to flash from `0x0`.

If the repository lives under a path containing non-ASCII characters, disable
ccache for the build (`export IDF_CCACHE_ENABLE=0`). ccache is a C++ program and
its path conversion throws `filesystem error: Cannot convert character sequence:
Illegal byte sequence`, which makes every compile fail. ESP-IDF enables ccache by
default.

The Chinese font is generated, not committed by hand. It is a subset built from
the characters the application actually renders, which keeps a full CJK face off
an 8 MB flash budget:

```bash
python3 tools/gen_pet_font.py --font /path/to/NotoSansSC-VF.ttf
```

Re-run it after adding or changing any Chinese string in `main/`. Noto Sans SC is
licensed under the SIL Open Font License 1.1.

A stale font fails silently: the character simply renders as nothing, while the
compiler, the repository checks and the host tests all stay green. So
`./tools/validate.sh --static` runs `tools/check_font_coverage.py`, which fails
the gate if any character used in `main/` has no glyph. The required character
set is derived by the check itself rather than taken from the generator, so a
collector that drops characters is caught as well — that is how U+2026 "…" was
found missing from the interface copy. The check also validates the font
structure LVGL depends on — sparse cmap offsets must be sorted and stay inside
`range_length`, `glyph_id_start` values must be contiguous, and the cmap
coverage must match the glyph descriptor table.

## Layout

| Path | Contents |
| --- | --- |
| `main/pet_model.h` / `.c` | Pure state machine: decay, moods, bonds, clock formatting. No hardware dependencies. |
| `main/pet_store.h` / `.c` | NVS double-slot persistence with CRC32 and a background writer. |
| `main/pet_audio.h` / `.c` | Code-synthesized jingles and their playback task. |
| `main/pet_view.h` / `.c` | The LVGL interface: room, pet, status bubble, bond bar, HUD, status page, clock page. |
| `main/pet_app.h` / `.c` | Orchestration: one task owns the model and the interface; buttons and the 1 Hz tick only enqueue. |
| `main/lv_font_pet_16.c` | Generated 2 bpp Chinese subset font; glyph data is const and lives in flash. |
| `main/main.c` | Boot: bring up I2C, display, LVGL, then hand over to the application. |
| `tools/gen_pet_font.py` | Font generator. |
| `tools/check_font_coverage.py` | Verifies the generated font still covers every character used in `main/`. |
| `tests/test_pet_model.c` | Host test for the state machine; runs under `./tools/validate.sh --static`. |

The BSP reference demos are still compiled by the build — that way they cannot
rot unnoticed — but they are no longer reachable from the boot path, so the
linker drops them from the image.

## Threading

The application keeps a single owner for its state: one task handles every model
read, model write and LVGL call. The button callback and the 1 Hz timer run on
shared infrastructure, so they only push a message onto a queue and return
immediately. Because LVGL is not thread-safe, the task still holds the BSP's LVGL
lock while it touches the interface.
