"""Render a reviewed, committed project-documentation snapshot into dev-vault."""

from __future__ import annotations

import argparse
import hashlib
import ipaddress
import json
import os
import re
import stat
import subprocess
import tempfile
from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path

SOURCE_ROOT = Path(__file__).resolve().parents[1]
GENERATOR_INPUT = "tools/render_vault.py"
MANIFEST_INPUT = "tools/vault-docs/reviewed-export.json"
SOURCE_INPUTS = (
    "README.md",
    "docs/multiroom-control.md",
    "docs/private-module-contract.md",
    "control/README.md",
)
OUTPUTS = {
    "status.md.in": "docs/projects/soundtouch-pipewire/status.md",
    "architecture.md.in": "docs/projects/soundtouch-pipewire/architecture.md",
    "multiroom-safety.md.in": "docs/projects/soundtouch-pipewire/multiroom-safety.md",
}
TEMPLATE_INPUTS = tuple(f"tools/vault-docs/{name}" for name in OUTPUTS)
EXPORT_INPUTS = SOURCE_INPUTS + (GENERATOR_INPUT,) + TEMPLATE_INPUTS + (MANIFEST_INPUT,)
INDEX_BLOCK = "soundtouch-pipewire-index"
PLACEHOLDER_RE = re.compile(r"\{\{[A-Z0-9_]+\}\}")
PRIVATE_KEY_RE = re.compile(r"-----BEGIN [A-Z0-9 ]*PRIVATE KEY-----", re.IGNORECASE)
IPV4_RE = re.compile(r"(?<![0-9])(?:[0-9]{1,3}\.){3}[0-9]{1,3}(?![0-9])")
MAC_RE = re.compile(
    r"(?i)(?<![0-9a-f])(?:[0-9a-f]{2}[:-]){5}[0-9a-f]{2}(?![0-9a-f])"
    r"|(?<![0-9a-f])(?:[0-9a-f]{4}\.){2}[0-9a-f]{4}(?![0-9a-f])"
    r"|(?<![0-9a-f])[0-9a-f]{12}(?![0-9a-f])"
)
SECRET_ASSIGNMENT_RE = re.compile(
    r"""(?im)(?<![a-z0-9_])(?:\$\s*|export\s+)?["']?"""
    r"""(?:password|passwd|token|secret|cookie|authorization|api[_-]?key|"""
    r"""client[_-]?secret|access[_-]?token|aws_secret_access_key|private_key|"""
    r"""[A-Z0-9_]*(?:PASSWORD|PASSWD|TOKEN|SECRET|API_KEY|PRIVATE_KEY)[A-Z0-9_]*)"""
    r"""["']?\s*[:=]\s*(?:["'][^"'\n]+["']|`[^`\n]+`|[^\s#,}\]]+)"""
)
AUTHORIZATION_RE = re.compile(
    r"""(?i)(?<![a-z0-9_-])["']?authorization["']?\s*[:=]\s*"""
    r"""["']?(?:bearer|basic)\s+[^\s"'`|]+"""
)
BASIC_AUTH_URL_RE = re.compile(r"(?i)\b[a-z][a-z0-9+.-]*://[^/\s:@]+:[^/\s@]+@")
SECRET_TABLE_RE = re.compile(
    r"""(?im)^\s*\|\s*`?(?:password|passwd|token|secret|cookie|"""
    r"""authorization|api[_-]?key|client[_-]?secret|access[_-]?token|"""
    r"""aws_secret_access_key|private_key)`?\s*\|\s*(?![-:]+\s*\|)"""
    r"""[^|\n]*\S[^|\n]*\|"""
)
TOKEN_VALUE_RE = re.compile(
    r"""(?i)(?<![a-z0-9_])(?:github_pat_[a-z0-9_]{10,}|"""
    r"""gh[pousr]_[a-z0-9]{20,}|glpat-[a-z0-9_-]{10,}|"""
    r"""xox[baprs]-[a-z0-9-]{10,}|(?:AKIA|ASIA)[A-Z0-9]{16})"""
    r"""(?![a-z0-9_])"""
)
CURL_USER_RE = re.compile(r"""(?i)(?:^|\s)(?:--user|-u)\s+["']?[^:\s"']+:[^\s"']+""")
IPV6_CANDIDATE_RE = re.compile(r"(?<![0-9A-Fa-f:])[0-9A-Fa-f:]{2,}(?![0-9A-Fa-f:])")
SHA256_RE = re.compile(r"[0-9a-f]{64}")
REVISION_RE = re.compile(r"[0-9a-f]{40,64}")


@dataclass(frozen=True)
class ComponentProvenance:
    revision: str
    digest: str


@dataclass(frozen=True)
class SourceSnapshot:
    export_revision: str
    date: str
    source: ComponentProvenance
    templates: ComponentProvenance
    generator: ComponentProvenance
    manifest: ComponentProvenance

    @property
    def revision(self) -> str:
        return self.source.revision

    @property
    def digest(self) -> str:
        return self.source.digest

    @property
    def short_revision(self) -> str:
        # Ten hex digits stay visually distinct from SoundTouch's 12-hex
        # hardware identifiers, which the export rejects.
        return self.revision[:10]


@dataclass(frozen=True)
class RenderPlan:
    desired: dict[Path, str]
    baselines: dict[Path, str | None]


def run_git(source_root: Path, *args: str) -> bytes:
    proc = subprocess.run(
        ["git", "-C", str(source_root), *args],
        capture_output=True,
        check=False,
    )
    if proc.returncode:
        detail = proc.stderr.decode("utf-8", "replace").strip()
        raise RuntimeError(f"git {' '.join(args)} failed: {detail}")
    return proc.stdout


def committed_content(source_root: Path, commit: str, relative: str) -> bytes:
    return run_git(source_root, "show", f"{commit}:{relative}")


def aggregate_digest(contents: Iterable[tuple[str, bytes]]) -> str:
    digest = hashlib.sha256()
    for relative, content in contents:
        digest.update(relative.encode("utf-8"))
        digest.update(b"\0")
        digest.update(content)
        digest.update(b"\0")
    return digest.hexdigest()


def component_provenance(
    source_root: Path, commit: str, inputs: tuple[str, ...]
) -> ComponentProvenance:
    revision = (
        run_git(source_root, "rev-list", "-1", commit, "--", *inputs)
        .decode("ascii")
        .strip()
    )
    if not revision:
        raise RuntimeError(f"no committed revision for: {', '.join(inputs)}")
    digest = aggregate_digest(
        (relative, committed_content(source_root, commit, relative))
        for relative in inputs
    )
    return ComponentProvenance(revision=revision, digest=digest)


def assert_export_inputs_clean(source_root: Path, commit: str) -> None:
    head = (
        run_git(source_root, "rev-parse", "--verify", "HEAD^{commit}")
        .decode("ascii")
        .strip()
    )
    if commit != head:
        raise ValueError(
            "export source ref must resolve to HEAD so all inputs belong to one "
            "checked-out commit"
        )
    dirty = run_git(
        source_root,
        "status",
        "--porcelain=v1",
        "--untracked-files=all",
        "--",
        *EXPORT_INPUTS,
    ).decode("utf-8", "replace")
    if dirty.strip():
        paths = ", ".join(line[3:] for line in dirty.splitlines())
        raise ValueError(
            "export inputs must be committed and clean before rendering: " + paths
        )
    # Status catches staged and worktree changes. Comparing every byte also
    # proves that an ignored or otherwise unusual checkout did not evade it.
    for relative in EXPORT_INPUTS:
        path = source_root / relative
        try:
            working = path.read_bytes()
        except OSError as exc:
            raise ValueError(f"cannot read export input {path}: {exc}") from exc
        committed = committed_content(source_root, commit, relative)
        if working != committed:
            raise ValueError(f"export input does not match {commit}: {relative}")
    try:
        executed_generator = Path(__file__).resolve(strict=True).read_bytes()
    except OSError as exc:
        raise ValueError(f"cannot read the executed export generator: {exc}") from exc
    committed_generator = committed_content(source_root, commit, GENERATOR_INPUT)
    if executed_generator != committed_generator:
        raise ValueError(
            "the executed export generator does not match the generator "
            "committed in the selected source repository"
        )


def parse_review_manifest(content: bytes) -> dict[str, object]:
    try:
        manifest = json.loads(content.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValueError(f"invalid reviewed export manifest: {exc}") from exc
    if not isinstance(manifest, dict) or manifest.get("schema") != 1:
        raise ValueError("invalid reviewed export manifest schema")
    required = {
        "source_docs_sha256": SHA256_RE,
        "templates_sha256": SHA256_RE,
        "generator_revision": REVISION_RE,
        "generator_sha256": SHA256_RE,
    }
    for key, pattern in required.items():
        value = manifest.get(key)
        if not isinstance(value, str) or pattern.fullmatch(value.lower()) is None:
            raise ValueError(f"invalid {key} in reviewed export manifest")
    return manifest


def reviewed_manifest(snapshot: SourceSnapshot) -> dict[str, object]:
    return {
        "schema": 1,
        "source_docs_sha256": snapshot.source.digest,
        "templates_sha256": snapshot.templates.digest,
        "generator_revision": snapshot.generator.revision,
        "generator_sha256": snapshot.generator.digest,
    }


def assert_snapshot_reviewed(
    snapshot: SourceSnapshot, manifest: dict[str, object]
) -> None:
    expected = reviewed_manifest(snapshot)
    if manifest != expected:
        mismatched = sorted(
            key for key, value in expected.items() if manifest.get(key) != value
        )
        raise ValueError(
            "export inputs changed since the Obsidian summary was reviewed "
            f"({', '.join(mismatched)}); review the summaries and update "
            f"{Path(MANIFEST_INPUT).name}"
        )


def source_snapshot(
    source_root: Path, source_ref: str, *, require_reviewed: bool = True
) -> SourceSnapshot:
    commit = (
        run_git(
            source_root,
            "rev-parse",
            "--verify",
            "--end-of-options",
            f"{source_ref}^{{commit}}",
        )
        .decode("ascii")
        .strip()
    )
    assert_export_inputs_clean(source_root, commit)
    source = component_provenance(source_root, commit, SOURCE_INPUTS)
    templates = component_provenance(source_root, commit, TEMPLATE_INPUTS)
    generator = component_provenance(source_root, commit, (GENERATOR_INPUT,))
    manifest_component = component_provenance(source_root, commit, (MANIFEST_INPUT,))
    date = (
        run_git(source_root, "show", "-s", "--format=%cs", source.revision)
        .decode("ascii")
        .strip()
    )
    snapshot = SourceSnapshot(
        export_revision=commit,
        date=date,
        source=source,
        templates=templates,
        generator=generator,
        manifest=manifest_component,
    )
    manifest = parse_review_manifest(
        committed_content(source_root, commit, MANIFEST_INPUT)
    )
    if require_reviewed:
        assert_snapshot_reviewed(snapshot, manifest)
    return snapshot


def provenance_values(snapshot: SourceSnapshot) -> dict[str, str]:
    return {
        "EXPORT_REVISION": snapshot.export_revision,
        "SOURCE_REVISION": snapshot.source.revision,
        "SOURCE_SHORT_REVISION": snapshot.short_revision,
        "SOURCE_DATE": snapshot.date,
        "SOURCE_DIGEST": snapshot.source.digest,
        "TEMPLATE_REVISION": snapshot.templates.revision,
        "TEMPLATE_DIGEST": snapshot.templates.digest,
        "GENERATOR_REVISION": snapshot.generator.revision,
        "GENERATOR_DIGEST": snapshot.generator.digest,
        "MANIFEST_REVISION": snapshot.manifest.revision,
        "MANIFEST_DIGEST": snapshot.manifest.digest,
    }


def render_template(content: bytes, path: Path, snapshot: SourceSnapshot) -> str:
    try:
        rendered = content.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise ValueError(f"template is not UTF-8: {path}") from exc
    for name, value in provenance_values(snapshot).items():
        rendered = rendered.replace(f"{{{{{name}}}}}", value)
    unresolved = sorted(set(PLACEHOLDER_RE.findall(rendered)))
    if unresolved:
        raise ValueError(
            f"unresolved template placeholders in {path}: {', '.join(unresolved)}"
        )
    if not rendered.endswith("\n"):
        rendered += "\n"
    return rendered


def provenance_comments(snapshot: SourceSnapshot) -> str:
    values = provenance_values(snapshot)
    return (
        f"<!-- export-input-revision: {values['EXPORT_REVISION']} -->\n"
        f"<!-- source-docs-revision: {values['SOURCE_REVISION']} -->\n"
        f"<!-- source-docs-sha256: {values['SOURCE_DIGEST']} -->\n"
        f"<!-- templates-revision: {values['TEMPLATE_REVISION']} -->\n"
        f"<!-- templates-sha256: {values['TEMPLATE_DIGEST']} -->\n"
        f"<!-- generator-revision: {values['GENERATOR_REVISION']} -->\n"
        f"<!-- generator-sha256: {values['GENERATOR_DIGEST']} -->\n"
        f"<!-- review-manifest-revision: {values['MANIFEST_REVISION']} -->\n"
        f"<!-- review-manifest-sha256: {values['MANIFEST_DIGEST']} -->\n"
    )


def generated_block(name: str, body: str, snapshot: SourceSnapshot) -> str:
    return (
        f"<!-- BEGIN GENERATED: {name} -->\n"
        f"{provenance_comments(snapshot)}"
        f"{body.rstrip()}\n"
        f"<!-- END GENERATED: {name} -->"
    )


def replace_block(text: str, name: str, generated: str) -> str:
    start = f"<!-- BEGIN GENERATED: {name} -->"
    end = f"<!-- END GENERATED: {name} -->"
    if text.count(start) != 1 or text.count(end) != 1:
        raise ValueError(f"missing or ambiguous generated block markers for {name}")
    before, remainder = text.split(start, 1)
    _old, after = remainder.split(end, 1)
    return before + generated + after


def assert_safe_output(path: Path, content: str) -> None:
    checks = (
        (PRIVATE_KEY_RE, "private key material"),
        (IPV4_RE, "an IPv4 address"),
        (MAC_RE, "a hardware identifier"),
        (AUTHORIZATION_RE, "an Authorization credential"),
        (BASIC_AUTH_URL_RE, "credentials in a URL"),
        (SECRET_ASSIGNMENT_RE, "a credential-like assignment"),
        (SECRET_TABLE_RE, "credential-like table data"),
        (TOKEN_VALUE_RE, "a recognizable access token"),
        (CURL_USER_RE, "command-line basic-auth credentials"),
    )
    for pattern, description in checks:
        if pattern.search(content):
            raise ValueError(f"refusing to render {path}: found {description}")
    for candidate in IPV6_CANDIDATE_RE.findall(content):
        if ":" not in candidate:
            continue
        try:
            address = ipaddress.ip_address(candidate)
        except ValueError:
            continue
        if address.version == 6:
            raise ValueError(f"refusing to render {path}: found an IPv6 address")


def assert_safe_vault_path(
    vault: Path, path: Path, *, require_existing_file: bool = False
) -> None:
    if not vault.is_absolute() or not path.is_absolute():
        raise ValueError("vault output paths must be absolute")
    try:
        relative = path.relative_to(vault)
    except ValueError as exc:
        raise ValueError(f"vault output escapes the target vault: {path}") from exc
    if not vault.is_dir() or vault.is_symlink():
        raise ValueError(f"vault root is not a real directory: {vault}")

    current = vault
    final_exists = False
    for index, part in enumerate(relative.parts):
        current /= part
        try:
            metadata = current.lstat()
        except FileNotFoundError:
            break
        except OSError as exc:
            raise ValueError(
                f"cannot inspect vault output path {current}: {exc}"
            ) from exc
        if stat.S_ISLNK(metadata.st_mode):
            raise ValueError(f"vault output path contains a symlink: {current}")
        final = index == len(relative.parts) - 1
        if final:
            final_exists = True
            if not stat.S_ISREG(metadata.st_mode):
                raise ValueError(f"vault output is not a regular file: {current}")
        elif not stat.S_ISDIR(metadata.st_mode):
            raise ValueError(f"vault output parent is not a directory: {current}")
    if require_existing_file and not final_exists:
        raise ValueError(f"required vault file does not exist: {path}")


def read_vault_file(vault: Path, path: Path) -> str | None:
    assert_safe_vault_path(vault, path)
    if not path.exists():
        return None
    try:
        return path.read_text(encoding="utf-8")
    except OSError as exc:
        raise ValueError(f"cannot read vault output {path}: {exc}") from exc


def desired_files(
    source_root: Path, vault: Path, snapshot: SourceSnapshot
) -> RenderPlan:
    desired: dict[Path, str] = {}
    baselines: dict[Path, str | None] = {}
    for template_name, relative_output in OUTPUTS.items():
        template_relative = f"tools/vault-docs/{template_name}"
        output = vault / relative_output
        body = render_template(
            committed_content(source_root, snapshot.export_revision, template_relative),
            source_root / template_relative,
            snapshot,
        )
        content = (
            generated_block(f"soundtouch-pipewire-{output.stem}", body, snapshot) + "\n"
        )
        assert_safe_output(output, content)
        baselines[output] = read_vault_file(vault, output)
        desired[output] = content

    index_path = vault / "docs/index.md"
    assert_safe_vault_path(vault, index_path, require_existing_file=True)
    index_baseline = read_vault_file(vault, index_path)
    if index_baseline is None:
        raise ValueError(f"vault index does not exist: {index_path}")
    generated_index = generated_block(
        INDEX_BLOCK,
        (
            "- [soundtouch-pipewire](projects/soundtouch-pipewire/status.md) - "
            "bezpečný PipeWire companion pro hardwarové ovládání SoundTouch "
            "a řízení zón."
        ),
        snapshot,
    )
    # The rest of docs/index.md belongs to other projects and can legitimately
    # mention their private runtime data. Only our replacement block is ours to
    # validate.
    assert_safe_output(index_path, generated_index)
    index = replace_block(index_baseline, INDEX_BLOCK, generated_index)
    baselines[index_path] = index_baseline
    desired[index_path] = index
    return RenderPlan(desired=desired, baselines=baselines)


def atomic_write(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    mode = path.stat().st_mode & 0o777 if path.exists() else 0o644
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=path.parent
    )
    temporary_path = Path(temporary_name)
    try:
        os.fchmod(descriptor, mode)
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="") as handle:
            handle.write(content)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary_path, path)
        directory_flags = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
        directory_descriptor = os.open(path.parent, directory_flags)
        try:
            os.fsync(directory_descriptor)
        finally:
            os.close(directory_descriptor)
    except BaseException:
        try:
            os.close(descriptor)
        except OSError:
            pass
        temporary_path.unlink(missing_ok=True)
        raise


def update_files(plan: RenderPlan, vault: Path, check: bool) -> list[Path]:
    changed: list[Path] = []
    for path, content in plan.desired.items():
        current = read_vault_file(vault, path)
        if current != plan.baselines[path]:
            raise ValueError(
                f"vault output changed concurrently while rendering: {path}"
            )
        if current != content:
            changed.append(path)
    if check:
        return changed
    # Planning, provenance verification, rendering and sanitization have all
    # completed before the first target is touched.
    for path in changed:
        assert_safe_vault_path(vault, path)
        current = read_vault_file(vault, path)
        if current != plan.baselines[path]:
            raise ValueError(
                f"vault output changed concurrently before replacement: {path}"
            )
        atomic_write(path, plan.desired[path])
    return changed


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=SOURCE_ROOT)
    parser.add_argument("--source-ref", default="HEAD")
    parser.add_argument("--vault", type=Path)
    parser.add_argument("--check", action="store_true")
    parser.add_argument(
        "--print-reviewed-manifest",
        action="store_true",
        help="print the manifest for clean committed inputs after manual review",
    )
    args = parser.parse_args(argv)

    source_root = args.source.resolve()
    snapshot = source_snapshot(
        source_root,
        args.source_ref,
        require_reviewed=not args.print_reviewed_manifest,
    )
    if args.print_reviewed_manifest:
        print(json.dumps(reviewed_manifest(snapshot), indent=2, sort_keys=True))
        return 0
    if args.vault is None:
        parser.error("--vault is required unless --print-reviewed-manifest is used")

    try:
        vault = args.vault.resolve(strict=True)
    except OSError as exc:
        raise ValueError(f"cannot resolve target vault {args.vault}: {exc}") from exc
    plan = desired_files(source_root, vault, snapshot)
    # Recheck after reading templates and the target index to narrow the chance
    # of rendering across a concurrent source-tree edit.
    assert_export_inputs_clean(source_root, snapshot.export_revision)
    changed = update_files(plan, vault, args.check)
    if args.check and changed:
        for path in changed:
            print(f"out of date: {path}")
        return 1
    if changed:
        print("updated: " + ", ".join(str(path) for path in changed))
    else:
        print("vault documentation is current")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
