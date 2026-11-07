# 2-Step Launch Control (FH3)

A 2-step launch control for **Forza Horizon 3** (the Xbox One build on PC, through the Azimuth launcher), on the
**[FH3 Mod Platform](https://github.com/Levilente/FH3_ModPlatform)**. Buy it in the garage, set the launch RPM in the tuning menu, then
launch with the handbrake and the throttle held.

> Version **1.0.1**. Needs the FH3 Mod Platform **1.2.0** or newer.

## What it does

- **An upgrade you buy.** **Garage > Upgrades > Engine > 2-Step Launch Control**: *Stock* or *2-Step* (4 000 CR by default). It is stored per owned car.
- **Set the launch RPM per car.** The car's **Tuning > Springs** page gets a **Launch Control** row: **2-Step RPM**, 1000 to 8000 RPM in 100 RPM steps
  (3500 by default). The row is only there on cars that have the upgrade.
- **How to launch.** In **first gear**, **standing still**, hold the **handbrake** and **full throttle** at the same time. The engine climbs to the set
  RPM and stays there. Let go of the handbrake and the car launches from that RPM.
- **A real limiter.** The engine is held by cutting the throttle inside the game's physics (a fuel-cut limiter: when the engine reaches the set RPM the
  throttle is closed for 10 ms, again and again), not by capping the tachometer.
- **Clutch dump.** When you let go of the handbrake the clutch is closed at once, instead of the game's slow clutch feathering.

## Requirements

| What | Version |
|---|---|
| Forza Horizon 3 on PC with the **Azimuth** launcher | Xbox One build 1.0.125.0 (the one the platform targets) |
| **[FH3 Mod Platform](https://github.com/Levilente/FH3_ModPlatform)** | **1.2.0 or newer** |
| Input | Right trigger + handbrake (**A** or **B** on a controller, **Space** on a keyboard; **W** / **Up** count as full throttle). No wheel support. |

## Install

Put `LaunchControl.dll` into the `fh3-mods` folder next to the game's platform files, and start the game. Buy the upgrade on a car in the garage.

## Settings

`fh3-mods\LaunchControl.ini` (optional), section `[LaunchControl]`:

| Key | Default | What |
|---|---|---|
| `DefaultRpm` | 3500 | RPM used by a car whose row was never touched |
| `Price` | 4000 | Price of the upgrade |
| `ThrottleMin` | 230 | Right trigger value (0 to 255) that counts as full throttle |
| `StandstillMps` | 1.5 | Speed in m/s below which the car counts as standing still |
| `DumpMs` | 1500 | How long the clutch is dumped after the launch (0 = off) |
| `Log` | 0 | 1 writes diagnostic lines to `platform.log` |

## Build

Needs CMake and a MinGW-w64 (llvm-mingw) toolchain. Put the FH3 Mod Platform checkout at `./platform` (or pass `-DFH3_PLATFORM_DIR=...`) and build:

    cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=<your mingw toolchain>
    cmake --build build

To work on the mod without restarting the game, turn on the platform's hot reload (`fh3-mods\platform.ini` `[dev] HotReload=1`) and press **F9** after a
build: the mod exports `fh3mod_shutdown` (see the platform's `docs/writing-a-mod.md`).

The icon is `icons/launchcontrol.png`; `python3 scripts/gen_icons.py --prefix launchcontrol` turns it into `src/icon_data.h`.

## How it works

The mod hooks the player car's physics input step: after the game has set the throttle it forces it to 0 for 10 ms whenever the engine speed is at the
set RPM, and it holds the clutch pedal at 0 right after the handbrake is released. All reads and writes of game memory are guarded.

## License

MIT, see `LICENSE`.
