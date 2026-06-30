Import("env")
import subprocess

try:
    hash = subprocess.check_output(
        ["git", "rev-parse", "--short", "HEAD"],
        cwd=env["PROJECT_DIR"],
        stderr=subprocess.DEVNULL,
    ).decode().strip()
except Exception:
    hash = "dev"

env.Append(CPPDEFINES=[("FIRMWARE_VERSION", f'\\"{hash}\\"')])
