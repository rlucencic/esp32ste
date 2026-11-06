# Atari STE four-plane conversion experiment

Date: 2026-11-06. Reference source: esp32ste `a69843a` (same renderer as
`c104035`), shared runtime `23aee05`. Tab5 ESP32-P4 at 360 MHz,
ESP-IDF 5.5.6 / GCC 14.2.0, machine component at -O2, 32 MB PSRAM at
200 MHz with instructions/rodata in PSRAM. S3 is build-tested only.

Decision: retain the revised four-plane C path. The lookup's measured gain
is useful for this workload, with a small rendering-cost regression in the
other modes. No assembly change or general game speedup is claimed.

## Workload and scope

The first monochrome GEM profile put the renderer at only 1.1% of core-0
samples. A synthetic guest animation makes rendering measurable without
relying on a particular game or private disk image. Through the existing
TCP debugger, pause the guest, set SR=$2700 and inject code at $020000.
The program switches to 320×200 four-plane, 50 Hz, screen base $080000,
zero extra stride and fine scroll, and sets all 16 palette registers.
It repeatedly writes 8,000 longwords to the entire 32,000-byte screen:

```asm
        ; D1=$12345678, D2=$01020304 on entry; interrupts masked
frame:  lea     $80000.l,a0
        move.w  #7999,d0
pixel:  move.l  d1,(a0)+
        add.l   d2,d1
        dbra    d0,pixel
        addq.l  #1,d2
        bra     frame
```

Palette word i is `((i & 7) << 8) | (((i * 3) & 7) << 4) | ((i * 5) & 7)`.
This deliberately has eight distinct colors but updates and decodes all
four planes. The guest runs at the 8 MHz setting. All guest modifications
are in RAM and video registers; no disk image or shared runtime code changes.

The scout found 8.1% renderer, 19.2% memcpy, 6.3% presenter and 54.9% idle
in core 0. Core 1 was 57.0% idle, 10.8% Musashi execution and 5.8% DBRA.
This synthetic workload supports investigating planar conversion; it does
not establish performance in games, raster effects or blitter-heavy software.

## Candidate

For four planes, expand each plane's four-bit nibble through a 16-entry
uint16_t lookup table. Shift/OR the four expansions to form four packed
palette indexes, then emit four RGB565 pixels. Make the mode decision once
per scanline. Preserve the original screen-word bounds check, addressing,
line stride, palette conversion, and generic one-/two-plane conversion.
No assembly is needed for this candidate.

The initial variant checked the mode once per 16-pixel group. The revised
version moves that decision outside the group loop. On P4, `render_line`
grows from 270 to 542 bytes, plus a 32-byte lookup table. This renderer is
outside IRAM; the change adds no explicit internal code allocation.

## Isolated P4 renderer measurements

A temporary firmware links both source versions and their real rendering
APIs at -O2. Source RAM is allocated in PSRAM and populated with a seeded
LCG; output is an internal uint16_t buffer, consumed through a volatile
checksum. Each timed pass renders 40 complete frames, including per-line
palette conversion. Seven ABBA repetitions run original/candidate/candidate/
original in the same boot. Cycle-counter measurements include interrupts.
This excludes the LCD/presenter/audio/CPU tasks and is a kernel measurement.

| Mode | Original median cycles/frame | Candidate median cycles/frame | Median cost change | Range |
| --- | ---: | ---: | ---: | ---: |
| Low, four planes | 1,735,151 | 1,115,961 | −35.685% | −35.719 to −35.681% |
| Medium, two planes | 3,198,305 | 3,222,553 | +0.756% | +0.754 to +0.759% |
| High, one plane | 6,029,644 | 6,075,386 | +0.759% | +0.757 to +0.761% |

The small regression in the other modes remains a tradeoff. The first
variant was −34.172% / +0.928% / about +0.85% respectively. These are
rendering costs, not FPS changes or whole-emulator speedups.

## Full firmware profiles

Each capture uses 32,000 periodic samples on core 0 at 25 microseconds,
resolved against the exact flashed ELF. Three consecutive captures follow
each guest launch. The reference was restored after the first candidate
and benchmark, then the revised candidate was tested after another fresh
boot. Both use the same guest code and 8 MHz setting.

| Build | Renderer samples, three captures | memcpy samples | Idle samples |
| --- | --- | --- | --- |
| Initial reference | 7.9%, 7.8%, 8.1% | 17.7–19.3% | 55.5–57.8% |
| First candidate | 5.3%, 5.3%, 5.2% | 19.1–19.3% | 58.6–59.2% |
| Fresh restored reference | 7.3%, 7.4%, 7.5% | 17.5–17.7% | 57.9–58.7% |
| Revised candidate | 5.0%, 5.2%, 5.0% | 19.1% | 58.5–58.9% |

The revised renderer's sample share is consistently lower than the fresh
reference. The absolute shares vary, and memcpy residency differs between
builds: periodic sampling, redraw scheduling and concurrent presentation
prevent translating these samples into an exact whole-emulator throughput
gain. The isolated cycle comparison provides the renderer-cost measurement.
Frame copying remains a larger core-0 cost than this decoder. The revised
build runs the animation at 50.0 FPS; this is not a demonstrated FPS increase.
After testing, a cold guest reset returns to the normal 640×400 GEM desktop
at about 71 FPS, verified visually. The bus-error counter stays at the three
normal TOS startup probes during both animation and restored desktop.

## Correctness and builds

- 347,680 complete original/candidate scanline and buffer-canary comparisons
  passed at -O2 under UBSan. Coverage includes every 16-bit word in each
  plane, all 65,536 joint four-plane nibble combinations, seeded palettes,
  all mode values, line offsets, RAM bounds and scanlines.
- Original/candidate native TOS 1.62 replays ran 900 frames in each of low,
  medium and high resolution with scripted mouse/key events. Final pixels
  and full WAV output were byte-identical in all three modes. These are
  desktop replays, not a game compatibility suite.
- Revised full firmware builds pass for both P4 and S3. No S3 hardware
  performance or runtime correctness result is claimed.

Raw captures, private ROMs and the complete pre-test Acorn flash backup
are outside git under `/tmp/ste-planar-2026-11-06`. The rejected PSG trial
is documented separately in [the initial report](optimization-tests-2026-11-06.md).

## Next targets

Measure animated real software and the other modes on hardware before
claiming broader STE gains. A separate bounded test can examine the
remaining one-/two-plane loop or frame copying; changes to the shared
presenter need cross-emulator validation. The next machine-level targets
remain mac2/mac3 color conversion and MSX2 bitmap/sprite workloads.

## One-/two-plane follow-up

The subsequent [medium/monochrome experiment](one-two-plane-experiment-2026-11-06.md)
specializes the remaining generic paths. It supersedes the small one-/two-
plane regression above: versus this retained four-plane build, their P4
isolated rendering costs fall 53.840% and 54.937%. Four-plane cost stays
effectively unchanged. See that report for full-firmware profiles and checks.
