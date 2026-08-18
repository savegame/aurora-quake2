# Quake II for Aurora OS

English | [Русский](README.ru.md)

A port of Quake II to Aurora OS.

Quake II is a first-person shooter released by id Software in 1997.
In 2001 id Software open-sourced the Quake II engine (GPL), and the
community has kept it building on modern platforms ever since. This
port is based on Yamagi Quake II via the [Thenesis
Quake II](https://github.com/thenesis-org/lp-public) fork
(`Ports/Quake2` subdirectory) — original project sources:
https://github.com/thenesis-org/lp-public

I write about new game ports for Aurora OS (in Russian) in my
channel: https://t.me/auroraosgames

## Disclaimer

This package contains only the game **engine** (open source, GPL).
It does **not** include the game data (levels, textures, sounds,
`pak0.pak` and friends) — those are still copyrighted by id
Software / Bethesda. You need a legally purchased copy of Quake II
to play. Buy it on Steam:
https://store.steampowered.com/app/2320/Quake_II/

On first launch, the in-app launcher lets you pick the folder where
your legally owned game data is installed; mission packs (Xatrix,
Rogue, CTF) are picked up automatically if present alongside the
base game.

## Port features

- In-process launcher (Dear ImGui): browse to your game data folder,
  pick base game or mission packs (Xatrix, Ground Zero/Rogue, CTF),
  adjust the 3D render scale — all before the game window appears.
- Touch UI: on-screen movement stick, look pad and action buttons
  during gameplay, plus a touch-friendly menu navigation overlay —
  fully playable without keyboard, mouse or gamepad.
- Content rotation and resize on Aurora OS: rendering goes through
  an FBO and correctly rotates/rescales with the device orientation
  (buffer transform over Wayland), including external display
  support.
- In-game on-screen keyboard (same Dear ImGui UI) for the console,
  chat and text fields — no reliance on the system input method.
- Display blanking is suppressed while the game is in the
  foreground, even during gamepad-only play with no touches.
- Gamepad support through the SDL_GameController API with a bundled
  community mappings database, including hot-plug (connect/
  disconnect) detection.
- Sound via PulseAudio.
- Builds for aarch64 and armv7hl.

## Screenshots

|<img width="1280" height="720" alt="Снимок_Экрана_20260818_015" src="https://github.com/user-attachments/assets/dd29d863-4fe6-4ff2-84cf-04a72593946c" />|<img width="1280" height="720" alt="Снимок_Экрана_20260818_013" src="https://github.com/user-attachments/assets/acf9be3c-d1dc-4947-bf20-825a82943a9d" />|
|:-:|:-:|
|<img width="1280" height="720" alt="Снимок_Экрана_20260818_005" src="https://github.com/user-attachments/assets/0827e1a2-9a37-4e89-b7ed-3213d5f4a6f3" />|<img width="1280" height="720" alt="Снимок_Экрана_20260818_001" src="https://github.com/user-attachments/assets/32a2ad96-c1df-4800-9887-62f2e386c831" />|
