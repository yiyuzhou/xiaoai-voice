"""Build deployment files and manage an LX06 over authenticated OpenSSH."""

import argparse
import configparser
import getpass
import io
from ipaddress import IPv4Address
from pathlib import Path
import re
import shlex
import subprocess
import tarfile
import tempfile

from speaker import ROOT, prepare_assets

APP = "/data/xiaoai-plus"
SOURCE = ROOT / "xiaoai-plus"
BINARY = SOURCE / "build-armv7" / "xiaoai_plus_speaker"
KEYWORDS = Path("keywords/keywords_doubao_repeat_end.txt")
ASSETS = ("ca-certificates.crt", "encoder.onnx", "decoder.onnx",
          "joiner.onnx", "tokens.txt", "en.phone")


def ssh(args, script, data=None, timeout=30):
    command = [
        "ssh", "-T", "-p", str(args.port), "-i", str(args.key),
        "-o", "BatchMode=yes", "-o", "ConnectTimeout=10",
        "-o", "IdentitiesOnly=yes", "-o", "StrictHostKeyChecking=yes",
    ]
    if args.legacy_rsa:
        command += ["-o", "HostKeyAlgorithms=+ssh-rsa",
                    "-o", "PubkeyAcceptedAlgorithms=+ssh-rsa"]
    command += ["root@" + str(args.host), "sh -c " + shlex.quote(script)]
    result = subprocess.run(command, input=data, timeout=timeout, check=False)
    if result.returncode:
        raise RuntimeError(f"SSH or device check failed (exit {result.returncode}).")


def model_check(args):
    return (
        "set -eu; "
        "grep -Eiq '(^|[^[:alnum:]_])LX06([^[:alnum:]_]|$)' /usr/share/mico/version || "
        "{ echo 'Expected LX06 in /usr/share/mico/version' >&2; exit 1; }; "
        f"grep -Eq '(^|[^0-9.]){re.escape(args.firmware)}([^0-9.]|$)' /usr/share/mico/version || "
        "{ echo 'Speaker firmware differs from --firmware' >&2; exit 1; }; "
    )


def probe(args):
    log = shlex.quote(args.native_log)
    ssh(args, model_check(args) + f"""
        command -v arecord
        command -v aplay
        test -r {log} || {{ echo 'Native wake log is not readable' >&2; exit 1; }}
        bytes=$(arecord -q -D noop -t raw -f S16_LE -r 16000 -c 1 \
            --buffer-size=1440 --period-size=360 -d 1 | wc -c)
        test "$bytes" -eq 32000 || {{ echo 'Expected one second of mono S16 PCM' >&2; exit 1; }}
        aplay -q -D default -t raw -f S16_LE -r 24000 -c 1 -d 1 /dev/zero
        df -Pk /data
        echo 'LX06 PCM capture/playback checks passed'
    """)


def make_config(native_log, key):
    if not re.fullmatch(r"[!-~]+", key):
        raise RuntimeError("Enter a single non-empty API Key.")
    config = configparser.ConfigParser(interpolation=None)
    config.read(SOURCE / "config.ini.example", encoding="utf-8")
    config["realtime"]["api_key"] = key
    config["audio"].update(speaker_profile="lx06", native_wake_log=native_log)
    config["conversation"]["duplex_mode"] = "half"
    config["memory"]["enabled"] = "off"
    buffer = io.StringIO()
    config.write(buffer)
    return buffer.getvalue().encode("utf-8")


def probe_wake(args):
    log = shlex.quote(args.native_log)
    print('Say "小爱同学" to the speaker after READY appears.', flush=True)
    ssh(args, model_check(args) + rf"""
        test -r {log}
        offset=$(wc -c < {log})
        echo READY
        for attempt in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
            size=$(wc -c < {log})
            if [ "$size" -lt "$offset" ]; then offset=0; fi
            if tail -c "+$((offset + 1))" {log} |
                grep -Eq 'mipns-xiaomi\[.*send real_wakeup event|wakeup\.sh: (asrstart|WuW(_oneshot|_first(_unregister)?)?)([[:space:]]|$)'; then
                echo 'Native wake event detected'
                exit 0
            fi
            sleep 1
        done
        echo 'No supported native wake event appeared' >&2
        exit 1
    """)


def deployment_archive(binary, assets, config):
    files = {
        "xiaoai_plus_speaker": (binary.read_bytes(), 0o700),
        "boot.sh": ((SOURCE / "boot.sh").read_bytes().replace(b"\r\n", b"\n"), 0o700),
        KEYWORDS.as_posix(): ((SOURCE / KEYWORDS).read_bytes(), 0o600),
        "config.ini": (config, 0o600),
    }
    files.update({f"assets/{name}": ((assets / name).read_bytes(), 0o600) for name in ASSETS})
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w", format=tarfile.USTAR_FORMAT) as archive:
        for path, (data, mode) in files.items():
            item = tarfile.TarInfo(path)
            item.size = len(data)
            item.mode = mode
            archive.addfile(item, io.BytesIO(data))
    return buffer.getvalue()


def install(args):
    if not args.binary.is_file():
        raise RuntimeError("Build the speaker binary with setup/build-lx06.ps1 first.")
    probe(args)
    ssh(args, f"test ! -e {APP} && test ! -L {APP} || "
              "{ echo 'An installation already exists' >&2; exit 1; }")
    with tempfile.TemporaryDirectory(prefix="xiaoai-lx06-") as directory:
        stage = Path(directory)
        prepare_assets(stage)
        config = make_config(args.native_log, getpass.getpass("Volcengine O2 API Key: "))
        archive = deployment_archive(args.binary, stage / "assets", config)

    minimum_kib = (len(archive) + 1023) // 1024 + 4096
    asset_paths = " ".join(f'"$stage/assets/{name}"' for name in ASSETS)
    ssh(args, model_check(args) + f"""
        umask 077
        test "$(df -Pk /data | awk 'END {{ print $4 }}')" -ge {minimum_kib}
        mkdir {APP}
        stage={APP}
        cleanup() {{
            cd /data
            rm -f "$stage/config.ini" "$stage/boot.sh" "$stage/xiaoai_plus_speaker" \
                "$stage/{KEYWORDS.as_posix()}" {asset_paths}
            rmdir "$stage/assets" "$stage/keywords" "$stage" 2>/dev/null || true
        }}
        trap cleanup EXIT
        mkdir "$stage/assets" "$stage/keywords"
        tar -xf - -C "$stage"
        cd "$stage"
        chmod 600 config.ini
        ./xiaoai_plus_speaker --check-config -c config.ini
        trap - EXIT
    """, archive, timeout=120)
    print("LX06 installed. Run the start command to launch Doubao.")


def start(args):
    ssh(args, model_check(args) + f"/bin/sh {APP}/boot.sh", timeout=90)


def status(args):
    ssh(args, model_check(args) + f"""
        if [ -r {APP}/xiaoai_plus.log ]; then
            grep -E 'app components started|kws keywords:|native XiaoAi wake|recorder.*failed' \
                {APP}/xiaoai_plus.log | tail -n 12
        fi
        pidof xiaoai_plus_speaker || {{ echo 'Doubao is not running' >&2; exit 1; }}
    """)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("probe", "probe-wake", "install", "start", "status"))
    parser.add_argument("--host", required=True, type=IPv4Address)
    parser.add_argument("--firmware", default="1.94.13",
                        help="Exact version in Mi Home (default: 1.94.13)")
    parser.add_argument("--key", required=True, type=Path, help="SSH private key path")
    parser.add_argument("--port", type=int, default=22)
    parser.add_argument("--legacy-rsa", action="store_true",
                        help="Enable ssh-rsa for an older Dropbear on this connection")
    parser.add_argument("--native-log", default="/var/log/messages")
    parser.add_argument("--binary", type=Path, default=BINARY)
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+){2}", args.firmware):
        parser.error("Expected a firmware version such as 1.94.13.")
    if not re.fullmatch(r"/[A-Za-z0-9_./-]+", args.native_log):
        parser.error("Expected an absolute device log path.")
    if not args.key.is_file() or not 1 <= args.port <= 65535:
        parser.error("Expected an existing SSH key and a valid port.")
    try:
        actions = {"probe": probe, "probe-wake": probe_wake, "install": install,
                   "start": start, "status": status}
        actions[args.action](args)
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as exc:
        parser.exit(1, f"{exc}\n")


if __name__ == "__main__":
    main()
