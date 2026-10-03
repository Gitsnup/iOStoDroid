"""Offline source reconstruction for imported IPAs.

Pipeline: Mach-O images -> architecture slices -> disassembly -> function
discovery -> control-flow graph -> IR records -> data/control-flow analysis ->
Objective-C/Swift metadata recovery -> dependency graph -> readable listing.

Nothing in this package executes the imported machine code. The reconstruction is
an engineering artifact used to decide what a conversion would require; it is
explicitly **not** claimed to be the original source.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from pathlib import Path

from . import apis, objc, source, swift
from .disasm import disassemble
from .image import load

MAX_IMAGES = 8
MAX_SLICES_PER_IMAGE = 4
MAX_FUNCTIONS_IN_REPORT = 200
ARCH_PRIORITY = {"arm64": 0, "arm64e": 1, "armv7s": 2, "armv7": 3, "arm32-unknown": 4}


@dataclass
class SliceReconstruction:
    architecture: str
    entry: int | None = None
    functions: list = field(default_factory=list)
    listing: list = field(default_factory=list)
    objective_c: object = None
    swift_runtime: object = None
    apis: dict = field(default_factory=dict)
    call_graph: dict = field(default_factory=dict)
    stats: dict = field(default_factory=dict)
    error: str | None = None

    def report(self) -> dict:
        return {
            "architecture": self.architecture,
            "entryPoint": f"0x{self.entry:x}" if self.entry else None,
            "disassembly": self.stats,
            "functionCount": len(self.functions),
            "functions": [f.report() for f in self.listing[:MAX_FUNCTIONS_IN_REPORT]],
            "objectiveC": self.objective_c.report() if self.objective_c else None,
            "swift": self.swift_runtime.report() if self.swift_runtime else None,
            "apis": self.apis,
            "callGraph": self.call_graph,
            "error": self.error,
        }


@dataclass
class ImageReconstruction:
    path: str
    slices: list[SliceReconstruction] = field(default_factory=list)
    error: str | None = None

    def report(self) -> dict:
        return {"path": self.path, "slices": [s.report() for s in self.slices], "error": self.error}


def _ordered_slices(info: dict) -> list[dict]:
    ordered = sorted(
        info.get("slices", []), key=lambda s: ARCH_PRIORITY.get(s.get("architecture", ""), 50)
    )
    return ordered[:MAX_SLICES_PER_IMAGE]


def reconstruct_slice(path: Path, slice_info: dict, max_instructions: int) -> SliceReconstruction:
    item = SliceReconstruction(architecture=slice_info.get("architecture", "?"))
    try:
        image = load(path, slice_info)
        item.entry = image.entry
        runtime = objc.recover(image)
        item.objective_c = runtime
        item.swift_runtime = swift.recover(image)
        functions, stats = disassemble(image, budget=max_instructions)
        item.stats = stats
        item.functions = functions
        helper = source.Reconstructor(image, runtime)
        item.listing = [helper.function(f) for f in functions[:MAX_FUNCTIONS_IN_REPORT]]
        item.apis = apis.analyze(image, functions)
        item.call_graph = source.call_graph(functions, image)
    except (struct.error, ValueError, MemoryError) as exc:
        item.error = f"{type(exc).__name__}: {exc}"
    except Exception as exc:  # noqa: BLE001 - a reconstruction failure must not break analysis
        item.error = f"{type(exc).__name__}: {exc}"
    return item


def reconstruct_image(path: Path, info: dict, max_instructions: int = 400000) -> ImageReconstruction:
    result = ImageReconstruction(path=path.name)
    for slice_info in _ordered_slices(info):
        result.slices.append(reconstruct_slice(path, slice_info, max_instructions))
    return result


def reconstruct(
    app: Path,
    analyses: dict[str, dict],
    log=None,
    max_images: int = MAX_IMAGES,
    max_instructions: int = 400000,
) -> dict:
    """Reconstruct every Mach-O image in the bundle that has analyzer output."""
    images: list[ImageReconstruction] = []
    for relative, info in list(analyses.items())[:max_images]:
        path = app / relative
        if not path.is_file():
            continue
        if log:
            log("reconstruct", f"{relative}: {len(info.get('slices', []))} slice(s)")
        images.append(reconstruct_image(path, info, max_instructions))
    return {
        "schemaVersion": 1,
        "imageCount": len(images),
        "images": [image.report() for image in images],
        "status": "ok" if images else "unavailable",
    }
