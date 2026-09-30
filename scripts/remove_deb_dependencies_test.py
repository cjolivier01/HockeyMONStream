import re
import tempfile
import unittest
from pathlib import Path

try:
    import remove_deb_dependencies as relaxer
except ModuleNotFoundError:
    from scripts import remove_deb_dependencies as relaxer


VERSION_REGEX = re.compile(relaxer.DEFAULT_VERSION_REGEX)


def relax(value, relax_cuda_minor_pins=True):
    return relaxer.relax_relationship_value(value, VERSION_REGEX, relax_cuda_minor_pins)


class CudaMinorPinTest(unittest.TestCase):
    def test_alternative_minor_pins_collapse_onto_one_virtual_package(self):
        value, changed = relax("cuda-cudart-13-0 | cuda-cudart-13-2")
        self.assertEqual(value, "libcudart.so.13")
        self.assertTrue(changed)

    def test_dev_components_map_onto_the_dev_virtual_package(self):
        value, _ = relax("libnpp-dev-13-0 | libnpp-dev-13-2")
        self.assertEqual(value, "libnpp.so.13-dev")

    def test_soname_major_is_not_the_toolkit_major(self):
        # libcufft-13-3 provides libcufft.so.12, so the mapping cannot be
        # derived by appending the CUDA major to the component name.
        value, _ = relax("libcufft-13-2, libcurand-13-2, libcusolver-13-0")
        self.assertEqual(value, "libcufft.so.12, libcurand.so.10, libcusolver.so.12")

    def test_version_relation_survives_the_rewrite(self):
        value, _ = relax("libnpp-13-2 (>= 13.0.1)")
        self.assertEqual(value, "libnpp.so.13 (>= 13.0.1)")

    def test_architecture_qualifier_survives_the_rewrite(self):
        value, _ = relax("cuda-cudart-13-2:amd64 [linux-any]")
        self.assertEqual(value, "libcudart.so.13:amd64 [linux-any]")

    def test_components_without_a_virtual_package_are_left_alone(self):
        # Metapackages declare no Provides, so relaxing them would make the
        # dependency unsatisfiable rather than satisfiable.
        value, changed = relax("cuda-libraries-13-2 | cuda-toolkit-13-2")
        self.assertEqual(value, "cuda-libraries-13-2 | cuda-toolkit-13-2")
        self.assertFalse(changed)

    def test_names_that_merely_look_pinned_are_left_alone(self):
        value, changed = relax("cuda-cudart-13-0x, other-cuda-cudart-13-2, libnpp-13-two")
        self.assertEqual(value, "cuda-cudart-13-0x, other-cuda-cudart-13-2, libnpp-13-two")
        self.assertFalse(changed)

    def test_pins_are_kept_when_the_rewrite_is_disabled(self):
        value, changed = relax("cuda-cudart-13-0 | cuda-cudart-13-2", relax_cuda_minor_pins=False)
        self.assertEqual(value, "cuda-cudart-13-0 | cuda-cudart-13-2")
        self.assertFalse(changed)


class Ubuntu24VersionTagTest(unittest.TestCase):
    def test_ubuntu24_tagged_relations_are_dropped(self):
        value, changed = relax("libgbm1 (>= 23.0.4-0ubuntu1~24.04.1)")
        self.assertEqual(value, "libgbm1")
        self.assertTrue(changed)

    def test_untagged_relations_are_preserved(self):
        value, changed = relax("libcairo2 (>= 1.16.0), libx11-6")
        self.assertEqual(value, "libcairo2 (>= 1.16.0), libx11-6")
        self.assertFalse(changed)

    def test_24_04_must_start_the_version_or_follow_a_separator(self):
        # Substring matches are not Ubuntu tags: 124.04 is a version of its
        # own and 1.24.04x continues past the tag.
        value, changed = relax("libfoo (>= 124.04), libbar (>= 1.24.04x)")
        self.assertEqual(value, "libfoo (>= 124.04), libbar (>= 1.24.04x)")
        self.assertFalse(changed)

    def test_ubuntu_prefixed_tags_are_dropped(self):
        value, _ = relax("libfoo (>= 1.0ubuntu24.04), libbar (>= 9.24.04)")
        self.assertEqual(value, "libfoo, libbar")

    def test_empty_relationship_groups_are_dropped(self):
        # install_deb.sh drops them too; keep a stray comma from becoming an
        # empty dependency.
        value, changed = relax("libx11-6, , ")
        self.assertEqual(value, "libx11-6")
        self.assertTrue(changed)

    def test_both_relaxations_apply_to_one_field(self):
        value, _ = relax("cuda-cudart-13-0 | cuda-cudart-13-2, libgbm1 (>= 23.0.4-0ubuntu1~24.04.1)")
        self.assertEqual(value, "libcudart.so.13, libgbm1")


class ControlFileTest(unittest.TestCase):
    def rewrite(self, text):
        with tempfile.TemporaryDirectory() as root:
            control = Path(root) / "control"
            control.write_text(text, encoding="utf-8")
            changed = relaxer.rewrite_control(control, VERSION_REGEX, True)
            return control.read_text(encoding="utf-8"), changed

    def test_folded_relationship_fields_are_unfolded_and_relaxed(self):
        text, changed = self.rewrite(
            "Package: deepstream-9.1\n"
            "Depends: cuda-cudart-13-0 | cuda-cudart-13-2,\n"
            "         libnpp-13-0 | libnpp-13-2,\n"
            "         libx11-6\n"
            "Conflicts: libglapi-amber\n"
        )
        self.assertEqual(
            text,
            "Package: deepstream-9.1\n"
            "Depends: libcudart.so.13, libnpp.so.13, libx11-6\n"
            "Conflicts: libglapi-amber\n",
        )
        self.assertEqual(changed, ["Depends"])

    def test_control_is_left_untouched_when_nothing_matches(self):
        original = "Package: hstream\nDepends: libc6 (>= 2.38)\n"
        text, changed = self.rewrite(original)
        self.assertEqual(text, original)
        self.assertEqual(changed, [])

    def test_non_relationship_fields_keep_cuda_minor_names(self):
        # Only relationship fields are rewritten; a description mentioning the
        # toolkit must survive verbatim.
        text, _ = self.rewrite(
            "Package: deepstream-9.1\n"
            "Description: built against cuda-cudart-13-2\n"
            "Depends: cuda-cudart-13-2\n"
        )
        self.assertIn("Description: built against cuda-cudart-13-2\n", text)
        self.assertIn("Depends: libcudart.so.13\n", text)


class InstallerParityTest(unittest.TestCase):
    """install_deb.sh relaxes the same artifact on the target host.

    It is copied to each node on its own and cannot import this module, so it
    carries a second copy of the virtual-package table in awk.  Pin the two
    together: an entry added to one but not the other fails here instead of on
    a user's machine.
    """

    def test_awk_table_matches_the_python_table(self):
        self.maxDiff = None
        installer = Path(__file__).resolve().parent / "install_deb.sh"
        entries = re.findall(
            r'^\s*cuda_virtual\["([^"]+)"\]\s*=\s*"([^"]+)"$',
            installer.read_text(encoding="utf-8"),
            re.MULTILINE,
        )
        self.assertTrue(entries, "no cuda_virtual table found in install_deb.sh")
        self.assertEqual(dict(entries), relaxer.CUDA_MAJOR_VIRTUAL_PACKAGES)
        self.assertEqual(len(entries), len(relaxer.CUDA_MAJOR_VIRTUAL_PACKAGES))


if __name__ == "__main__":
    unittest.main()
