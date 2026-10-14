# Conker: Live & Reloaded — Reverse Engineering Concepts

> **Purpose.** This document summarizes the reverse-engineering knowledge captured in the
> ResHax thread *"[Xbox] Conker Live and Reloaded models"*
> (<https://reshax.com/topic/1835-xbox-conker-live-and-reloaded-models/>).
> It is written as **reference documentation to support an AI-assisted recompilation project**
> for the original-Xbox game *Conker: Live & Reloaded*.
>
> Constants, addresses, byte patterns, struct definitions, code snippets and file paths are
> preserved from the thread. Claims marked *(uncertain)* or *(WIP)* were left unresolved or
> under investigation by the thread participants at the time of writing.
>
> Full verbatim posts: `page-01.md` … `page-13.md` / `all-pages.md` (same directory).

---

## 0. Primary contributors & roles

| Handle | Contribution |
| --- | --- |
| **birdytsc** | Author. Reverse-engineered the compression and wrote `clr_unpack`; documented structs/data, scenegraph, pushbuffers, LVL/CAFF containers. |
| **Über Winfrey** | Author of the Blender `.rbm` importer; materials, textures, skinning, maps, lights, fur/eye shaders, animation, and later an Unreal Engine game loader. |
| **MilesMontana** | Primary tester; wrote the end-user step-by-step walkthrough; catalogued import bugs. |
| **shak-otay** | Knowledge of Mariusz Szkaradek123 / Xentax Blender 2.49b scripts. |
| **Shadowth117** | Confirmed the modern re-release files are the originals under emulation (Xbox 360 archives). |
| **kunigara** | Kameo (Xbox alpha) cross-format work. |
| **SpecularBark45 / Kritzkrig / IWantConkerLiveModelsNow / MrBlue2838 / uyaetaite77 / others** | Testing, assets, Kameo prototype, legacy tools. |

---

## 1. Toolchain

### 1.1 `clr_unpack` — decompressor + pointer mapper (birdytsc)
- Repo: <https://github.com/birdytsc/clr_unpack>
- Extracts the x86 decompression routine from the game (via Xemu debugging + IDA) and wraps it
  so game assets can be unpacked to a state similar to other Rareware titles.
- The decompressor also **runs the pointer remap**, which rebases file-local (relative) pointers
  to absolute ones — this is what makes it possible to navigate individual file connections
  inside the archives.
- **Usage:**
  ```bat
  :: full unpack (decompress) — produces <name>.unpacked
  clr_unpack default.rbm -d

  :: unpack a single asset by index — produces <name>.unpacked.mapped
  clr_unpack default.rbm -d index

  :: generate the listfile (names + offsets), then pick an index
  clr_unpack default.rbm -l
  ```
  - `-d` alone → decompressed but *not* pointer-mapped (`.unpacked`).
  - `-d index` → decompressed **and** pointer-mapped (`.mapped`) — this is the file the Blender
    importer needs. A correct mapped file ends in `.mapped`.
  - On Windows the binary is invoked as `clr_unpack` (not `clr_unpack.exe`) in some setups;
    run cmd as administrator and `cd` to the folder containing the `.rbm`.

### 1.2 Debugging / capture
- **Xemu + IDA** (birdytsc): extracted the x86 asset-decompression assembly.
- **Xemu + RenderDoc**: works, but must be compiled with the correct flags to step through the
  **NV2A guest frames / draw calls** instead of host frames. Reference:
  <https://github.com/abaire/nxdk_pgraph_tests/wiki/xemu-debugging-with-RenderDoc>
  - RenderDoc captures give good textures but **no armature** ("Xemu + Renderdoc is an old way
    of doing it"; requires heavy fixing).
- **Ghidra** (Über Winfrey): used to find face-data table code (see §4.3).

### 1.3 Blender `.rbm` importer (Über Winfrey)
~50% hand-written, ~50% GPT-assisted per the author. Evolved across the thread:

| Script | Notes |
| --- | --- |
| `RBMimport_animation_optimized3.py` | Early; had indentation/`\t`-vs-space and string-literal syntax errors. |
| `RBMimport_noAnimation.py` | Animations stripped out to avoid the indentation bug. |
| `RBMimport_noAnimation2.py` | + shader type detection, specular piping, cutout alpha, vertex color, fur length. |
| `RBMimport_noAnimation4.py` | + V8U8 bumpmaps, resource preload ("Preload ALL Textures"), map meshes, demo assets, prototype standalone models. |
| `RBMimport_noAnimation5.py` | + fixed material batching. |
| `RBMimport_7.py` | + light placement/colors, re-enabled multi-texture-per-handle autoimport (animated frames, alternate textures/eyes). |
| `RBMimport_12.py` | + fur support, per-shader custom materials (eyes/fur), **animation support** (Conker `Anims/Conker/Default.rbm`), search bar. |

- Requires **Blender 4.0+** (author demonstrated on 4.3.2; 5.0 unverified).
- Load model with the `.mapped` file, **not** the plain `.unpacked`.
- Run from the Blender **text editor** (load script → Run) rather than enabling as an addon.
- If a model misimports, reload the script (play button) and re-import, or open a fresh scene.

### 1.4 Texture extraction
- `batch_imgextract.py` → exports all textures currently in the scene; output path edited in-script.
- `batch_imgextract_2.py` → folder picker + extracts all images regardless of user standing.
- Use `/` (not `\`) in output paths.

### 1.5 Other tools referenced
- **Exiso** — dump the game ISO.
- **ImHex** (`*.hexpat`) — birdytsc's pattern files; `GbtgModelsV3.bt` (by "wrs", incomplete) for
  Grabbed by the Ghoulies.
- **ModelResearcherPro** — <https://mr.game-viewer.org/about_pro.php>; loads verts/tris via
  `aid_bfdmodel_characters_conker.py`, can export `.obj`.
- **Mariusz Szkaradek123 Blender 2.49b scripts** (Xentax) — work only on Blender **2.49b**;
  on Win7+ copy the `newGameLib` folder into the Blender root. Hard to port (opaque `g.i()/g.H()`
  style calls).
- **RareView** (<https://github.com/NCDyson/RareView>) — works for the Xbox 360 version, not OG Xbox.

---

## 2. Containers & archive formats

### 2.1 CAFF containers
- Assets are packaged inside individual **CAFF** containers.
- CAFF carries a **version string**, e.g. `CAFF28.01.05.0031` (found in a `.rbm`).
- The **Kameo alpha uses the exact same CAFF version as the Conker demo** — high cross-title overlap.
- `.rbm` / `.rba` files are CAFF containers. A model CAFF typically contains:
  a **model (header)**, a **scenegraph**, **textures**, and possibly **hits / animation data**
  (if relevant to the model and not shared across multiple models).
- `.bnl` is a related asset container (older Xentax-era scripts unpack it).

### 2.2 `.lvl` container (external archive)
- In some titles (Kameo, and loose debug builds) assets are stored inside an external `.lvl`
  container that holds individual CAFF files. **Conker instead stores assets inside a "ZPackage"
  CAFF container** rather than using an external `.lvl`.
- ImHex struct for the LVL container (birdytsc):
  ```c
  struct LVL_ENTRY
  {
      u32 unk_00;
      u32* address : u32;
  };

  struct LVL_FILE_TABLE
  {
      u32 count;
      LVL_ENTRY array[count];
  };

  LVL_FILE_TABLE table @ 0x00;
  ```
- The `.lvl` is "essentially like a zip/container file containing a bunch of `.rbm`/`.rba` (CAFF)
  files."

### 2.3 Pointer rebasing / segments
- CAFF archives are "designed just to be `malloc`'d into memory, and update the relative offsets to
  absolute pointers."
- The mapper parses a **table of pointers** in the CAFF container and adds the relative position to
  each one:
  - some pointers are rebased into **`.data`**,
  - some are rebased into **`.gpu`**.
- When decompressed but *not* mapped, files keep the **file-local pointers**, which makes
  following connections painful.

### 2.4 Dummy / orphaned asset entries (important for recomp)
- The `.xbe` asset-manager list contains many files that **do not exist** in the shipped game files
  — apparently reused N64-era source names / removed assets, e.g.:
  - **pikachu's tail**
  - different **unused Conkers**
  - numerous **CBD** files.
- These may be placeholder/dummy entries so the game doesn't crash when caching the asset list.
- `CombatFlag` archives for Tediz/SHC are **empty archives** (flags live in the level folders,
  probably `aid\zpackage\Level\Mp_Frontend` or another shared package).

---

## 3. Scenegraph & model structure

### 3.1 Scenegraph nodes
Every model/level has an associated **scenegraph** containing a **linked list of nodes**.
Observed node types:

- `ndVertexBuffer` / `ndVertexBuffer2` — vertex data (positions, normals, UVs, colors, skin indices…)
- `ndShaderParam2`
- `ndRigidSkinIdx`
- `ndVisibility` — often holds a pointer / relative offset to the triangle faces
- `ndStaticPushbuffer` — pointer (+size) to the start of a pushbuffer instruction array
- `ndPushBuffer` — in GBTG stores triangle data directly in node `.data`; in Conker points into `.gpu`
- `ndBlendShape2` — blendshapes (Blender importer enumerates keys prefixed `ndVertexBuffer2_`/`ndBlendShape2_`)
- A **model(header)** node at the top.

birdytsc's example chain (Conker):
```
ndVertexBuffer2 (verts/normals/uvs)
  -> ndShaderParam2
     -> ndRigidSkinIdx
        -> ndVisibility (pointer to an additional noop?)
        -> ndVisibility (sibling; contains a relative offset to the Tri faces)
```

### 3.2 Vertex data table
`ndVertexBuffer2` contains a **table** with a **pointer, size, and label** for each attribute, e.g.:
```
v:colour0, vposition, v:skinindices, v:normal, v:tex, ...
```
The importer keeps a GPU-id → vertex-buffer-key map (`gpu_to_buffer_key`, `gpu_to_buffer_key_pos`,
`gpu_to_buffer_key_all`) to associate face tables with their vertex buffer.

### 3.3 GPU vs data segments
- Triangle face data may live **directly in the `.data` segment** or be encoded as
  **pushbuffer instructions in the `.gpu` segment** (and pointed to from a `.data` node).
- Example: `retail_aid_zpackage_general_singleplayer.rbm` — an asset name pattern is
  `dvddata\aid\zpackage\...`; models like `dvddata\aid\bfdmodel\characters\SinglePlayerVehicles\MachineGunTurret`
  are uncompressed and directly inspectable.

### 3.4 Face tables
- In the `.xbe`, face-data table code is reachable at **`FUN_000417FC`** (Ghidra); files carry a
  flag **`FC 17 04 00`**.
- The final polygon is often left open ("stopping with a hole in the mesh") — scripts must **force
  the draw to close the final polygon**.
- **Multiple face-table formats exist**; the importer initially handled only one, which caused
  faces to be "stolen" by a previous vertex block on certain maps (`BeachRunup`, tree tops). Fixed
  by adding the missing format (Jan 2026).

---

## 4. Pushbuffers & NV2A rendering (core recomp knowledge)

### 4.1 What pushbuffers are
> *"Pushbuffers are the native rendering format of the Xbox GPU. When DirectX is used on the Xbox,
> the rendering instructions are converted into pushbuffers for execution on the GPU. A small
> subset of commands within the main render loop: draw primitive, z-writes, set vertexbuffer,
> select vertexshader, set pixelshader, etc, were reverse engineered to determine the resulting
> pushbuffer code. … Each object in the pushbuffer is preceded by a null operation (NOP), which is
> conditionally set as a NOP, if the following object is visible, and a jump, if the following
> object is not visible."* — State of Emergency port postmortem (quoted in-thread)

### 4.2 The leading NOP / reserved block
The first pushbuffer instruction is consistently a NOP with a reserved region:

```c
#define NV2A_NOINCREMENT_FLAG = 0x40000000
#define NV097_NO_OPERATION    = 0x100
D3DPUSH_ENCODE(NV2A_NOINCREMENT_FLAG | NV097_NO_OPERATION, 576);
```

- On disk this is the 4 bytes **`00 01 40 42`** (`0x42400100` little-endian), followed by **exactly
  576 bytes of `0x00`**, then the next pushbuffer instruction.
- `ndStaticPushbuffer` holds an **offset to the start of the pushbuffer array** (relative to the
  `.gpu` segment, written in-thread as `.gpu + <base>`).
- The 576-byte reserved block is theorized to be scratch space for later-written commands
  (the buffer is loaded into memory as-is first). This matches the "NOP if visible / jump if not"
  optimization from the interview.

### 4.3 NV2A command constants seen
| Name | Value / note |
| --- | --- |
| `NV097_NO_OPERATION` | `0x100` |
| `NV2A_NOINCREMENT_FLAG` | `0x40000000` |
| `NV097_SET_BEGIN_END` | `0x17FC` |
| `NV097_SET_VERTEX_DATA_ARRAY_OFFSET` | binds vertex buffers; also carries GPU ids (`0x1720`-style) |
| `NV097_INDEX_U16` / `NV097_INDEX_U32` | index elements (`ARRAY_ELEMENT16` / `ARRAY_ELEMENT32`) |
| `NV097_SET_COMBINER_ALPHA_ICW` | `0x0260` (combiner alpha) |
| begin/end primitives | `TRIANGLES`, `TRIANGLE_STRIP`, `TRIANGLE_FAN`, `END` |

### 4.4 Index packing oddities (unresolved)
- Comments in the Xbox *thug* source suggest a **2047-element limit** per single pushbuffer
  instruction (last 2 bits used as flags → 14 bits for size).
- In practice Conker loads faces "arbitrarily", not in multiples of 2047, e.g. a 1893-face part:
  ```
  NV097_ARRAY_ELEMENT16 -> 8
  NV097_ARRAY_ELEMENT16 -> 862
  NV097_ARRAY_ELEMENT16 -> 1022
  NV097_ARRAY_ELEMENT32 -> 1   (why 32-bit?)
  ```
- The final index is often 32-bit; if read as 32-bit the index never needs >16 bits, and if read as
  2×16-bit the last index is always `0x0000`. Significance *(uncertain)*.
- **Implementation note (importer):** "trust payload length, not `arg` bits" when reading index
  arrays; TRIANGLES are often encoded strip-like (single index stream) in Conker, so strip
  triangulation with alternating winding is used for both STRIP and TRIANGLES.

### 4.5 Face-building reference snippet
Key behaviors from Über Winfrey's replacement `build_faces()` / `process_pushbuffer_cmds()`:
- Reset GPU-id state at every **static-pushbuffer boundary** (`spb_ptr`).
- On `NV097_SET_VERTEX_DATA_ARRAY_OFFSET`, refresh GPU ids (`struct.unpack('<{n}I', payload)`).
- A **table opens on BEGIN** (TRIANGLES/STRIP/FAN) and **closes on END**, emitting one face table
  **per GPU id**; unmatched tables get `0xFFFFFFFF` and are resolved by static-pointer chain.
- `NV097_INDEX_U16` → read `<H` every 2 bytes; `NV097_INDEX_U32` → `<I` every 4 bytes.
- On END (or table end), flush collected indices: **fan** for FAN, else **strip** with winding flip
  every other triangle (`add_strip_tris(strip, even_flip_base)` / `add_fan_tris(fan)`).
- Public debug output example:
  ```
  === FACE BUILD ===
  'ndVertexBuffer2_00014328' → 32 faces, 16 blocks
  'ndVertexBuffer2_00014C68' → 4096 faces, 208 blocks
  ...
  'ndVertexBuffer2_000193D8' → 205792 faces, 1408 blocks
  'ndVertexBuffer2_0003A548' → 17568 faces, 144 blocks
  ```

---

## 5. Geometry

- Vertices/UVs/normals come from `ndVertexBuffer2`; **faces come from pushbuffers** and are stored
  in the same order as the vertex data found.
- **Mesh separation:** a model part (e.g. Conker's main model) is split into **3 sets of triangles
  sharing a single vertex set**; keeping them as one mesh produces artifacts, so separate tri-sets
  into their own meshes.
- **UV scaling issues** are common where the scale value is missing or not applied; fixes:
  - rescale UVs by ~`0.5` (2D cursor as pivot, then `g y .5` for the eye case), or
  - insert a **Mapping node** between the UV node and the texture sampler (has scale/rotation).
- **Mesh scaling flags**: some sneaker models carried an unrecorded scaling flag; import scale was
  off (one model needed ~`0.08`).
- **Bodygroups**: game files contain bodygroup/state variants (e.g. `Beach Dead` door has intact +
  damaged + broken states packed together). Importer does **not** yet separate bodygroups — done
  manually by isolating mesh selections into collections/objects.

---

## 6. Skeletons & skinning

- Models use **matrix palettes** with **bone remapping for GPU reasons** ("a right pain getting it
  to work for Conker").
- **Skin-weight limitation:** a mesh can carry at most **25 weighted bones**. Conker's **tail is a
  separate model** to bypass this limit (and because it's shared across all his skins/outfits). The
  main Conker models only contain the tail *bones*.
- **All characters reuse Conker's skeleton.** E.g. a character's ponytail is weighted to the tail
  bones but left connected to the waist, so it does not parent to the head — this is how the game
  is actually set up, not an import bug.
- **Multiple skin-weight formats exist.** Some models (`Bugga`, `JetCopterWW2`) use a "wildly
  different skin weight format"; support added case-by-case.
- **Bind pose / orientation:** the imported skeleton's bind pose/orientation was suspect; a stray
  piece of debug code was flipping orientation when baking to the skeleton (fixed May 2026).

---

## 7. Materials & textures

### 7.1 Texture storage & compression
- Textures are stored as **raw DXT data**, usually **DXT1** or **DXT5**; `.rbm` textures often carry
  **multiple LODs / mipmaps** (e.g. "256×256 DXT5 with 9 lods").
- Additional formats handled or discovered:
  - **L8** (`fmt_code 0x00`) — 1 byte/pixel luminance (was previewing "red-only").
  - **V8U8** — bumpmaps for "perturbation" shading (rain, drunkenness).
  - Missing DDS compression formats caused some textures not to decode.
  - Unknown formats remain (e.g. a `128×128` and `16×16` texture in a Kameo object).
- **Swizzling**: controlled by `FORCE_SWIZZLE` / `FORCE_SWIZZLE_BY_FMT`; format `0x28` forced
  swizzle, and `0x00` (L8) needed forcing too.

**L8 patch (Über Winfrey)** — replace the `# generic linear` block and force swizzle for `0x00`:
```python
# generic linear (ALL OTHER formats)
payload = self._unswizzle_linear_bytes(raw, w, h, spec['bpp'] // 8) if swz else raw

# --- PATCH: fmt 0x00 (L8) preview as RGBA so it doesn't show as "red-only" ---
if int(fmt_code) == 0x00:
    top_len = w * h  # 1 byte per pixel
    if len(payload) < top_len:
        print(f"[tex-decode] fmt0x00(L8) payload too small: have={len(payload)} need={top_len}")
        return None
    src = payload[:top_len]
    # Expand L8 -> RGBA8888 (R=G=B=A=v) (RenderDoc-like)
    out = bytearray(top_len * 4)
    for i, v in enumerate(src):
        j = i * 4
        out[j+0] = v  # R
        out[j+1] = v  # G
        out[j+2] = v  # B
        out[j+3] = v  # A
    rgba_masks = (0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000)
    return self._make_dds(w, h, bytes(out), bpp=32, rgba_masks=rgba_masks)

return self._make_dds(w, h, payload, bpp=spec['bpp'], rgba_masks=spec['rgba_masks'])
```
```python
self.FORCE_SWIZZLE = False   # None=auto, True=force on, False=force off
self.FORCE_SWIZZLE_BY_FMT = {0x28: True}
self.FORCE_SWIZZLE_BY_FMT[0x00] = True   # add this
```

### 7.2 Texture cache / resource files
- Some models reference textures that are **not inside their own archive**.
- A texture pointer array at the bottom of the `.Data` section (Demolisher example):
  ```
  00 00 00 00 00 00 00 00 A0 91 00 00 A0 92 00 00 00 00 00 00
  ```
  - If a texture is in the zpackage list, its pointer is **null** (`00 00 00 00`).
  - Null pointers also occur for textures kept in **resource files** (cache). The actual texture
    names are still recorded in the model file, e.g.:
    ```
    demolisher_Fbody.tga      in Resource file
    demolisher_detail.tga     in RBM
    ted_inside_texture_2.iff  in Resource file
    ```
- Resource files found so far (used by "Preload ALL Textures"):
  ```
  General/Singleplayer/
  Levels/MP_Frontend/
  General/Boot/
  General/TemporaryBoot/
  General/Multiplayer/
  ```
- Some multiplayer models don't import textures directly but have a package elsewhere that does.

### 7.3 UV channels
- Models can have **multiple UV channels**. Conker's **eyes have 3 UV channels**:
  1. **eye lids**, 2. **highlight**, 3. **pupil**.
- Channel selection was initially manual in the shader node editor; later automated.
- **Retail vs Demo Conker:** the **eye UV channel order differs** — Demo imports using the **pupil**
  channel as base, Retail uses the **eye-lid** channel as base.

### 7.4 Shaders
- The game uses **precompiled VSH/PSH** (Xbox vertex/pixel shaders), **not HLSL** — they must be
  decompiled from binary, and shader params + usage must be reverse-engineered.
- Material/shaders observed:
  - **Phong / specular** — `phong` uses the specular shader-type number but with different config.
  - **Specular** textures may be **separate** (e.g. `Barn_outside`) or **derived from alpha**.
  - **Cutout alpha** textures pipe into the alpha channel.
  - **Fur shader** (Conker's tail, Berri) — uses **fur shells** (raises polycount) plus a **fur mask**
    whose UV **scale/rotation** had to be set manually. **Berri uses a variant** with an extra
    texture.
  - **Eye shader** — an eye material template reproduces the reflection/highlight.
  - **Perturbation** shading — rain, drunkenness (V8U8 bumpmaps).
  - **Sheen mask** — a red texture used as a mask for a sheen-type effect *(uncertain)*.
  - **Lightmaps** — separate texture + UV channel; plug into **Emission** (Unity: multiply lightmap
    by albedo then into emission to avoid over-brightening).
- Importer note: originally the script **skipped the first shaderparam entry** (usually a default
  shadowmap color); models with a real texture there lost it. Also the **first StaticPushbuffer was
  being forced onto the material builder**, causing material misalignment for maps (fixed).

### 7.5 Texture handles
- A single texture **handle/file can contain multiple textures**; the importer gathers them into the
  scene cache. This yields **animated texture frames** and **alternate textures** (Conker's multiple
  pupil textures, fur masks, eye-lid textures; multiplayer character eye variants).

---

## 8. Animation *(WIP / recently working)*

- Animation clips live in animation RBMs, e.g. **`Anims/Conker/Default.rbm`**; level RBMs also carry
  animations (e.g. Conker's Dance 1 in the Rock Solid RBM).
- Approach: dump nearly every function from the `.xbe`, find **where the game dispatches the
  animation clip to the animation player**, then **re-implement the animation-clip processing
  function-by-function** — matching the **stack building exactly** and the exact values/registers.
- Status milestones:
  - Poses import; then bind-pose/orientation fix (a debug-code bug flipped orientation on bake).
  - **Movement works** (May 2026).
  - Later fixed a decoder bug calling a **nonexistent table address**, which enabled **other
    animations from level RBMs** to work.
  - **GMP** (Great Mighty Poo) animation support added.
- Practical notes:
  - Default import rate was **30 fps**, which kept animations **slower than game speed**.
  - Each animation is **decrypted frame-by-frame then keyframed** — loading can take **3–4 minutes**.
  - A caught bug: the animation **unloader caches memory for a few bones** (refresh the scene to load
    another animation).
  - Still missing: full **bone assignment** robustness and **skeleton orientation** correctness.

---

## 9. Levels & maps

- Maps use **multiple UV channels** and layered textures; the material importer remained buggy
  (lightmap/layering) for a while.
- **Lights**: light **placements and colors** import; Xbox attenuation differs from Blender/Unity so
  radius/attenuation need manual tuning. Maps can be lit **entirely by lightmaps** (no scene lights).
- **Missing mesh pieces** on some maps (e.g. `Beach Dead`, `BeachRunup`, tree tops) were traced to
  face-table/register-argument issues, not missing geometry.
- Missing props are listed in the **asset list** and must be placed manually.
- Moving map objects (e.g. pub door, capsule-room doors) are **skeleton-driven / weighted**, so they
  can be rigged; if a model has a skeleton it should be riggable.

---

## 10. Cross-title / Rareware engine notes

- **Grabbed by the Ghoulies (GBTG):** `ndPushBuffer` stores triangle data directly in node `.data`.
  A binary template `GbtgModelsV3.bt` exists (incomplete). Morph/expression data exists but the old
  2.49b tool doesn't add it.
- **Kameo (Xbox alpha, Oct 2004 prototype):**
  - `.lvl` files are containers of **uncompressed** CAFF packages, using the **same CAFF version as
    the Conker demo**.
  - Kameo also stores much data inside pushbuffers (triangles/shaders/shaderparams), unlike earlier
    GBTG.
  - Vertex data referenced via **localized pointers** (hard to follow).
  - `clr_unpack` (the **demo** build's version, same CAFF version) maps Kameo pointers fine once
    extracted from the `.lvl`.
  - Example object `tipping_cauldron` from `02d7d13f.lvl`; internal paths like
    `D:\Work\dolphin\Database\models\bin\Objects\...`.

---

## 11. Engine reimplementation / loader project (directly recomp-relevant)

Über Winfrey's post-Blender project: an **actual game loader** (not a Blender script), described as
"like those Mario 64 loaders where you have to provide your own game root folder", enabling modding.
- Implementation target: **Unreal Engine** port/loader.
- **The `.xbe` itself processes most of the game logic**; it feeds pushbuffers into an **HLSL
  Material** whose **vertex-shader and pixel-shader slots auto-fill**.
- Status: game boots all the way to the **menu**; a model viewer was being built for fast asset
  debugging; **shaders not yet implemented**, only mesh building.
- This is the strongest signal that a faithful recomp/reimplementation is feasible by **running or
  porting the original `.xbe` logic and reproducing the pushbuffer → GPU path**.

---

## 12. Content, versions & discoverable assets

- **Retail vs Demo Conker:**
  - Eye UV channel order differs (see §7.3).
  - **Tail hole is larger** in the demo.
  - Demo Conker has **higher-resolution textures** (e.g. 512×512 vs 128×128).
  - Demo contains an **N64 model** (e.g. a Sergeant Squirrel N64 model).
- **`Frontend_Table`** is the **N64 intro** complete with the **N64 logo** and associated files
  (slightly different format from the main L&R files). The **N64 logo and Rareware logo** import.
- **Unused / scrapped content:**
  - **Bounty Hunter** model (scrapped "Vendetta" mode); `MPBountyHunterSniperRifle.rbm`.
  - N64-era SHC grunts in the "It's War" maps.
  - A mangled Tediz head with the *Live and Uncut* design in the Labs level.
  - Berri's dresser and gargoyle statues contain easter eggs.
  - Almost all package files share **checksums with the retail game** — unreleased content is
    probably minimal.
- **Known asset names/paths referenced:**
  ```
  dvddata\aid\zpackage\general\singleplayer\...
  dvddata\aid\bfdmodel\characters\SinglePlayerVehicles\MachineGunTurret
  aid\zpackage\Level\Mp_Frontend
  Anims/Conker/Default.rbm
  MPBountyHunterSniperRifle.rbm
  aid_misc_ghoulies_actor_expressions_boy_angry   (morph faces)
  ```

---

## 13. Quick-reference constants & byte patterns

| Item | Value |
| --- | --- |
| Pushbuffer block header | `0x42400100` → bytes `00 01 40 42` |
| NOP payload size | 576 bytes of `0x00` |
| `NV097_NO_OPERATION` | `0x100` |
| `NV2A_NOINCREMENT_FLAG` | `0x40000000` |
| `NV097_SET_BEGIN_END` | `0x17FC` |
| `NV097_SET_COMBINER_ALPHA_ICW` | `0x0260` |
| Face-data flag | `FC 17 04 00` (code at `FUN_000417FC`) |
| Max weighted bones / mesh | 25 |
| Max elements / pushbuffer instr (theorized) | 2047 |
| CAFF version example | `CAFF28.01.05.0031` |
| Swizzle-forced formats | `0x28`, `0x00` (L8) |
| Texture-set pointer array | `00 00 00 00 … A0 91 00 00 A0 92 00 00 …` |

---

## 14. Open problems / unresolved items

- Exact rules for the **"arbitrary" face/index packing** and the **32-bit final index**.
- Fully general **face-table format** handling (multiple formats; caused missing faces on some maps).
- **Bodygroups** not implemented (packed mesh states must be separated manually).
- **Bind pose / skeleton orientation** and full **bone assignment** for animations.
- **Animation framerate/timing** (30 fps import below game speed).
- **Shader translation** (VSH/PSH → HLSL) — needed for correct fur/cloth/shine and for the Unreal loader.
- A few **texture formats** and **padding** cases still decode incorrectly.
- **DDS compression formats** not all implemented → occasional missing/blank textures.
- **Resource/cache texture** resolution not fully stable.
- *(uncertain)* Sheen mask semantics; Rock Solid orange/blue light-transition texture.

---

## 15. Artifacts / attachments referenced in the thread

Unpacker & patterns:
- `clr_unpack` — <https://github.com/birdytsc/clr_unpack>
- `conker_stuff.zip` (OP: ModelResearcherPro script, listfile CSV, `.unpacked`, `.unpacked.mapped`, `.hexpat`)
- `GbtgModelsV3.bt` (binary template, "wrs", incomplete)
- `Blender249[Rare][XBOX][2015-02-11].7z` (Mariusz Szkaradek123 script)
- `ghoulies_actor_player_boy.zip`
- `tipping_cauldron.zip` (Kameo ImHex pattern + extracted CAFF + ModelResearcherPro script)

Blender importers (chronological, approximate):
- `RBMimport_animation_optimized3.py`
- `RBMimport_noAnimation.py`
- `RBMimport_noAnimation2.py`
- `RBMimport_noAnimation4.py`
- `RBMimport_noAnimation5.py`
- `RBMimport_7.py`
- `RBMimport_12.py` (fur + animation + search bar)

Texture tools:
- `batch_imgextract.py`
- `batch_imgextract_2.py`

> Note: attachment download links require a ResHax account and may expire. Full URLs are preserved
> in the per-post attachments of `all-pages.md`.

---

## 16. Suggested recomp-relevant implementation order

1. **Archive layer:** CAFF parsing, version detection, pointer rebasing (`.data`/`.gpu`), `.lvl`
   container support. (`clr_unpack` behavior is the reference.)
2. **Scenegraph walker:** resolve `ndVertexBuffer2`, `ndShaderParam2`, `ndRigidSkinIdx`,
   `ndVisibility`, `ndStaticPushbuffer`, `ndBlendShape2`.
3. **NV2A pushbuffer decoder:** BEGIN/END, index U16/U32, array element counts, the 576-byte
   reserved NOP block, face-table extraction per GPU id, winding rules.
4. **Vertex/attribute binding:** map GPU ids → vertex buffers; attribute table labels
   (`vposition`, `v:normal`, `v:tex`, `v:colour0`, `v:skinindices`).
5. **Texture decode:** DXT1/DXT5, L8, V8U8; swizzle rules; mip/LOD handling; resource-cache
   resolution.
6. **Materials/shaders:** decompile VSH/PSH, map shader params (phong/specular/fur/eye/perturbation/
   lightmap) to engine (HLSL/Unreal) slots.
7. **Skinning:** matrix palettes, bone remapping, 25-bone limit, per-model weight formats.
8. **Animation:** mirror the `.xbe` animation stack build; timeline/bind-pose orientation.
9. **`.xbe` execution path:** since the `.xbe` "processes majority of the stuff" and feeds pushbuffers
   into HLSL slots, integrating/porting this path is the most faithful route to a recomp.
