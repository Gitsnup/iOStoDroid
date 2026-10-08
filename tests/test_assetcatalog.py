"""Tests for the compiled asset catalog (Assets.car) parser."""

import struct
import unittest

from iostodroid import assetcatalog
from iostodroid.archive import InputError
from iostodroid.pngcodec import Image, encode

from .carbuild import Builder, car_header, csi, facet, key, key_format

TOKENS = [1, 2, 11, 16]  # element, part, scale, identifier
ICON_ATTRIBUTES = [(1, 0x0055), (2, 0x00B5), (16, 0x8019)]
OTHER_ATTRIBUTES = [(1, 0x0055), (2, 0x00B5), (16, 0x1234)]


def icon(width: int, height: int, value: int = 200) -> bytes:
    pixels = bytes([value, 40, 90, 255]) * (width * height)
    return encode(Image(width, height, pixels))


def catalog(extra_renditions=(), corrupt_index=False):
    builder = Builder()
    entries = []
    for name, width, height, scale, payload, fmt, identifier in [
        ("AppIcon", 60, 60, 2, icon(120, 120), b"HEIF", 0x8019),
        ("AppIcon", 60, 60, 3, icon(180, 180, value=210), b"HEIF", 0x8019),
        ("OtherImage", 32, 32, 1, icon(32, 32, value=20), b"HEIF", 0x1234),
        *extra_renditions,
    ]:
        entries.append(
            (
                key(0x0055, 0x00B5, scale, identifier),
                csi(width, height, scale, payload, f"{name}{width}.png", fmt),
            )
        )
    facets = [
        (b"AppIcon", facet(ICON_ATTRIBUTES)),
        (b"OtherImage", facet(OTHER_ATTRIBUTES)),
    ]
    renditions = builder.tree(entries)
    facet_tree = builder.tree(facets)
    builder.var("CARHEADER", builder.add(car_header(len(entries))))
    builder.var("KEYFORMAT", builder.add(key_format(TOKENS)))
    builder.var("RENDITIONS", renditions)
    builder.var("FACETKEYS", facet_tree)
    data = builder.build()
    if corrupt_index:
        data = bytearray(data)
        struct.pack_into(">I", data, 16, 0xFFFFFF)  # point the block index outside the file
        data = bytes(data)
    return data


class AssetCatalogTests(unittest.TestCase):
    def test_parses_renditions_and_resolves_facet_names(self):
        result = assetcatalog.parse(catalog())
        self.assertEqual(result.header["storageVersion"], 17)
        self.assertEqual(len(result.renditions), 3)
        names = sorted(r.name for r in result.renditions)
        self.assertEqual(names, ["AppIcon", "AppIcon", "OtherImage"])
        scales = sorted(r.scale for r in result.renditions if r.name == "AppIcon")
        self.assertEqual(scales, [2.0, 3.0])
        self.assertTrue(all(r.encoding == "png" for r in result.renditions))
        self.assertTrue(all(r.decoded for r in result.renditions))
        self.assertEqual([r.width for r in result.renditions if r.name == "AppIcon"], [60, 60])

    def test_icon_selection_prefers_named_largest_rendition(self):
        result = assetcatalog.parse(catalog())
        best = result.icons("AppIcon")[0]
        self.assertEqual(best.name, "AppIcon")
        self.assertEqual(best.scale, 3.0)
        self.assertEqual(best.width, 60)  # point size recorded by the catalog
        from iostodroid.pngcodec import decode

        self.assertEqual(decode(best.decoded).width, 180)  # @3x pixel size
        self.assertEqual(best.decoded, icon(180, 180, value=210))

    def test_icon_selection_without_name_still_prefers_icons(self):
        result = assetcatalog.parse(catalog())
        best = result.icons()[0]
        self.assertIn("icon", best.name.casefold())

    def test_heif_rendition_is_detected_but_not_faked(self):
        data = catalog(
            extra_renditions=[("AppIcon", 40, 40, 1, b"\x00\x00\x00\x18ftypheic" + bytes(40), b"HEIF", 0x8019)]
        )
        result = assetcatalog.parse(data)
        heif = [r for r in result.renditions if r.encoding == "heif"]
        self.assertEqual(len(heif), 1)
        self.assertIsNone(heif[0].decoded)
        self.assertIn("HEIF", heif[0].decode_error)

    def test_raw_argb_payload_is_decoded(self):
        width = height = 4
        body = bytearray()
        for _ in range(width * height):
            body.extend((80, 40, 200, 255))  # B, G, R, A premultiplied
        data = catalog(extra_renditions=[("RawIcon", width, height, 1, bytes(body), b"ARGB", 0x5555)])
        result = assetcatalog.parse(data)
        raw = [r for r in result.renditions if r.pixel_format == "ARGB"][0]
        self.assertEqual(raw.encoding, "png")
        from iostodroid.pngcodec import decode

        image = decode(raw.decoded)
        self.assertEqual(image.pixels[:4], bytes((200, 40, 80, 255)))

    def test_damaged_index_falls_back_to_validated_scan(self):
        result = assetcatalog.open_catalog(catalog(corrupt_index=True))
        self.assertEqual(result.source, "scan")
        self.assertTrue(result.errors)
        self.assertEqual(len(result.renditions), 3)
        self.assertTrue(all(r.decoded for r in result.renditions))

    def test_non_catalog_input_is_rejected(self):
        with self.assertRaises((assetcatalog.AssetCatalogError, InputError)):
            assetcatalog.parse(b"this is not a BOM file at all")

    def test_empty_catalog_without_renditions(self):
        builder = Builder()
        builder.var("CARHEADER", builder.add(car_header(0)))
        data = builder.build()
        with self.assertRaises((assetcatalog.AssetCatalogError, InputError)):
            assetcatalog.parse(data)

    def test_report_is_serialisable(self):
        result = assetcatalog.parse(catalog())
        report = result.report()
        self.assertEqual(report["renditionCount"], 3)
        self.assertEqual(report["keyFormat"], ["element", "part", "scale", "identifier"])
        self.assertTrue(report["renditions"])
