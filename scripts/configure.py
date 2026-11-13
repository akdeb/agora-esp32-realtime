"""Generate local board bootstrap settings. Never embed provider/master keys."""
import argparse
import json
from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser()
parser.add_argument("--backend-url", help="Example: http://192.168.1.5:8080")
args = parser.parse_args()
url = args.backend_url
if not url:
    try:
        info = subprocess.check_output(["ifconfig", "en0"], text=True)
        ip = re.search(r"\binet (\d+\.\d+\.\d+\.\d+)", info).group(1)
        url = f"http://{ip}:8080"
    except Exception:
        raise SystemExit("Pass --backend-url with the computer's LAN URL")
if not re.fullmatch(r"https?://[a-zA-Z0-9.:_-]+(?::\d+)?", url):
    raise SystemExit("Use an http(s) backend origin without a path")
env = {}
for line in (root / "server/.env").read_text().splitlines():
    if "=" in line:
        k, v = line.split("=", 1)
        env[k] = v.strip().strip("\"'")
key = env["DEVICE_KEY"]
if len(key) >= 96 or not re.fullmatch(r"[A-Za-z0-9_-]+", key):
    raise SystemExit("DEVICE_KEY must contain 1-95 letters, digits, dashes or underscores")
out = root / "main/bootstrap.h"
out.write_text("#pragma once\n// Generated local settings; excluded from version control.\n"
               f"#define DEFAULT_BACKEND_URL {json.dumps(url)}\n"
               f"#define DEFAULT_DEVICE_KEY {json.dumps(key)}\n")
out.chmod(0o600)
print(f"Board backend configured: {url}. Device key kept private.")
