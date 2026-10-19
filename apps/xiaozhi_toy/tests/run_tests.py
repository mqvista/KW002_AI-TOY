"""Run native regression checks without a device or the embedded toolchain.

Uses the TuyaOpen submodule's cJSON source for MCP request/reply checks.

Usage: python run_tests.py --cc /path/to/gcc
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    args = ap.parse_args()
    cc = shutil.which(args.cc)
    if not cc:
        ap.error("native GCC not found; supply --cc")
    here = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix="kw002-power-test-") as tmp:
        exe = Path(tmp) / ("standby.exe" if os.name == "nt" else "standby")
        env = os.environ.copy()
        env["PATH"] = str(Path(cc).parent) + os.pathsep + env.get("PATH", "")
        subprocess.run([cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                        "-I" + str(here / "stubs"), "-I" + str(here.parent / "src"),
                        str(here / "test_standby.c"), "-o", str(exe)], check=True, env=env)
        subprocess.run([str(exe)], check=True, env=env)
        cjson = here.parents[2] / "TuyaOpen" / "src" / "libcjson" / "cJSON"
        if not (cjson / "cJSON.c").is_file():
            ap.error("TuyaOpen submodule is required for the MCP JSON regression checks")
        network_exe = Path(tmp) / ("network.exe" if os.name == "nt" else "network")
        subprocess.run([cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-DCJSON_HIDE_SYMBOLS",
                        '-DPROJECT_VERSION="test"', "-I" + str(here / "stubs"),
                        "-I" + str(here.parent / "src"), "-I" + str(cjson),
                        str(here / "test_network.c"), str(here.parent / "src" / "xz_mcp.c"),
                        str(here.parent / "src" / "xz_public_ip.c"),
                        str(here.parent / "src" / "xz_certs.c"),
                        str(cjson / "cJSON.c"), "-lm", "-o", str(network_exe)], check=True, env=env)
        subprocess.run([str(network_exe)], check=True, env=env)


if __name__ == "__main__":
    main()
