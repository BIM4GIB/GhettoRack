#!/usr/bin/env python3
"""HTTPS on the boxes without a browser warning, on your own devices.

    tools/boxcert.py ca                 once: your own certificate authority, kept in ~/.ghettorack-ca
    tools/boxcert.py box ghettopi       a certificate for that box (an ssh host), signed by it and put on the box
    tools/boxcert.py box ghettopi --restart     ... and restart its bridge (stops the sound for some seconds)

The authority's key never leaves this computer and never goes into the repo. It can only
sign names ending in .local and the hotspot's address, so a stolen key is no use against
any other site. Each device has to trust ca.crt once (printed after "ca"). A box
certificate lasts 800 days (Apple devices refuse longer ones): run "box" again then.
Needs openssl and, for "box", ssh with sudo on the box."""
import argparse, os, re, subprocess, sys, tempfile

HOTSPOT = "10.42.0.1"
STATE = "/var/lib/apcbox-bridge"


def run(*cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode:
        sys.exit("%s failed:\n%s" % (cmd[0], (r.stderr or r.stdout).strip()[-600:]))
    return r.stdout


def make_ca(d):
    key, crt = os.path.join(d, "ca.key"), os.path.join(d, "ca.crt")
    if os.path.exists(key) or os.path.exists(crt):
        sys.exit("there is an authority in %s already. Every device trusts that one: keep it, or remove the folder by hand to start over." % d)
    os.makedirs(d, mode=0o700, exist_ok=True)
    with tempfile.TemporaryDirectory() as t:
        cnf = os.path.join(t, "ca.cnf")
        with open(cnf, "w") as f:
            f.write("[req]\ndistinguished_name=dn\nx509_extensions=v3\nprompt=no\n[dn]\nCN=GhettoRack boxes (%s)\n"
                    "[v3]\nbasicConstraints=critical,CA:TRUE,pathlen:0\nkeyUsage=critical,keyCertSign,cRLSign\n"
                    "subjectKeyIdentifier=hash\nnameConstraints=critical,permitted;DNS:local,permitted;IP:%s/255.255.255.255\n"
                    % (os.environ.get("USER", "owner"), HOTSPOT))
        old = os.umask(0o077)
        try:
            run("openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes", "-days", "3650",
                "-config", cnf, "-keyout", key, "-out", crt)
        finally:
            os.umask(old)
    os.chmod(key, 0o600)
    os.chmod(crt, 0o644)
    print("made %s (10 years). Trust it once on each of your devices:\n" % crt)
    print("  this Mac:   security add-trusted-cert -r trustRoot -k ~/Library/Keychains/login.keychain-db %s" % crt)
    print("              (asks for your password; Firefox has its own list: Settings > Certificates > Import)")
    print("  iPhone/iPad: after \"box\", open http://<box>.local:8040/ca.crt in Safari, allow the profile, install it in Settings,")
    print("              then Settings > General > About > Certificate Trust Settings: switch it on")
    print("  Android:    open the same address, then Settings > Security > Install a certificate > CA certificate")


def make_box(d, host, name, restart):
    key, crt = os.path.join(d, "ca.key"), os.path.join(d, "ca.crt")
    if not (os.path.exists(key) and os.path.exists(crt)):
        sys.exit("no authority yet: run  tools/boxcert.py ca  first")
    name = name or run("ssh", host, "hostname").strip()
    if not re.fullmatch(r"[a-z0-9]([a-z0-9-]{0,30}[a-z0-9])?", name):
        sys.exit("the box's name %r can't go into a certificate" % name)
    with tempfile.TemporaryDirectory() as t:
        os.chmod(t, 0o700)
        bkey, csr, bcrt, ext = (os.path.join(t, n) for n in ("own.key", "own.csr", "own.crt", "ext.cnf"))
        with open(ext, "w") as f:
            f.write("basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=serverAuth\n"
                    "subjectAltName=DNS:%s.local,IP:%s\n" % (name, HOTSPOT))
        old = os.umask(0o077)
        try:
            run("openssl", "req", "-new", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes",
                "-subj", "/CN=%s.local" % name, "-keyout", bkey, "-out", csr)
            run("openssl", "x509", "-req", "-in", csr, "-CA", crt, "-CAkey", key, "-CAcreateserial", "-days", "800",
                "-sha256", "-extfile", ext, "-out", bcrt)
        finally:
            os.umask(old)
        run("openssl", "verify", "-CAfile", crt, bcrt)
        # onto the box: through a folder only this login reads, then into the bridge's own, with its owner
        tmp = run("ssh", host, "mktemp -d").strip()
        if not re.fullmatch(r"/tmp/[A-Za-z0-9._-]+", tmp):
            sys.exit("the box gave no temporary folder")
        run("scp", "-q", bkey, bcrt, crt, "%s:%s/" % (host, tmp))
        run("ssh", host, "set -e; d=%s; sudo test -d $d; o=$(sudo stat -L -c %%u:%%g $d/); "
            "sudo install -m 600 %s/own.key $d/own.key; sudo install -m 644 %s/own.crt $d/own.crt; "
            "sudo install -m 644 %s/ca.crt $d/own-ca.crt; sudo chown $o $d/own.key $d/own.crt $d/own-ca.crt; rm -rf %s"
            % (STATE, tmp, tmp, tmp, tmp))
        if restart:
            run("ssh", host, "sudo systemctl restart apcbox-bridge")
    print("%s has a certificate for https://%s.local:8443 and https://%s:8443 (800 days)." % (host, name, HOTSPOT))
    print("the bridge takes it at its next start" + ("." if not restart else ": restarted.")
          + ("" if restart else "  (ssh %s sudo systemctl restart apcbox-bridge, when nothing is playing)" % host))
    print("for a phone: http://%s.local:8040/ca.crt" % name)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("what", choices=("ca", "box"))
    ap.add_argument("host", nargs="?", help="box: the ssh host")
    ap.add_argument("--dir", default=os.path.expanduser("~/.ghettorack-ca"), help="where the authority lives (never inside the repo)")
    ap.add_argument("--name", help="box: its name, if not what `hostname` says there")
    ap.add_argument("--restart", action="store_true", help="box: restart the bridge so it takes the certificate now")
    a = ap.parse_args()
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if os.path.abspath(a.dir).startswith(here + os.sep):
        sys.exit("not inside the repo: it is public")
    if a.what == "ca":
        make_ca(a.dir)
    elif not a.host:
        sys.exit("box: which ssh host?")
    else:
        make_box(a.dir, a.host, a.name, a.restart)


if __name__ == "__main__":
    main()
