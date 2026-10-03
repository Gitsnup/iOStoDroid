import struct
import tempfile
import unittest
import zlib
from pathlib import Path
from radek.resources import *
from .fixtures import macho


class ResourceTests(unittest.TestCase):
    def test_png_roundtrip(self):
        data = fallback_icon(); self.assertEqual(normalize_png(data), data)
    def test_cgbi_channel_and_alpha(self):
        raw = b'\x00\x10\x20\x40\x80'
        co = zlib.compressobj(wbits=-15); compressed = co.compress(raw) + co.flush()
        image = PNG + png_chunk(b'CgBI', b'') + png_chunk(b'IHDR', struct.pack('>IIBBBBB', 1, 1, 8, 6, 0, 0, 0)) + png_chunk(b'IDAT', compressed) + png_chunk(b'IEND', b'')
        converted = normalize_png(image)
        self.assertNotIn(b'CgBI', converted)
        pos = converted.index(b'IDAT'); size = struct.unpack_from('>I', converted, pos-4)[0]
        self.assertEqual(zlib.decompress(converted[pos+4:pos+4+size]), b'\x00\x80\x40\x20\x80')
    def test_invalid_png_crc(self):
        data = bytearray(fallback_icon()); data[30] ^= 1
        with self.assertRaises(InputError): normalize_png(bytes(data))
    def test_resource_paths_and_exclusions(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); app = root / 'app'; app.mkdir()
            (app / 'Main').write_bytes(macho()); (app / 'plugin').write_bytes(macho())
            (app / 'Info.plist').write_bytes(b'plist'); (app / 'data.json').write_text('{}')
            (app / 'icon.png').write_bytes(fallback_icon()); (app / '_CodeSignature').mkdir(); (app / '_CodeSignature/sign').write_text('omit')
            (app / 'en.lproj').mkdir(); (app / 'en.lproj/Localizable.strings').write_text('data')
            inventory = copy_resources(app, root / 'assets', 'Main')
            names = {r['path'] for r in inventory}
            self.assertEqual(names, {'Info.plist', 'data.json', 'icon.png', 'en.lproj/Localizable.strings'})
