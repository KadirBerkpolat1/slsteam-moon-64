#!/usr/bin/env python3
"""Unpack a registry image's linux/amd64 root filesystem without a daemon.

Used by scripts/build-portable-container.sh on hosts that have bubblewrap but
no podman/docker. Pulls from registry-1.docker.io (or IMAGE's own registry
when it is fully qualified), verifies every layer against its digest, and
applies layers in order with OCI whiteouts. Device nodes and ownership are not
reproduced: the tree is only ever entered through a user namespace with /dev
and /proc supplied by bwrap.

Usage: _oci-rootfs.py IMAGE[:TAG|@DIGEST] DEST
Prints the resolved manifest digest on stdout.
"""

import hashlib
import json
import os
import shutil
import sys
import tarfile
import tempfile
import urllib.parse
import urllib.request

INDEX_TYPES = (
    "application/vnd.oci.image.index.v1+json",
    "application/vnd.docker.distribution.manifest.list.v2+json",
)
MANIFEST_TYPES = (
    "application/vnd.oci.image.manifest.v1+json",
    "application/vnd.docker.distribution.manifest.v2+json",
)


def parse_ref(ref):
    registry = "registry-1.docker.io"
    first, _, rest = ref.partition("/")
    if rest and ("." in first or ":" in first or first == "localhost"):
        registry, ref = first, rest
    if registry in ("docker.io", "index.docker.io"):
        registry = "registry-1.docker.io"
    if registry == "registry-1.docker.io" and "/" not in ref:
        ref = "library/" + ref
    if "@" in ref:
        repo, _, tag = ref.partition("@")
    else:
        repo, _, tag = ref.rpartition(":") if ":" in ref.split("/")[-1] else (ref, "", "latest")
    return registry, repo, tag


class Registry:
    def __init__(self, registry, repo):
        self.base = f"https://{registry}/v2/{repo}"
        self.repo = repo
        self.token = None

    def _open(self, url, accept=None):
        headers = {}
        if accept:
            headers["Accept"] = ", ".join(accept)
        if self.token:
            headers["Authorization"] = f"Bearer {self.token}"
        try:
            return urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=120)
        except urllib.error.HTTPError as e:
            challenge = e.headers.get("WWW-Authenticate", "")
            if e.code != 401 or self.token or not challenge.startswith("Bearer "):
                raise
            params = dict(
                part.split("=", 1) for part in challenge[len("Bearer "):].replace('"', "").split(",")
            )
            realm = params.pop("realm")
            params.setdefault("scope", f"repository:{self.repo}:pull")
            with urllib.request.urlopen(f"{realm}?{urllib.parse.urlencode(params)}", timeout=60) as r:
                body = json.load(r)
            self.token = body.get("token") or body["access_token"]
            return self._open(url, accept)

    def manifest(self, ref):
        with self._open(f"{self.base}/manifests/{ref}", INDEX_TYPES + MANIFEST_TYPES) as r:
            raw = r.read()
            return json.loads(raw), r.headers.get("Docker-Content-Digest") or "sha256:" + hashlib.sha256(raw).hexdigest()

    def blob(self, digest, out):
        algo, _, want = digest.partition(":")
        if algo != "sha256":
            sys.exit(f"unsupported digest algorithm: {digest}")
        h = hashlib.sha256()
        with self._open(f"{self.base}/blobs/{digest}") as r, open(out, "wb") as f:
            while chunk := r.read(1 << 20):
                h.update(chunk)
                f.write(chunk)
        if h.hexdigest() != want:
            sys.exit(f"layer digest mismatch: expected {digest}, got sha256:{h.hexdigest()}")


def remove(path):
    if os.path.islink(path) or not os.path.isdir(path):
        if os.path.lexists(path):
            os.unlink(path)
    else:
        shutil.rmtree(path)


def inside(dest, name):
    path = os.path.normpath(os.path.join(dest, name))
    if path != dest and not path.startswith(dest + os.sep):
        sys.exit(f"layer member escapes the root: {name}")
    return path


def apply_layer(layer, dest):
    with tarfile.open(layer) as tar:
        members = []
        for m in tar:
            parent, base = os.path.split(m.name)
            if base == ".wh..wh..opq":
                d = inside(dest, parent)
                if os.path.isdir(d):
                    for child in os.listdir(d):
                        remove(os.path.join(d, child))
            elif base.startswith(".wh."):
                remove(inside(dest, os.path.join(parent, base[4:])))
            elif m.ischr() or m.isblk() or m.isfifo():
                continue
            else:
                inside(dest, m.name)
                target = os.path.join(dest, m.name)
                if os.path.lexists(target) and not (m.isdir() and os.path.isdir(target) and not os.path.islink(target)):
                    remove(target)
                m.mode |= 0o200 if not m.isdir() else 0o700
                m.uid = m.gid = os.getuid()
                m.uname = m.gname = ""
                members.append(m)
        # Only member *names* are confined above (symlinked parents are not
        # re-checked); the trust boundary is the digest-verified registry pull.
        # The stdlib "data" filter would reject the absolute symlinks every
        # distro rootfs contains, so it cannot be used here.
        extra = {"filter": "fully_trusted"} if hasattr(tarfile, "fully_trusted_filter") else {}
        tar.extractall(dest, members=members, numeric_owner=True, **extra)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip().splitlines()[-2])
    registry, repo, ref = parse_ref(sys.argv[1])
    dest = os.path.abspath(sys.argv[2])
    reg = Registry(registry, repo)
    manifest, digest = reg.manifest(ref)
    if manifest.get("mediaType") in INDEX_TYPES or "manifests" in manifest:
        picks = [
            m for m in manifest["manifests"]
            if m.get("platform", {}).get("os") == "linux"
            and m.get("platform", {}).get("architecture") == "amd64"
        ]
        if not picks:
            sys.exit(f"{sys.argv[1]} has no linux/amd64 manifest")
        digest = picks[0]["digest"]
        manifest, _ = reg.manifest(digest)
    os.makedirs(dest, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        for i, layer in enumerate(manifest["layers"]):
            path = os.path.join(tmp, f"layer{i}")
            reg.blob(layer["digest"], path)
            apply_layer(path, dest)
            os.unlink(path)
    print(digest)


if __name__ == "__main__":
    main()
