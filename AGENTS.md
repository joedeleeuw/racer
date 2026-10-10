# RACER runtime instructions

This repository owns the Star Wars Episode I: Racer N64 static recompilation. `symbols.toml` and `recomp.toml` own guest discovery and generation; `tools/post_recomp_patches.py`, `tools/fix_statics.py`, and the generation tools preserve necessary native redirects. Generated `RecompiledFuncs/` is ignored. Keep lasting fixes in source or generation owners.

`src/main.cpp` owns host startup and guest ABI adapters, `src/rt64_render_context.cpp` owns RT64 rendering, and `src/audio.cpp` owns host sample playback. The sibling N64ModernRuntime and RT64 repositories are independent owners; read both of their context files before editing them.

The current acceptance runtime is Steam Deck: SSH `deck`, checkout `/home/deck/racer-dev/racer`, Arch distrobox `arch`, executable `build-deck/SWE1Racer`. Build inside the distrobox with `cmake --build build-deck -j4`. Local source lives at `/Users/sushi/code/racer-dev/racer`. Preserve dirty and shared paths; copy only files owned by the current fix after comparing source hashes.

Acceptance requires repeatable menu/startup through sustained gameplay with real render and host input evidence. Display-list or VI counters alone do not prove gameplay. Name incomplete runtime capabilities honestly. Do not use fabricated guest state, stubbed services, or canned input as acceptance proof. Each probe needs a changed binary or a concrete new hypothesis; preserve complete raw output once in a unique artifact directory.

Before every runtime probe inspect live game/debugger processes and never overwrite active resources. Use GDB as the inferior's parent when ptrace restrictions prohibit attach. Own and wait for every launched job; clean up only processes launched by the current probe. Keep durable evidence outside Git and substantive findings in `docs/field-notes/YYYY-MM-DD.md`.

Recompiled guest memory uses native word storage with XOR byte/halfword accessors. Guest addresses passed to MEM_* must retain the sign-extended 64-bit register representation; do not truncate an address before using these macros. Decode hardware requests at the native adapter and preserve the guest's command layout.
