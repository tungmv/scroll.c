# Scroll Interceptor - C Port

C port of UnnaturalScrollWheels scroll interceptor that modifies macOS scroll behavior by intercepting scroll wheel events. (https://github.com/ther0n/UnnaturalScrollWheels/blob/master/UnnaturalScrollWheels/ScrollInterceptor.swift)

Inverts mouse (non-continuous) scroll wheel direction. **Trackpad gestures are
passed through untouched.**

**Requirements**: Accessibility permissions (System Settings → Privacy & Security → Accessibility)

## Build & deploy

```sh
make            # build ./scroll
make check      # strict warnings, no output binary
make test       # unit test for the wheel-vs-trackpad decision
make deploy     # install to ~/.local/bin, reload the launchd agent
make log        # tail ~/Library/Logs/scroll.log
```

`make deploy` prints the new cdhash. **Read the re-grant note below before the
first deploy after a code change.**

## Which events get inverted

The filter is `is_mouse_wheel()` in `main.c`, unit tested by `test_filter.c`:

```c
is_continuous == 0 && scroll_phase == 0 && momentum_phase == 0
```

An earlier revision tested only `is_continuous == 0`. That looked correct but
inverted the **trackpad** as well: the internal trackpad frequently emits events
with `kCGScrollWheelEventIsContinuous` clear, especially during momentum. With
no external mouse attached the daemon still logged 15,750 "inversions", and
mixing inverted and non-inverted events made scrolling feel erratic — the
"sometimes it flips the wrong way" behaviour.

A real wheel tick belongs to no gesture sequence, so both phase fields are zero.
A trackpad event always belongs to a scroll phase (`1` begin, `2` changed,
`4` ended) or a momentum phase (`1`–`4`). Requiring both to be zero selects
wheel events and leaves the trackpad alone.

Run `make test` after touching this logic. The test fails loudly (8 cases) if the
filter regresses to the `is_continuous`-only form.

## Why a tap goes silent (the bug this version fixes)

A `CGEventTap` stops working in three ways that do not crash the process:

| Signal | Meaning | Old behaviour |
|---|---|---|
| `kCGEventTapDisabledByTimeout` | callback missed its deadline (system load, or another tap on the same stream stalled) | ignored, tap stayed off **forever** |
| `kCGEventTapDisabledByUserInput` | secure input became active (password field, `sudo`) | ignored, tap stayed off |
| mach port invalidation | WindowServer went away | process lived on with a dead tap |

Because the callback returned the event without calling `CGEventTapEnable`, the
process stayed alive and healthy-looking (`launchctl` reported `state = running`,
`last exit code = (never exited)`) while scroll inversion silently stopped. That
is exactly the "works for a few milliseconds" symptom.

Current behaviour:

- both disable notifications re-enable the tap immediately, and are counted;
- a 2 s watchdog re-enables the tap if a notification is ever missed;
- an invalidation callback exits `1` so launchd relaunches a fresh tap;
- `SIGTERM`/`SIGINT` shut down cleanly and report counters;
- everything goes to `~/Library/Logs/scroll.log` (the plist sets
  `StandardErrorPath`; the old plist sent output to `/dev/null`).

A quiet log is the healthy state — stats only print when counters moved.
Counters are `wheel_inverted` (mouse events flipped) and `trackpad_passed`
(trackpad events deliberately ignored). If `trackpad_passed` stays at 0 while
you scroll on the trackpad, the filter is wrong — run `make test`.

There is no crash to chase: the daemon reports `state = running` with
`last exit code = (never exited)` for weeks. Long gaps in the log mean the
machine slept, not that the tap died — check `pmset -g log` before assuming a
failure.

## The cdhash / TCC trap

Accessibility and PostEvent grants are pinned to the binary's **code signature
hash**, not its path. Rebuilding changes the hash, so the existing grants no
longer match: `CGEventTapCreate` returns NULL, the process exits, and with
`KeepAlive` it restarts every `ThrottleInterval` seconds. That loop is a real
crash loop and looks nothing like the original bug.

Two ways to re-grant after a rebuild:

1. **GUI**: remove and re-add the entry in System Settings → Privacy & Security →
   Accessibility. `scroll` is an ad-hoc-signed bare executable, so it appears as
   a path entry and may need to be added via the `+` button using ⌘⇧G.
2. **Rewrite the stored requirement** (only works with SIP disabled; this
   machine has SIP off). The `csreq` blob is
   `FADE0C0000000028000000010000000800000014` followed by the 20-byte SHA-1
   cdhash:

```sh
NEW=$(codesign -dvvv ~/.local/bin/scroll 2>&1 | grep '^CDHash=' | cut -d= -f2)
HEX=$(echo "$NEW" | tr 'a-f' 'A-F')
sudo cp -p "/Library/Application Support/com.apple.TCC/TCC.db" /tmp/TCC.db.bak
sudo sqlite3 "/Library/Application Support/com.apple.TCC/TCC.db" \
  "UPDATE access SET csreq=X'FADE0C0000000028000000010000000800000014${HEX}' \
   WHERE client='/Users/tony/.local/bin/scroll';"
sudo killall tccd
launchctl kickstart -k gui/$(id -u)/com.scroll.interceptor
```

Naming matters: the signature identifier is derived from the output filename, so
build as `scroll`, never `scroll.new`, or System Settings will list that name.

## Configuration

Inverts vertical scroll (`kCGScrollWheelEventDeltaAxis1`) for non-continuous
events only. To also invert horizontal or change the condition, edit the
`kCGEventScrollWheel` branch of `event_callback()` in `main.c`.

`kCGEventTapOptionDefault` is used deliberately: it allows modifying events.
`kCGEventTapOptionListenOnly` would let the tap miss deadlines more gracefully
but cannot invert anything.
