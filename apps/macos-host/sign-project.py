"""Sign with the isolated development identity, restoring the user's keychain list."""
import argparse
import shlex
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("--identity", required=True)
parser.add_argument("--keychain", required=True)
parser.add_argument("--app", required=True)
args = parser.parse_args()
if len(args.identity) != 40 or any(char not in "0123456789abcdefABCDEF" for char in args.identity):
    raise SystemExit("Invalid signing certificate fingerprint")
security = "/usr/bin/security"
original = shlex.split(subprocess.check_output([security, "list-keychains", "-d", "user"], text=True))
if not original:
    raise SystemExit("Refusing to replace an empty keychain search list")
changed = args.keychain not in original
try:
    if changed:
        subprocess.run([security, "list-keychains", "-d", "user", "-s", *original, args.keychain], check=True)
    requirement = '=designated => identifier "dev.remote-native.host" and certificate leaf = H"' + args.identity + '"'
    subprocess.run(["/usr/bin/codesign", "--force", "--sign", args.identity,
                    "--keychain", args.keychain, "--identifier", "dev.remote-native.host",
                    "--requirements", requirement, args.app], check=True, timeout=30)
    subprocess.run(["/usr/bin/codesign", "--verify", "--strict", args.app], check=True, timeout=15)
finally:
    if changed:
        subprocess.run([security, "list-keychains", "-d", "user", "-s", *original], check=True)
