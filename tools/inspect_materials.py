"""Print composed UsdShade material networks for renderer diagnostics."""

import argparse

from pxr import Usd, UsdShade


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("scene")
    parser.add_argument("--limit", type=int, default=40)
    args = parser.parse_args()

    stage = Usd.Stage.Open(args.scene)
    if not stage:
        raise RuntimeError(f"could not open {args.scene}")

    count = 0
    for prim in stage.Traverse():
        if not prim.IsA(UsdShade.Shader):
            continue
        shader = UsdShade.Shader(prim)
        print(f"{prim.GetPath()} id={shader.GetIdAttr().Get()}")
        for shader_input in shader.GetInputs():
            sources, _ = shader_input.GetConnectedSources()
            source_text = ", ".join(
                f"{source.source.GetPrim().GetPath()}.{source.sourceName}"
                for source in sources
            )
            value = shader_input.Get()
            if value is not None or source_text:
                color_space = shader_input.GetAttr().GetColorSpace()
                color_space_text = (
                    f" colorspace={color_space}" if color_space else ""
                )
                print(
                    f"  {shader_input.GetBaseName()}: {value!r}"
                    f"{color_space_text} <- {source_text}"
                )
        count += 1
        if count >= args.limit:
            break
    print(f"inspected {count} shader prims")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
