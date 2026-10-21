#!/usr/bin/env python3
"""Generate firmware constants from the pinned OpenXiino source and HTML5 entities."""
import ast
import html.entities
from pathlib import Path

root = Path(__file__).resolve().parents[1]
upstream = root / "third_party/openxiino"
assets = ["// Generated from OpenXiino e9ef506; see third_party/openxiino/LICENSE.\n"]
for name in ("about", "about2", "github"):
    assets.append(f'static const char {name}_page[] = R"XIINO(' +
                  (upstream / f"{name}.html").read_text() + ')XIINO";\n')
(root / "src/xiino_assets.inc").write_text("".join(assets))
tree = ast.parse((upstream / "lib/xiino_palette_common.py").read_text())
palette = next(ast.literal_eval(node.value) for node in tree.body
               if isinstance(node, ast.Assign) and node.targets[0].id == "PALETTE")
(root / "src/xiino_palette.inc").write_text(
    "// OpenXiino palette; see third_party/openxiino/LICENSE.\n"
    "static const unsigned char xiino_palette[][3] = {\n" +
    ",\n".join("{%d,%d,%d}" % colour for colour in palette) + "\n};\n")
entities = ["// HTML5 character references, generated from Python html.entities.html5.\n",
            "static const Entity entities[] = {\n"]
for name, value in sorted(html.entities.html5.items()):
    encoded = "".join(f"\\{byte:03o}" for byte in value.encode())
    entities.append('{"' + name + '","' + encoded + '"},\n')
entities.append("};\n")
(root / "src/xiino_entities.inc").write_text("".join(entities))
