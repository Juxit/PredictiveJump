# Predictive Input Buffer

A [Geode](https://geode-sdk.org) mod for Geometry Dash (Windows).

Simulates your click a few ticks ahead. If clicking now kills you and a slightly later click survives, your input is held back and delivered later (1 tick = 0.25 frames @ 60fps).

## Download

Grab the latest **`.geode`** file from the [Releases](../../releases/latest) page.

## Install

1. Install [Geode](https://geode-sdk.org) if you haven't already.
2. Put the `.geode` file in your Geometry Dash folder under `geode/mods`.
3. Restart Geometry Dash.

## Controls

- **F6** (while GD is focused) cycles the max delay: 0.5f → 1f → 2f → 4f → 6f → 8f → custom → off
- **K** (in a level) opens/closes the panel: current stage + live log of saved inputs. Hidden by default.
- The custom stage, prediction budget, orb/pad/portal prediction and debug console are in the mod settings.

## What's new in v2

- Bigger windows (up to 8 frames, custom up to 16)
- Ghost simulates orbs, pads, gravity portals and speed portals
- Inputs during a delay keep their rhythm (no more swallowed taps)
- Per-click time budget, K panel with stage + live log, bug fixes

## Building

The `.geode` file is built automatically by GitHub Actions on every push (see `.github/workflows/build.yml`), and pushes to `main` publish it as a release. To build locally, set up the [Geode SDK](https://docs.geode-sdk.org/getting-started/) and run `geode build`.

## Note

Ship isn't buffered. This mod changes your input timing, so completions made with it don't count as legit on lists.
