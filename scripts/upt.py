"""ISO-контейнер прошивки .upt (Joliet + Rock Ridge, как genisoimage -J -r).

    python upt.py extract <firmware.upt> <out_dir>
    python upt.py build   <ota_dir> <out.upt>     # ota_dir содержит ota_config.in и ota_v0/
"""
import os
import sys

import pycdlib


def extract(src, dst):
    iso = pycdlib.PyCdlib()
    iso.open(src)
    for root, _dirs, files in iso.walk(rr_path='/'):
        for f in files:
            p = root.rstrip('/') + '/' + f
            out = os.path.join(dst, p.lstrip('/'))
            os.makedirs(os.path.dirname(out), exist_ok=True)
            iso.get_file_from_iso(out, rr_path=p)
    iso.close()


def build(src, out):
    iso = pycdlib.PyCdlib()
    iso.new(interchange_level=3, joliet=3, rock_ridge='1.09', sys_ident='LINUX', vol_ident='CDROM')
    iso.add_file(os.path.join(src, 'ota_config.in'), iso_path='/OTA_CONF.IN;1',
                 rr_name='ota_config.in', joliet_path='/ota_config.in')
    iso.add_directory(iso_path='/OTA_V0', rr_name='ota_v0', joliet_path='/ota_v0')
    for n, f in enumerate(sorted(os.listdir(os.path.join(src, 'ota_v0')))):
        iso.add_file(os.path.join(src, 'ota_v0', f), iso_path=f'/OTA_V0/F{n:05d}.;1',
                     rr_name=f, joliet_path=f'/ota_v0/{f}')
    iso.write(out)
    iso.close()


if __name__ == '__main__':
    cmd, a, b = sys.argv[1:4]
    {'extract': extract, 'build': build}[cmd](a, b)
    print(cmd, 'ok:', b)
