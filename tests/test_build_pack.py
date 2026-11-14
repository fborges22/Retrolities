"""Host-side tests: python3 -m unittest discover -s tests -p test_build_pack.py."""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import zipfile


spec = importlib.util.spec_from_file_location(
    "build_pack", Path(__file__).resolve().parents[1] / "tools" / "build_pack.py"
)
build_pack = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build_pack)


class BuildPackTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.output = self.root / "build" / "retrolities.zip"
        self.artifacts = {
            "build/dos/bin/HELLO.EXE": b"DOS binary",
            "build/os2/bin/ARROWO.EXE": b"OS/2 Arrow",
            "build/win16/bin/HELLO.EXE": b"Win16 binary",
            "build/win64/bin/ARROW64.EXE": b"x64 binary",
            "build/win16/lib/l/retro.lib": b"library",
            "build/win9x/obj/hello/hello.obj": b"object",
            "build/winnt/obj/arrow/arrow.map": b"map",
        }
        self.expected = {
            "HELLOD.EXE": b"DOS binary",
            "ARROWO.EXE": b"OS/2 Arrow",
            "HELLO16.EXE": b"Win16 binary",
            "ARROW64.EXE": b"x64 binary",
        }

    def create_artifacts(self, *args, **kwargs):
        for name, data in self.artifacts.items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)

    def run_main(self, *args):
        with patch.object(build_pack, "ROOT", self.root), \
                contextlib.redirect_stdout(io.StringIO()), \
                contextlib.redirect_stderr(io.StringIO()):
            return build_pack.main(list(args))

    def test_builds_all_and_packs_only_executables_at_root(self):
        self.output.parent.mkdir()
        self.output.write_bytes(b"previous ZIP")
        with patch.object(
            build_pack.subprocess, "run", side_effect=self.create_artifacts
        ) as run:
            self.assertEqual(self.run_main("--local"), 0)
        self.assertEqual(run.call_args.args[0], ["make", "all"])
        self.assertEqual(run.call_args.kwargs["cwd"], self.root)
        self.assertTrue(run.call_args.kwargs["check"])
        with zipfile.ZipFile(self.output) as archive:
            self.assertEqual(set(archive.namelist()), set(self.expected))
            for name, data in self.expected.items():
                self.assertEqual(archive.read(name), data)

    def test_default_uses_docker_and_supports_custom_destination(self):
        output = self.root / "releases" / "all.zip"
        with patch.dict(os.environ, {"UID": "1234", "GID": "5678"}), \
                patch.object(
                    build_pack.subprocess, "run", side_effect=self.create_artifacts
                ) as run:
            self.assertEqual(self.run_main("--output", str(output)), 0)
        self.assertEqual(run.call_args.args[0], [
            "docker", "compose", "run", "--rm", "--build", "-T",
            "build", "make", "all",
        ])
        self.assertEqual(run.call_args.kwargs["env"]["UID"], "1234")
        self.assertEqual(run.call_args.kwargs["env"]["GID"], "5678")
        with zipfile.ZipFile(output) as archive:
            self.assertEqual(set(archive.namelist()), set(self.expected))

    def test_platform_selection_changes_build_targets_and_zip_contents(self):
        with patch.object(
            build_pack.subprocess, "run", side_effect=self.create_artifacts
        ) as run:
            self.assertEqual(
                self.run_main("--local", "--platforms", "win64", "dos", "dos", "-v"), 0
            )
        self.assertEqual(run.call_args.args[0], ["make", "dos", "win64", "V=1"])
        with zipfile.ZipFile(self.output) as archive:
            self.assertEqual(set(archive.namelist()), {"HELLO.EXE", "ARROW64.EXE"})

    def test_os2_is_a_selectable_packaging_target(self):
        with patch.object(
            build_pack.subprocess, "run", side_effect=self.create_artifacts
        ) as run:
            self.assertEqual(self.run_main("--local", "--platforms", "os2"), 0)
        self.assertEqual(run.call_args.args[0], ["make", "os2"])
        with zipfile.ZipFile(self.output) as archive:
            self.assertEqual(archive.namelist(), ["ARROWO.EXE"])

    def test_skip_build_and_case_insensitive_executable_filters(self):
        self.create_artifacts()
        with patch.object(build_pack.subprocess, "run") as run:
            self.assertEqual(
                self.run_main("--skip-build", "--executables", "arrow*.exe"), 0
            )
        run.assert_not_called()
        with zipfile.ZipFile(self.output) as archive:
            self.assertEqual(set(archive.namelist()), {"ARROWO.EXE", "ARROW64.EXE"})

    def test_long_names_and_stored_compression(self):
        self.create_artifacts()
        self.assertEqual(
            self.run_main("--skip-build", "--long-names", "--compression", "stored"), 0
        )
        with zipfile.ZipFile(self.output) as archive:
            self.assertEqual(set(archive.namelist()), {
                "HELLO-dos.EXE", "HELLO-win16.EXE", "ARROWO.EXE", "ARROW64.EXE",
            })
            self.assertTrue(
                all(info.compress_type == zipfile.ZIP_STORED for info in archive.infolist())
            )

    def test_short_names_handle_truncation_and_reserved_case_insensitive_names(self):
        self.artifacts.update({
            "build/dos/bin/hello16.exe": b"reserved",
            "build/win16/bin/ARROW16.EXE": b"Win16 Arrow",
            "build/win9x/bin/ARROW32.EXE": b"Win9x Arrow",
            "build/win16/bin/PICASSO.EXE": b"Win16 paint",
            "build/winnt/bin/PICASSO.EXE": b"NT paint",
            "build/dos/bin/SMALL.com": b"COM binary",
        })
        self.create_artifacts()
        self.assertEqual(self.run_main("--skip-build"), 0)
        with zipfile.ZipFile(self.output) as archive:
            names = archive.namelist()
            self.assertEqual(len(names), len({name.casefold() for name in names}))
            self.assertTrue(all(
                len(Path(name).stem) <= 8 and len(Path(name).suffix) <= 4
                and "/" not in name for name in names
            ))
            self.assertEqual(archive.read("hello16.exe"), b"reserved")
            self.assertEqual(archive.read("HELLO161.EXE"), b"Win16 binary")
            self.assertEqual(archive.read("PICASS16.EXE"), b"Win16 paint")
            self.assertEqual(archive.read("PICASSNT.EXE"), b"NT paint")
            self.assertEqual(archive.read("ARROW16.EXE"), b"Win16 Arrow")
            self.assertEqual(archive.read("ARROW32.EXE"), b"Win9x Arrow")
            self.assertEqual(archive.read("ARROW64.EXE"), b"x64 binary")
            self.assertEqual(archive.read("SMALL.com"), b"COM binary")

    def test_filter_without_matches_preserves_existing_zip(self):
        self.create_artifacts()
        self.output.write_bytes(b"previous ZIP")
        self.assertEqual(
            self.run_main("--skip-build", "--executables", "MISSING.EXE"), 1
        )
        self.assertEqual(self.output.read_bytes(), b"previous ZIP")

    def test_failed_build_preserves_existing_zip(self):
        self.create_artifacts()
        self.output.write_bytes(b"previous ZIP")
        with patch.object(
            build_pack.subprocess, "run",
            side_effect=subprocess.CalledProcessError(2, ["make", "all"]),
        ):
            self.assertEqual(self.run_main("--local"), 1)
        self.assertEqual(self.output.read_bytes(), b"previous ZIP")

    def test_empty_build_does_not_create_zip(self):
        with patch.object(build_pack.subprocess, "run"):
            self.assertEqual(self.run_main("--local"), 1)
        self.assertFalse(self.output.exists())

    def test_pack_failure_preserves_zip_and_removes_temporary_file(self):
        self.create_artifacts()
        self.output.write_bytes(b"previous ZIP")
        with patch.object(
            build_pack.zipfile.ZipFile, "write", side_effect=OSError("write failed")
        ):
            with self.assertRaises(OSError):
                build_pack.pack_artifacts(self.root, self.output)
        self.assertEqual(self.output.read_bytes(), b"previous ZIP")
        self.assertEqual(list(self.output.parent.glob(".retrolities-*")), [])


if __name__ == "__main__":
    unittest.main()
