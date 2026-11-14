# Changelog

## 1.2.0

- Offers its state to other mods as the interface `LaunchControl.State` (version 1: `has_kit`, `is_active`), so the FH6 Speedometer mod can show the LC light on the HUD: shown when the car has the kit, lit while the 2-step holds the engine.

## 1.1.0

- The **2-Step RPM** row moved to the new **ENGINE** tab of Tune Car (after Differential), next to the other engine settings, instead of the end of Springs.
- Needs FH3 Mod Platform **1.4.2** or newer (API 17; the ENGINE tab).

## 1.0.1

- The platform's update check now knows this mod's repository: MODS > Updates and the start-up notice tell when a newer release is out.
- Hot reload (FH3 Mod Platform 1.2.0, a developer feature, off by default): the mod exports `fh3mod_shutdown`, so a rebuilt `LaunchControl.dll` can be swapped in with F9 while the game runs.
- Needs FH3 Mod Platform 1.2.0 or newer.

## 1.0.0

First release of **2-Step Launch Control for Forza Horizon 3**, on the [FH3 Mod Platform](https://github.com/Levilente/FH3_ModPlatform).

- **Garage > Upgrades > Engine > 2-Step Launch Control** (Stock / 2-Step, 4 000 CR), stored per owned car, with its own icon.
- In first gear, standing still, hold the handbrake and full throttle: the engine is held at the set RPM by a real throttle cut (a fuel-cut limiter: the throttle is closed for at least 10 ms each time the engine reaches the limit).
- Let go of the handbrake and the clutch is dumped at once, so the car launches from the set RPM instead of the game slowly feathering the clutch.
- **Tuning > Springs > Launch Control > 2-Step RPM**: 1000 to 8000 RPM per car, in 100 RPM steps (3500 by default).
