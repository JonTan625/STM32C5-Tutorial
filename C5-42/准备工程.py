"""从本篇归档创建独立SDK子集，保留相对依赖；不覆盖已有输出。"""
from pathlib import Path, PurePosixPath
import argparse
import hashlib
import json
import zipfile

p=argparse.ArgumentParser()
p.add_argument('--sdk-zip',required=True,type=Path)
p.add_argument('--out',type=Path)
p.add_argument('--check-only',action='store_true')
a=p.parse_args()
here=Path(__file__).resolve().parent
manifest=json.loads((here/'改动清单.json').read_text(encoding='utf-8'))
top=('arch/','part_drivers/','stm32c5xx_dfp/','stm32c5xx_drivers/','utilities/','middleware/usbx/','examples/middleware/usbx/device_hid_mouse_no_os/')
def sha(data):return hashlib.sha256(data).hexdigest()
with a.sdk_zip.open('rb') as f:
    digest=hashlib.file_digest(f,'sha256').hexdigest()
if digest!=manifest['sdk_sha256']:
    raise SystemExit('SDK ZIP与本篇归档不同，先核对版本，不自动覆盖。')
with zipfile.ZipFile(a.sdk_zip) as z:
    for item in manifest['files']:
        if sha(z.read(item['relative_path']))!=item['official_sha256']:
            raise SystemExit('官方文件不匹配：'+item['relative_path'])
        if sha((here/item['package_file']).read_bytes())!=item['teaching_sha256']:
            raise SystemExit('教学文件已变化：'+item['package_file'])
    if a.check_only:
        print('ZIP与两个改写文件哈希通过；未创建工程。')
    else:
        if a.out is None:raise SystemExit('请指定 --out 新目录，或用 --check-only 仅核对。')
        if a.out.exists():raise SystemExit('输出目录已经存在，不覆盖：'+str(a.out))
        a.out.mkdir(parents=True)
        for name in z.namelist():
            if name.endswith('/') or not name.startswith(top):continue
            if '..' in PurePosixPath(name).parts or PurePosixPath(name).is_absolute():
                raise SystemExit('归档路径异常：'+name)
            dest=a.out.joinpath(*PurePosixPath(name).parts)
            dest.parent.mkdir(parents=True,exist_ok=True)
            dest.write_bytes(z.read(name))
        for item in manifest['files']:
            (a.out/item['relative_path']).write_bytes((here/item['package_file']).read_bytes())
        print('教学工程已创建：'+str(a.out.resolve()))
        print('构建入口：examples/middleware/usbx/device_hid_mouse_no_os/NUCLEO-C542RC/cmake')
