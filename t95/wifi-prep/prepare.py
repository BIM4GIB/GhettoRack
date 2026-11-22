#!/usr/bin/env python3
"""Prepare an experimental Wi-Fi-enabled T95 SD image. Never writes a device."""
import argparse
import getpass
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

EXPECTED = 'a2af7a34571a5dbf6b3f6f9ad093d5c9af94fe5e98af364bd05c8a354889efa7'
SIZE = 1656750080
OFFSET = 4194304
DT_DEST = '/boot/dtb-6.18.63-current-sunxi64/allwinner/sun50i-h616-t95.dtb'

def sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(8*1024*1024), b''):
            h.update(b)
    return h.hexdigest()

def find_debugfs():
    for p in ['/opt/homebrew/opt/e2fsprogs/sbin/debugfs', '/usr/local/opt/e2fsprogs/sbin/debugfs', shutil.which('debugfs')]:
        if p and Path(p).is_file():
            return p
    raise RuntimeError('Install tools first: brew install python e2fsprogs')

def build(source, output, ssid, password, debugfs):
    source, output = Path(source).expanduser(), Path(output).expanduser()
    if not source.is_file() or source.is_symlink():
        raise RuntimeError('Source must be a regular image file, not a device or symlink.')
    if output.exists() or output.is_symlink() or output.suffix != '.img':
        raise RuntimeError('Output must be a new .img file. Existing files are never overwritten.')
    print('Checking original image SHA-256...', flush=True)
    if source.stat().st_size != SIZE or sha(source) != EXPECTED:
        raise RuntimeError('Image differs from the verified base release. Nothing changed.')
    dtb = Path(__file__).resolve().with_name('t95-uwe5622.dtb')
    expected_dtb = dtb.with_suffix('.sha256').read_text().split()[0]
    if sha(dtb) != expected_dtb:
        raise RuntimeError('Packaged DTB checksum mismatch.')
    if not 1 <= len(ssid.encode('utf-8')) <= 32:
        raise RuntimeError('SSID must be 1–32 bytes.')
    if not 8 <= len(password) <= 63 or not all(32 <= ord(c) <= 126 for c in password):
        raise RuntimeError('This first version expects an 8–63 character ASCII WPA2-Personal passphrase.')
    # A derived WPA2 key avoids embedding the literal passphrase, but remains a secret.
    psk = hashlib.pbkdf2_hmac('sha1', password.encode(), ssid.encode(), 4096, 32).hex()
    network = {'network': {'version': 2, 'renderer': 'networkd', 'wifis': {
        'wlan0': {'dhcp4': True, 'dhcp6': True,
                    'optional': True, 'access-points': {ssid: {'password': psk}}}}}}
    # JSON is a subset of YAML, understood by netplan's YAML parser.
    with tempfile.TemporaryDirectory(prefix='t95-') as temporary:
        work = Path(temporary)
        root = work/'root.ext4'
        print('Extracting Linux filesystem...', flush=True)
        with source.open('rb') as src, root.open('wb') as dst:
            src.seek(OFFSET)
            shutil.copyfileobj(src, dst, 8*1024*1024)
        payloads = [
            (dtb.read_bytes(), DT_DEST, '0100644'),
            (b'sprdwl_ng\n', '/etc/modules-load.d/t95-wifi.conf', '0100644'),
            ((json.dumps(network, indent=2)+'\n').encode(), '/etc/netplan/30-t95-wifi.yaml', '0100600'),
        ]
        def debug(command, write=False):
            result = subprocess.run([debugfs] + (['-w'] if write else []) + ['-R', command, str(root)],
                                    text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            if result.returncode:
                raise RuntimeError(result.stdout)
            return result.stdout
        print('Applying board configuration, driver loading and Wi-Fi settings...', flush=True)
        for index, (data, dest, mode) in enumerate(payloads):
            local = work/f'payload{index}'
            local.write_bytes(data)
            local.chmod(0o600)
            if index == 0:
                debug('rm '+dest, True)
            debug(f'write "{local}" {dest}', True)
            debug(f'set_inode_field {dest} mode {mode}', True)
            check = work/f'check{index}'
            debug(f'dump {dest} "{check}"')
            if not check.exists() or check.read_bytes() != data:
                raise RuntimeError('Filesystem write verification failed: '+dest)
            inode_info = debug('stat '+dest)
            if '0600' not in inode_info and mode == '0100600':
                raise RuntimeError('Wi-Fi credential file permissions not verified.')
        checker = str(Path(debugfs).with_name('e2fsck'))
        result = subprocess.run([checker, '-fn', str(root)], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if result.returncode:
            raise RuntimeError('Filesystem validation failed:\n'+result.stdout)
        print('Writing separate image...', flush=True)
        try:
            with output.open('xb') as dst, source.open('rb') as src, root.open('rb') as patched:
                os.chmod(output, 0o600)
                dst.write(src.read(OFFSET))
                shutil.copyfileobj(patched, dst, 8*1024*1024)
        except Exception:
            # Leave any partial image visible for diagnosis; never delete a pre-existing file.
            raise
    if output.stat().st_size != SIZE:
        raise RuntimeError('Output length verification failed.')
    digest = sha(output)
    print(f'\nPrepared: {output}\nSHA-256: {digest}')
    print('Original unchanged. No SD card or internal eMMC was written.')
    print('This image contains your Wi-Fi key. Keep it private.')
    print('Experimental: Wi-Fi and boot still need validation on your physical board.')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    args = parser.parse_args()
    debugfs = find_debugfs()
    ssid = input('Wi-Fi network name (SSID): ')
    password = getpass.getpass('Wi-Fi password (hidden): ')
    output = args.image.expanduser().with_name('t95-uwe5622-wifi.img')
    build(args.image, output, ssid, password, debugfs)

if __name__ == '__main__':
    try:
        main()
    except (Exception, KeyboardInterrupt) as error:
        raise SystemExit('Stopped: '+str(error))
