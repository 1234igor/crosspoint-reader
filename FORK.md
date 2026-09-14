# Igor's CrossPoint fork — how this repo is used

Personal fork of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader)
for an **Xteink X3**. Goal: small quality-of-life patches while still taking every
upstream release. Nothing here is meant to diverge from upstream for long.

## Layout

| What | Where |
|---|---|
| Fork on GitHub | https://github.com/1234igor/crosspoint-reader (`origin`) |
| Upstream | https://github.com/crosspoint-reader/crosspoint-reader (`upstream`) |
| Working branch | `igor` = latest upstream release tag + my commits on top |
| Build env | `[env:igor]` at the **end** of `platformio.ini` (gh_release + `-igor` version suffix) |
| Output | `.pio/build/igor/firmware.bin` → copied to the SD card as `update.bin` |
| Device kit (built binaries, SD notes) | `../xteink/` |

Rules that keep rebases painless:

- One commit per feature. Small diffs. New code goes at the end of a file or in a
  new file, never interleaved with upstream logic when avoidable.
- Don't touch `freeink-sdk/` (submodule) or translation tables unless unavoidable.
- If a patch is general enough, send it upstream as a PR and drop it here once merged.

## Patches on `igor`

1. **Double-tap power = input lock** (`src/main.cpp`, `src/MappedInputManager.*`,
   `lib/GfxRenderer/*`). Two short taps (<300 ms each, within 500 ms) lock every
   button; two more unlock. A 32×32 padlock badge appears top-right, drawn straight
   into the framebuffer with one FAST differential refresh and restored from a
   saved snapshot on unlock; unlock restores the framebuffer only and the next page
   turn (or a refresh 1.5 s later) clears the panel, so unlock never waits on a
   waveform. While locked the device never auto-sleeps; after 1 s of quiet the CPU
   light-sleeps between power-button polls (page, SD mount and position retained,
   GPIO-level wake on the button, upstream-measured ~2.8 mA vs ~9.7 mA). Long-press
   sleep still works. After 30 s locked the device **deep-sleeps with the page +
   badge on the panel** (`enterDeepLockSleep`, RTC flags `deepLockMagic`/`deepLockPin`);
   the wake requires a second tap (or a 400 ms hold), verified by `deepLockWakeGate()`
   as the FIRST line of `setup()` on raw GPIO (the normal init reaches the button
   ~250 ms after the wake edge, too late for a natural double tap). A lone tap
   re-sleeps via `freeink::PowerManager::deepSleepUntilPowerButton()` with nothing
   initialized. **Hardware facts from the device's own lock.log (2026-09-14), which
   outrank any comment:** (1) the stock sleep path drives GPIO13 LOW and on this X3
   that cuts power to the chip — the next press is reset=POWERON, RTC RAM gone, ~1 s of
   image validation before setup(); no double tap can survive that, only a hold wakes
   it. (2) With GPIO13 held HIGH and a real `esp_deep_sleep_start`
   (`HalPowerManager::startRetainedDeepSleep`) the wake is reset=8/wake=7, the gate
   ran ~60 ms in and caught tap 2 every time (`gate=0xf rel=16-62 tap2=80-120`).
   (3) That build still drained ~12 mA asleep: `esp_sleep_config_gpio_isolate` floats
   every unheld pad and the powered SD card sat on a floating CS/SCLK. r9 holds every
   bus pad (SD CS 12 HIGH, display CS 21 HIGH, SCLK 8 LOW, MOSI 10 HIGH, DC 4, MISO/BUSY
   pulled up, RST 5 HIGH, GPIO13 HIGH) and releases them first thing on wake
   (`releaseRetainedSleepHolds`; held pads ignore muxing). (4) The lock's light sleep
   ignores the USB heuristic (X3 infers USB from the gauge's charge-current sign) and a
   declined sleep idles at 10 MHz, not 160. Every lock/unlock/sleep/boot line logs
   battery %, gauge current and the RTC clock; two lines bracketing a sleep give the
   average sleep current (% delta x 6.5 mAh / hours). A 12.8 µA power-off is
   unreachable with a fast wake on this board; the target is the SD card's standby,
   expected 0.3-0.8 mA.
   A hit restores the saved frame minus the badge with one FAST refresh and reloads
   the reader behind it. Two reviewer passes (2026-09-14) signed off on this shape;
   the alternative they offered is a 300 ms hold instead of tap-tap if the double
   tap ever proves unreliable. Wake from any other deep sleep
   always unlocks (chip reset). Single tap keeps whatever "Short power button
   press" is set to (default Ignore). If that setting is *Sleep* the lock is
   unreachable, because each tap sleeps before a second can land.

2. **Reading-speed batch** (8 commits, see `../xteink/docs/reading-speed-audit-2026-09-13.md`
   for the audit and status table). Progress writes debounced off the page turn;
   status-bar title cached; built-in glyphs retained across pages; `HalFile`
   read-ahead + one persistent section-file handle with cached page LUTs; image
   dimensions memoized per book (`imgdims.bin`); chapter layout finishes while idle;
   persistent ZIP central-directory index (`zip.idx`) + read-ahead on scans; row-walking
   glyph blit (`lib/GfxRenderer/GlyphBlit.h`, host-verified by
   `test/glyph_blit/glyph_blit_selftest.cpp`). Plus two upstream cherry-picks
   (#3463 input wake, #3527 font cache release).

## Taking a new upstream release

```bash
cd ~/Desktop/repos/crosspoint-reader
git fetch upstream --tags
git rebase 1.7.0            # the new tag
git submodule update --init --recursive
pio run -e igor
```

Expected conflicts at the next rebase: upstream #3521 (font-cache heap fragmentation,
`lib/EpdFont/SdCardFont.*`, `lib/GfxRenderer/FontCacheManager.cpp`) overlaps the
retained glyph cache commit; keep ours for `PrewarmScope`, take theirs elsewhere.

Fix conflicts if any (they'll be in the files listed under Patches), then build,
flash, and push: `git push -f origin igor` (the branch is rebased, so force is expected).

## Toolchain (macOS)

Stock PlatformIO 6.2.0 **cannot build this**: the pioarduino platform deletes the
core's `tool-scons` mid-build and the link step dies with
`No module named 'SCons.Tool.FortranCommon'`. Use the pioarduino fork of the core,
exactly what upstream CI does (`.github/workflows/ci.yml`):

```bash
uv tool install --force "pioarduino-core @ https://github.com/pioarduino/platformio-core/archive/refs/tags/v6.1.19.zip" \
  --with "littlefs-python>=0.16.0" --with "fatfs-ng>=0.1.14" --with "pyyaml>=6.0.2" \
  --with "rich-click>=1.8.6" --with "zopfli>=0.2.2" --with "intelhex>=2.3.0" --with "rich>=14.0.0" \
  --with "urllib3<2" --with "cryptography>=45.0.3" --with "certifi>=2025.8.3" --with "ecdsa>=0.19.1" \
  --with "bitstring>=4.3.1" --with "reedsolo>=1.5.3,<1.8" --with "esp-idf-size>=2.0.0" --with "esp-coredump>=1.14.0"
```

`pio` lands in `~/.local/bin`. First build downloads ~1 GB of toolchain and takes
~5 min; incremental builds ~2 min.

Other gotchas:

- `freeink-sdk` is a git submodule. Without `git submodule update --init --recursive`
  the library install fails with "Can not create a symbolic link".
- Never flash `-e default`: it defines `CROSSPOINT_WAIT_FOR_USB_SERIAL` and the
  device sits waiting for a serial monitor at boot.
- The ESP app descriptor (not the Settings screen) shows `1.6.0-dirty` or
  `1.6.0-1-gabc123` — that's ESP-IDF's `git describe`, harmless.
- `./bin/clang-format-fix -g` is upstream's formatter; it needs `clang-format`
  (`brew install clang-format`). Only matters before sending a PR upstream.

## Flashing the X3 from the SD card

1. Copy `.pio/build/igor/firmware.bin` to the card root as `update.bin`
   (`../xteink/sd/INSTRUCTIONS.txt` has the full sheet).
2. Card into the X3, power fully off.
3. Hold **LEFT** side button, then long-press **POWER** until the bootloader shows
   it flashing. Alternative: **UP + POWER** jumps to the SD firmware picker.
4. After it boots, Settings should show version `x.y.z-igor`. Delete `update.bin`
   from the card afterwards so a later recovery boot doesn't re-flash it.

The device never appears on USB from the Mac side; the card goes through a reader
and mounts as `/Volumes/xteink`.

## Where things live in the code (for the next patch)

- `src/main.cpp` — boot, the main loop, power button, sleep, the lock toggle and badge.
- `src/MappedInputManager.*` — the only input API activities use; gate input here.
- `src/activities/reader/EpubReaderActivity.cpp` — the reader; page render at ~1300–1700.
- `src/CrossPointSettings.h` + `src/SettingsList.h` — persisted settings and the menu.
  New enum values must be appended (indices are persisted).
- `lib/GfxRenderer/` — drawing into the 48 KB 1-bpp framebuffer; `readFramebufferRegion`
  / `writeFramebufferRegion` are the cheap overlay tools.
- `lib/hal/HalDisplay.h` — refresh modes: FULL, HALF (~1.7 s), FAST (differential).
- `CLAUDE.md` / `AGENTS.md` — upstream's own hard rules (380 KB RAM ceiling, no heap
  without justification). Read before any non-trivial change.
