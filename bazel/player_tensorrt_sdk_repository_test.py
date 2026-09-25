#!/usr/bin/env python3
"""Exercise SDK selection with real ELF libraries and isolated repository files.

The rule uses Python-compatible Starlark. Only Bazel's filesystem/download API is
adapted here; readelf, the native version query and deb extraction execute for real.
Actual NVIDIA downloads and Bazel evaluation are covered by the full build.
"""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from types import SimpleNamespace
import unittest


def fail(message):
    raise RuntimeError(message)


RULE = {"fail": fail, "repository_rule": lambda **kwargs: kwargs}
exec(Path(__file__).with_name("player_tensorrt_sdk_repository.bzl").read_text(), RULE)


class RepoPath:
    def __init__(self, path):
        self.path = Path(path)

    def __str__(self):
        return str(self.path)

    @property
    def exists(self):
        return self.path.exists()

    @property
    def basename(self):
        return self.path.name

    @property
    def realpath(self):
        return RepoPath(self.path.resolve())

    def get_child(self, name):
        return RepoPath(self.path / name)


class Context:
    def __init__(self, root, archives):
        self.root = root
        self.archives = archives
        self.os = SimpleNamespace(name="linux", environ={"DEEPSTREAM_ROOT": str(root / "deepstream")})
        self.downloads = []
        self.commands = []

    def path(self, path):
        value = str(path)
        for prefix in ("/opt/jetson-sysroot/usr", "/usr"):
            if value == prefix or value.startswith(prefix + "/"):
                return RepoPath(self.root / "system" / value[len(prefix):].lstrip("/"))
        return RepoPath(Path(value) if Path(value).is_absolute() else self.root / "repository" / value)

    def read(self, path):
        return Path(str(path)).read_text()

    def which(self, name):
        return shutil.which(name)

    def execute(self, command, environment=None):
        command = list(map(str, command))
        self.commands.append(command)
        result = subprocess.run(command, capture_output=True, text=True, env={**os.environ, **(environment or {})})
        return SimpleNamespace(return_code=result.returncode, stdout=result.stdout, stderr=result.stderr)

    def download(self, *, url, output, sha256):
        self.downloads.append((url, sha256))
        if not url.startswith(RULE["_NVIDIA_PACKAGES"]) or len(sha256) != 64:
            raise AssertionError("unbounded/unverified download")
        destination = self.path(output).path
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(self.archives[Path(url).name], destination)

    def delete(self, path):
        shutil.rmtree(self.path(path).path)

    def symlink(self, source, destination):
        path = self.path(destination).path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.symlink_to(str(source))

    def file(self, path, text):
        self.path(path).path.write_text(text)


def headers(root, version):
    include = root / "include/x86_64-linux-gnu"
    include.mkdir(parents=True, exist_ok=True)
    for name in ("NvInfer.h", "NvOnnxParser.h"):
        (include / name).touch()
    (include / "NvInferVersion.h").write_text("".join(
        f"#define NV_TENSORRT_{field} {value}\n"
        for field, value in zip(("MAJOR", "MINOR", "PATCH", "BUILD"), version)
    ))


def library(directory, name, version, build=11):
    directory.mkdir(parents=True, exist_ok=True)
    major, minor, patch = map(int, version.split("."))
    path = directory / f"lib{name}.so.{version}"
    source = f"int getInferLibVersion(void) {{ return {major * 10000 + minor * 100 + patch}; }}\n"
    source += f"int getInferLibBuildVersion(void) {{ return {build}; }}\n"
    subprocess.run(["cc", "-shared", "-fPIC", "-x", "c", "-", f"-Wl,-soname,lib{name}.so.{major}",
                    "-o", str(path)], input=source, text=True, check=True, capture_output=True)
    link = directory / f"lib{name}.so.{major}"
    link.unlink(missing_ok=True)
    link.symlink_to(path.name)
    return path


class SdkSelectionTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.package_tmp = tempfile.TemporaryDirectory(prefix="hstream-sdk-packages-")
        cls.archives = {}
        for index, (filename, _) in enumerate(RULE["_TRT_10_16_HEADERS"]):
            package = Path(cls.package_tmp.name) / str(index)
            (package / "DEBIAN").mkdir(parents=True)
            (package / "DEBIAN/control").write_text(
                "Package: sdk-test\nVersion: 1\nArchitecture: amd64\n"
                "Maintainer: Test <test@example.org>\nDescription: Test headers\n"
            )
            headers(package / "usr", (10, 16, 1, 11))
            (package / "usr/lib").mkdir()
            archive = Path(cls.package_tmp.name) / filename
            subprocess.run(["dpkg-deb", "--build", str(package), str(archive)], check=True, capture_output=True)
            cls.archives[filename] = archive

    @classmethod
    def tearDownClass(cls):
        cls.package_tmp.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="hstream-sdk-selection-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.context = Context(self.root, self.archives)
        self.system = self.root / "system"
        self.libs = self.system / "lib/x86_64-linux-gnu"
        headers(self.system, (11, 3, 0, 99))
        for name in ("nvinfer", "nvonnxparser"):
            library(self.libs, name, "10.16.1")
            newest = library(self.libs, name, "11.3.0", 99)
            (self.libs / f"lib{name}.so").symlink_to(newest.name)
        ds = self.root / "deepstream/lib"
        ds.mkdir(parents=True)
        subprocess.run(["cc", "-shared", "-fPIC", "-x", "c", "-", "-x", "none", "-Wl,--no-as-needed",
                        str(self.libs / "libnvinfer.so.10"), "-o", str(ds / "libnvds_infer.so")],
                       input="int ds_test;\n", text=True, check=True, capture_output=True)

    def select(self):
        RULE["_player_tensorrt_sdk_impl"](self.context)

    def test_newer_development_sdk_uses_pinned_headers_and_installed_runtime(self):
        self.select()
        self.assertEqual(len(self.context.downloads), 2)
        repository = self.root / "repository"
        self.assertIn("development", str((repository / "include").resolve()))
        self.assertEqual((repository / "lib/libnvinfer.so").resolve(), self.libs / "libnvinfer.so.10.16.1")
        self.assertFalse((repository / "development/usr/lib").exists())
        self.assertIn("11", (self.system / "include/x86_64-linux-gnu/NvInferVersion.h").read_text())

    def test_coherent_installed_headers_need_no_download(self):
        headers(self.system, (10, 16, 1, 11))
        self.select()
        self.assertEqual(self.context.downloads, [])

    def test_same_major_older_minor_uses_compatible_headers(self):
        headers(self.system, (10, 13, 0, 1))
        self.select()
        self.assertEqual(len(self.context.downloads), 2)

    def test_missing_development_headers_use_managed_headers(self):
        shutil.rmtree(self.system / "include")
        self.select()
        self.assertEqual(len(self.context.downloads), 2)

    def test_unknown_runtime_build_never_downloads_known_headers(self):
        library(self.libs, "nvinfer", "10.16.1", 12)
        with self.assertRaisesRegex(RuntimeError, "Automatic headers are available only"):
            self.select()
        self.assertEqual(self.context.downloads, [])

    def test_different_parser_release_rejects_managed_headers(self):
        library(self.libs, "nvonnxparser", "10.15.1")
        with self.assertRaisesRegex(RuntimeError, "parser release .* disagrees"):
            self.select()
        self.assertEqual(self.context.downloads, [])

    def test_installed_headers_reject_different_parser_release(self):
        headers(self.system, (10, 16, 1, 11))
        library(self.libs, "nvonnxparser", "10.15.1")
        with self.assertRaisesRegex(RuntimeError, "parser release .* disagrees"):
            self.select()
        self.assertEqual(self.context.downloads, [])

    def test_explicit_sdk_rejects_different_parser_release(self):
        headers(self.system, (10, 16, 1, 11))
        library(self.libs, "nvonnxparser", "10.15.1")
        self.context.os.environ["HSTREAM_PLAYER_TENSORRT_SDK_ROOT"] = str(self.system)
        with self.assertRaisesRegex(RuntimeError, "parser release .* disagrees"):
            self.select()
        self.assertEqual(self.context.downloads, [])

    def test_header_build_mismatch_is_not_accepted(self):
        headers(self.system, (10, 16, 1, 99))
        self.select()
        self.assertEqual(len(self.context.downloads), 2)

    def test_explicit_override_never_silently_falls_back(self):
        self.context.os.environ["HSTREAM_PLAYER_TENSORRT_SDK_ROOT"] = str(self.system)
        with self.assertRaisesRegex(RuntimeError, "explicit overrides never fall back"):
            self.select()
        self.assertEqual(self.context.downloads, [])

    def test_explicit_build_mismatch_is_rejected(self):
        headers(self.system, (10, 16, 1, 99))
        self.context.os.environ["HSTREAM_PLAYER_TENSORRT_SDK_ROOT"] = str(self.system)
        with self.assertRaisesRegex(RuntimeError, "disagree with runtime"):
            self.select()
        self.assertEqual(self.context.downloads, [])

    def test_cross_build_never_downloads_native_headers(self):
        self.context.os.environ["HM_BAZEL_PREFER_FIRST_LOCAL_PATH"] = "1"
        with self.assertRaisesRegex(RuntimeError, "Automatic headers are available only"):
            self.select()
        self.assertEqual(self.context.downloads, [])
        self.assertFalse(any("-S" in command for command in self.context.commands))

    def test_coherent_sysroot_never_executes_target_library(self):
        headers(self.system, (10, 16, 1, 11))
        self.context.os.environ["HM_BAZEL_PREFER_FIRST_LOCAL_PATH"] = "1"
        self.select()
        self.assertEqual(self.context.downloads, [])
        self.assertFalse(any("-S" in command for command in self.context.commands))

    def test_multiarch_skips_foreign_libraries(self):
        foreign = self.system / "lib/aarch64-linux-gnu"
        for name in ("nvinfer", "nvonnxparser"):
            path = library(foreign, name, "10.16.1")
            data = bytearray(path.read_bytes())
            data[18:20] = (183).to_bytes(2, "little")  # EM_AARCH64
            path.write_bytes(data)
        self.select()
        self.assertEqual((self.root / "repository/lib/libnvinfer.so").resolve(), self.libs / "libnvinfer.so.10.16.1")

    def test_deepstream_override_is_authoritative(self):
        self.context.os.environ["DEEPSTREAM_ROOT"] = str(self.root / "missing")
        with self.assertRaisesRegex(RuntimeError, "DEEPSTREAM_ROOT does not exist"):
            self.select()
        self.assertEqual(self.context.downloads, [])


if __name__ == "__main__":
    unittest.main()
