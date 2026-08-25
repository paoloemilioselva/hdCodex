
## Gallery

These images are versioned visual baselines, not golden-reference renders.
Intel Sponza and the StandardShaderBall variants currently exercise unsupported
material fallbacks; KitchenSet exercises the unbound-mesh `displayColor`
fallback. Image diffs should therefore reveal both intentional improvements and
regressions as those paths evolve.

### Intel Sponza Base Scene
**Reference:** [Intel Graphics Research Samples](http://intel.com/content/www/us/en/developer/topic-technology/graphics-research/samples.html)

**Command:**
```cmd
.\render_codex.bat --imageWidth 512 --camera PhysCamera001 gallery\intel_sponza.usda gallery\intel_sponza.jpg
```
**Render Time:** 18.54 seconds

![Intel Sponza Base Scene](gallery/intel_sponza.jpg)

### OpenChessSet
**Reference:** [OpenChessSet Repository](https://github.com/usd-wg/assets/tree/main/full_assets/OpenChessSet)

**Command:**
```cmd
.\render_codex.bat --imageWidth 512 --camera renderCam gallery\chess_board.usda gallery\chess_board.jpg
```
**Render Time:** 32.20 seconds

![OpenChessSet](gallery/chess_board.jpg)

### StandardShaderBall BubbleGum
**Reference:** [StandardShaderBall Repository](https://github.com/usd-wg/assets/tree/main/full_assets/StandardShaderBall)

**Command:**
```cmd
.\render_codex.bat --imageWidth 512 --camera camera gallery\shader_ball_bubblegum.usda gallery\shader_ball_bubblegum.jpg
```
**Render Time:** 9.32 seconds

![StandardShaderBall BubbleGum](gallery/shader_ball_bubblegum.jpg)

### StandardShaderBall Glass
**Reference:** [StandardShaderBall Repository](https://github.com/usd-wg/assets/tree/main/full_assets/StandardShaderBall)

**Command:**
```cmd
.\render_codex.bat --imageWidth 512 --camera camera gallery\shader_ball_glass.usda gallery\shader_ball_glass.jpg
```
**Render Time:** 9.33 seconds

![StandardShaderBall Glass](gallery/shader_ball_glass.jpg)

### StandardShaderBall Gold
**Reference:** [StandardShaderBall Repository](https://github.com/usd-wg/assets/tree/main/full_assets/StandardShaderBall)

**Command:**
```cmd
.\render_codex.bat --imageWidth 512 --camera camera gallery\shader_ball_gold.usda gallery\shader_ball_gold.jpg
```
**Render Time:** 9.33 seconds

![StandardShaderBall Gold](gallery/shader_ball_gold.jpg)

### Pixar's KitchenSet
**Reference:** [Pixar's KitchenSet](https://openusd.org/release/dl_kitchen_set.html)

**Command:**
```cmd
.\render_codex.bat --imageWidth 512 --camera renderCam gallery\pixar_kitchen.usda gallery\pixar_kitchen.jpg
```
**Render Time:** 8.38 seconds

![Pixar's KitchenSet](gallery/pixar_kitchen.jpg)

### Collective Project 001
**Reference:** [Collective Project 001](https://github.com/usd-wg/collectiveproject001/blob/main/shots/s001_001/index.usda)

**Command:**
```cmd
.\render_codex.bat --imageWidth 512 --purposes render --camera mono gallery\collectiveproject001.usda gallery\collectiveproject001.jpg
```
**Render Time:** 13.36 seconds

![Collective Project 001](gallery/collectiveproject001.jpg)
