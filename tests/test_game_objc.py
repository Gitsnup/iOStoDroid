"""ObjC metadata parse for the playable-game path."""

import unittest
import zipfile
from pathlib import Path

from radek.game import macho, objc_meta

DATA = Path(__file__).resolve().parent / "data" / "AngryBirds_v1.0_os30.ipa"


class GameObjcTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with zipfile.ZipFile(DATA) as archive:
            cls.image = macho.parse(archive.read("Payload/AngryBirds.app/AngryBirds"))
        cls.model = objc_meta.parse(cls.image)

    def test_classes_methods_protocols(self):
        self.assertEqual(len(self.model.problems), 0)
        names = sorted(name for _, _, name in self.model.classes)
        self.assertEqual(names, ["AppController", "MyEAGLView"])
        self.assertEqual(len(self.model.methods), 60)
        proto_names = sorted(name for _, name in self.model.protocols)
        self.assertEqual(
            proto_names,
            ["NSObject", "UIAccelerometerDelegate", "UIApplicationDelegate"],
        )

    def test_sites_and_bindings(self):
        self.assertEqual(len(self.model.sites), 483)
        self.assertEqual(len(self.model.bound_symbols), 28)


if __name__ == "__main__":
    unittest.main()
