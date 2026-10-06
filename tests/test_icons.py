"""Icon recovery tests: explicit plist icons, variants, catalogs and fallbacks."""

import plistlib
import struct
import tempfile
import unittest
import zipfile
from pathlib import Path

from radek import icons
from radek.archive import discover_app, extract_ipa, read_plist
from radek.pngcodec import Image, decode, encode

from .carbuild import Builder, car_header, csi, facet, key, key_format
from .test_pngcodec import build as png_build

TOKENS = [1, 2, 11, 16]
ICON_ATTRIBUTES = [(1, 0x0055), (2, 0x00B5), (16, 0x8019)]


def png(width: int, height: int, value: int = 120, alpha: int = 255) -> bytes:
    pixels = bytes([value, 40, 90, alpha]) * (width * height)
    return encode(Image(width, height, pixels))


def catalog_bytes() -> bytes:
    """A small Assets.car holding two AppIcon renditions and one unrelated image."""
    builder = Builder()
    entries = []
    for name, size, scale, value, identifier in [
        ("AppIcon", 120, 3, 150, 0x8019),
        ("AppIcon", 180, 2, 200, 0x8019),
        ("OtherImage", 32, 1, 20, 0x1234),
    ]:
        entries.append(
            (
                key(0x0055, 0x00B5, scale, identifier),
                csi(size, size, scale, png(size, size, value), f"{name}{size}.png"),
            )
        )
    facets = [
        (b"AppIcon", facet(ICON_ATTRIBUTES)),
        (b"OtherImage", facet([(1, 0x0055), (2, 0x00B5), (16, 0x1234)])),
    ]
    renditions = builder.tree(entries)
    facet_tree = builder.tree(facets)
    builder.var("CARHEADER", builder.add(car_header(len(entries))))
    builder.var("KEYFORMAT", builder.add(key_format(TOKENS)))
    builder.var("RENDITIONS", renditions)
    builder.var("FACETKEYS", facet_tree)
    return builder.build()


class IconTests(unittest.TestCase):
    def make_app(self, files: dict[str, bytes]) -> Path:
        root = Path(tempfile.mkdtemp())
        app = root / "Fixture.app"
        for name, data in files.items():
            path = app / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        return app

    # --- explicit Info.plist icons -------------------------------------------

    def test_explicit_plist_icon_is_used(self):
        app = self.make_app({"Icon.png": png(76, 76), "Background.png": png(8, 8)})
        result = icons.extract(app, {"CFBundleIconFile": "Icon.png"})
        self.assertEqual(result.status, "SUPPORTED")
        self.assertEqual(result.source, "Icon.png")
        self.assertEqual(result.kind, "file")
        self.assertEqual(result.decoder, "pngcodec")
        self.assertEqual((result.width, result.height), (76, 76))
        self.assertEqual(result.scale, 1.0)
        self.assertEqual(decode(result.image).width, 76)

    def test_scale_variants_are_preferred_and_recorded(self):
        app = self.make_app(
            {
                "AppIcon60x60.png": png(60, 60),
                "AppIcon60x60@2x.png": png(120, 120),
                "AppIcon60x60@3x.png": png(180, 180),
            }
        )
        result = icons.extract(app, {"CFBundleIconFiles": ["AppIcon60x60"]})
        self.assertEqual(result.status, "SUPPORTED")
        self.assertEqual(result.source, "AppIcon60x60@3x.png")
        self.assertEqual(result.scale, 3.0)
        self.assertEqual((result.width, result.height), (180, 180))
        sources = [a.source for a in result.attempts]
        self.assertIn("AppIcon60x60@3x.png", sources)

        # A declared name that already carries an extension still reaches @2x/@3x.
        app2 = self.make_app({"Icon.png": png(60, 60), "Icon@2x.png": png(120, 120, value=250)})
        second = icons.extract(app2, {"CFBundleIconFile": "Icon.png"})
        self.assertEqual(second.source, "Icon@2x.png")
        self.assertEqual(second.scale, 2.0)

    def test_highest_pixel_rendition_wins_over_scale_suffix(self):
        app = self.make_app(
            {
                "AppIcon@3x.png": png(96, 96, value=30),
                "AppIcon@2x.png": png(128, 128, value=220),
            }
        )
        result = icons.extract(app, {"CFBundleIconFiles": ["AppIcon"]})
        self.assertEqual("SUPPORTED", result.status)
        self.assertEqual("AppIcon@2x.png", result.source)
        self.assertEqual((128, 128), (result.width, result.height))
        self.assertEqual(2.0, result.scale)

    def test_cfbundleicons_and_device_variants_are_collected(self):
        info = {
            "CFBundleIcons": {
                "CFBundlePrimaryIcon": {"CFBundleIconFiles": ["AppIcon"], "CFBundleIconName": "AppIcon"}
            },
            "CFBundleIcons~ipad": {"CFBundlePrimaryIcon": {"CFBundleIconFiles": ["AppIcon-iPad"]}},
        }
        self.assertEqual(icons.plist_icon_names(info), ["AppIcon", "AppIcon-iPad"])
        self.assertEqual(icons.catalog_icon_name(info), "AppIcon")

        app = self.make_app({"AppIcon-iPad.png": png(76, 76, value=90)})
        result = icons.extract(app, info)
        self.assertEqual(result.source, "AppIcon-iPad.png")
        self.assertEqual(result.status, "SUPPORTED")

    # --- compiled asset catalogs ---------------------------------------------

    def test_assets_car_icon_is_decoded(self):
        app = self.make_app({"Assets.car": catalog_bytes()})
        info = {"CFBundleIcons": {"CFBundlePrimaryIcon": {"CFBundleIconName": "AppIcon"}}}
        result = icons.extract(app, info)
        self.assertEqual(result.status, "SUPPORTED")
        self.assertEqual(result.kind, "assets.car")
        self.assertEqual(result.decoder, "assetcatalog+pngcodec")
        self.assertIn("Assets.car", result.source or "")
        self.assertEqual(decode(result.image).width, 180)
        self.assertEqual(result.scale, 2.0)
        self.assertEqual((result.width, result.height), (180, 180))
        self.assertIsNotNone(result.catalog)

    def test_higher_resolution_catalog_icon_beats_smaller_declared_file(self):
        app = self.make_app({"AppIcon@2x.png": png(64, 64), "Assets.car": catalog_bytes()})
        info = {
            "CFBundleIcons": {
                "CFBundlePrimaryIcon": {"CFBundleIconFiles": ["AppIcon"], "CFBundleIconName": "AppIcon"}
            }
        }
        result = icons.extract(app, info)
        self.assertEqual("SUPPORTED", result.status)
        self.assertEqual("assets.car", result.kind)
        self.assertIn("AppIcon", result.source or "")
        self.assertEqual((180, 180), (result.width, result.height))

    def test_catalog_is_used_when_no_loose_icon_exists(self):
        app = self.make_app({"Assets.car": catalog_bytes(), "Readme.txt": b"no images here"})
        result = icons.extract(app, {})
        self.assertEqual(result.kind, "assets.car")
        self.assertEqual(result.status, "SUPPORTED")

    # --- fallback chain -------------------------------------------------------

    def test_loose_icon_resource_is_ranked_above_other_images(self):
        app = self.make_app(
            {
                "images/background.png": png(512, 512, value=10),
                "images/AppIconLarge.png": png(64, 64, value=200),
            }
        )
        result = icons.extract(app, {})
        self.assertEqual(result.status, "SUPPORTED")
        self.assertEqual(result.source, "images/AppIconLarge.png")
        self.assertEqual(result.kind, "file")

    def test_malformed_candidate_falls_back_to_a_valid_one(self):
        app = self.make_app(
            {
                "Icon@3x.png": png(180, 180)[:120],  # truncated PNG body
                "Icon@2x.png": png(120, 120, value=210),
            }
        )
        result = icons.extract(app, {"CFBundleIconFile": "Icon"})
        self.assertEqual(result.status, "SUPPORTED")
        self.assertEqual(result.source, "Icon@2x.png")
        failed = [a for a in result.attempts if not a.ok]
        self.assertTrue(failed, "the broken candidate must be recorded")
        self.assertEqual(failed[0].source, "Icon@3x.png")
        self.assertTrue(failed[0].detail)

    def test_truncated_declared_icon_falls_back_to_bundle_image(self):
        app = self.make_app({"Icon.png": png(60, 60)[:120], "images/AppIcon.png": png(120, 120)})
        result = icons.extract(app, {"CFBundleIconFile": "Icon.png"})
        self.assertEqual(result.status, "SUPPORTED")
        self.assertEqual(result.source, "images/AppIcon.png")
        self.assertTrue(any(not a.ok for a in result.attempts))

    def test_unsupported_candidate_falls_through_to_any_image(self):
        app = self.make_app({"Icon.png": b"not an image at all", "artwork.png": png(48, 48)})
        result = icons.extract(app, {"CFBundleIconFile": "Icon.png"})
        self.assertEqual(result.status, "SUPPORTED")
        self.assertEqual(result.source, "artwork.png")

    def test_unavailable_icon_state_is_reported(self):
        app = self.make_app({"Info.plist": b"<?xml version='1.0'?><plist/>"})
        result = icons.extract(app, {"CFBundleIconFile": "Missing.png"})
        self.assertEqual(result.status, "UNAVAILABLE")
        self.assertIsNone(result.image)
        self.assertIn("No decodable icon", result.reason)
        self.assertIsNone(icons.launcher(result))
        self.assertIsNone(icons.display(result))
        self.assertIn("status", result.report())

    # --- Apple PNG variants ---------------------------------------------------

    def test_apple_png_is_normalised(self):
        width = height = 8
        samples = bytearray()
        for _ in range(width * height):
            samples.extend((200, 40, 90, 255))
        apple = png_build(width, height, 8, 6, bytes(samples), apple=True)
        self.assertIn(b"CgBI", apple[:512])
        app = self.make_app({"Icon@2x.png": apple})
        result = icons.extract(app, {"CFBundleIconFile": "Icon"})
        self.assertEqual(result.status, "SUPPORTED")
        self.assertNotIn(b"CgBI", result.image[:512])
        decoded = decode(result.image)
        self.assertEqual((decoded.width, decoded.height), (8, 8))
        self.assertEqual(tuple(decoded.pixels[0:4]), (90, 40, 200, 255))  # CgBI stores BGR

    # --- output generation ----------------------------------------------------

    def test_launcher_and_display_are_generated(self):
        app = self.make_app({"Icon.png": png(64, 96, value=30, alpha=0)})
        result = icons.extract(app, {"CFBundleIconFile": "Icon.png"})
        launcher = icons.launcher(result, 512)
        self.assertIsNotNone(launcher)
        image = decode(launcher)
        self.assertEqual((image.width, image.height), (512, 512))
        self.assertEqual(image.pixels[3], 0)  # transparency preserved
        display = icons.display(result, 64)
        shown = decode(display)
        self.assertEqual(max(shown.width, shown.height), 64)  # aspect ratio kept

    def test_report_records_resolution_and_decoder(self):
        app = self.make_app({"Assets.car": catalog_bytes(), "Icon@2x.png": png(120, 120)})
        result = icons.extract(app, {"CFBundleIconFile": "Icon"})
        report = result.report()
        self.assertEqual(report["status"], "SUPPORTED")
        self.assertEqual(report["scale"], 2.0)
        self.assertEqual(report["width"], 120)
        self.assertEqual(report["decoder"], "pngcodec")
        self.assertTrue(report["attempts"])

    def test_corrupt_catalog_index_still_yields_icons(self):
        data = bytearray(catalog_bytes())
        struct.pack_into(">I", data, 16, 0xFFFFFF)  # block index outside the file
        app = self.make_app({"Assets.car": bytes(data)})
        result = icons.extract(app, {})
        # The BOM index is unusable, so renditions are recovered by scanning.
        self.assertEqual(result.status, "SUPPORTED")
        self.assertEqual(result.kind, "assets.car")

    def test_unreadable_catalog_falls_back_to_bundle_images(self):
        app = self.make_app({"Assets.car": b"not a car file at all" * 8, "Icon.png": png(57, 57)})
        result = icons.extract(app, {})
        self.assertEqual(result.status, "SUPPORTED")
        self.assertEqual(result.source, "Icon.png")
        self.assertTrue(any(not a.ok and a.kind == "assets.car" for a in result.attempts))


BASE_PLIST = {
    "CFBundleExecutable": "Fixture",
    "CFBundleIdentifier": "org.example.fixture",
    "CFBundleName": "Fixture",
    "CFBundleDisplayName": "Fixture",
    "CFBundleVersion": "1",
    "CFBundleShortVersionString": "1.0",
}


def build_ipa(path: Path, files: dict[str, bytes], info: dict | None = None) -> Path:
    """Write a real IPA (zipped Payload/*.app) for end-to-end icon recovery."""
    merged = {**BASE_PLIST, **(info or {})}
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("Payload/Fixture.app/Info.plist", plistlib.dumps(merged))
        archive.writestr("Payload/Fixture.app/Fixture", b"\xcf\xfa\xed\xfe" + bytes(64))
        for name, data in files.items():
            archive.writestr(f"Payload/Fixture.app/{name}", data)
    return path


class IpaFixtureIconTests(unittest.TestCase):
    """Icon recovery through the real IPA import path (unzip -> .app -> plist)."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def recover(self, files: dict[str, bytes], info: dict | None = None):
        source = build_ipa(self.root / "input.ipa", files, info)
        work = self.root / "extracted"
        extract_ipa(source, work)
        app = discover_app(work)
        return icons.extract(app, read_plist(app / "Info.plist"))

    def test_ordinary_png_icon(self):
        result = self.recover({"Icon.png": png(57, 57)}, {"CFBundleIconFile": "Icon.png"})
        self.assertEqual(result.status, "SUPPORTED")
        self.assertEqual(result.source, "Icon.png")
        self.assertEqual((result.width, result.height), (57, 57))

    def test_retina_variants(self):
        result = self.recover(
            {"AppIcon.png": png(60, 60), "AppIcon@2x.png": png(120, 120), "AppIcon@3x.png": png(180, 180)},
            {"CFBundleIconFiles": ["AppIcon"]},
        )
        self.assertEqual(result.source, "AppIcon@3x.png")
        self.assertEqual(result.scale, 3.0)

    def test_cfbundleicons_and_cfbundleiconfiles(self):
        info = {
            "CFBundleIcons": {"CFBundlePrimaryIcon": {"CFBundleIconFiles": ["Primary"], "CFBundleIconName": "Catalog"}},
            "CFBundleIcons~ipad": {"CFBundlePrimaryIcon": {"CFBundleIconFiles": ["Primary-iPad"]}},
            "CFBundleIconFiles": ["Legacy"],
        }
        result = self.recover({"Primary@2x.png": png(120, 120)}, info)
        self.assertEqual(result.source, "Primary@2x.png")
        self.assertEqual(icons.catalog_icon_name(info), "Catalog")

    def test_device_variants(self):
        result = self.recover(
            {"AppIcon~ipad.png": png(76, 76, value=40), "AppIcon~iphone.png": png(60, 60, value=90)},
            {"CFBundleIconFiles": ["AppIcon"]},
        )
        self.assertEqual(result.status, "SUPPORTED")
        self.assertIn(result.source, {"AppIcon~ipad.png", "AppIcon~iphone.png"})

    def test_assets_car_icon(self):
        result = self.recover(
            {"Assets.car": catalog_bytes()},
            {"CFBundleIcons": {"CFBundlePrimaryIcon": {"CFBundleIconName": "AppIcon"}}},
        )
        self.assertEqual(result.status, "SUPPORTED")
        self.assertEqual(result.kind, "assets.car")
        self.assertEqual(decode(result.image).width, 180)

    def test_malformed_candidate_with_valid_fallback(self):
        result = self.recover(
            {"Icon@3x.png": png(180, 180)[:80], "Icon@2x.png": png(120, 120), "Icon.png": b"garbage"},
            {"CFBundleIconFile": "Icon"},
        )
        self.assertEqual(result.status, "SUPPORTED")
        self.assertEqual(result.source, "Icon@2x.png")
        self.assertTrue(any(not a.ok for a in result.attempts))

    def test_missing_icon_is_unavailable(self):
        result = self.recover({"config.json": b"{}"}, {"CFBundleIconFile": "Missing"})
        self.assertEqual(result.status, "UNAVAILABLE")
        self.assertIsNone(result.image)
        self.assertIn("No decodable icon", result.reason)

    def test_launcher_icon_is_written_for_packaging(self):
        result = self.recover({"Icon.png": png(64, 64, alpha=0)}, {"CFBundleIconFile": "Icon.png"})
        launcher = icons.launcher(result)
        image = decode(launcher)
        self.assertEqual((image.width, image.height), (512, 512))
        self.assertEqual(image.pixels[3], 0)


if __name__ == "__main__":
    unittest.main()
