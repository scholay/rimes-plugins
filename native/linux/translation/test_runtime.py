import hashlib
import importlib.util
import json
import os
from pathlib import Path
import stat
import sys
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location("translation_resources", Path(__file__).with_name("runtime.py"))
runtime = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runtime)


class ResourceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.archives = {}
        models = []
        for name in ("zh_en", "en_zh"):
            file = self.root / (name + ".zip")
            with zipfile.ZipFile(file, "w") as archive:
                for member, data in (("metadata.json", "{}"), ("sentencepiece.model", "fixture"), ("model/model.bin", "fixture")):
                    archive.writestr(f"translate-{name}-1_9/{member}", data)
            file.chmod(0o600)
            self.archives[name] = file
            models.append({"name": name, "kind": "model", "sha256": hashlib.sha256(file.read_bytes()).hexdigest(), "bytes": file.stat().st_size})
        self.lock = {"format": 1, "python": f"{sys.version_info.major}.{sys.version_info.minor}", "runtime": "fixture", "runtimes": {"fixture": []}, "models": models}
        self.manager = runtime.Resources(self.root / "private", self.lock, "fixture")
        self.manager.fetch = lambda resource, _: self.archives[resource["name"]]

    def tearDown(self):
        self.temp.cleanup()

    def test_atomic_install_restart_and_tamper(self):
        self.assertEqual(self.manager.status()["state"], "missing")
        self.manager.install(lambda _: None)
        self.assertEqual(self.manager.status()["state"], "ready")
        reopened = runtime.Resources(self.root / "private", self.lock, "fixture")
        bundle = reopened.active()
        self.assertEqual((self.root / "private").stat().st_mode & 0o777, 0o700)
        model = bundle / "models/translate-zh_en-1_9/model/model.bin"
        self.assertEqual(model.stat().st_mode & 0o777, 0o600)
        model.write_bytes(b"corrupt")
        self.assertEqual(reopened.status()["state"], "damaged")
        reopened.fetch = self.manager.fetch
        reopened.install(lambda _: None)
        self.assertEqual(reopened.status()["state"], "ready")
        self.assertNotEqual(reopened.active(), bundle)

    def test_cancel_keeps_previous_pointer_and_cleans_staging(self):
        self.manager.install(lambda _: None)
        pointer = (self.manager.root / "current.json").read_bytes()
        model = self.manager.active() / "models/translate-zh_en-1_9/model/model.bin"
        model.write_bytes(b"corrupt")
        def cancelled(*_):
            raise runtime.Cancelled("cancelled")
        self.manager.fetch = cancelled
        with self.assertRaises(runtime.Cancelled):
            self.manager.install(lambda _: None)
        self.assertEqual((self.manager.root / "current.json").read_bytes(), pointer)
        self.assertFalse(list(self.manager.root.glob(".staging-*")))

    def test_bad_archive_does_not_publish(self):
        file = self.archives["zh_en"]
        with zipfile.ZipFile(file, "w") as archive:
            archive.writestr("../../escape", "bad")
        with self.assertRaises(runtime.ResourceError):
            self.manager.install(lambda _: None)
        self.assertFalse((self.manager.root / "current.json").exists())
        self.assertFalse(list(self.manager.root.glob(".staging-*")))
        self.assertFalse((self.root / "escape").exists())

    def test_incomplete_model_does_not_publish(self):
        file = self.archives["zh_en"]
        with zipfile.ZipFile(file, "w") as archive:
            archive.writestr("translate-zh_en-1_9/metadata.json", "{}")
        with self.assertRaises(runtime.ResourceError):
            self.manager.install(lambda _: None)
        self.assertFalse((self.manager.root / "current.json").exists())

    def test_symlinks_and_hardlinks_refused(self):
        self.manager.install(lambda _: None)
        model = self.manager.active() / "models/translate-zh_en-1_9/model/model.bin"
        original = model.read_bytes()
        model.unlink()
        target = self.root / "outside"
        target.write_bytes(original)
        target.chmod(0o600)
        model.symlink_to(target)
        self.assertEqual(self.manager.status()["state"], "damaged")
        model.unlink()
        os.link(target, model)
        self.assertEqual(self.manager.status()["state"], "damaged")
        link = self.root / "link"
        link.symlink_to(self.manager.root, target_is_directory=True)
        with self.assertRaises(runtime.ResourceError):
            runtime.Resources(link, self.lock, "fixture")

    def test_archive_link_and_duplicate_refused(self):
        for kind in ("symlink", "duplicate"):
            archive = self.root / (kind + ".zip")
            with zipfile.ZipFile(archive, "w") as stream:
                if kind == "symlink":
                    entry = zipfile.ZipInfo("link")
                    entry.external_attr = (stat.S_IFLNK | 0o777) << 16
                    stream.writestr(entry, "outside")
                else:
                    stream.writestr("member", "one")
                    stream.writestr("member", "two")
            archive.chmod(0o600)
            with self.assertRaises((runtime.ResourceError, FileExistsError)):
                runtime.Resources.extract(archive, runtime.private_directory(self.root / kind), {})

    def test_lock_and_urls(self):
        with self.manager.exclusive():
            with self.assertRaises(runtime.ResourceError):
                with self.manager.exclusive():
                    pass
        for url in ("http://argos-net.com/model", "https://evil.example/model", "https://user:pass@argos-net.com/model", "file:///tmp/model"):
            with self.assertRaises(runtime.ResourceError):
                runtime.verify_url(url)
        runtime.verify_url("https://argos-net.com/v1/translate-zh_en-1_9.argosmodel")


if __name__ == "__main__":
    unittest.main()
