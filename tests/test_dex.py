import hashlib
import struct
import unittest
import zlib
from iostodroid.dex import classes
from iostodroid.archive import InputError


def dex():
    descriptor = b"Ldev/iostodroid/generated/MainActivity;"
    data = bytearray(152) + bytes([len(descriptor)]) + descriptor + b"\x00"
    data[:8] = b"dex\n035\x00"
    struct.pack_into("<III", data, 32, len(data), 112, 0x12345678)
    struct.pack_into("<II", data, 56, 1, 112)
    struct.pack_into("<II", data, 64, 1, 116)
    struct.pack_into("<II", data, 96, 1, 120)
    struct.pack_into("<I", data, 112, 152)
    data[12:32] = hashlib.sha1(data[32:]).digest()
    struct.pack_into("<I", data, 8, zlib.adler32(data[12:]) & 0xFFFFFFFF)
    return data


class DexTests(unittest.TestCase):
    def test_class_identity_structure_fixture(self):
        self.assertEqual(classes(dex()), {"Ldev/iostodroid/generated/MainActivity;"})

    def test_modified_dex_rejected(self):
        data = dex()
        data[-2] ^= 1
        with self.assertRaises(InputError):
            classes(data)

    def test_not_dex_rejected(self):
        with self.assertRaises(InputError):
            classes(b"dex\n035\x00")

    def test_corrupt_index_with_valid_checksum_rejected(self):
        data = dex()
        struct.pack_into("<I", data, 120, 100)
        data[12:32] = hashlib.sha1(data[32:]).digest()
        struct.pack_into("<I", data, 8, zlib.adler32(data[12:]) & 0xFFFFFFFF)
        with self.assertRaises(InputError):
            classes(data)
