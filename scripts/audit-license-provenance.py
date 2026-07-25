#!/usr/bin/env python3
"""Byte-identity audit: compare every vendored license file against upstream.

Verdicts:
  IDENTICAL          byte-equal to the upstream default-branch license
  IDENTICAL@PINNED   byte-equal to the license at the manifest's pinned commit
  FIRST-PARTY-OK     byte-equal to the project root LICENSE
  FIRST-PARTY-RETAINED same MIT terms with retained original copyright lines
  FIRST-PARTY-VAR    first-party but text differs from root LICENSE (inspect)
  DIFFERS            no byte-equal upstream candidate found (inspect)
  ERROR              upstream fetch failed
"""
import base64
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GRAMMARS = os.path.join(ROOT, "internal/cbm/vendored/grammars")

FIRST_PARTY = {"cobol", "form", "janet", "magma", "protobuf", "wolfram"}
FORKS = {  # self-maintained forks: vendored LICENSE must match the original upstream
    "cfml": "cfmleditor/tree-sitter-cfml",
    "cfscript": "cfmleditor/tree-sitter-cfml",
    "dotenv": "pnx/tree-sitter-dotenv",
    "qml": "yuja/tree-sitter-qmljs",
}
SPECIAL_NOTICE = {
    "assembly": "RETAINED-MIT (upstream RubixDev/tree-sitter-assembly deleted from GitHub)",
    "pine": "PROVENANCE-NOTICE (upstream kvarenzn/tree-sitter-pine declares ISC, ships no license file)",
}
DISAGREEMENT = {
    "jinja2": "dbt-labs/tree-sitter-jinja2",
    "just": "casey/tree-sitter-just",
    "move": "tzakian/tree-sitter-move",
    "sshconfig": "ObserverOfTime/tree-sitter-ssh-config",
    "zsh": "georgeharker/tree-sitter-zsh",
}
LIBS = {
    "vendored/mimalloc": ("microsoft/mimalloc", None),
    "vendored/tre": ("laurikari/tre", None),
    "vendored/xxhash": ("Cyan4973/xxHash", None),
    "vendored/yyjson": ("ibireme/yyjson", None),
    "internal/cbm/vendored/lz4": ("lz4/lz4", "lib/LICENSE"),
    "internal/cbm/vendored/zstd": ("facebook/zstd", None),
    "internal/cbm/vendored/simplecpp": ("danmar/simplecpp", None),
    "internal/cbm/vendored/verstable": ("JacksonAllan/Verstable", None),
    "internal/cbm/vendored/wyhash": ("wangyi-fudan/wyhash", None),
    "internal/cbm/vendored/ts_runtime": ("tree-sitter/tree-sitter", None),
    "internal/cbm/vendored/common": ("tree-sitter/tree-sitter-html", None),
    "internal/cbm/vendored/common/tree_sitter": ("tree-sitter/tree-sitter", None),
}
CANDIDATE_NAMES = ["LICENSE", "LICENSE.md", "LICENSE.txt", "COPYING",
                   "COPYING.txt", "LICENSE-MIT", "UNLICENSE", "LICENCE",
                   "license", "License.txt", "NOTICE"]
COPYRIGHT_LINE = re.compile(r"^\s*Copyright \(c\) .+$")


def gh_api(path):
    r = subprocess.run(["gh", "api", path], capture_output=True, text=True)
    if r.returncode != 0:
        return None
    return r.stdout


def upstream_default_license(repo):
    out = gh_api(f"repos/{repo}/license")
    if not out:
        return None
    try:
        d = json.loads(out)
        return base64.b64decode(d.get("content", "")).decode("utf-8", "replace")
    except Exception:
        return None


def upstream_file(repo, path, ref=None):
    url = f"repos/{repo}/contents/{path}"
    if ref:
        url += f"?ref={ref}"
    out = gh_api(url)
    if not out:
        return None
    try:
        d = json.loads(out)
        if isinstance(d, dict) and d.get("content"):
            return base64.b64decode(d["content"]).decode("utf-8", "replace")
    except Exception:
        pass
    return None


def local_license(dirpath):
    if not os.path.isdir(dirpath):
        return None, None
    for f in sorted(os.listdir(dirpath)):
        if re.match(r"^(LICENSE|LICENCE|COPYING|UNLICENSE)", f, re.I):
            p = os.path.join(dirpath, f)
            with open(p, encoding="utf-8", errors="replace") as fh:
                return f, fh.read()
    return None, None


def first_party_license_retained(candidate, root_license):
    """Verify retained first-party MIT terms and original copyright ownership.
    验证保留的第一方 MIT 条款及原始版权归属。

    Args:
        candidate: First-party grammar license text.
        root_license: Current project root license text.
    参数：
        candidate：第一方 grammar 许可证文本。
        root_license：当前项目根许可证文本。

    Returns:
        True when non-copyright terms match and every candidate copyright is
        still present in the root license.
        当非版权条款一致且候选许可证的全部版权行仍存在于根许可证时返回 True。
    """
    # Candidate lines normalized for line endings and trailing whitespace.
    # 对候选许可证行进行换行符和行尾空白规范化。
    candidate_lines = [line.rstrip() for line in candidate.strip().splitlines()]
    # Root lines normalized with the same deterministic rule.
    # 使用相同确定性规则规范化根许可证行。
    root_lines = [line.rstrip() for line in root_license.strip().splitlines()]
    # Original copyrights declared by the first-party grammar.
    # 第一方 grammar 声明的原始版权。
    candidate_copyrights = {line.strip() for line in candidate_lines
                            if COPYRIGHT_LINE.match(line)}
    # Copyrights retained by the current root license.
    # 当前根许可证保留的版权。
    root_copyrights = {line.strip() for line in root_lines
                       if COPYRIGHT_LINE.match(line)}
    # License terms excluding additive copyright ownership lines.
    # 排除新增版权归属行后的许可证条款。
    candidate_terms = [line for line in candidate_lines
                       if not COPYRIGHT_LINE.match(line)]
    root_terms = [line for line in root_lines if not COPYRIGHT_LINE.match(line)]
    return (bool(candidate_copyrights) and
            candidate_copyrights.issubset(root_copyrights) and
            candidate_terms == root_terms)


def parse_manifest():
    """grammar -> (repo, pinned_commit) from the verified-upstream table."""
    out = {}
    with open(os.path.join(GRAMMARS, "MANIFEST.md"), encoding="utf-8") as fh:
        for line in fh:
            m = re.match(
                r"^\| (\w[\w.+-]*) \| \d+ \| ([\w.-]+/[\w.-]+) \| `([^`]+)` \|", line)
            if m:
                out[m.group(1)] = (m.group(2), m.group(3))
    return out


def main():
    with open(os.path.join(ROOT, "LICENSE"), encoding="utf-8") as fh:
        root_license = fh.read()

    manifest = parse_manifest()
    results = {}

    def check_upstream(key, dirpath, repo, pinned=None, exact_path=None):
        fname, ours = local_license(dirpath)
        if ours is None:
            results[key] = ("NO-LOCAL-LICENSE", "")
            return
        # 1) exact upstream path (e.g. lz4 lib/LICENSE), at HEAD then pinned
        if exact_path:
            for ref in (None, pinned):
                up = upstream_file(repo, exact_path, ref)
                if up is not None and up == ours:
                    results[key] = ("IDENTICAL" if ref is None else "IDENTICAL@PINNED",
                                    f"{repo}:{exact_path}")
                    return
        # 2) default-branch detected license
        up = upstream_default_license(repo)
        if up is not None and up == ours:
            results[key] = ("IDENTICAL", f"{repo} (default branch)")
            return
        # 3) pinned-commit candidates by filename
        if pinned:
            tried = [fname] + [c for c in CANDIDATE_NAMES if c != fname]
            for cand in tried:
                up2 = upstream_file(repo, cand, pinned)
                if up2 is not None and up2 == ours:
                    results[key] = ("IDENTICAL@PINNED", f"{repo}:{cand}@{pinned}")
                    return
        # 4) HEAD candidates by filename
        for cand in CANDIDATE_NAMES:
            up3 = upstream_file(repo, cand)
            if up3 is not None and up3 == ours:
                results[key] = ("IDENTICAL", f"{repo}:{cand} (default branch)")
                return
        results[key] = ("DIFFERS" if up is not None else "ERROR", f"{repo}")

    # Libraries
    for rel, (repo, exact) in LIBS.items():
        check_upstream(rel, os.path.join(ROOT, rel), repo, exact_path=exact)

    # Special: nomic = canonical Apache-2.0 text; sqlite3 = first-party notice
    fname, ours = local_license(os.path.join(ROOT, "vendored/nomic"))
    apache = subprocess.run(
        ["curl", "-fsSL", "https://www.apache.org/licenses/LICENSE-2.0.txt"],
        capture_output=True, text=True).stdout
    results["vendored/nomic"] = (
        "IDENTICAL" if ours == apache else "DIFFERS",
        "apache.org canonical LICENSE-2.0.txt")
    results["vendored/sqlite3"] = ("FIRST-PARTY-NOTICE",
                                   "own public-domain notice (sqlite has no upstream LICENSE)")

    # Grammars
    for g in sorted(os.listdir(GRAMMARS)):
        d = os.path.join(GRAMMARS, g)
        if not os.path.isdir(d):
            continue
        key = f"grammars/{g}"
        if g in FIRST_PARTY:
            fname, ours = local_license(d)
            if ours == root_license:
                results[key] = ("FIRST-PARTY-OK", "== project root LICENSE")
            elif ours is not None and first_party_license_retained(ours, root_license):
                results[key] = (
                    "FIRST-PARTY-RETAINED",
                    f"{fname}: original copyright retained; MIT terms match root LICENSE")
            else:
                results[key] = ("FIRST-PARTY-VAR", f"{fname}: differs from root LICENSE")
            continue
        if g in FORKS:
            check_upstream(key, d, FORKS[g])
            continue
        if g in SPECIAL_NOTICE:
            results[key] = ("MANUAL-VERIFIED", SPECIAL_NOTICE[g])
            continue
        if g in DISAGREEMENT:
            check_upstream(key, d, DISAGREEMENT[g])
            continue
        if g in manifest:
            repo, pinned = manifest[g]
            check_upstream(key, d, repo, pinned=pinned)
            continue
        results[key] = ("NO-MANIFEST-ENTRY", "")

    # Report
    from collections import Counter
    counts = Counter(v[0] for v in results.values())
    print("=== verdict histogram ===")
    for k, v in counts.most_common():
        print(f"  {k}: {v}")
    print()
    print("=== everything that is NOT byte-identical ===")
    for key in sorted(results):
        verdict, detail = results[key]
        if verdict not in ("IDENTICAL", "IDENTICAL@PINNED", "FIRST-PARTY-OK"):
            print(f"  {key}: {verdict} [{detail}]")
    json.dump({k: list(v) for k, v in results.items()},
              open("/tmp/audit_licenses_results.json", "w"), indent=1)
    print("\nfull results: /tmp/audit_licenses_results.json")

    accepted = {"IDENTICAL", "IDENTICAL@PINNED", "FIRST-PARTY-OK",
                "FIRST-PARTY-RETAINED",
                "FIRST-PARTY-NOTICE", "MANUAL-VERIFIED"}
    bad = {k: v for k, v in results.items() if v[0] not in accepted}
    if bad:
        print(f"\nPROVENANCE AUDIT FAILED: {len(bad)} unexplained verdict(s)")
        sys.exit(1)
    print("\nPROVENANCE AUDIT PASSED: every vendored license accounted for")


if __name__ == "__main__":
    main()
