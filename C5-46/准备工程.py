"""C5 #46：从官方 SDK 压缩包恢复本项目工程。
用法：python 准备工程.py --sdk-zip <STM32C5 HAL 2.1.0 zip> --out <尚不存在的新目录>
只解压、覆盖改写文件并核对哈希，不构建、不接板。"""
import argparse
import hashlib
import json
import shutil
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
BASE = 'examples/hal/adc/single_conversion_trigger_sw_it'
PROJECT = 'examples/hal/adc/c5_46_alarm_terminal'
# 只解压构建用得到的部分：arch/ 只取 cmsis，并跳过各处 Documentation（CMSIS-DSP 文档文件名很长，深目录下会超出 Windows 路径上限）
PREFIXES = ('arch/cmsis/', 'part_drivers/', 'stm32c5xx_dfp/', 'stm32c5xx_drivers/', 'utilities/', BASE + '/')

ap = argparse.ArgumentParser()
ap.add_argument('--sdk-zip', required=True)
ap.add_argument('--out', required=True)
args = ap.parse_args()
out = Path(args.out)
if out.exists():
    raise SystemExit(f'{out} 已存在：请换一个新目录，脚本不覆盖已有工程')

record = json.loads((HERE / '固件构建记录.json').read_text(encoding='utf-8'))
zip_sha = hashlib.sha256(Path(args.sdk_zip).read_bytes()).hexdigest()
if zip_sha != record['sdk_zip_sha256']:
    raise SystemExit(f'SDK 压缩包哈希不符：{zip_sha}，期望 {record["sdk_zip_sha256"]}')

with zipfile.ZipFile(args.sdk_zip) as z:
    names = [n for n in z.namelist() if not n.endswith('/') and n.startswith(PREFIXES) and '/Documentation/' not in n]
    for n in names:
        rel = PROJECT + n[len(BASE):] if n.startswith(BASE + '/') else n
        dst = out / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(z.read(n))

manifest = json.loads((HERE / '改动清单.json').read_text(encoding='utf-8'))
for item in manifest['files']:
    src = HERE / '改写文件' / item['path']
    if hashlib.sha256(src.read_bytes()).hexdigest() != item['sha256']:
        raise SystemExit(f'改写文件哈希不符：{item["path"]}')
    dst = out / PROJECT / item['path']
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)
print(f'已解压 {len(names)} 个官方文件，覆盖 {len(manifest["files"])} 个改写文件：{out / PROJECT}')
