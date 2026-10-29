# Cost of one marker

BL1 with `BL_TIMING_PIN 16`, `BL_TIMING_PHASE_PIN 10`, `BL_TIMING_CLOCK_PIN 13` and `BL_TIMING_TOGGLE_BURST 10000` in [src/main.c](../../src/main.c) calls `BL_PHASE()` 10000 times in a row before P0.16 rises. `BL_PHASE()` is `nrf_gpio_pin_toggle()` on P0.10, which GCC does not inline at `-Os`.

| Part | Setting |
|---|---|
| Channels | CH1 P0.10, CH3 P0.16 (trigger), CH4 P0.13 (clock reference) |
| Capture | 3 Mpts, 10 ms/div, 40 ns per sample |
| Burst | first rising to last falling edge of P0.10, 9999 intervals |
| Boots | five normal boots by debugger reset |

`python3 burst.py <image .bin>` measures with the image already flashed and prints one row per boot; [results.csv](results.csv) holds the run of 2026-10-29.
