# Boot time of BL1, riot-tee and RIOT

BL1 toggles a GPIO pin at its start, after every operation and at its end. riot-tee (branch `eval/boot-markers` of riot-tee) toggles the same pin after every step of its start-up, and RIOT's `tests/sys/psa_riot_tee` (branch `eval/boot-markers` of RIOT) toggles it once more when `main()` starts. An oscilloscope captures the pin; [bootbench.py](bootbench.py) resets the board, reads the raw sample memory and computes the time between the edges.

## Instrumentation

The defines at the top of [src/main.c](../../src/main.c) select what BL1 drives.

| Define | Pin | Effect |
|---|---|---|
| `BL_TIMING_PHASE_PIN 10` | P0.10 | toggles at the start of BL1, after every operation and at the end (runs with all markers) |
| `BL_TIMING_PIN 10` | P0.10 | high from the start to the end of BL1 only (reference run) |
| `BL_TIMING_CLOCK_PIN 13` | P0.13 | TIMER2 toggles it every 8000 ticks of the 16 MHz clock (0.5 ms), as a reference for the clock frequency |
| `BL_TIMING_TOGGLE_BURST 10000` | P0.10 | 10000 markers in a row before `BL_TIMING_PIN` rises (cost of one marker, see [../toggle-cost](../toggle-cost)) |

## Setup

| Part | Setting |
|---|---|
| Board | nRF9160 DK, J-Link on USB, I/O at 1.8 V |
| Scope | Rigol DS1000Z series (reports DS1104Z), LAN, 169.254.215.74, SCPI on TCP port 5555 |
| PC | static address 169.254.215.1/16 on the LAN adapter |
| Channels | CH1 P0.10 (markers), CH4 P0.13 (clock reference), probes 10:1 |

Build the image from the RIOT tree, with this repository and riot-tee as local package sources:

```
cd ~/project_hauptprojekt/riot_eval
USEPKG=riot-tee-bl PKG_SOURCE_LOCAL_RIOT_TEE_BL=$HOME/work/riot-tee-bl PKG_SOURCE_LOCAL_RIOT_TEE=$HOME/work/riot-tee-eval make -C tests/sys/psa_riot_tee BOARD=nrf9160dk clean all
```

## Procedure

Each round erases the chip (flash and UICR, which removes the root key, the firmware hash and the marker) and programs the image. Then it measures one provisioning boot and a number of normal boots.

| Boot | Check that the boot is of this kind |
|---|---|
| provisioning (boot 0) | OTP[8] reads `FFFFFFFF` before the reset and `52544231` ("RTB1") after it |
| normal (boots 1 to n) | OTP[8] reads `52544231` before and after the reset |

The runs of 2026-10-29:

```
IMG=~/project_hauptprojekt/riot_eval/tests/sys/psa_riot_tee/bin/nrf9160dk/tests_psa_riot_tee.bl.combined.bin
# all markers
python3 bootbench.py run --rounds 20 --boots 4 --marker-channel 1 --clock-channel 4 --probe 10 --mdepth 6000000 --timebase 0.02 --pre-divs 0.5 --image $IMG
# reference (BL_TIMING_PIN 10 instead of BL_TIMING_PHASE_PIN 10)
python3 bootbench.py run --rounds 20 --boots 4 --channel 1 --clock-channel 4 --probe 10 --mdepth 600000 --timebase 0.01 --pre-divs 1 --image $IMG
python3 bootbench.py summary runs/<stamp>/results.csv
```

## Data

Each run writes `runs/<stamp>/`:

| File | Content |
|---|---|
| `meta.txt` | command line, image SHA-256, commits and modified files of riot-tee-bl, riot-tee and RIOT, tool versions, scope settings as read back |
| `image.bin` | the image that was measured |
| `results.csv` | one row per boot: kind, status, BL1 run time (`width_us`), duration of every operation (`*_us`), measured clock (`hfclk_mhz`), OTP[8] before and after |
| `*.diff` | uncommitted changes of the three repositories at the time of the run |
| `build-flags/` | compiler flags of BL1 and riot-tee as CMake generated them |
