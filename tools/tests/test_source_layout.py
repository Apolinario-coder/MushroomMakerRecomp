import ast
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[2]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class SourceLayoutTests(unittest.TestCase):
    def test_sources_have_no_packaged_programs(self):
        self.assertTrue((ROOT/'runtime/host/hostrt.c').is_file())
        self.assertTrue((ROOT/'tools/launcher/Launcher.cs').is_file())
        self.assertFalse((ROOT/'MushroomMakerRecomp.exe').exists())
        self.assertFalse((ROOT/'portable').exists())
        for directory in ('runtime', 'tools', 'third_party'):
            for path in (ROOT/directory).rglob('*'):
                self.assertNotIn(path.suffix.lower(), {'.exe', '.dll', '.a', '.pyd', '.zip', '.elf', '.keys'}, str(path))

    def test_python_sources_parse(self):
        for directory in ('runtime', 'tools'):
            for path in (ROOT/directory).rglob('*.py'):
                ast.parse(path.read_text(encoding='utf-8'), filename=str(path))

    def test_setup_resolves_runtime_and_identity(self):
        setup = load('source_setup', ROOT/'tools/setup.py')
        self.assertEqual(setup.ROOT, ROOT)
        self.assertEqual(setup.SOURCE_ROOT, ROOT/'runtime')
        self.assertEqual(len(setup.source_identity()), 16)

    def test_game_discovery_without_control_metadata(self):
        setup = load('discovery_setup', ROOT/'tools/setup.py')
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            for name in ('main', 'sdk', 'main.npdm', 'romfs/Pack/LayoutNetwork.pack', 'romfs/Pack/LayoutStatic.pack'):
                path = folder/name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(name.encode())
            with patch.object(setup, 'MAIN_SHA', hashlib.sha256(b'main').hexdigest()), patch.object(setup, 'SDK_SHA', hashlib.sha256(b'sdk').hexdigest()):
                found = setup.locate_game(folder)
                self.assertEqual(set(found), {'main', 'sdk', 'main.npdm', 'romfs'})
                (folder/'control.nacp').write_bytes(b'obsolete')
                self.assertEqual(found, setup.locate_game(folder))

    def test_retry_optimization_is_retained(self):
        tree = ast.parse((ROOT/'runtime/host/build_mm2.py').read_text())
        selected = next(n.value for n in tree.body if isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id == 'FAST_HOT_SOURCES' for t in n.targets))
        hot = eval(compile(ast.Expression(selected), '<hot units>', 'eval'), {'range': range})
        self.assertEqual(len(hot), 17)
        self.assertIn('c_0039.c', hot)
        self.assertIn('c_0063.c', hot)
        helper = (ROOT/'runtime/host/build_opt.py').read_text()
        self.assertIn('optimization=None', helper)
        self.assertIn('opt = optimization or OPT', helper)

    def test_portable_bundle_verified_and_tampering_rejected(self):
        packaging = load('packaging', ROOT/'tools/package_release.py')
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for bad in (False, True):
                archive = root/('bad.zip' if bad else 'good.zip')
                data = b'portable fixture'
                manifest = {'files': {'prebuilt/helper.txt': hashlib.sha256(data).hexdigest()}}
                with zipfile.ZipFile(archive, 'w') as output:
                    output.writestr('app/portable/manifest.json', json.dumps(manifest))
                    output.writestr('app/portable/prebuilt/helper.txt', b'tampered' if bad else data)
                if bad:
                    with self.assertRaises(ValueError):
                        packaging.unpack_portable(archive, root/'bad')
                else:
                    packaging.unpack_portable(archive, root/'good')
                    self.assertEqual((root/'good/portable/prebuilt/helper.txt').read_bytes(), data)

    def test_unsafe_portable_path_rejected(self):
        packaging = load('safe_packaging', ROOT/'tools/package_release.py')
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for i, name in enumerate(('../escape', 'C:/escape', 'dir\\escape', '/escape')):
                archive = root/(str(i)+'.zip')
                with zipfile.ZipFile(archive, 'w') as output:
                    output.writestr('portable/manifest.json', json.dumps({'files': {name: 'invalid'}}))
                with self.assertRaises(ValueError):
                    packaging.unpack_portable(archive, root/'stage')


if __name__ == '__main__':
    unittest.main()
