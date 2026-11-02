"""Check local source inventory, public Python docstrings, and guide file links.

Run with Python 3 from any directory; no third-party packages or imports needed.
This is a structural check, not proof of prose accuracy or dependency coverage.
"""
import ast
from pathlib import Path
import re
from urllib.parse import unquote, urlsplit


ROOT = Path(__file__).resolve().parents[1]
GUIDES = (
    "README.md", "docs/code-guide.md", "docs/build-and-test.md", "docs/quickstart.md", "docs/publishing.md", "THIRD_PARTY.md",
    "docs/windows-build.md", "docs/security-fixes-validation.md",
    "docs/five-node-video-test.md",
    "docs/review-2026-10-06.md", "docs/review-2026-10-29.md",
    "docs/deployment-validation-2026-10-29.md", "docs/storage-review-2026-10-29.md",
    "docs/storage-completion-2026-10-29.md", "docs/crc-review-2026-10-29.md", "docs/ccs-review-2026-10-30.md", "docs/ccs-sender-review-2026-10-30.md", "docs/publication-durability-review-2026-10-30.md", "docs/completion-validation-review-2026-10-30.md", "docs/file-read-error-review-2026-10-30.md", "docs/custody-index-review-2026-10-30.md", "docs/custody-admission-review-2026-10-30.md", "docs/storage-parallel-review-2026-10-30.md", "docs/sqlite-bindings-review-2026-10-31.md", "docs/storage-completeness-review-2026-10-31.md", "docs/custody-transactions-review-2026-10-31.md", "docs/sql-ownership-review-2026-10-31.md", "docs/db-size-review-2026-11-01.md", "docs/selection-review-2026-11-01.md", "docs/egress-retry-review-2026-11-01.md", "docs/accounting-review-2026-11-01.md", "docs/scalar-accounting-review-2026-11-01.md",
    "integration/README.md", "integration/bpnode-tcpclv4-example.md",
    "client-test/README.md",
)


def check_inventory(errors):
    """Require every direct local source/build manifest in the guide inventory."""
    guide = (ROOT / "docs/code-guide.md").read_text(encoding="utf-8")
    count = 0
    for directory in ("integration", "client-test"):
        for path in sorted((ROOT / directory).iterdir()):
            if path.suffix not in (".c", ".h", ".py") and path.name not in (
                    "CMakeLists.txt", "requirements.txt"):
                continue
            name = path.relative_to(ROOT).as_posix()
            if f"`{name}`" not in guide:
                errors.append(f"source absent from inventory: {name}")
            count += 1
    return count


def check_python(errors):
    """Parse demo files and require module/public top-level and method docs."""
    count = 0
    for path in sorted((ROOT / "client-test").glob("*.py")):
        tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
        name = path.relative_to(ROOT).as_posix()
        if not ast.get_docstring(tree):
            errors.append(f"missing module docstring: {name}")
        # Test methods describe assertions through test names; inventory covers
        # the suite. Do not treat them as a reusable application API.
        if path.name.startswith("test_"):
            continue
        for node in tree.body:
            if not isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
                continue
            members = [node]
            if isinstance(node, ast.ClassDef):
                members.extend(child for child in node.body if isinstance(
                    child, (ast.FunctionDef, ast.AsyncFunctionDef)))
            for member in members:
                if member.name.startswith("_") and member.name != "__init__":
                    continue
                if not ast.get_docstring(member):
                    errors.append(f"missing docstring: {name}:{member.lineno} {member.name}")
                count += 1
    return count


def check_links(errors):
    """Check relative inline Markdown file links; ignore URLs and anchor targets."""
    count = 0
    for name in GUIDES:
        path = ROOT / name
        source = path.read_text(encoding="utf-8")
        for target in re.findall(r"\[[^\]]+\]\(([^\s)]+)\)", source):
            parts = urlsplit(target)
            if parts.scheme or parts.netloc or not parts.path:
                continue
            if not (path.parent / unquote(parts.path)).exists():
                errors.append(f"broken relative link: {name}: {target}")
            count += 1
    return count


def main():
    """Print check results and fail if any documented structural gap is found."""
    errors = []
    inventory = check_inventory(errors)
    callables = check_python(errors)
    links = check_links(errors)
    if errors:
        raise SystemExit("\n".join(errors))
    print(f"Documentation checks passed: {inventory} inventory entries, "
          f"{callables} public Python classes/callables, {links} relative links.")


if __name__ == "__main__":
    main()
