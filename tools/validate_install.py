from pathlib import Path

from pxr import Usd, UsdImagingGL


PLUGIN_ID = "HdCodexRendererPlugin"
SCENES = (
    Path(r"C:\Users\paolo\Desktop\openusd\Kitchen_set\Kitchen_set.usd"),
    Path(r"C:\Users\paolo\Desktop\code\assets\full_assets\OpenChessSet\chess_set.usda"),
)


def main() -> None:
    plugins = UsdImagingGL.Engine.GetRendererPlugins()
    names = {
        plugin: UsdImagingGL.Engine.GetRendererDisplayName(plugin)
        for plugin in plugins
    }
    if PLUGIN_ID not in plugins:
        raise RuntimeError(f"{PLUGIN_ID} was not discovered; found {names}")
    print(f"Discovered {PLUGIN_ID}: {names[PLUGIN_ID]}")

    for scene_path in SCENES:
        stage = Usd.Stage.Open(str(scene_path))
        if stage is None:
            raise RuntimeError(f"OpenUSD could not open {scene_path}")
        prim_count = sum(1 for _ in stage.Traverse())
        print(f"Opened {scene_path.name}: {prim_count} traversable prims")


if __name__ == "__main__":
    main()
