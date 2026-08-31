from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "render_vault", ROOT / "tools/render_vault.py"
)
assert SPEC is not None and SPEC.loader is not None
render_vault = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = render_vault
SPEC.loader.exec_module(render_vault)


class RenderVaultTest(unittest.TestCase):
    def git(self, source: Path, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["git", "-C", str(source), *args],
            check=True,
            capture_output=True,
            text=True,
        )

    def commit(self, source: Path, message: str) -> None:
        self.git(source, "add", ".")
        self.git(
            source,
            "-c",
            "user.name=Vault test",
            "-c",
            "user.email=vault-test@example.invalid",
            "commit",
            "-q",
            "-m",
            message,
        )

    def make_source(self, root: Path) -> Path:
        source = root / "source"
        source.mkdir()
        self.git(source, "init", "-q")
        for relative in render_vault.SOURCE_INPUTS:
            path = source / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(f"committed {relative}\n", encoding="utf-8")
        generator = source / render_vault.GENERATOR_INPUT
        generator.parent.mkdir(parents=True, exist_ok=True)
        generator.write_bytes(Path(render_vault.__file__).read_bytes())
        for relative in render_vault.TEMPLATE_INPUTS:
            path = source / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(
                "# Fixture\n\n"
                "source={{SOURCE_REVISION}}\n"
                "source-sha={{SOURCE_DIGEST}}\n"
                "template={{TEMPLATE_REVISION}}\n"
                "template-sha={{TEMPLATE_DIGEST}}\n"
                "generator={{GENERATOR_REVISION}}\n"
                "generator-sha={{GENERATOR_DIGEST}}\n"
                "manifest={{MANIFEST_REVISION}}\n"
                "manifest-sha={{MANIFEST_DIGEST}}\n",
                encoding="utf-8",
            )
        manifest = source / render_vault.MANIFEST_INPUT
        manifest.write_text(
            json.dumps(
                {
                    "schema": 1,
                    "source_docs_sha256": "0" * 64,
                    "templates_sha256": "0" * 64,
                    "generator_revision": "0" * 40,
                    "generator_sha256": "0" * 64,
                },
                indent=2,
                sort_keys=True,
            )
            + "\n",
            encoding="utf-8",
        )
        self.commit(source, "fixture inputs")

        candidate = render_vault.source_snapshot(source, "HEAD", require_reviewed=False)
        manifest.write_text(
            json.dumps(
                render_vault.reviewed_manifest(candidate), indent=2, sort_keys=True
            )
            + "\n",
            encoding="utf-8",
        )
        self.commit(source, "review export inputs")
        return source

    def make_vault(self, root: Path, prefix: str = "# Index\n\n") -> Path:
        vault = root / "vault"
        index = vault / "docs/index.md"
        index.parent.mkdir(parents=True)
        index.write_text(
            prefix
            + "<!-- BEGIN GENERATED: soundtouch-pipewire-index -->\n"
            + "old\n"
            + "<!-- END GENERATED: soundtouch-pipewire-index -->\n",
            encoding="utf-8",
        )
        return vault

    def test_integration_committed_reviewed_render_and_check(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = self.make_source(root)
            # Foreign index content is deliberately not scanned as our output.
            vault = self.make_vault(root, "# Index\n\npassword: foreign-secret\n\n")

            self.assertEqual(
                render_vault.main(["--source", str(source), "--vault", str(vault)]),
                0,
            )
            self.assertEqual(
                render_vault.main(
                    ["--source", str(source), "--vault", str(vault), "--check"]
                ),
                0,
            )
            snapshot = render_vault.source_snapshot(source, "HEAD")
            status = vault / "docs/projects/soundtouch-pipewire/status.md"
            rendered = status.read_text(encoding="utf-8")
            self.assertIn(snapshot.export_revision, rendered)
            self.assertIn(snapshot.source.digest, rendered)
            self.assertIn(snapshot.templates.digest, rendered)
            self.assertIn(snapshot.generator.digest, rendered)
            self.assertIn(snapshot.manifest.digest, rendered)
            self.assertIn(
                "password: foreign-secret",
                (vault / "docs/index.md").read_text(encoding="utf-8"),
            )

    def test_dirty_template_fails_before_any_write(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = self.make_source(root)
            vault = self.make_vault(root)
            before = (vault / "docs/index.md").read_bytes()
            (source / render_vault.TEMPLATE_INPUTS[0]).write_text(
                "dirty template\n", encoding="utf-8"
            )

            with self.assertRaisesRegex(ValueError, "committed and clean"):
                render_vault.main(["--source", str(source), "--vault", str(vault)])
            self.assertEqual((vault / "docs/index.md").read_bytes(), before)
            self.assertFalse((vault / "docs/projects").exists())

    def test_manifest_digest_mismatch_fails_before_any_write(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = self.make_source(root)
            vault = self.make_vault(root)
            before = (vault / "docs/index.md").read_bytes()
            manifest_path = source / render_vault.MANIFEST_INPUT
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            manifest["templates_sha256"] = "f" * 64
            manifest_path.write_text(
                json.dumps(manifest, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            self.commit(source, "stale review manifest")

            with self.assertRaisesRegex(ValueError, "changed since"):
                render_vault.main(["--source", str(source), "--vault", str(vault)])
            self.assertEqual((vault / "docs/index.md").read_bytes(), before)
            self.assertFalse((vault / "docs/projects").exists())

    def test_check_reports_drift_without_writing(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = self.make_source(root)
            vault = self.make_vault(root)
            before = (vault / "docs/index.md").read_bytes()

            self.assertEqual(
                render_vault.main(
                    ["--source", str(source), "--vault", str(vault), "--check"]
                ),
                1,
            )
            self.assertEqual((vault / "docs/index.md").read_bytes(), before)
            self.assertFalse((vault / "docs/projects").exists())

    def test_non_head_source_ref_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = self.make_source(root)
            previous = self.git(source, "rev-parse", "HEAD^").stdout.strip()
            with self.assertRaisesRegex(ValueError, "must resolve to HEAD"):
                render_vault.source_snapshot(source, previous)

    def test_executed_generator_must_match_selected_source(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = self.make_source(root)
            generator = source / render_vault.GENERATOR_INPUT
            generator.write_text("different committed generator\n", encoding="utf-8")
            self.commit(source, "replace generator")

            with self.assertRaisesRegex(ValueError, "executed export generator"):
                render_vault.source_snapshot(source, "HEAD", require_reviewed=False)

    def test_missing_index_markers_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = self.make_source(root)
            snapshot = render_vault.source_snapshot(source, "HEAD")
            vault = root / "vault"
            index = vault / "docs/index.md"
            index.parent.mkdir(parents=True)
            index.write_text("# Index\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "generated block markers"):
                render_vault.desired_files(source, vault, snapshot)

    def test_symlinked_vault_descendant_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = self.make_source(root)
            snapshot = render_vault.source_snapshot(source, "HEAD")
            vault = root / "vault"
            vault.mkdir()
            external_docs = root / "external-docs"
            external_docs.mkdir()
            external_index = external_docs / "index.md"
            external_index.write_text(
                "<!-- BEGIN GENERATED: soundtouch-pipewire-index -->\n"
                "external\n"
                "<!-- END GENERATED: soundtouch-pipewire-index -->\n",
                encoding="utf-8",
            )
            (vault / "docs").symlink_to(external_docs, target_is_directory=True)
            before = external_index.read_bytes()

            with self.assertRaisesRegex(ValueError, "contains a symlink"):
                render_vault.desired_files(source, vault.resolve(), snapshot)
            self.assertEqual(external_index.read_bytes(), before)
            self.assertFalse((external_docs / "projects").exists())

    def test_concurrent_index_change_fails_before_any_write(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = self.make_source(root)
            snapshot = render_vault.source_snapshot(source, "HEAD")
            vault = self.make_vault(root).resolve()
            plan = render_vault.desired_files(source, vault, snapshot)
            index = vault / "docs/index.md"
            concurrent = index.read_text(encoding="utf-8") + "concurrent edit\n"
            index.write_text(concurrent, encoding="utf-8")

            with self.assertRaisesRegex(ValueError, "changed concurrently"):
                render_vault.update_files(plan, vault, False)
            self.assertEqual(index.read_text(encoding="utf-8"), concurrent)
            self.assertFalse((vault / "docs/projects").exists())

    def test_sensitive_runtime_and_credential_values_are_rejected(self) -> None:
        cases = (
            "endpoint: 192.0.2.1\n",
            "endpoint: 2001:db8::1\n",
            "device: 00:11:22:33:44:55\n",
            "device: 001122334455\n",
            "device: 0011.2233.4455\n",
            "token=do-not-export\n",
            "`TOKEN=do-not-export`\n",
            "$ TOKEN=do-not-export\n",
            "- password: do-not-export\n",
            '"secret": "do-not-export"\n',
            '{"token":"do-not-export"}\n',
            "export SERVICE_TOKEN=do-not-export\n",
            "export AWS_SECRET_ACCESS_KEY=do-not-export\n",
            "private_key: do-not-export\n",
            "Authorization: Bearer do-not-export\n",
            '"Authorization": "Basic do-not-export"\n',
            "curl -H 'Authorization: Bearer do-not-export' example.invalid\n",
            "| token | do-not-export |\n",
            "https://user:password@example.invalid/path\n",
            "curl -u user:password example.invalid\n",
            "github_pat_1234567890abcdefghijklmnop\n",
            "ghp_1234567890abcdefghijklmnopqrstuv\n",
            "glpat-1234567890abcdef\n",
            "AKIAIOSFODNN7EXAMPLE\n",
            "-----BEGIN PRIVATE KEY-----\n",
            "-----BEGIN ENCRYPTED PRIVATE KEY-----\n",
        )
        for content in cases:
            with (
                self.subTest(content=content),
                self.assertRaisesRegex(ValueError, "refusing to render"),
            ):
                render_vault.assert_safe_output(Path("test.md"), content)

    def test_atomic_write_uses_replace(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "output.md"
            with mock.patch.object(
                render_vault.os, "replace", wraps=render_vault.os.replace
            ) as replace:
                render_vault.atomic_write(path, "new\n")
            replace.assert_called_once()
            self.assertEqual(path.read_text(encoding="utf-8"), "new\n")
            self.assertEqual(list(path.parent.glob(".output.md.*.tmp")), [])


if __name__ == "__main__":
    unittest.main()
