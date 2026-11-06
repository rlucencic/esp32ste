# STE medium-resolution and monochrome conversion

Date: 2026-11-06. Reference: esp32ste `9d93d58`, shared runtime `bfcc9f9`.
This follows the retained [four-plane decoder](planar-conversion-experiment-2026-11-06.md).
Hardware is the same Tab5 ESP32-P4 at 360 MHz, 32 MB / 200 MHz PSRAM,
with instructions and rodata in PSRAM. ESP-IDF 5.5.6, GCC 14.2.0,
machine component at -O2. S3 is build-tested only.

Decision: retain both specialized C paths. They reduce rendering cost in
the isolated P4 benchmark and sampled renderer residency in the full
firmware. Four-plane cost remains effectively unchanged.

## Change

The remaining generic renderer constructs a four-plane index for every
pixel, even when only one or two planes are active. Split these cases once
per scanline. Two-plane output uses the same 16-entry nibble expansion table
as four-plane output; monochrome directly extracts the selected bit from
the screen word. Keep the existing addressing, line offset, RAM bounds
check, palette conversion and pixel order.

P4 `render_line` grows from 542 to 666 bytes (+124). The same 32-byte
lookup table now serves both color paths; no second table or allocation
is added. The renderer remains outside IRAM.

## Isolated P4 measurements

The temporary firmware links both actual source versions. The rendering
API runs at -O2, with seeded source RAM in PSRAM and an internal output
buffer consumed through a volatile checksum. Each pass renders 40 complete
frames, including per-line palette conversion. Seven ABBA repetitions run
reference/candidate/candidate/reference in one boot. Cycle measurements
include interrupts. This is the same method as the four-plane experiment.

| Mode | Reference median cycles/frame | Candidate median cycles/frame | Median cost change | Range |
| --- | ---: | ---: | ---: | ---: |
| Low, four planes | 1,116,331 | 1,115,128 | −0.108% | −0.187 to −0.105% |
| Medium, two planes | 3,223,618 | 1,488,034 | −53.840% | −53.841 to −53.838% |
| High, one plane | 6,078,337 | 2,739,085 | −54.937% | −54.938 to −54.937% |

The medium/monochrome savings are rendering cost reductions. Four-plane
cost is effectively unchanged; the small measured difference is not a
meaningful additional gain. These are not whole-emulator FPS improvements.

## Full firmware workloads

Use the [previous synthetic guest animation](planar-conversion-experiment-2026-11-06.md#workload-and-scope),
setting the shifter to mode 1 (640×200, two planes, 50 Hz) and then mode 2
(640×400, one plane, 71 Hz). Both layouts occupy 32,000 bytes, so the same
guest loop updates the entire screen. CPU setting remains 8 MHz, interrupts
masked. Each fresh firmware boot enters normal GEM first; inject the same
code and initial registers for each mode, wait two seconds, then collect
three 32,000-sample core-0 profiles at a 25 microsecond interval. Resolve
against the exact flashed ELF. Guest and video state change only in RAM;
no files on the SD card are changed.

| Mode / build | Renderer samples, three captures | memcpy samples | Idle samples |
| --- | --- | --- | --- |
| Medium reference | 13.7%, 13.6%, 13.7% | 16.2–16.4% | 53.1–54.7% |
| Medium candidate | 6.6%, 6.6%, 6.8% | 17.8–18.6% | 58.0–58.2% |
| Monochrome reference | 22.1%, 21.6%, 21.8% | 13.4–13.9% | 44.7–44.9% |
| Monochrome candidate | 11.4%, 11.6%, 11.4% | 16.2–16.3% | 50.6–51.8% |

Renderer residency falls consistently in both modes. The shared presenter
waits 40 ms between passes, renders source rows for change detection, then
renders needed rows for scaling. Faster conversion changes presentation
scheduling and the number of updates; copying and presenter sample shares
rise. These periodic bursts do not measure an exact whole-emulator speedup
or displayed update rate. The candidate runs at 50.0 emulated FPS in medium
and 71.0 in monochrome. The isolated cycle results measure conversion cost.
After testing, a cold guest reset returns to the normal 640×400 GEM desktop,
verified visually. Its screen signature matches the reference (`ea195797`);
the three normal TOS startup bus-error probes do not increase during either
animation or the restored desktop.

## Correctness and compatibility

- 609,824 complete scanline and buffer-canary comparisons against the
  committed reference pass at -O2 under UBSan. They cover every 16-bit
  screen word in the monochrome path and each color plane, both monochrome
  palette polarities across the cases, all joint two-plane byte patterns,
  all joint four-plane nibble patterns, palettes, mode values, strides,
  scanlines and RAM boundaries.
- Native TOS 1.62 replays with 900 frames and scripted mouse/key events
  produce byte-identical final pixels and full WAV output in all three
  resolutions, compared with the original generic renderer. This is
  desktop regression coverage, not a game compatibility suite.
- Full P4 and S3 firmware builds pass. No physical S3 performance or runtime
  result is claimed. No shared runtime source changes.

Raw captures and temporary fixtures are outside git under
`/tmp/ste-planar-2026-11-06/remaining-*`; private ROMs stay outside git.

## Remaining coverage

Animated real-software compatibility, S3 hardware measurements and visible
update-rate measurements remain open. The three planar paths are now
specialized in portable C; no assembly has been justified by these trials.
Next machine-level investigations remain mac2/mac3 color conversion and
MSX2 graphics.
