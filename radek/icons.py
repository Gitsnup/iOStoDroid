"""Application icon recovery for imported IPA bundles.

Icons are resolved through an explicit fallback chain and every attempt is
recorded, so a report can show which source was used, which were tried, and why
others failed. Nothing is invented: when no decodable icon exists the result is
``UNAVAILABLE`` and callers show that state instead of a fake image.

Order (first decodable match wins):

1. icons named directly by ``Info.plist`` (``CFBundleIconFile``,
   ``CFBundleIconFiles``, ``CFBundleIcons``/``CFBundleIcons~ipad``)
2. those names with device/scale variants (``@2x``, ``@3x``, ``~ipad``, ...)
3. compiled asset catalogs (``Assets.car``), preferring ``CFBundleIconName``
4. other icon-like resources inside the bundle (``iTunesArtwork``, ``*Icon*``)
5. any remaining image resource in the bundle, ranked by icon likelihood
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from pathlib import Path

from . import assetcatalog, pngcodec
from .archive import InputError, safe_name

LAUNCHER_SIZE = 512
DISPLAY_SIZE = 256
MAX_CATALOG_BYTES = 256 * 1024 * 1024
MAX_FILE_CANDIDATES = 512

# Highest scale first: the best quality representation of an icon is preferred.
SUFFIXES = (
    "@3x.png",
    "@2x.png",
    ".png",
    "@3x~ipad.png",
    "@2x~ipad.png",
    "~ipad.png",
    "@3x~iphone.png",
    "@2x~iphone.png",
    "~iphone.png",
    "",
    "@3x.jpg",
    "@2x.jpg",
    ".jpg",
)


@dataclass
class Attempt:
    source: str
    kind: str
    detail: str
    ok: bool
    width: int | None = None
    height: int | None = None
    scale: float | None = None

    def as_dict(self) -> dict:
        return {
            "source": self.source,
            "kind": self.kind,
            "detail": self.detail,
            "ok": self.ok,
            "width": self.width,
            "height": self.height,
            "scale": self.scale,
        }


@dataclass
class IconResult:
    status: str
    image: bytes | None = None
    extension: str = "png"
    source: str | None = None
    kind: str | None = None
    format: str | None = None
    decoder: str | None = None
    width: int | None = None
    height: int | None = None
    scale: float = 1.0
    reason: str = ""
    attempts: list[Attempt] = field(default_factory=list)
    catalog: assetcatalog.Catalog | None = None

    def report(self) -> dict:
        return {
            "status": self.status,
            "source": self.source,
            "kind": self.kind,
            "format": self.format,
            "decoder": self.decoder,
            "width": self.width,
            "height": self.height,
            "scale": self.scale,
            "extension": self.extension,
            "reason": self.reason,
            "attempts": [a.as_dict() for a in self.attempts[:40]],
        }


def _scale_of(name: str) -> float:
    lowered = name.lower()
    if "@3x" in lowered:
        return 3.0
    if "@2x" in lowered:
        return 2.0
    return 1.0


def plist_icon_names(info: dict) -> list[str]:
    """Collect icon base names declared by Info.plist in priority order."""
    names: list[str] = []
    for key in ("CFBundleIcons", "CFBundleIcons~ipad"):
        icons = info.get(key)
        if not isinstance(icons, dict):
            continue
        primary = icons.get("CFBundlePrimaryIcon")
        if isinstance(primary, dict):
            declared = primary.get("CFBundleIconName")
            if isinstance(declared, str) and declared:
                names.append(declared)
            for item in primary.get("CFBundleIconFiles", []) or []:
                if isinstance(item, str):
                    names.append(item)
        for item in icons.get("CFBundleIconFiles", []) or []:
            if isinstance(item, str):
                names.append(item)
    for item in info.get("CFBundleIconFiles", []) or []:
        if isinstance(item, str):
            names.append(item)
    declared = info.get("CFBundleIconFile")
    if isinstance(declared, str) and declared:
        names.append(declared)
    ordered: list[str] = []
    for name in names:
        if name not in ordered:
            ordered.append(name)
    return ordered


def catalog_icon_name(info: dict) -> str | None:
    """Asset-catalog icon set name, when Info.plist declares one."""
    for key in ("CFBundleIcons", "CFBundleIcons~ipad"):
        icons = info.get(key)
        if isinstance(icons, dict):
            primary = icons.get("CFBundlePrimaryIcon")
            if isinstance(primary, dict):
                declared = primary.get("CFBundleIconName")
                if isinstance(declared, str) and declared:
                    return declared
    return None


def _file_candidates(app: Path, names: list[str]) -> list[Path]:
    found: list[Path] = []
    seen: set[str] = set()
    for name in names:
        # Declared names may include an extension ("Icon.png"); scale and device
        # variants are built from the base name so @2x/@3x files are reachable.
        variants = [name]
        lowered = name.lower()
        for extension in (".png", ".jpg", ".jpeg"):
            if lowered.endswith(extension):
                variants.append(name[: -len(extension)])
        for base in variants:
            for suffix in SUFFIXES:
                try:
                    relative = safe_name(base + suffix)
                except InputError:
                    continue
                if len(relative.parts) > 4:
                    continue
                candidate = app.joinpath(*relative.parts)
                if not candidate.is_file():
                    continue
                key = candidate.as_posix().casefold()
                if key in seen:
                    continue
                seen.add(key)
                found.append(candidate)
    found.sort(key=lambda path: (-_scale_of(path.name), -_pixels(path), path.name.lower()))
    return found


def _pixels(path: Path) -> int:
    """Pixel count of an image, best effort, without decoding it."""
    try:
        if path.stat().st_size > (1 << 20):
            return 0
        with path.open("rb") as handle:
            head = handle.read(64)
    except OSError:
        return 0
    try:
        width, height = pngcodec.dimensions(head)
    except (pngcodec.PngError, struct.error, ValueError):
        return 0
    return width * height


def _bundle_image_candidates(app: Path, limit: int = MAX_FILE_CANDIDATES) -> list[Path]:
    """Loose images in the bundle, ranked by how icon-like they look."""
    candidates: list[tuple[tuple[int, int, int], Path]] = []
    for path in app.rglob("*"):
        if not path.is_file() or len(candidates) >= limit:
            continue
        suffix = path.suffix.lower()
        if suffix not in (".png", ".jpg", ".jpeg"):
            continue
        lowered = path.name.lower()
        if "assets.car" in lowered or lowered.endswith(".car"):
            continue
        depth = len(path.relative_to(app).parts)
        named = 0 if "appicon" in lowered else 1 if "icon" in lowered else 2
        if lowered.startswith("itunesartwork"):
            named = 0
        try:
            size = path.stat().st_size
        except OSError:
            continue
        candidates.append(((named, depth, -size), path))
    return [path for _rank, path in sorted(candidates)]


def _decode_image(data: bytes, source: str, kind: str) -> tuple[pngcodec.Image | None, str, str, str]:
    """Return (image, format, decoder, error). Images are normalised to RGBA PNG."""
    try:
        sniffed = pngcodec.sniff(data)
    except pngcodec.PngError as exc:
        return None, "unknown", "none", str(exc)
    if sniffed == "png":
        try:
            return pngcodec.decode(data), "png", "pngcodec", ""
        except pngcodec.PngError as exc:
            return None, "png", "pngcodec", str(exc)
    if sniffed == "jpeg":
        try:
            width, height = pngcodec.jpeg_dimensions(data)
        except pngcodec.PngError as exc:
            return None, "jpeg", "jpeg-header", str(exc)
        return pngcodec.Image(width, height, b""), "jpeg", "jpeg-header", ""
    if sniffed == "heif":
        return None, "heif", "none", "HEIF/HEIC images require a decoder that is not implemented"
    return None, sniffed, "none", f"unsupported image format: {sniffed}"


def _from_file(path: Path, app: Path) -> tuple[IconResult | None, Attempt]:
    try:
        data = path.read_bytes()
    except OSError as exc:
        return None, Attempt(_rel(path, app), "file", f"unreadable: {exc}", False)
    if len(data) > pngcodec.MAX_FILE_BYTES:
        return None, Attempt(_rel(path, app), "file", "image exceeds size limit", False)
    image, fmt, decoder, error = _decode_image(data, str(path), "file")
    if image is None:
        return None, Attempt(_rel(path, app), fmt, error or "cannot decode", False)
    attempt = Attempt(
        _rel(path, app),
        fmt,
        "decoded" if fmt == "png" else "header only",
        True,
        image.width,
        image.height,
        _scale_of(path.name),
    )
    if fmt == "png":
        png = data if not _is_apple_png(data) else pngcodec.encode(image)
    else:
        png = None
    return (
        IconResult(
            status="SUPPORTED",
            image=png,
            extension="jpg" if fmt == "jpeg" else "png",
            source=_rel(path, app),
            kind="file",
            format=fmt,
            decoder=decoder,
            width=image.width,
            height=image.height,
            scale=_scale_of(path.name),
            reason="Decoded bundle icon",
        ),
        attempt,
    )


def _is_apple_png(data: bytes) -> bool:
    return b"CgBI" in data[:512]


def _rel(path: Path, app: Path) -> str:
    try:
        return path.relative_to(app).as_posix()
    except ValueError:
        return path.as_posix()


def _from_catalog(path: Path, app: Path, preferred: str | None) -> tuple[IconResult | None, list[Attempt], assetcatalog.Catalog | None]:
    attempts: list[Attempt] = []
    try:
        if path.stat().st_size > MAX_CATALOG_BYTES:
            return None, [Attempt(_rel(path, app), "assets.car", "catalog exceeds size limit", False)], None
        catalog = assetcatalog.open_catalog(path.read_bytes())
    except (InputError, OSError, struct.error, ValueError) as exc:
        return None, [Attempt(_rel(path, app), "assets.car", f"catalog parse failed: {exc}", False)], None
    for error in catalog.errors:
        attempts.append(Attempt(_rel(path, app), "assets.car", error, False))
    for rendition in catalog.icons(preferred):
        label = f"{_rel(path, app)}:{rendition.name or rendition.filename or 'unnamed'}"
        if rendition.decoded:
            return (
                IconResult(
                    status="SUPPORTED",
                    image=rendition.decoded,
                    extension="png",
                    source=label,
                    kind="assets.car",
                    format=rendition.encoding,
                    decoder="assetcatalog+pngcodec",
                    width=rendition.width,
                    height=rendition.height,
                    scale=rendition.scale,
                    reason="Decoded compiled asset catalog icon",
                    catalog=catalog,
                ),
                attempts,
                catalog,
            )
        attempts.append(
            Attempt(
                label,
                "assets.car",
                rendition.decode_error or "rendition is not decodable",
                False,
                rendition.width,
                rendition.height,
                rendition.scale,
            )
        )
    return None, attempts, catalog


def extract(app: Path, info: dict, log=None) -> IconResult:
    """Resolve the best available icon for a bundle, recording every attempt."""
    attempts: list[Attempt] = []
    names = plist_icon_names(info)
    catalogs = sorted(app.rglob("Assets.car"))
    result: IconResult | None = None

    # 1-2. Info.plist declared files and their device/scale variants.
    for candidate in _file_candidates(app, names):
        found, attempt = _from_file(candidate, app)
        attempts.append(attempt)
        if found is not None and found.image is not None:
            result = found
            break
        if found is not None and result is None:
            result = found  # decodable but not PNG (e.g. JPEG); keep as a fallback

    # 3. Compiled asset catalogs.
    preferred = catalog_icon_name(info) or (names[0] if names else None)
    catalog: assetcatalog.Catalog | None = None
    if result is None or result.image is None:
        for path in catalogs:
            found, extra, catalog = _from_catalog(path, app, preferred)
            attempts.extend(extra)
            if found is not None:
                result = found
                break
            catalog = catalog or catalog

    # 4-5. Any other icon-like resource, then any other image.
    if result is None or result.image is None:
        for candidate in _bundle_image_candidates(app):
            found, attempt = _from_file(candidate, app)
            attempts.append(attempt)
            if found is not None:
                if found.image is not None:
                    result = found
                    break
                if result is None:
                    result = found

    if result is None:
        result = IconResult(
            status="UNAVAILABLE",
            reason=(
                "No decodable icon found in the bundle"
                + (" (asset catalogs were inspected)" if catalogs else "")
            ),
        )
    elif result.image is None:
        result.status = "PARTIAL"
        result.reason = result.reason or "Icon detected but not transcodable to PNG"
    result.attempts = attempts
    if catalog is not None:
        result.catalog = catalog
    if log:
        for attempt in attempts:
            log("icon", f"{attempt.source}: {attempt.detail}")
        log("icon", f"selected {result.source or 'none'} ({result.status})")
    return result


def launcher(result: IconResult, size: int = LAUNCHER_SIZE) -> bytes | None:
    """Produce an Android launcher icon: square, transparent padding, RGBA PNG."""
    if result.image is None:
        return None
    try:
        image = pngcodec.decode(result.image)
    except pngcodec.PngError:
        return None
    # Enlarge small icons: a 48px icon centred in a large transparent canvas is not
    # a usable launcher icon, so the artwork is scaled to the requested size first.
    return pngcodec.encode(pngcodec.square(image, size, enlarge=True))


def display(result: IconResult, size: int = DISPLAY_SIZE) -> bytes | None:
    """Smaller PNG for the library UI."""
    if result.image is None:
        return None
    try:
        image = pngcodec.decode(result.image)
    except pngcodec.PngError:
        return None
    return pngcodec.encode(pngcodec.resize(image, size))
