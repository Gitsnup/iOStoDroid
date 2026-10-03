"""Offline parser for Apple compiled asset catalogs (``Assets.car``).

The format is undocumented. This implementation follows the structure that has
been publicly reverse engineered and published (BOM container, ``CARHEADER``,
``KEYFORMAT``, ``RENDITIONS``/``FACETKEYS`` B+ trees, 184-byte ``csi`` rendition
headers). Every structure is validated before use and every rendition payload is
sniffed rather than trusted, so a partially understood catalog degrades into
"detected but not decoded" instead of producing a fabricated image.

Nothing here is executed: renditions are static data interpreted by our own
decoders.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

from .archive import InputError

BOM_MAGIC = b"BOMStore"
MAX_BLOCKS = 1 << 21
MAX_RENDITIONS = 1 << 17
MAX_FILE_BYTES = 256 * 1024 * 1024

# Attribute names published for the rendition key format. Timac's 2018 research
# and later write-ups disagree on a few identifiers, so these are used for
# labelling and soft heuristics only; asset names are resolved by matching
# FACETKEYS attribute values against the KEYFORMAT token order.
ATTRIBUTES = {
    0: "look",
    1: "element",
    2: "part",
    3: "size",
    4: "direction",
    5: "value",
    6: "appearance",
    7: "dimension1",
    8: "dimension2",
    9: "state",
    10: "layer",
    11: "scale",
    12: "unknown13",
    13: "presentationState",
    14: "idiom",
    15: "subtype",
    16: "identifier",
    17: "previousValue",
    18: "previousState",
    19: "sizeClassHorizontal",
    20: "sizeClassVertical",
    21: "memoryClass",
    22: "graphicsClass",
    23: "displayGamut",
    24: "deploymentTarget",
    25: "localization",
}

LAYOUTS = {
    0: "image",
    1: "data",
    2: "externalLink",
    3: "layerStack",
    4: "internalLink",
    5: "texture",
    6: "pixel",
    7: "externalReference",
    8: "vector",
    9: "multisizeImage",
    1000: "color",
    1001: "imageSet",
    1002: "cubeMap",
    1004: "layerReference",
    1005: "effectStack",
    1006: "appearanceLibrary",
    1007: "customColor",
    1008: "namedColor",
    1009: "color",
    1010: "multisizeImageSet",
}

RAW_PIXEL_FORMATS = {
    b"ARGB": 4,  # 32-bit, byte order B,G,R,A, premultiplied
    b"RGBW": 8,  # 16 bits per channel
    b"GA16": 2,  # 8-bit gray + 8-bit alpha
    b"RGB5": 2,  # 16-bit 5-5-5-1
}


class AssetCatalogError(InputError):
    """Raised when a catalog cannot be parsed. Callers fall back elsewhere."""


@dataclass
class Rendition:
    """One decoded asset-catalog rendition."""

    name: str
    filename: str
    width: int
    height: int
    scale: float
    pixel_format: str
    layout: int
    layout_name: str
    color_space: int
    key: tuple[int, ...]
    encoding: str
    payload: bytes
    decoded: bytes | None = None
    decode_error: str | None = None

    @property
    def pixels(self) -> int:
        return self.width * self.height

    def report(self) -> dict:
        return {
            "name": self.name,
            "filename": self.filename,
            "width": self.width,
            "height": self.height,
            "scale": self.scale,
            "pixelFormat": self.pixel_format,
            "layout": self.layout,
            "layoutName": self.layout_name,
            "encoding": self.encoding,
            "bytes": len(self.payload),
            "decoded": self.decoded is not None,
            "decodeError": self.decode_error,
        }


@dataclass
class Catalog:
    """A parsed ``Assets.car``."""

    header: dict = field(default_factory=dict)
    key_format: list[int] = field(default_factory=list)
    facets: dict[str, list[tuple[int, int]]] = field(default_factory=dict)
    renditions: list[Rendition] = field(default_factory=list)
    errors: list[str] = field(default_factory=list)
    source: str = "bom"

    def icons(self, preferred: str | None = None) -> list[Rendition]:
        """Return image renditions ordered by icon suitability (best first)."""
        images = [r for r in self.renditions if r.encoding in ("png", "jpeg") or r.decoded]
        if not images:
            return []
        named = []
        if preferred:
            wanted = preferred.casefold()
            named = [
                r
                for r in images
                if wanted in r.name.casefold() or wanted in r.filename.casefold()
            ]

        def rank(item: Rendition) -> tuple:
            facet = item.name.casefold()
            is_icon = "icon" in facet or "icon" in item.filename.casefold()
            square = 1 if item.width == item.height else 0
            return (
                0 if named else 1,  # named matches first when a preferred name exists
                0 if is_icon else 1,
                -item.scale,
                -item.pixels,
                -square,
                item.name,
            )

        return sorted(images, key=rank)

    def report(self) -> dict:
        return {
            "source": self.source,
            "header": self.header,
            "keyFormat": [ATTRIBUTES.get(t, t) for t in self.key_format],
            "facetCount": len(self.facets),
            "renditionCount": len(self.renditions),
            "errors": self.errors,
            "renditions": [r.report() for r in self.renditions[:200]],
        }


# --- BOM container ------------------------------------------------------------


def _u32(data: bytes, at: int, big: bool = True) -> int:
    if at < 0 or at + 4 > len(data):
        raise AssetCatalogError("BOM structure outside file")
    return struct.unpack_from(">I" if big else "<I", data, at)[0]


def _u16(data: bytes, at: int, big: bool = True) -> int:
    if at < 0 or at + 2 > len(data):
        raise AssetCatalogError("BOM structure outside file")
    return struct.unpack_from(">H" if big else "<H", data, at)[0]


class _Bom:
    """Minimal read-only BOM (Bill of Materials) reader."""

    def __init__(self, data: bytes):
        if len(data) < 32 or data[:8] != BOM_MAGIC:
            raise AssetCatalogError("not a BOM/asset catalog")
        if len(data) > MAX_FILE_BYTES:
            raise AssetCatalogError("asset catalog exceeds size limit")
        self.data = data
        self.version = _u32(data, 8)
        count = _u32(data, 12)
        index_offset = _u32(data, 16)
        index_length = _u32(data, 20)
        vars_offset = _u32(data, 24)
        vars_length = _u32(data, 28)
        if count > MAX_BLOCKS or index_offset + index_length > len(data):
            raise AssetCatalogError("invalid BOM block index")
        if index_length < 4 or vars_offset + vars_length > len(data):
            raise AssetCatalogError("invalid BOM variables")
        declared = _u32(data, index_offset)
        self.blocks: list[tuple[int, int]] = []
        available = min(declared, (index_length - 4) // 8, count)
        for i in range(available):
            at = index_offset + 4 + i * 8
            self.blocks.append((_u32(data, at), _u32(data, at + 4)))
        self.vars: dict[str, int] = {}
        at = vars_offset + 4
        end = vars_offset + vars_length
        seen = 0
        while at + 5 <= end and seen < 4096:
            index = _u32(data, at)
            length = data[at + 4]
            at += 5
            name = data[at : at + length]
            at += length
            if not name or index >= len(self.blocks):
                continue
            self.vars.setdefault(name.decode("utf-8", "replace"), index)
            seen += 1

    def block(self, index: int) -> bytes:
        if index >= len(self.blocks):
            raise AssetCatalogError("BOM block index outside table")
        offset, length = self.blocks[index]
        if offset + length > len(self.data):
            raise AssetCatalogError("BOM block outside file")
        return self.data[offset : offset + length]

    def walk(self, index: int, budget: int = MAX_RENDITIONS) -> list[tuple[bytes, bytes]]:
        """Enumerate (key, value) pairs of a B+ tree.

        Nodes are visited breadth first so leaves stay in key order. A value that
        is itself a tree block is treated as a child node; anything else is a
        leaf payload. Cycles and out-of-range indices are ignored.
        """
        from collections import deque

        out: list[tuple[bytes, bytes]] = []
        queue = deque([index])
        visited: set[int] = set()
        while queue and len(out) < budget:
            node = queue.popleft()
            if node in visited or node >= len(self.blocks):
                continue
            visited.add(node)
            raw = self.block(node)
            if raw[:4] != b"tree":
                raise AssetCatalogError("invalid BOM tree node")
            entries = self.block(_u32(raw, 8))
            if len(entries) < 12:
                raise AssetCatalogError("truncated BOM tree node")
            count = _u16(entries, 2)
            limit = min(count, (len(entries) - 12) // 8)
            for i in range(limit):
                key_block = _u32(entries, 12 + i * 8)
                value_block = _u32(entries, 12 + i * 8 + 4)
                try:
                    value = self.block(value_block)
                except AssetCatalogError:
                    continue
                if value[:4] == b"tree":
                    queue.append(value_block)
                    continue
                key = b""
                try:
                    key = self.block(key_block)
                except AssetCatalogError:
                    pass
                out.append((key, value))
        return out


# --- car structures -----------------------------------------------------------


def _car_header(data: bytes) -> dict:
    if len(data) < 436:
        raise AssetCatalogError("short CARHEADER")
    tag = data[:4]
    swap = tag == b"RATC"
    if tag not in (b"CTAR", b"RATC"):
        raise AssetCatalogError("invalid CARHEADER tag")
    order = ">" if swap else "<"
    values = struct.unpack_from(order + "IIIII", data, 4)
    return {
        "coreuiVersion": values[0],
        "storageVersion": values[1],
        "storageTimestamp": values[2],
        "renditionCount": values[3],
        "mainVersion": data[24 : 24 + 128].split(b"\0", 1)[0].decode("utf-8", "replace"),
        "versionString": data[152 : 152 + 256].split(b"\0", 1)[0].decode("utf-8", "replace"),
    }


def _key_format(data: bytes) -> list[int]:
    if len(data) < 12 or data[:4] != b"kfmt":
        raise AssetCatalogError("invalid KEYFORMAT")
    version, count = struct.unpack_from("<II", data, 4)
    if count > 64 or 12 + count * 4 > len(data):
        raise AssetCatalogError("invalid KEYFORMAT token count")
    tokens = [struct.unpack_from("<I", data, 12 + i * 4)[0] for i in range(count)]
    if any(token > 64 for token in tokens):
        raise AssetCatalogError("unsupported rendition key token")
    return tokens


def _facet_tokens(value: bytes) -> list[tuple[int, int]]:
    if len(value) < 6:
        return []
    count = struct.unpack_from("<H", value, 4)[0]
    attributes = []
    for i in range(min(count, (len(value) - 6) // 4)):
        name, item = struct.unpack_from("<HH", value, 6 + i * 4)
        attributes.append((name, item))
    return attributes


def _csi(data: bytes) -> dict | None:
    """Parse a 184-byte rendition header. Returns None when it does not validate."""
    if len(data) < 184 or data[:4] not in (b"CTSI", b"ISTC"):
        return None
    version, _flags, width, height, scale, pixel_format, color_space = struct.unpack_from(
        "<IIIIIII", data, 4
    )
    layout = struct.unpack_from("<H", data, 36)[0]
    name = data[40 : 40 + 128].split(b"\0", 1)[0].decode("utf-8", "replace")
    tvl_length, bitmap_count, _zero, rendition_length = struct.unpack_from("<IIII", data, 168)
    if width > 16384 or height > 16384 or scale > 100000 or bitmap_count > 4096:
        return None
    if tvl_length > len(data) or bitmap_count * 4 > len(data):
        return None
    return {
        "version": version,
        "width": width,
        "height": height,
        "scale": scale / 100.0,
        "pixelFormat": pixel_format,
        "colorSpace": color_space & 0xF,
        "layout": layout,
        "name": name,
        "tvlLength": tvl_length,
        "bitmapCount": bitmap_count,
        "renditionLength": rendition_length,
    }


def _payload(data: bytes, header: dict) -> bytes:
    """Locate rendition bytes after the header, bitmap lengths and TLV section."""
    candidates = (
        184 + header["bitmapCount"] * 4 + header["tvlLength"],
        184 + header["tvlLength"],
        184,
    )
    for start in candidates:
        if start < 0 or start >= len(data):
            continue
        length = header["renditionLength"] or len(data) - start
        if length <= 0 or start + length > len(data):
            continue
        chunk = data[start : start + length]
        if _sniff(chunk) not in ("unknown",):
            return chunk
    start = min(candidates)
    if start >= len(data):
        return b""
    return data[start : min(len(data), start + max(0, header["renditionLength"]))]


def _sniff(payload: bytes) -> str:
    if payload[:8] == b"\x89PNG\r\n\x1a\n":
        return "png"
    if payload[:3] == b"\xff\xd8\xff":
        return "jpeg"
    if payload[4:8] == b"ftyp":
        return "heif"
    if payload[:4] in (b"bvx1", b"bvx2", b"bvxn", b"bvxe"):
        return "lzfse"
    if payload[:2] == b"\x1f\x8b":
        return "gzip"
    if payload[:1] == b"\x78" and payload[1:2] in (b"\x01", b"\x9c", b"\xda", b"\x5e"):
        return "zlib"
    if payload[:4] == b"PDF-":
        return "pdf"
    return "unknown"


def _decode_raw(header: dict, payload: bytes) -> tuple[bytes | None, str | None]:
    """Convert uncompressed pixel payloads into a standard PNG."""
    from .pngcodec import Image, encode  # local import: icon decoding is optional

    width, height = header["width"], header["height"]
    if not width or not height:
        return None, "rendition has no pixel dimensions"
    stride = RAW_PIXEL_FORMATS.get(header["pixelFormatBytes"])
    if not stride:
        return None, f"unsupported raw pixel format {header['pixelFormatName']}"
    expected = width * height * stride
    if len(payload) < expected:
        return None, f"raw payload is {len(payload)} bytes, expected {expected}"
    body = payload[:expected]
    pixels = bytearray(width * height * 4)
    if header["pixelFormatBytes"] == b"ARGB":
        for i in range(width * height):
            b, g, r, a = body[i * 4 : i * 4 + 4]
            if a:
                pixels[i * 4 : i * 4 + 4] = bytes(
                    (
                        min(255, r * 255 // a),
                        min(255, g * 255 // a),
                        min(255, b * 255 // a),
                        a,
                    )
                )
    elif header["pixelFormatBytes"] == b"GA16":
        for i in range(width * height):
            gray, alpha = body[i * 2], body[i * 2 + 1]
            if alpha:
                value = min(255, gray * 255 // alpha)
            else:
                value = 0
            pixels[i * 4 : i * 4 + 4] = bytes((value, value, value, alpha))
    elif header["pixelFormatBytes"] == b"RGB5":
        for i in range(width * height):
            word = struct.unpack_from("<H", body, i * 2)[0]
            r = (word >> 10) & 31
            g = (word >> 5) & 31
            b = word & 31
            a = 255 if word & 1 else 0
            pixels[i * 4 : i * 4 + 4] = bytes((r * 8, g * 8, b * 8, a))
    else:  # RGBW: 16 bits per channel, little endian
        for i in range(width * height):
            r, g, b = struct.unpack_from("<HHH", body, i * 8)
            pixels[i * 4 : i * 4 + 4] = bytes((r >> 8, g >> 8, b >> 8, 255))
    return encode(Image(width, height, bytes(pixels))), None


def _make_rendition(data: bytes, key: tuple[int, ...], name: str) -> Rendition | None:
    header = _csi(data)
    if header is None:
        return None
    header["pixelFormatBytes"] = struct.pack("<I", header["pixelFormat"])
    header["pixelFormatName"] = header["pixelFormatBytes"].decode("latin-1")
    payload = _payload(data, header)
    encoding = _sniff(payload)
    decoded: bytes | None = None
    error: str | None = None
    if encoding == "png":
        decoded = payload
    elif encoding == "jpeg":
        decoded = payload
    elif encoding in ("gzip", "zlib"):
        import zlib

        try:
            inner = zlib.decompressobj(32 + 15 if encoding == "gzip" else 15).decompress(payload)
            inner_encoding = _sniff(inner)
            if inner_encoding in ("png", "jpeg"):
                decoded, encoding = inner, inner_encoding
            else:
                error = f"{encoding}-wrapped {inner_encoding} payload is not decodable"
        except zlib.error as exc:
            error = f"{encoding} payload is corrupt: {exc}"
    elif encoding == "lzfse":
        raw, failure = _lzfse(payload)
        if raw is None:
            error = failure or "lzfse payload cannot be decoded"
        else:
            expected = header["width"] * header["height"] * (RAW_PIXEL_FORMATS.get(header["pixelFormatBytes"], 0))
            if expected and len(raw) >= expected:
                decoded, error = _decode_raw(header, raw)
                encoding = "png"
            else:
                error = (
                    f"lzfse decoded {len(raw)} bytes but {header['pixelFormatName']} "
                    f"requires {expected}"
                )
    elif encoding == "unknown" and header["pixelFormatBytes"] in RAW_PIXEL_FORMATS:
        decoded, error = _decode_raw(header, payload)
        if decoded is not None:
            encoding = "png"
    elif encoding == "heif":
        error = "HEIF/HEIC renditions require an HEIF decoder that is not implemented"
    elif encoding == "pdf":
        error = "vector (PDF) rendition is not rasterised"
    else:
        error = f"{encoding} payload is not a decodable image"
    return Rendition(
        name=name,
        filename=header["name"],
        width=header["width"],
        height=header["height"],
        scale=header["scale"],
        pixel_format=header["pixelFormatName"],
        layout=header["layout"],
        layout_name=LAYOUTS.get(header["layout"], f"layout-{header['layout']}"),
        color_space=header["colorSpace"],
        key=key,
        encoding=encoding,
        payload=payload,
        decoded=decoded,
        decode_error=error,
    )


def _lzfse(payload: bytes) -> tuple[bytes | None, str | None]:
    """Decode LZFSE blocks that use the uncompressed (``bvxn``) container.

    Only the trivial container form is implemented. V1/V2 blocks are reported as
    undecodable instead of returning fabricated pixels.
    """
    out = bytearray()
    at = 0
    while at + 8 <= len(payload):
        magic = payload[at : at + 4]
        if magic == b"bvxe":
            return bytes(out), None
        if magic not in (b"bvxn", b"bvx1", b"bvx2"):
            return None, "unrecognised LZFSE block magic"
        size = struct.unpack_from("<I", payload, at + 4)[0]
        if magic == b"bvxn":
            body = payload[at + 8 : at + 8 + size]
            if len(body) != size:
                return None, "truncated LZFSE block"
            out.extend(body)
            at += 8 + size
        else:
            return None, "LZFSE V1/V2 compression is not implemented"
    return (bytes(out), None) if out else (None, "empty LZFSE stream")


# --- catalog parsing ----------------------------------------------------------


def _resolve_names(keys: list[tuple[int, ...]], tokens: list[int], facets: dict) -> list[str]:
    """Map rendition keys to facet (asset) names using attribute matching."""
    names: list[str] = []
    identifier_index = tokens.index(16) if 16 in tokens else None
    for key in keys:
        best: str | None = None
        best_score = 0
        for name, attributes in facets.items():
            score = 0
            matched = True
            for attribute, value in attributes:
                if attribute in tokens:
                    position = tokens.index(attribute)
                    if position < len(key) and key[position] == value:
                        score += 1
                    else:
                        matched = False
                        break
            if matched and score > best_score:
                best, best_score = name, score
        if best is None and identifier_index is not None and identifier_index < len(key):
            value = key[identifier_index]
            for name, attributes in facets.items():
                if any(a == 16 and v == value for a, v in attributes):
                    best = name
                    break
        names.append(best or "")
    return names


def parse(data: bytes) -> Catalog:
    """Parse an ``Assets.car`` into renditions, resolving facet names."""
    catalog = Catalog()
    try:
        bom = _Bom(data)
    except AssetCatalogError as exc:
        raise AssetCatalogError(str(exc)) from exc
    if "CARHEADER" in bom.vars:
        try:
            catalog.header = _car_header(bom.block(bom.vars["CARHEADER"]))
        except AssetCatalogError as exc:
            catalog.errors.append(f"CARHEADER: {exc}")
    if "KEYFORMAT" in bom.vars:
        try:
            catalog.key_format = _key_format(bom.block(bom.vars["KEYFORMAT"]))
        except AssetCatalogError as exc:
            catalog.errors.append(f"KEYFORMAT: {exc}")
    if "FACETKEYS" in bom.vars:
        try:
            for key, value in bom.walk(bom.vars["FACETKEYS"]):
                catalog.facets[key.decode("utf-8", "replace")] = _facet_tokens(value)
        except AssetCatalogError as exc:
            catalog.errors.append(f"FACETKEYS: {exc}")
    if "RENDITIONS" not in bom.vars:
        raise AssetCatalogError("catalog has no RENDITIONS tree")
    try:
        entries = bom.walk(bom.vars["RENDITIONS"])
    except AssetCatalogError as exc:
        raise AssetCatalogError(str(exc)) from exc
    keys = [_key_of(key) for key, _value in entries]
    names = _resolve_names(keys, catalog.key_format, catalog.facets)
    for (key, value), key_values, name in zip(entries, keys, names):
        if len(catalog.renditions) >= MAX_RENDITIONS:
            catalog.errors.append("rendition limit reached; catalog truncated")
            break
        try:
            rendition = _make_rendition(value, key_values, name)
        except (AssetCatalogError, struct.error, ValueError) as exc:
            catalog.errors.append(f"rendition: {exc}")
            continue
        if rendition is None:
            catalog.errors.append("rendition without a valid csi header")
            continue
        catalog.renditions.append(rendition)
    return catalog


def _key_of(key: bytes) -> tuple[int, ...]:
    if len(key) < 2:
        return ()
    return tuple(struct.unpack_from("<%dH" % (len(key) // 2), key, 0))


def scan(data: bytes) -> list[Rendition]:
    """Fallback recovery: locate rendition headers without the BOM index.

    Used when the container index is damaged or in an unexpected layout. Results
    are validated individually so the scan cannot invent images.
    """
    found: list[Rendition] = []
    at = data.find(b"ISTC")
    while at >= 0 and len(found) < MAX_RENDITIONS:
        rendition = _make_rendition(data[at : at + 64 * 1024 * 1024], (), "")
        if rendition is not None and (rendition.decoded or rendition.width):
            found.append(rendition)
        at = data.find(b"ISTC", at + 4)
    if not found:
        at = data.find(b"CTSI")
        while at >= 0 and len(found) < MAX_RENDITIONS:
            rendition = _make_rendition(data[at : at + 64 * 1024 * 1024], (), "")
            if rendition is not None and (rendition.decoded or rendition.width):
                found.append(rendition)
            at = data.find(b"CTSI", at + 4)
    return found


def open_catalog(data: bytes) -> Catalog:
    """Parse a catalog, falling back to a validated header scan."""
    try:
        return parse(data)
    except AssetCatalogError as exc:
        catalog = Catalog(errors=[str(exc)], source="scan")
        catalog.renditions = scan(data)
        if not catalog.renditions:
            catalog.errors.append("no renditions recovered by scan")
        return catalog
