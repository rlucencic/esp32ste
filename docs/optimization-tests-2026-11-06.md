# Atari STE: first Tab5 optimization tests

Date: 2026-11-06. Baseline: esp32ste `c104035`, shared esp32emu `76fb898`.
Decision: reject the PSG fixed-volume cache candidate; original source and
baseline STE firmware restored. No assembly change retained.

## Hardware scout

Tab5 ESP32-P4, 360 MHz, ESP-IDF 5.5.6 / GCC 14.2.0, 32 MB PSRAM
at 200 MHz with instructions/rodata in PSRAM. TOS 1.62, 1 MB guest RAM,
8 MHz CPU, monochrome 640×400 at 71 Hz, sound enabled. A complete 16 MB
flash backup was taken before switching from Acorn because their ROM/storage
partitions overlap. Private ROMs and raw captures stay outside git.

The GEM desktop was booted and visually checked. During continuous horizontal
cursor motion, 32,000 samples per core at a 25 microsecond interval showed:

| Core | Function | All samples |
| --- | --- | ---: |
| 0 | idle (`esp_cpu_wait_for_intr`) | 88.7% |
| 0 | `psg_render` | 4.2% |
| 0 | critical-section exit | 3.6% |
| 0 | `render_line` | 1.1% |
| 1 | idle | 52.7% |
| 1 | `slow_read16` | 8.1% |
| 1 | `m68k_execute` | 6.1% |
| 1 | `m68k_read_memory_16` | 3.7% |
| 1 | `m68k_read_memory_32` | 3.2% |

This is a single workload scout, not a ranking for games or color modes.
It suggested testing PSG before rewriting the planar renderer. The desktop
is paced; these figures do not establish a potential FPS increase.

## Rejected PSG cache

Registers remain fixed inside each event-delimited PSG render call. The
candidate cached the three fixed DAC levels once per call, using UINT32_MAX
as an envelope marker. Tone, noise and envelope advancement stayed unchanged.

A temporary P4 app linked both actual source versions at -O2 with their
render functions in IRAM. Each measurement used 4,000 render calls at
25,033 Hz, identical seeded initial state, and ABBA order (original,
candidate, candidate, original), seven repetitions. The table reports the
median change in the sum of candidate cycle counts versus the original;
positive means slower. The cycle counter includes interrupts. Output was
consumed through a volatile checksum. This isolates PSG cost without the
LCD, emulator or audio task running, so it is not a whole-emulator result.

| Synth setup | 1 sample/call | 8 samples/call | 64 samples/call |
| --- | ---: | ---: | ---: |
| Silent, mixer 63, levels 0/0/0 | +29.08% | +3.72% | −2.80% |
| Fixed, mixer 56, levels 1/5/15 | +15.27% | +0.11% | −3.37% |
| Envelope, mixer 56, shape 10, all envelope | +13.28% | +3.70% | +1.38% |
| Mixed, mixer 0, shape 14, envelope/7/15 | +17.17% | +2.94% | −0.14% |
| Noise, mixer 7, levels 15/15/15 | +19.02% | −3.96% | −9.77% |

`audio.c` renders chunks of at most 64 samples, and register events can
split them into smaller calls. The short-call and envelope regressions make
this unsuitable as a general replacement. No full-firmware candidate
speedup is claimed, and no S3 performance or build result is claimed.

## Correctness checks

- Full P4 candidate firmware build passed.
- Native -O2 differential check with UBSan: 229,392 cases, 688,176 complete
  output-buffer and internal-state comparisons, 21,697,521 samples identical.
  Covered all mixer combinations, all envelope shapes, all 32,768 packed
  volume combinations, six sample rates plus extreme-rate cases, sizes
  0/1/2/8/17/64/127, and register writes between blocks. Buffer canaries matched.
- Independent original/candidate native emulator builds ran TOS 1.62 for
  900 frames in each of low, medium and high resolution, with scripted
  mouse/key input. Final PPM pixels and complete WAV files were byte-identical
  in each mode. This desktop replay is not an active-music regression suite.

## Next tests

1. Profile an animated low-resolution, four-plane ST workload before deciding
   whether table expansion or bit transposition is worth implementing.
2. On CPU-heavy STE software, inspect Musashi direct RAM reads versus
   `slow_read16`; preserve I/O boundaries and bus errors.
3. Profile active PSG music and measure the actual render-call length
   distribution before another audio specialization.
4. Move to mac2/mac3 indexed/direct-color conversion and MSX2 bitmap/sprite
   workloads, establishing separate baselines for each.

P4 results do not predict S3 gains. The next retained change needs its own
whole-emulator measurement and relevant correctness coverage.

## Rejected patch

```diff
diff --git acomponents/atari_st/psg.c bcomponents/atari_st/psg.c
index 31c4f39..f2218fe 100644
--- acomponents/atari_st/psg.c
+++ bcomponents/atari_st/psg.c
@@ -188,6 +188,13 @@ void ST_HOT psg_render(int32_t *out, int count, int sample_rate)
 {
     uint32_t step = ((uint64_t)YM_TICK_HZ << 16) / sample_rate;
     uint8_t mixer = ym.reg[7];
+    /* Registers are fixed for this event-delimited render block. */
+    uint32_t levels[3];
+    for (int ch = 0; ch < 3; ch++) {
+        uint8_t vol = ym.reg[8 + ch];
+        uint8_t fixed = vol & 0x0F;
+        levels[ch] = (vol & 0x10) ? UINT32_MAX : ym_level[fixed ? fixed * 2 + 1 : 0];
+    }

     for (int i = 0; i < count; i++) {
         ym.tick_frac += step;
@@ -209,9 +216,10 @@ void ST_HOT psg_render(int32_t *out, int count, int sample_rate)
             if (!(noise_off || ym.noise_out) || high == 0) {
                 continue;
             }
-            uint8_t vol = ym.reg[8 + ch];
-            uint8_t fixed = vol & 0x0F;
-            uint32_t level = ym_level[(vol & 0x10) ? env : fixed ? fixed * 2 + 1 : 0];
+            uint32_t level = levels[ch];
+            if (level == UINT32_MAX) {
+                level = ym_level[env];
+            }
             sum += level * high;
         }
         out[i] = sum / ticks;
```

## Subsequent four-plane test

The next experiment uses a full-screen guest animation and tests a four-pixel
lookup decoder. See [the planar conversion report](planar-conversion-experiment-2026-11-06.md)
for measurements, correctness checks and the tradeoff in other display modes.
