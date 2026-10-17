"""Select signaling ABI from real ELF dependencies without running target code."""

import os
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest


def fail(message):
    raise RuntimeError(message)


RULE = {
    "fail": fail,
    "repository_rule": lambda **kwargs: kwargs,
    "attr": SimpleNamespace(label=lambda **kwargs: kwargs),
}
exec(Path(__file__).with_name("libsoup_repository.bzl").read_text(), RULE)


class RepoPath:
    def __init__(self, path):
        self.path = Path(path)

    def __str__(self):
        return str(self.path)

    @property
    def exists(self):
        return self.path.exists()


class Context:
    def __init__(self, root, cross=False):
        self.root = root
        self.os = SimpleNamespace(
            arch="amd64",
            environ={"HM_BAZEL_PREFER_FIRST_LOCAL_PATH": "1"} if cross else {},
        )
        self.attr = SimpleNamespace(build_file=Path(__file__).resolve().parents[1] / "buildfiles/third_party/libsoup.BUILD")
        self.commands = []

    def path(self, path):
        value = str(path)
        for prefix, directory in (("/opt/jetson-sysroot/usr", "target"), ("/usr", "host")):
            if value == prefix or value.startswith(prefix + "/"):
                return RepoPath(self.root / directory / value[len(prefix):].lstrip("/"))
        return RepoPath(value)

    def execute(self, command):
        command = list(map(str, command))
        self.commands.append(command)
        result = subprocess.run(command, check=False, capture_output=True, text=True)
        return SimpleNamespace(return_code=result.returncode, stdout=result.stdout, stderr=result.stderr)

    def read(self, path):
        return Path(str(path)).read_text()

    def symlink(self, source, destination):
        target = self.root / "repository" / destination
        target.parent.mkdir(parents=True, exist_ok=True)
        target.symlink_to(str(source))

    def file(self, name, text):
        (self.root / "repository" / name).write_text(text)


def library(directory, name, dependencies=()):
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / name
    subprocess.run(
        ["cc", "-shared", "-fPIC", "-x", "c", "-", "-x", "none", "-Wl,--no-as-needed",
         *map(str, dependencies), "-Wl,-soname," + name, "-o", str(path)],
        input="int sdk_test;\n", text=True, check=True, capture_output=True,
    )
    return path


def install(root, major, arm=False):
    include = root / "include" / ("libsoup-3.0" if major == 3 else "libsoup-2.4") / "libsoup"
    include.mkdir(parents=True, exist_ok=True)
    (include / "soup-version.h").write_text(f"#define SOUP_MAJOR_VERSION ({major})\n")
    (include / "soup.h").touch()
    libs = root / "lib" / ("aarch64-linux-gnu" if arm else "x86_64-linux-gnu")
    soup = library(libs, "libsoup-3.0.so.0" if major == 3 else "libsoup-2.4.so.1")
    upnp = library(libs, "libgupnp-1.6.so.0", [soup])
    library(libs, "libnice.so.10", [upnp])


class SoupSelectionTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="hstream-soup-selection-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def select(self, context=None):
        context = context or Context(self.root)
        RULE["_libsoup_impl"](context)
        return (self.root / "repository/version.bzl").read_text()

    def test_modern_ice_selects_soup3_even_with_legacy_headers(self):
        install(self.root / "host", 2)
        install(self.root / "host", 3)
        self.assertIn("libsoup-3.0.so.0", self.select())

    def test_legacy_ice_keeps_soup2_even_with_modern_headers(self):
        install(self.root / "host", 3)
        install(self.root / "host", 2)
        self.assertIn("libsoup-2.4.so.1", self.select())

    def test_missing_matching_sdk_fails_before_native_process_start(self):
        install(self.root / "host", 3)
        (self.root / "host/include/libsoup-3.0/libsoup/soup-version.h").unlink()
        with self.assertRaisesRegex(RuntimeError, "Install libsoup-3.0-dev"):
            self.select()

    def test_explicit_headers_cannot_override_runtime_major(self):
        install(self.root / "host", 2)
        install(self.root / "host", 3)
        context = Context(self.root)
        context.os.environ["HSTREAM_LIBSOUP_INCLUDE_DIR"] = str(self.root / "host/include/libsoup-2.4")
        with self.assertRaisesRegex(RuntimeError, "runtime requires 3"):
            self.select(context)

    def test_private_matching_headers_are_supported(self):
        install(self.root / "host", 3)
        private = self.root / "private"
        (self.root / "host/include/libsoup-3.0").rename(private)
        context = Context(self.root)
        context.os.environ["HSTREAM_LIBSOUP_INCLUDE_DIR"] = str(private)
        self.assertIn("libsoup-3.0.so.0", self.select(context))

    def test_jetson_sysroot_ignores_host_abi_and_never_executes_target(self):
        install(self.root / "host", 3)
        install(self.root / "target", 2, arm=True)
        context = Context(self.root, cross=True)
        self.assertIn("libsoup-2.4.so.1", self.select(context))
        self.assertTrue(all(command[:2] == ["readelf", "-d"] for command in context.commands))


if __name__ == "__main__":
    unittest.main()
