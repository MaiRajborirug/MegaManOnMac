# In-game Controls screen (replaces OPTION → BUTTON CONFIG)

## Decided
- Lives in the framework (psxrecomp-v4 runtime + recomp-ui); X4 supplies action labels via game.toml.
- Opened by OPTION → BUTTON CONFIG (hook replaces Capcom's screen).
- X4's internal action→PS-button table is pinned to default; the new screen rebinds keyboard
  keys AND gamepad buttons per action.
- Clash policy: action↔action swaps keys; host-hotkey clash warns, second press confirms.
- Reset restores the PC-version layout: Fire C, Jump X, Dash Z, Special V, Giga F,
  Weapon ← A / → S, Pause Enter, arrows move.
- Design (approved): docs/design/controls_screen.html. BUTTON CONFIG → chooser (Keyboard /
  Controller / Exit) → Capcom-style page in Capcom's row order. Controller page shows the action's
  key in brackets ("✕ (C)"); every prompt names its key ("PRESS START (ENTER) TO EXIT"). Start's
  key exits and can't be bound.
- Runtime menu currently renders in ImGui's default bitmap font (seen in present_shot), not Lato.
- Font rendering is a risk: Capcom's labels are a bitmap font at 320×240; the prototype uses Lato
  Bold Italic at host res. Check side-by-side in game before shipping (scale, italics, outline).

## Evidence (static, from mmx4/SLUS_005.61)
- Live config: 16 × u16 at 0x800EE430 (EXE file offset 0xDEC30), default
  `2000 8000 1000 4000 | 0080 0010 0002 0040 0020 0008 0004 | 0 0 0 0 0`.
  Entries 4–10 use PS high-byte bits (□80 ✕40 ○20 △10 R1 08 L1 04 R2 02 L2 01):
  □ △ R2 ✕ ○ R1 L1 → inferred Fire, Special, Giga, Jump, Dash, Weapon→, Weapon← (UNCONFIRMED).
  Entries 0–3 likely menu confirm/cancel masks (UNCONFIRMED).
- func_8001C07C: config → save buffer (*0x801721BC + 8).
- func_8001C210 / func_8001C30C: save buffer → config (load). Pin-default hook goes after these.
- func_8001C3E8: compares/checksums save fields.
- func_8002A098 (158 instr, no calls): reads 0x800EE430 — likely pad→action translator.
- func_80035FC4, func_800D37E4: small readers.
- acediez undub readme's A–G action labels for this table are wrong; don't rely on them.

## Evidence (runtime, build-debug, debug port 4525)
- BUTTON CONFIG screen rows (on-screen labels) confirm entries 4–10:
  4 □ X-Buster,Weapon / Z-Saber · 5 △ X-Buster / Learned Move · 6 R2 Giga Attack ·
  7 ✕ Jump · 8 ○ Dash · 9 R1 Weapon Select R · 10 L1 Weapon Select L.
- Capcom UX: highlight a row, press the wanted button → assigned; the old holder gets the
  displaced button (swap). No separate "select" press.
- Writers (wtrace): PC 0x8002A2F8 and 0x8002A334, both in func_8002A098 (screen RUN state).
- Screen dispatcher func_8002A41C: state byte u8 @0x8013B7D0 indexes handler table @0x800F4590
  = {0 init 0x80029E1C, 1 run 0x8002A098, 2 exit 0x8002A394}; handler(a0 = screen obj).
  Live: state byte = 1 while on the screen.
- Dispatcher is entry 10 of a scene table @0x800F21B0..0x800F21D8 (title-side scenes);
  also jumped to from 0x80030E6C (likely the OPTION menu selecting BUTTON CONFIG; unconfirmed).
- No config writes happened during boot without a memory card (RAM == EXE default).
- Debug `press` takes ACTIVE-LOW masks (0xFFFF = none). drive.py inverts.
- FIXED (framework, uncommitted): `screenshot_file` black frames. Not a draw/display-buffer mixup:
  `gl_renderer_fbo_peek` (per-vblank display ring) packed hi-res→raw before flushing queued
  flat/tex batches, consuming their `s_pack_dirty`; `ensure_cpu` then read a stale (post-fill
  black) raw mirror. Fix: flush batches first in `fbo_peek` and `vram_diff`
  (runtime/src/gpu_gl_renderer.c). Regression check in runtime/tests/test_gl_readback_region.c.
  X4 title: 0/100 black after (was 7/30 and 27/40).

## Mac build state
- build/MegaManX4Recomp (arm64) works: title + OPTION menus render; SDL3 backend
  (`-DPSX_SDL_BACKEND=SDL3`; SDL2 path no longer compiles).
- build/game.toml line 49 patched to `python3` + non-.exe recompiler (re-configure overwrites).

## Checklist
- [x] Debug build (PSX_DEBUG_TOOLS=ON) + write trace on 0x800EE430..0x800EE44F → func_8002A098.
- [x] Confirm action order of entries 4–10 at runtime (on-screen labels match).
- [x] Find BUTTON CONFIG entry point → dispatcher func_8002A41C, state byte 0x8013B7D0.
- [x] Fix screenshot_file black-frame bug (framework rule 15) — commit in psxrecomp-v4 pending.
- [x] Confirm 0x80030E6C is OPTION→BUTTON CONFIG (state 2 of 0x80030DF8); exit path traced.
- [x] Framework (recomp-ui): Controls page in the game-menu provider — Capcom-style chooser +
      Keyboard / Controller pages, capture on highlighted row, swap on clash, host-hotkey warn
      (second press confirms), Start's key exits. API in recomp_ui_psx_menu.h:
      `recomp_ui_psx_controls_set_actions / _open / _is_open`. Files: consoles/psx/
      psx_controls_page{.h,.cpp,_imgui.cpp}; provider pauses while open, `reload_bindings` per write.
      Lato Bold/Regular from assets/fonts, synthetic slant; PS face buttons drawn as shapes.
- [x] X4: BUTTON CONFIG opens the page (src/mods/mmx4_controls.c, package
      mods/preloaded/packages/mmx4.enhancement.controls, filters 0x80029E1C + 0x8002A098 in
      game.toml). Off → Capcom's screen. Verified in build-debug: page renders, rebinds write
      keybinds.ini, hotkey warn shows. Exit → OPTION not yet confirmed in game.
- [x] Tests: recomp-ui tests/psx_controls_page_test.cpp (10 partitions incl. triangle cancel, cues).
- [x] Game art: X4 mod decodes font/diamond/backdrop from VRAM (psx_mod_read_vram) into a skin;
      recomp-ui draws it at native 320x240 (Lato kept as fallback). Diamond spins (3 frames / 7 vbl).
- [x] Menu sounds: page cues -> X4 sfx 0x8001540C (0x0C move, 0x22 select); page no longer pauses.
- [x] Framework: blanked display keeps presenting while an overlay holds input (main.cpp).
- [x] Menu sounds confirmed in game by ear.
- [x] Capcom's own prompts name the key: "✕ (W) ···CHOOSE" (src/mods/mmx4_prompt_keys.c, feature
      prompt-keys). Object renderer 0x80024334 drawn twice per call; parens drawn into empty item-set
      cells 0xBE/0xBF via psx_mod_write_vram. Verified on OPTION.
- [ ] Check prompt keys on other screens with ✕/△ prompts (stage select, weapon get, game over).
- [ ] Reset to PC layout (not in the approved design; decide whether to add a row).
- [ ] Framework: per-game action labels from game.toml `[controls]` (labels live in the X4 mod now).
- [ ] X4: pin default table after load (hook after func_8001C210/func_8001C30C).
- [ ] Controller page untested with a real pad (unit-tested with synthetic SDL events only).
