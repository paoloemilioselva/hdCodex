
## Gallery

These images are versioned visual baselines, not golden-reference renders.
The checked-in baselines are 1024 pixels wide and use 1024 spatial samples to
reduce residual path-tracing noise. They render in 32 progressive updates.
The StandardShaderBall variants exercise OpenPBR glass, metal, subsurface, and
image-texture lowering. KitchenSet remains in its authored Z-up coordinates and
exercises the unbound-mesh `displayColor` fallback. CollectiveProject exercises
UsdSkel deformation and per-face material subsets. Image diffs should reveal
both intentional improvements and regressions as those paths evolve.

### Intel Sponza Base Scene
**Reference:** [Intel Graphics Research Samples](http://intel.com/content/www/us/en/developer/topic-technology/graphics-research/samples.html)

This asset's bound materials are USD-native `UsdPreviewSurface` networks. They
are intentionally reported as unsupported rather than translated into
MaterialX, so this baseline exercises the bound-material fallback and shader
provenance diagnostics.

**Command:**
```cmd
set "HDCODEX_SAMPLES_PER_PIXEL=1024"
set "HDCODEX_SAMPLES_PER_UPDATE=32"
.\render_codex.bat --imageWidth 1024 --camera PhysCamera001 gallery\intel_sponza.usda gallery\intel_sponza.jpg
```

![Intel Sponza Base Scene](gallery/intel_sponza.jpg)

### OpenChessSet
**Reference:** [OpenChessSet Repository](https://github.com/usd-wg/assets/tree/main/full_assets/OpenChessSet)

**Command:**
```cmd
set "HDCODEX_SAMPLES_PER_PIXEL=1024"
set "HDCODEX_SAMPLES_PER_UPDATE=32"
.\render_codex.bat --imageWidth 1024 --camera renderCam gallery\chess_board.usda gallery\chess_board.jpg
```

![OpenChessSet](gallery/chess_board.jpg)

### StandardShaderBall BubbleGum
**Reference:** [StandardShaderBall Repository](https://github.com/usd-wg/assets/tree/main/full_assets/StandardShaderBall)

**Command:**
```cmd
set "HDCODEX_SAMPLES_PER_PIXEL=1024"
set "HDCODEX_SAMPLES_PER_UPDATE=32"
.\render_codex.bat --imageWidth 1024 --camera camera gallery\shader_ball_bubblegum.usda gallery\shader_ball_bubblegum.jpg
```

![StandardShaderBall BubbleGum](gallery/shader_ball_bubblegum.jpg)

### StandardShaderBall Glass
**Reference:** [StandardShaderBall Repository](https://github.com/usd-wg/assets/tree/main/full_assets/StandardShaderBall)

**Command:**
```cmd
set "HDCODEX_SAMPLES_PER_PIXEL=1024"
set "HDCODEX_SAMPLES_PER_UPDATE=32"
.\render_codex.bat --imageWidth 1024 --camera camera gallery\shader_ball_glass.usda gallery\shader_ball_glass.jpg
```

![StandardShaderBall Glass](gallery/shader_ball_glass.jpg)

### StandardShaderBall Gold
**Reference:** [StandardShaderBall Repository](https://github.com/usd-wg/assets/tree/main/full_assets/StandardShaderBall)

**Command:**
```cmd
set "HDCODEX_SAMPLES_PER_PIXEL=1024"
set "HDCODEX_SAMPLES_PER_UPDATE=32"
.\render_codex.bat --imageWidth 1024 --camera camera gallery\shader_ball_gold.usda gallery\shader_ball_gold.jpg
```

![StandardShaderBall Gold](gallery/shader_ball_gold.jpg)

### Pixar's KitchenSet
**Reference:** [Pixar's KitchenSet](https://openusd.org/release/dl_kitchen_set.html)

**Command:**
```cmd
set "HDCODEX_SAMPLES_PER_PIXEL=1024"
set "HDCODEX_SAMPLES_PER_UPDATE=32"
.\render_codex.bat --imageWidth 1024 --camera renderCam gallery\pixar_kitchen.usda gallery\pixar_kitchen.jpg
```

![Pixar's KitchenSet](gallery/pixar_kitchen.jpg)

### Collective Project 001
**Reference:** [Collective Project 001](https://github.com/usd-wg/collectiveproject001/blob/main/shots/s001_001/index.usda)

**Command:**
```cmd
set "HDCODEX_SAMPLES_PER_PIXEL=1024"
set "HDCODEX_SAMPLES_PER_UPDATE=32"
.\render_codex.bat --imageWidth 1024 --purposes render --camera mono gallery\collectiveproject001.usda gallery\collectiveproject001.jpg
```

![Collective Project 001](gallery/collectiveproject001.jpg)

### OpenPBR Playground
**Reference:** [OpenPBR Playground](https://github.com/DigitalProductionExampleLibrary/OpenPBRShaderPlayground/blob/main/ShdrPlygrnd/ShdrPlygrnd_OpenPBR.usda)

This is the MaterialX/OpenPBR generation baseline. The authored `OJfoam`
material has a color3-to-float `geometry_opacity` interface mismatch and is
intentionally diagnosed and omitted.

**Command:**
```cmd
set "HDCODEX_SAMPLES_PER_PIXEL=1024"
set "HDCODEX_SAMPLES_PER_UPDATE=32"
.\render_codex.bat --imageWidth 1024 --purposes render --camera renderCam_mainCU gallery\openpbr_playground.usda gallery\openpbr_playground.jpg
```

![OpenPBR Playground](gallery/openpbr_playground.jpg)
