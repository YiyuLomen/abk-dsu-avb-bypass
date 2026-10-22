#!/usr/bin/env python3
"""
ABK external module: first-stage orange bootconfig view.

Patches a kernel tree so that /proc/bootconfig reports
androidboot.verifiedbootstate="orange" to PID 1 during first-stage init,
and closes that view as soon as PID 1 execs /system/bin/init.

Stages (ABK CUSTOM_EXTERNAL_MODULE_STAGE):
  after_patch  - install payload files, patch bootconfig.c/Kconfig/Makefile,
                 enable CONFIG_ANDROID_DSU_AVB_BYPASS in DEFCONFIG
  before_build - read-only audit; fails when the patch is incomplete
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import sys
from pathlib import Path

MARKER = "android_dsu_avb_bootconfig_active"
CONFIG_SYMBOL = "ANDROID_DSU_AVB_BYPASS"
VIEW_SYMBOL = "ANDROID_DSU_AVB_VBMETA_VIEW"

HOOK_LINES = (
    "\n\tif (android_dsu_avb_bootconfig_active())\n"
    '\t\tseq_puts(m, "androidboot.verifiedbootstate = \\"orange\\"\\n");\n'
)

KCONFIG_BLOCK = """
config ANDROID_DSU_AVB_BYPASS
\tbool "Android first-stage orange bootconfig view"
\tdepends on BOOT_CONFIG && PROC_FS && TRACEPOINTS
\tdefault n
\thelp
\t  Present an orange verified-boot state to PID 1 when it reads
\t  /proc/bootconfig, until its first exec of /system/bin/init.
\t  This lets first-stage AVB handling accept verification errors.
\t  The helper does not change vbmeta contents or SELinux enforcement.
\t  It applies to normal boots as well as DSU boots.

config ANDROID_DSU_AVB_VBMETA_VIEW
\tbool "Android first-stage vbmeta read-view override"
\tdepends on BOOT_CONFIG && PROC_FS && TRACEPOINTS && KPROBES && BLOCK
\tdefault n
\thelp
\t  Rewrite AVB headers read by PID 1 during first-stage init so they
\t  report VERIFICATION_DISABLED, without touching the vbmeta partition
\t  on disk.  The view is torn down when PID 1 execs /system/bin/init.
\t  Nothing is written to storage and SELinux is not affected.

"""


def log(msg: str) -> None:
    print(f"[ABK module] {msg}", flush=True)


def warn(msg: str) -> None:
    print(f"[ABK module][warn] {msg}", file=sys.stderr, flush=True)


def die(msg: str) -> "NoReturn":  # noqa: F821
    print(f"[ABK module][error] {msg}", file=sys.stderr, flush=True)
    raise SystemExit(1)


# Kernel sources are not guaranteed to be valid UTF-8; surrogateescape keeps
# every byte intact across a read/modify/write cycle.  newline="" disables
# newline translation so CRLF files are not silently rewritten.
_IO_KWARGS = {"encoding": "utf-8", "errors": "surrogateescape", "newline": ""}


def read_text(path: Path) -> str:
    with open(path, "r", **_IO_KWARGS) as handle:
        return handle.read()


def write_text(path: Path, text: str) -> None:
    tmp = path.with_name(path.name + ".abktmp")
    with open(tmp, "w", **_IO_KWARGS) as handle:
        handle.write(text)
    os.replace(tmp, path)


def kernel_tree(root: Path) -> Path:
    for candidate in (root / "common", root):
        if (candidate / "fs/proc/bootconfig.c").is_file():
            return candidate
    die(f"cannot locate fs/proc/bootconfig.c under {root} (nor {root / 'common'})")


def kernel_version(tree: Path) -> str:
    values = {}
    makefile = tree / "Makefile"
    if not makefile.is_file():
        return "unknown"
    for line in makefile.read_text(errors="replace").splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[1] == "=" and parts[0] in (
            "VERSION", "PATCHLEVEL", "SUBLEVEL"
        ):
            values[parts[0]] = parts[2]
    if len(values) == 3:
        return "{VERSION}.{PATCHLEVEL}.{SUBLEVEL}".format(**values)
    return "unknown"


def patch_bootconfig(path: Path) -> bool:
    text = read_text(path)
    if MARKER in text:
        log("bootconfig hook already present; skipping")
        return False

    if "#include <linux/bootconfig.h>" not in text:
        die(f"cannot find '#include <linux/bootconfig.h>' in {path}")

    text = text.replace(
        "#include <linux/bootconfig.h>",
        "#include <linux/bootconfig.h>\n#include <linux/android_dsu_avb.h>",
        1,
    )

    sig = re.search(
        r"static\s+int\s+boot_config_proc_show\s*\(\s*struct\s+seq_file\s*\*\s*m\s*,"
        r"\s*void\s*\*\s*v\s*\)\s*\{",
        text,
    )
    if not sig:
        # fall back to locating the brace after the symbol
        pos = text.find("boot_config_proc_show")
        if pos < 0:
            die(f"cannot find boot_config_proc_show() in {path}")
        brace = text.find("{", pos)
        if brace < 0:
            die(f"cannot find opening brace of boot_config_proc_show() in {path}")
        insert_at = brace + 1
    else:
        insert_at = sig.end()

    text = text[:insert_at] + HOOK_LINES + text[insert_at:]
    write_text(path, text)
    log(f"patched {path}")
    return True


def patch_kconfig(path: Path) -> None:
    text = read_text(path)
    if f"config {CONFIG_SYMBOL}" in text:
        log("Kconfig option already present; skipping")
        return

    matches = list(re.finditer(r"^endmenu\s*$", text, re.M))
    if len(matches) != 1:
        die(f"unexpected {path} layout: endmenu count={len(matches)}")

    at = matches[0].start()
    text = text[:at] + KCONFIG_BLOCK + text[at:]
    write_text(path, text)
    log(f"patched {path}")


def patch_makefile(path: Path) -> None:
    text = read_text(path)
    added = []
    for symbol, obj in ((CONFIG_SYMBOL, "dsu_avb.o"),
                        (VIEW_SYMBOL, "dsu_avb_view.o")):
        if symbol in text:
            continue
        if not text.endswith("\n"):
            text += "\n"
        text += f"obj-$(CONFIG_{symbol})\t+= {obj}\n"
        added.append(symbol)
    if not added:
        log("Makefile entry already present; skipping")
        return
    write_text(path, text)
    log(f"patched {path} ({', '.join(added)})")


def set_config(defconfig: Path, symbol: str = CONFIG_SYMBOL) -> None:
    if not defconfig.is_file():
        die(f"DEFCONFIG not found: {defconfig}")
    lines = read_text(defconfig).splitlines()
    pattern = re.compile(rf"^(CONFIG_{symbol}=|# CONFIG_{symbol} is not set$)")
    kept = [line for line in lines if not pattern.match(line)]
    kept.append(f"CONFIG_{symbol}=y")
    write_text(defconfig, "\n".join(kept) + "\n")
    log(f"set CONFIG_{symbol}=y in {defconfig}")


def install_payloads(module_dir: Path, tree: Path) -> None:
    pairs = [
        (module_dir / "payload" / "android_dsu_avb.h", tree / "include/linux/android_dsu_avb.h"),
        (module_dir / "payload" / "dsu_avb.c", tree / "drivers/android/dsu_avb.c"),
        (module_dir / "payload" / "dsu_avb_view.c", tree / "drivers/android/dsu_avb_view.c"),
    ]
    for src, dst in pairs:
        if not src.is_file():
            die(f"payload missing: {src}")
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, dst)
        log(f"installed {dst}")


def do_apply(module_dir: Path, tree: Path, defconfig: Path) -> None:
    log(f"kernel tree: {tree} (Linux {kernel_version(tree)})")
    install_payloads(module_dir, tree)
    patch_bootconfig(tree / "fs/proc/bootconfig.c")
    patch_kconfig(tree / "drivers/android/Kconfig")
    patch_makefile(tree / "drivers/android/Makefile")
    set_config(defconfig, CONFIG_SYMBOL)
    set_config(defconfig, VIEW_SYMBOL)
    log("after_patch complete")


def do_verify(tree: Path, defconfig: Path, strict: bool) -> None:
    log(f"auditing kernel tree: {tree} (Linux {kernel_version(tree)})")
    checks = []

    bootconfig = tree / "fs/proc/bootconfig.c"
    text = read_text(bootconfig) if bootconfig.is_file() else ""

    checks.append(("header installed",
                   (tree / "include/linux/android_dsu_avb.h").is_file()))
    checks.append(("source installed",
                   (tree / "drivers/android/dsu_avb.c").is_file()))
    checks.append(("bootconfig hook present", MARKER in text))
    checks.append(("hook emits orange", 'verifiedbootstate = \\"orange\\"' in text))
    checks.append(("Kconfig option present",
                   f"config {CONFIG_SYMBOL}" in read_text(tree / "drivers/android/Kconfig")))
    checks.append(("Makefile entry present",
                   CONFIG_SYMBOL in read_text(tree / "drivers/android/Makefile")))
    checks.append(("defconfig enables it",
                   f"CONFIG_{CONFIG_SYMBOL}=y" in read_text(defconfig)))
    checks.append(("view source installed",
                   (tree / "drivers/android/dsu_avb_view.c").is_file()))
    checks.append(("Kconfig view option present",
                   f"config {VIEW_SYMBOL}" in read_text(tree / "drivers/android/Kconfig")))
    checks.append(("Makefile view entry present",
                   VIEW_SYMBOL in read_text(tree / "drivers/android/Makefile")))
    checks.append(("defconfig enables view",
                   f"CONFIG_{VIEW_SYMBOL}=y" in read_text(defconfig)))
    checks.append(("view patches AVB flags",
                   "AVB_FLAGS_OFFSET 120" in
                   read_text(tree / "drivers/android/dsu_avb_view.c")))

    failed = 0
    for name, ok in checks:
        if ok:
            log(f"ok   {name}")
        else:
            warn(f"FAIL {name}")
            failed += 1

    if failed:
        if strict:
            die("first-stage orange bootconfig patch is incomplete")
        warn(f"{failed} check(s) failed, continuing because strict=0")
    else:
        log("audit passed")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--stage", required=True,
                        choices=("after_patch", "before_build"))
    parser.add_argument("--kernel-root", default=os.environ.get("KERNEL_ROOT"))
    parser.add_argument("--defconfig", default=os.environ.get("DEFCONFIG"))
    parser.add_argument("--module-dir",
                        default=str(Path(__file__).resolve().parent.parent))
    args = parser.parse_args()

    if not args.kernel_root:
        die("KERNEL_ROOT is empty")
    if not args.defconfig:
        die("DEFCONFIG is empty")

    module_dir = Path(args.module_dir)
    tree = kernel_tree(Path(args.kernel_root))

    defconfig = Path(args.defconfig)
    if not defconfig.is_file() and not defconfig.is_absolute():
        # ABK may hand us a tree-relative path such as arch/arm64/configs/gki_defconfig
        candidate = tree / args.defconfig
        if candidate.is_file():
            defconfig = candidate

    if args.stage == "after_patch":
        do_apply(module_dir, tree, defconfig)
    else:
        strict = os.environ.get("ABK_DSU_AVB_STRICT", "1") != "0"
        do_verify(tree, defconfig, strict)


if __name__ == "__main__":
    main()
