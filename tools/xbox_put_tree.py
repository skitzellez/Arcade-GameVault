# Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader, GPL-3.0.
# xbox_put_tree.py <local folder> <LocalState sub-path> [sub-folders...]: copies a game's folders into the arcade
# loader test app's LocalState on the console through the Device Portal (folders made first; a file already there
# with the same size is skipped, so a stopped copy can be run again).
import json, os, sys, urllib.parse
import sys as _sys
_sys.stdout.reconfigure(errors="replace")

sys.path.insert(0, r"C:\Users\GamerDad\XboxEmu\XemuUWP\tools")
os.environ.setdefault("XB_FAMILY", "GamerDad.ArcadeLoaderTest")
import xb  # noqa: E402

local, remote = sys.argv[1], sys.argv[2].strip("/\\")
subs = sys.argv[3:] or [""]
pkg = xb.newest()["PackageFullName"]
tok = xb.token()


# XB_KNOWN=DevelopmentFiles: into the console's D:\DevelopmentFiles (where Game Vault's search finds games) instead
DEV = os.environ.get("XB_KNOWN") == "DevelopmentFiles"
WHERE = "knownfolderid=DevelopmentFiles" if DEV else "knownfolderid=LocalAppData&packagefullname=" + pkg


def q(path):
    return (("/" if DEV else "/LocalState/") + path).replace("/", "%5C")


def listing(path):
    try:
        d = json.loads(xb.curl(xb.B + "/api/filesystem/apps/files?" + WHERE +
                               "&path=" + q(path)))
    except Exception:
        return None
    return {i["Name"]: i.get("FileSize", 0) for i in d.get("Items", [])}


def mkdir(parent, name):
    xb.curl("-X", "POST", "-H", "X-CSRF-Token: " + tok, "-H", "Content-Length: 0",
            xb.B + "/api/filesystem/apps/folder?" + WHERE + "&path=" +
            q(parent) + "&newfoldername=" + urllib.parse.quote(name))


def ensure(path):
    parts = [p for p in path.split("/") if p]
    for i in range(len(parts)):
        parent, name = "/".join(parts[:i]), parts[i]
        have = listing(parent) or {}
        if name not in have:
            mkdir(parent, name)


total = sent = 0
for sub in subs:
    top = os.path.join(local, sub)
    for root, dirs, files in os.walk(top):
        rel = os.path.relpath(root, local).replace("\\", "/")
        rpath = remote + ("" if rel == "." else "/" + rel)
        ensure(rpath)
        have = listing(rpath) or {}
        for f in files:
            src = os.path.join(root, f)
            size = os.path.getsize(src)
            total += size
            if have.get(f) == size:
                continue
            r = xb.curl("-H", "X-CSRF-Token: " + tok, "-H", "Expect:", "-F", 'f=@"' + src + '"',
                        xb.B + "/api/filesystem/apps/file?" + WHERE +
                        "&path=" + q(rpath), "-w", " http %{http_code}", timeout=7200)
            sent += size
            print("%s/%s %d MB: %s" % (rpath, f, size >> 20, r.strip()[-8:]), flush=True)
print("done: %d MB in the tree, %d MB sent" % (total >> 20, sent >> 20))
