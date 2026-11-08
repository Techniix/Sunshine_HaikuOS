#!/usr/bin/env python3
"""Run pkgman through a local curl relay, for systems whose Network Kit has no HTTPS.

Some Haiku builds (typically self-built ones without the `openssl` build feature) can't make
HTTPS requests from the Network Kit, so `pkgman` and HaikuDepot fail with "Operation not
supported", even though `curl` (which uses OpenSSL directly) works.

This tool works around that without changing how pkgman resolves packages:

1. It starts a small HTTP server on 127.0.0.1 that fetches every requested file from the real
   HTTPS server with curl (including HTTP range requests, which pkgman uses to resume downloads).
2. It temporarily points each HTTPS repository's `baseurl` at that server. pkgman builds all its
   URLs from `baseurl`: `<baseurl>/repo.sha256`, `<baseurl>/repo`, `<baseurl>/packages/<file>`.
3. It runs pkgman with the given arguments, then always restores the original settings.

Repository identity, checksums and package signatures are unchanged: pkgman still verifies the
repository checksum and every package checksum against the repository data.

The original settings are saved next to each file as `<name>.curl-relay-backup` before anything is
changed. If the tool is interrupted in a way that prevents cleanup (for example `kill -9` or a power
loss), the next run restores them first, and `--restore` restores them without doing anything else.

Usage:
    pkgman-curl-relay.py install -y cmd:cmake devel:libssl
    pkgman-curl-relay.py refresh
    pkgman-curl-relay.py --restore
"""

import http.server
import os
import re
import signal
import subprocess
import sys
import tempfile
import threading

BACKUP_SUFFIX = ".curl-relay-backup"
CHUNK_SIZE = 1024 * 1024

# Repository settings directories, as BPackageRoster finds them
# (B_SYSTEM_SETTINGS_DIRECTORY and B_USER_SETTINGS_DIRECTORY + "/package-repositories").
# Overridable for testing.
CONFIG_DIRS = os.environ.get(
    "PKGMAN_RELAY_CONFIG_DIRS",
    "/boot/system/settings/package-repositories:/boot/home/config/settings/package-repositories",
).split(":")

PKGMAN = os.environ.get("PKGMAN_RELAY_PKGMAN", "pkgman")

# One driver_settings parameter per line: `key=value` or `key value`, optionally quoted.
PARAMETER_RE = re.compile(r'^(?P<indent>\s*)(?P<key>[A-Za-z_]+)(?P<sep>\s*=\s*|\s+)(?P<value>"[^"]*"|\S+)(?P<rest>.*)$')


def log(message):
    print(f"[curl-relay] {message}", file=sys.stderr, flush=True)




def config_files():
    for directory in CONFIG_DIRS:
        if not os.path.isdir(directory):
            continue
        for name in sorted(os.listdir(directory)):
            path = os.path.join(directory, name)
            if os.path.isfile(path) and not name.endswith(BACKUP_SUFFIX):
                yield path


def base_url_key(text):
    """Return the key holding the base URL: `baseurl` in version 2 files, `url` in version 1."""
    for line in text.splitlines():
        match = PARAMETER_RE.match(line)
        if match and match.group("key") == "cfgversion":
            try:
                return "baseurl" if int(match.group("value").strip('"')) >= 2 else "url"
            except ValueError:
                break
    return "url"


def rewrite_base_url(text, rewrite):
    """Apply `rewrite(url) -> new url or None` to the base URL line; return (text, old, new)."""
    key = base_url_key(text)
    lines = text.splitlines(keepends=True)
    for index, line in enumerate(lines):
        body = line.rstrip("\r\n")
        match = PARAMETER_RE.match(body)
        if not match or match.group("key") != key:
            continue
        old = match.group("value").strip('"')
        new = rewrite(old)
        if new is None:
            return text, None, None
        ending = line[len(body):]
        lines[index] = f'{match.group("indent")}{key}{match.group("sep")}{new}{match.group("rest")}{ending}'
        return "".join(lines), old, new
    return text, None, None


def write_atomically(path, text):
    temporary = path + ".curl-relay-tmp"
    with open(temporary, "w") as file:
        file.write(text)
        file.flush()
        os.fsync(file.fileno())
    os.replace(temporary, path)


def restore_settings():
    """Restore every repository setting saved by an earlier run. Returns the number restored."""
    restored = 0
    for directory in CONFIG_DIRS:
        if not os.path.isdir(directory):
            continue
        for name in sorted(os.listdir(directory)):
            if not name.endswith(BACKUP_SUFFIX):
                continue
            backup = os.path.join(directory, name)
            original = backup[: -len(BACKUP_SUFFIX)]
            os.replace(backup, original)
            log(f"restored {original}")
            restored += 1
    return restored


def point_repositories_at(relay_base):
    """Point each HTTPS repository at the relay. Returns the list of changed files."""
    changed = []

    def rewrite(url):
        if not url.lower().startswith("https://"):
            return None  # plain HTTP or local repositories work without the relay
        return relay_base + "/" + url[len("https://"):]

    for path in config_files():
        with open(path) as file:
            text = file.read()
        new_text, old, new = rewrite_base_url(text, rewrite)
        if old is None:
            continue
        # Save the original first, so an interruption at any point can be undone.
        backup = path + BACKUP_SUFFIX
        with open(backup, "w") as file:
            file.write(text)
            file.flush()
            os.fsync(file.fileno())
        write_atomically(path, new_text)
        log(f"{os.path.basename(path)}: {old} -> relay")
        changed.append(path)
    return changed




def parse_headers(raw):
    """Parse curl's --dump-header output; with redirects, the last response block counts."""
    blocks = [block for block in re.split(r"\r?\n\r?\n", raw) if block.strip()]
    if not blocks:
        return 0, {}
    lines = blocks[-1].splitlines()
    status_match = re.match(r"HTTP/\S+\s+(\d{3})", lines[0])
    status = int(status_match.group(1)) if status_match else 0
    headers = {}
    for line in lines[1:]:
        if ":" in line:
            name, value = line.split(":", 1)
            headers[name.strip().lower()] = value.strip()
    return status, headers


class RelayHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "pkgman-curl-relay"

    def log_message(self, format, *args):  # noqa: A002 - signature from the base class
        log(format % args)

    def do_GET(self):
        self._relay(send_body=True)

    def do_HEAD(self):
        self._relay(send_body=False)

    def _relay(self, send_body):
        # Path is /<host>/<path> for https://<host>/<path>.
        path = self.path
        if not path.startswith("/") or "://" in path or len(path) < 2:
            self.send_error(400, "Bad relay path")
            return
        upstream = "https:/" + path

        range_header = self.headers.get("Range")
        if range_header is not None and not re.fullmatch(r"bytes=\d*-\d*", range_header.strip()):
            self.send_error(400, "Unsupported range")
            return

        with tempfile.TemporaryDirectory(prefix="pkgman-curl-relay-") as workdir:
            body_path = os.path.join(workdir, "body")
            header_path = os.path.join(workdir, "headers")
            command = ["curl", "--silent", "--show-error", "--location", "--max-redirs", "5",
                       "--connect-timeout", "30", "--retry", "2",
                       "--dump-header", header_path, "--output", body_path]
            if not send_body:
                command.append("--head")
            if range_header is not None:
                command += ["--range", range_header.strip()[len("bytes="):]]
            command.append(upstream)

            result = subprocess.run(command, stderr=subprocess.PIPE, text=True)
            raw_headers = ""
            if os.path.exists(header_path):
                with open(header_path, errors="replace") as file:
                    raw_headers = file.read()
            status, headers = parse_headers(raw_headers)

            if result.returncode != 0 or status == 0:
                log(f"curl failed for {upstream}: {result.stderr.strip() or result.returncode}")
                self.send_error(502, "curl could not fetch the file")
                return

            if status == 200 and range_header is not None:
                # The server ignored the range. pkgman appends what it receives to the partial
                # file, so sending the whole file here would corrupt it; slice it ourselves.
                status, headers = self._apply_range(body_path, range_header, headers)
                if status is None:
                    return

            self.send_response(status)
            size = os.path.getsize(body_path) if os.path.exists(body_path) else 0
            for name in ("content-type", "content-range", "last-modified", "etag"):
                if name in headers:
                    self.send_header(name.title(), headers[name])
            if send_body:
                self.send_header("Content-Length", str(size))
            elif "content-length" in headers:
                self.send_header("Content-Length", headers["content-length"])
            self.send_header("Connection", "close")
            self.end_headers()

            if send_body and size > 0:
                with open(body_path, "rb") as file:
                    while True:
                        chunk = file.read(CHUNK_SIZE)
                        if not chunk:
                            break
                        self.wfile.write(chunk)
            self.close_connection = True

    def _apply_range(self, body_path, range_header, headers):
        total = os.path.getsize(body_path)
        start_text, end_text = range_header.strip()[len("bytes="):].split("-", 1)
        if start_text == "":
            # Suffix range: the last N bytes.
            length = min(int(end_text), total)
            start, end = total - length, total - 1
        else:
            start = int(start_text)
            end = min(int(end_text), total - 1) if end_text else total - 1
        if start >= total or start > end:
            self.send_response(416)
            self.send_header("Content-Range", f"bytes */{total}")
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
            self.close_connection = True
            return None, None
        with open(body_path, "rb") as source, open(body_path + ".range", "wb") as target:
            source.seek(start)
            remaining = end - start + 1
            while remaining > 0:
                chunk = source.read(min(CHUNK_SIZE, remaining))
                if not chunk:
                    break
                target.write(chunk)
                remaining -= len(chunk)
        os.replace(body_path + ".range", body_path)
        headers = dict(headers)
        headers["content-range"] = f"bytes {start}-{end}/{total}"
        return 206, headers


def start_relay():
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), RelayHandler)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, name="relay", daemon=True)
    thread.start()
    return server, f"http://127.0.0.1:{server.server_address[1]}"




def main(argv):
    if argv[:1] in (["-h"], ["--help"]):
        print(__doc__)
        return 0

    # Undo a previous run that could not clean up.
    if restore_settings():
        log("restored repository settings left over from an interrupted run")
    if argv == ["--restore"]:
        return 0

    if subprocess.run(["curl", "--version"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode != 0:
        log("curl is required but not available")
        return 1

    if argv[:1] == ["--serve"]:
        # Relay only, for testing: prints the base URL and runs until interrupted.
        server, base = start_relay()
        print(base, flush=True)
        try:
            threading.Event().wait()
        except KeyboardInterrupt:
            pass
        server.shutdown()
        return 0

    if not argv:
        print(__doc__)
        return 2

    server, base = start_relay()
    log(f"relay listening on {base}")

    # Restore the settings however this process ends (except SIGKILL, see the module docs).
    def terminate(signum, frame):
        raise KeyboardInterrupt

    for signum in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(signum, terminate)

    status = 1
    try:
        changed = point_repositories_at(base)
        if not changed:
            log("no HTTPS repositories found; running pkgman directly")
        status = subprocess.run([PKGMAN] + argv).returncode
    except KeyboardInterrupt:
        log("interrupted")
        status = 130
    finally:
        restore_settings()
        server.shutdown()
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
