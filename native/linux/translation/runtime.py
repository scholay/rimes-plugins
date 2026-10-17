#!/usr/bin/env python3
"""Pinned, private Linux translation resources. No network in translate/verify."""
import argparse
import contextlib
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import platform
import shutil
import signal
import stat
import sys
import tempfile
import urllib.parse
import urllib.request
import uuid
import zipfile

LIMIT = 256 * 1024
HERE = Path(__file__).absolute().parent


class ResourceError(Exception):
    pass


class Cancelled(ResourceError):
    pass


def emit(value):
    print(json.dumps(value, ensure_ascii=False), flush=True)


def checked_file(path):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
    value = os.fstat(fd)
    if not stat.S_ISREG(value.st_mode) or value.st_uid != os.geteuid() or value.st_nlink != 1 or value.st_mode & 0o077:
        os.close(fd)
        raise ResourceError("unsafe_file")
    return os.fdopen(fd, "rb")


def private_directory(path):
    path = Path(os.path.abspath(path))
    current = Path(path.anchor)
    for part in path.parts[1:]:
        current /= part
        try:
            current.mkdir(mode=0o700)
        except FileExistsError:
            pass
        info = current.lstat()
        if not stat.S_ISDIR(info.st_mode):
            raise ResourceError("unsafe_directory")
    info = path.lstat()
    if info.st_uid != os.geteuid() or info.st_mode & 0o077:
        raise ResourceError("unsafe_directory")
    return path


def safe_member(value):
    if not isinstance(value, str) or not value or "\\" in value or "\0" in value:
        raise ResourceError("unsafe_member")
    path = PurePosixPath(value)
    if path.is_absolute() or any(part in ("", ".", "..") for part in value.rstrip("/").split("/")):
        raise ResourceError("unsafe_member")
    return path


def digest_file(path):
    with checked_file(path) as stream:
        digest = hashlib.sha256()
        size = 0
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
            size += len(chunk)
        return digest.hexdigest(), size


def write_atomic(path, value):
    temporary = path.parent / (".pending-" + uuid.uuid4().hex)
    try:
        with os.fdopen(os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600), "wb") as stream:
            stream.write((json.dumps(value, sort_keys=True) + "\n").encode())
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
    finally:
        temporary.unlink(missing_ok=True)


def read_json(path, limit=1024 * 1024):
    with checked_file(path) as stream:
        data = stream.read(limit + 1)
    if len(data) > limit:
        raise ResourceError("oversized_receipt")
    return json.loads(data)


class TrustedRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, newurl):
        verify_url(newurl)
        return super().redirect_request(request, fp, code, message, headers, newurl)


def verify_url(url):
    value = urllib.parse.urlsplit(url)
    if value.scheme != "https" or value.hostname not in ("files.pythonhosted.org", "argos-net.com") or value.username or value.password or value.port not in (None, 443) or value.fragment:
        raise ResourceError("untrusted_download")


class Resources:
    def __init__(self, root, lock=None, architecture=None):
        self.root = private_directory(root)
        self.lock = lock if lock is not None else json.loads((HERE / "resources.lock.json").read_text())
        self.arch = architecture or platform.machine()
        if self.lock.get("format") != 1 or self.arch not in self.lock["runtimes"]:
            raise ResourceError("unsupported_architecture")
        if f"{sys.version_info.major}.{sys.version_info.minor}" != self.lock["python"]:
            raise ResourceError("unsupported_python")
        self.identity = hashlib.sha256(json.dumps(self.lock, sort_keys=True).encode()).hexdigest()

    @contextlib.contextmanager
    def exclusive(self):
        fd = os.open(self.root / ".lock", os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
        try:
            info = os.fstat(fd)
            if info.st_uid != os.geteuid() or info.st_mode & 0o077 or info.st_nlink != 1:
                raise ResourceError("unsafe_lock")
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as error:
                raise ResourceError("busy") from error
            yield
        finally:
            os.close(fd)

    def active(self, verify=True):
        try:
            current = read_json(self.root / "current.json")
        except FileNotFoundError as error:
            raise ResourceError("missing") from error
        name = current.get("bundle", "")
        if len(name) != 39 or not name.startswith("bundle-") or any(c not in "0123456789abcdef" for c in name[7:]):
            raise ResourceError("invalid_receipt")
        bundle = private_directory(self.root / name)
        return self.verify_bundle(bundle, verify)

    def verify_bundle(self, bundle, verify=True):
        receipt = read_json(bundle / "ready.json", 4 * 1024 * 1024)
        if receipt.get("lock") != self.identity or receipt.get("architecture") != self.arch:
            raise ResourceError("incompatible")
        files = receipt.get("files")
        if not isinstance(files, dict) or not files or len(files) > 15000:
            raise ResourceError("invalid_receipt")
        for name, info in files.items():
            relative = safe_member(name)
            path = bundle.joinpath(*relative.parts)
            private_directory(path.parent)
            if verify:
                if digest_file(path) != (info["sha256"], info["bytes"]):
                    raise ResourceError("integrity_failed")
            else:
                with checked_file(path):
                    pass
        for direction in ("zh_en", "en_zh"):
            for filename in ("model/model.bin", "sentencepiece.model", "metadata.json"):
                if f"models/translate-{direction}-1_9/{filename}" not in files:
                    raise ResourceError("invalid_receipt")
        return bundle

    def status(self):
        try:
            self.active()
            return {"state": "ready", "offline": True, "languages": ["zh-Hans", "en"], "runtime": self.lock["runtime"]}
        except FileNotFoundError:
            return {"state": "missing", "offline": False}
        except (ResourceError, ValueError, KeyError, OSError) as error:
            return {"state": "missing" if str(error) == "missing" else "damaged", "offline": False}

    def fetch(self, resource, notify):
        cache = private_directory(self.root / "cache")
        name = resource["file"]
        if str(safe_member(name)) != name or "/" in name:
            raise ResourceError("unsafe_resource")
        destination = cache / name
        try:
            if digest_file(destination) == (resource["sha256"], resource["bytes"]):
                return destination
        except FileNotFoundError:
            pass
        # A mismatching regular owned cache entry may be replaced, never followed.
        temporary = cache / (".download-" + uuid.uuid4().hex)
        verify_url(resource["url"])
        opener = urllib.request.build_opener(TrustedRedirect())
        try:
            request = urllib.request.Request(resource["url"], headers={"User-Agent": "XSIME-local-translation/1"})
            with opener.open(request, timeout=30) as response:
                verify_url(response.url)
                if response.headers.get("Content-Length") and int(response.headers["Content-Length"]) != resource["bytes"]:
                    raise ResourceError("download_size")
                with os.fdopen(os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600), "wb") as stream:
                    digest = hashlib.sha256()
                    count, percent = 0, -1
                    while True:
                        chunk = response.read(256 * 1024)
                        if not chunk:
                            break
                        count += len(chunk)
                        if count > resource["bytes"]:
                            raise ResourceError("download_size")
                        digest.update(chunk)
                        stream.write(chunk)
                        value = count * 100 // resource["bytes"]
                        if value != percent:
                            notify({"state": "downloading", "resource": resource["name"], "received": count, "total": resource["bytes"]})
                            percent = value
                    stream.flush()
                    os.fsync(stream.fileno())
            if count != resource["bytes"] or digest.hexdigest() != resource["sha256"]:
                raise ResourceError("integrity_failed")
            os.replace(temporary, destination)
            return destination
        finally:
            temporary.unlink(missing_ok=True)

    @staticmethod
    def extract(archive, destination, records):
        with checked_file(archive) as stream, zipfile.ZipFile(stream) as package:
            if len(package.infolist()) > 15000 or sum(info.file_size for info in package.infolist()) > 1024 ** 3:
                raise ResourceError("oversized_archive")
            for entry in package.infolist():
                relative = safe_member(entry.filename)
                mode = entry.external_attr >> 16
                if stat.S_ISLNK(mode) or (stat.S_IFMT(mode) not in (0, stat.S_IFREG, stat.S_IFDIR)):
                    raise ResourceError("unsafe_member")
                target = destination.joinpath(*relative.parts)
                if entry.is_dir():
                    private_directory(target)
                    continue
                private_directory(target.parent)
                with package.open(entry) as source, os.fdopen(os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600), "wb") as output:
                    digest, size = hashlib.sha256(), 0
                    for chunk in iter(lambda: source.read(1024 * 1024), b""):
                        size += len(chunk)
                        if size > entry.file_size or size > 512 * 1024 * 1024:
                            raise ResourceError("oversized_member")
                        output.write(chunk)
                        digest.update(chunk)
                    output.flush()
                    os.fsync(output.fileno())
                if size != entry.file_size:
                    raise ResourceError("archive_size")
                records[target] = {"sha256": digest.hexdigest(), "bytes": size}

    def install(self, notify=emit):
        with self.exclusive():
            if self.status()["state"] == "ready":
                notify(self.status())
                return
            staging = Path(tempfile.mkdtemp(prefix=".staging-", dir=self.root))
            try:
                records = {}
                for resource in self.lock["runtimes"][self.arch] + self.lock["models"]:
                    archive = self.fetch(resource, notify)
                    notify({"state": "verifying", "resource": resource["name"]})
                    destination = private_directory(staging / ("site-packages" if resource["kind"] == "wheel" else "models"))
                    self.extract(archive, destination, records)
                receipt = {"lock": self.identity, "architecture": self.arch,
                           "files": {str(path.relative_to(staging)): info for path, info in records.items()}}
                write_atomic(staging / "ready.json", receipt)
                self.verify_bundle(staging)
                name = "bundle-" + uuid.uuid4().hex
                os.rename(staging, self.root / name)
                # The old bundle remains intact until the new pointer is durable.
                write_atomic(self.root / "current.json", {"bundle": name})
                self.active()
                notify(self.status())
            finally:
                if staging.exists():
                    shutil.rmtree(staging)

    def translate(self, request):
        bundle = self.active()
        # -I ignores user site/PYTHONPATH. Import only the hash-verified wheel tree.
        sys.path.insert(0, str(bundle / "site-packages"))
        spec = importlib.util.spec_from_file_location("xsime_translation_core", HERE / "translation_core.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        try:
            return module.translate(request, bundle)
        finally:
            sys.path.pop(0)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    args = parser.parse_args()
    for kind in (signal.SIGTERM, signal.SIGINT):
        signal.signal(kind, lambda *_: (_ for _ in ()).throw(Cancelled("cancelled")))
    try:
        data = sys.stdin.buffer.read(LIMIT * 6 + 1025)
        if len(data) > LIMIT * 6 + 1024:
            raise ResourceError("request_too_large")
        request = json.loads(data)
        resources = Resources(args.root)
        operation = request.get("op", "status")
        if operation == "install":
            resources.install()
        elif operation in ("status", "verify"):
            emit(resources.status())
        elif operation == "translate":
            emit({"text": resources.translate(request)})
        else:
            raise ResourceError("unknown_operation")
        return 0
    except Cancelled:
        emit({"state": "cancelled", "error": "cancelled"})
        return 2
    except ResourceError as error:
        emit({"state": "error", "error": str(error)})
        return 1
    except Exception:
        # Exception text may contain paths, provider payloads or source text.
        emit({"state": "error", "error": "resources_unavailable"})
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
