# Contributing to Live & Recompiled

You may open issues for requesting features or reporting bugs, and submit PRs if you fix a bug or want to add a new feature yourself. There is no guarantee it will be merged, but you won't be laughed at either. We DO accept LLM-written PRs obviously.

**Bug reporting advice**

If you notice a bug, you can take a frame capture by pressing F3. It will be saved in a subdirectory of the `cap` directory. *Please remove the `tex_XXXXXXX.bmp` and `.hlsl` files from the capture directory*, as they come from the game itself and must not be submitted. Then zip the directory and attach it to your bug report.

If you notice frame interpolation glitches at high FPS, run the game with `XBE_INTERP_RECORD=120` (or whichever framerate you are playing at) and press F4 *immediately after* experiencing the glitch. Captures are generated inside the `cap` directory with the `interp_` prefix. Zip and attach the generated directory to your bug report.
