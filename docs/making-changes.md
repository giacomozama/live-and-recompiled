# Making changes

Recipes for the common kinds of change, and the rules that have caused bugs when ignored. Read
[architecture.md](architecture.md) first.

## SDK or game?

- **`game/sdk/`** gets behaviour of the Xbox, the XDK or the host that any title could hit: a kernel export, an
  NV2A method the decoder drops, a renderer feature. Keep it title-agnostic: gate on `HAS_<name>` / config values,
  never on Conker addresses. Changes can go back upstream with `git subtree split --prefix=game/sdk`.
- **`game/src/` and `game/config/game.toml`** get anything that names a Conker address or engine structure.
- An SDK fix is often found through a Conker symptom. Comments in the SDK may cite Conker as the example (many do),
  but the code shouldn't depend on it.

Pull upstream SDK fixes with
`git subtree pull --prefix=game/sdk https://github.com/TGP482/XboxXBERecomp.git main`.

## Recipes

### Replace a guest function

```cpp
// game/src/game/hooks.cpp
X86_FUNC(sub_004C3F2F) {       // strong definition: replaces the recompiled one
    (void)base;
    kernel::Return(ctx, 0, 2);  // eax = 0, pop 2 stack arguments (stdcall)
}
```

Read the function's `ret n` in the disassembly to get the pop count right: `n / 4` stack arguments, 0 for cdecl. A
wrong count corrupts the caller's `esp`, and the crash usually shows up later somewhere unrelated. Arguments are at
`ctx.esp + 4`, `+8`, … on entry (`kernel::Args` reads them).

### Wrap a guest function (run code before/after the original)

```cpp
X86_EXTERN_FUNC(__imp__sub_0053AD90);
X86_FUNC(sub_0053AD90) {
    d3d::FlushPending();
    __imp__sub_0053AD90(ctx, base);
}
```

### Patch in the middle of a function

Add a `[[hook]]` to `game.toml` and define the function in `src/game` (see `display.cpp`):

```toml
[[hook]]
at = 0x64468               # the instruction address
call = "ConkerCameraAspect" # void ConkerCameraAspect(X86Context& ctx, uint8_t* base)
# after = true             # run after the instruction instead of before
# on_true = "ret" / on_false = 0x...   # the function returns bool; leave or jump within the function
```

The hook sees and may change registers and flags in `ctx`. This needs codegen to run again (any build does it,
since the config changed).

### Give the runtime a library function or global

- A function: add `Name = 0xADDR` to `[symbols]`. If the LTCG build passes arguments in registers, add its map to
  `[abi]` (API argument order, `"stack"` for the next stack slot), proven from the disassembly. The runtime's HLE for
  that name then replaces it; `GUEST_CALL(Name, ctx, {…})` calls it from the host.
- A global: add it to `[data]`. Verify it against the code that writes it: the signature scan has reported a
  *field* of a table as the table's base before (`D3D_DeferredTextureState`, `D3D_Streams`).
- Implement a new HLE in `game/sdk/runtime/xdk/` with `HLE_STDCALL(Name, nargs) { … return value; }`; `a[i]` is API
  argument *i*. Review what the library body did besides its return value (pushes, device fields, dirty flags) and
  reproduce what later code depends on.

### Fix the recompiler's analysis

In `game.toml`: `[functions]` for an entry reached only through a pointer (`0x1E6590`), `[[function]] at, end` to
bound one, `[[switch]] at, targets` for a jump table it misreads, `[xbe] data_ranges` for data inside code sections,
`[xbe] trace = [...]` to log every call of some functions with arguments and results.

### Add a setting

1. A global in `src/game` (`bool` or `uint32_t`), declared in a `src/common/*.h` header.
2. An entry in `app::kGame.options` (`game_info.cpp`): it becomes a `settings.toml` key and is saved with the
   rest. Settings that need more than a value (the `[keys]` table) go through the `load`/`save` hooks.
3. A control in the right tab of `ui.cpp`.
4. Read the global on each use, not once at startup, so the F1 menu applies it live. Add an `XBE_*` environment
   override if the harness should be able to set it.

### Handle a raw NV2A method the engine writes

1. Look the method up in xemu (`xemu/hw/xbox/nv2a/`, the reference tree in this workspace) for what the chip does.
2. Decode it in `Walk()` (`game/sdk/runtime/xdk/d3d8.cpp`). Storing to `simple[]` is the default; that only helps
   if a renderer reads that register.
3. Decide the ownership rule: which D3D call or flush takes the state back, and record that in `gpu::State` if the
   renderers need to know (the `chip_*` flags).
4. Make the renderers read it (`d3d12.cpp`, `shaders.cpp`; and `harness/reference_renderer.cpp` if harness frames
   should show it).

### Change the D3D12 renderer

- If a `PixelKey`, `PsoKey` or vertex record changes meaning without changing size, bump `kRecordRevision` in
  `shader_record` so old `shaders.rec` files start over.
- New per-draw state belongs in `DrawConstants` / the keys, never read from guest memory on the render thread unless
  the console GPU would read it at draw time too.
- Add what the change depends on to the F3 capture (`CaptureDraw`), so the next investigation can see it.

## Rules that have bitten

- **Never distribute game code.** No `Game.exe`, generated sources, objects, dumps or `shader_programs.bin` in
  releases, issues or chats. Releases are made with `tools/package.sh`; see [distribution.md](distribution.md).
  Keep the host side free of the title's code too: `libGame_host.a` ships to players.

- **Walk before you apply.** Any new HLE that changes GPU state or draws must `Walk(Dev(kDevPush))` first. This has
  caused black frames, missing glow and shadow smears.
- **An HLE'd body's side effects are gone.** Model what later code reads (see the list in architecture.md).
- **Pointer identity.** Renderers cache shaders by object pointer; synthesize one object per key and keep it alive.
- **Physical addresses.** Jump targets, inline texture offsets and vertex array offsets are physical; read them as
  `0x80000000 | pa`. Reading a raw low address faults.
- **Don't read D3D objects at `Present`.** The title reuses them; a render-target header there may already be a bare
  refcount. Read them at draw time.
- **Release builds only log `LOGR` / `LOGE`.** Opt-in traces must use `LOGR` or they vanish from Proton logs.
- **Never edit generated code.** Change the config or add a hook. Never regenerate `game.toml` with `xbe_symbols`
  (it drops the hand edits).
- **Frames are not reproducible**, run to run: the game's animation follows the wall clock. Compare structure or
  averages, not single-run draw counts.
- **The harness is not the renderer.** It has no render thread, answers every visibility test "visible", keeps 4:3
  and 30 FPS unless told otherwise, and its reference renderer is a separate implementation. Visual fixes are
  confirmed on Proton.
- **Threads.** Hooks run on guest threads, `ui::Draw` on the render thread, `ui::Message` on the window thread.
  Settings globals are read across threads without locks by design (single words, applied live); anything bigger
  needs a lock.
