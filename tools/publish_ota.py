#!/usr/bin/env python3
"""把编译产物发布成本地 OTA 服务器的内容。

**为什么需要这一步**：HFS（或任何静态文件服务器）只负责"把文件发出去"，
它不会替你算大小和哈希，也不知道你刚编的是哪个版本。而设备侧的
version.txt 里必须有 size 和 sha256，才能在下载后自校验。

所以每次 `idf.py build` 之后跑一次本脚本，它做三件事：
  1. 把 build/esp_handheld.bin 复制到发布目录（**复制而不是软链** —— 目标目录常常
     不在同一个卷上，而且复制让"服务器上的那个文件"成为一个不可变快照）
  2. 算 size + sha256，生成 version.txt
  3. 校验复制过去的文件哈希与源文件一致（复制本身出错的概率很低，但这一步是免费的）

★★ 版本号是从**编译产物**里读的，不是从源码 ★★
清单里的版本号取 build/esp_handheld.bin 内嵌的 esp_app_desc.version，
而不是工程根 version.txt 的内容。原因是这两者**可以不一致**：

    改了 version.txt 但没重新编译 → 源码说 1.0.4，固件里嵌的还是 1.0.3
    → 清单说 1.0.4，设备下载下来发现是 1.0.3
    → 设备侧的回读校验拒绝切换分区，报「固件版本与清单不符」

从产物读的话，清单和固件在**构造上**就不可能不一致。两者不同时本脚本会
大声警告（那是"你忘了重新编译"的信号），但**以产物为准**。

用法：
    python tools/publish_ota.py                       # 版本号取自固件内嵌值
    python tools/publish_ota.py --notes "新增天气应用"
    python tools/publish_ota.py --version 1.0.2 --base-url http://192.168.0.138

发布出来的 version.txt 格式（竖线分隔，**不用 JSON** —— 与 weather_task.c 里
解析天气响应是同一套思路：几十字节、切一刀就完事、不用引 cJSON）：

    1.0.1|2776544|http://192.168.0.138/esp_handheld.bin|<sha256>|新增天气应用
     版本   字节数   下载地址                                       哈希      说明
"""

import argparse
import hashlib
import io
import os
import shutil
import struct
import sys

# 工程根 = 本文件的上上级
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN_SRC = os.path.join(ROOT, 'build', 'esp_handheld.bin')
VERSION_FILE = os.path.join(ROOT, 'version.txt')
BIN_NAME = 'esp_handheld.bin'
MANIFEST_NAME = 'version.txt'

# esp_app_desc_t 在镜像里的位置：img_header(24) + segment_header(8) = 32 = 0x20。
# 结构开头是 magic_word(4) + secure_version(4) + reserv1[2](8) = 16 字节，
# 紧接着就是 version[32]。这些常量与 ota_task.c 里回读校验用的是同一套。
APP_DESC_OFFSET = 0x20
APP_DESC_MAGIC = 0xABCD5432
APP_DESC_VERSION_OFF = 16
APP_DESC_VERSION_LEN = 32


def sha256_of(path, chunk=1 << 20):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while True:
            b = f.read(chunk)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def version_in_binary(path):
    """读出固件内部嵌的 esp_app_desc.version。读不出来返回 None。

    这就是设备侧 ota_task.c 回读校验时读的那份数据 —— 用同一份数据填清单，
    清单与固件就不可能对不上。
    """
    try:
        with open(path, 'rb') as f:
            # 描述符在文件开头附近，只需读一小段
            head = f.read(APP_DESC_OFFSET + APP_DESC_VERSION_LEN + APP_DESC_VERSION_OFF + 16)
    except OSError:
        return None
    if len(head) < APP_DESC_OFFSET + APP_DESC_VERSION_OFF + 4:
        return None
    magic, = struct.unpack_from('<I', head, APP_DESC_OFFSET)
    if magic != APP_DESC_MAGIC:
        return None
    raw = head[APP_DESC_OFFSET + APP_DESC_VERSION_OFF:
               APP_DESC_OFFSET + APP_DESC_VERSION_OFF + APP_DESC_VERSION_LEN]
    v = raw.split(b'\x00')[0].decode('utf-8', 'replace').strip()
    return v or None


def read_version_file():
    """读工程根的 version.txt（只用来和产物里的版本对一下，作为"忘了重编"的提示）。"""
    if not os.path.exists(VERSION_FILE):
        return None
    with io.open(VERSION_FILE, encoding='utf-8') as f:
        v = f.read().strip()
    return v or None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', default=os.path.join(ROOT, 'build'),
                    help='发布目录。默认就是工程自己的 build/ —— '
                         '因为 HFS 现在直接服务这个目录，清单写在这里等于**零拷贝**，'
                         '服务器上的固件一定就是刚编出来的那个，不存在"忘了复制"这种可能。')
    ap.add_argument('--base-url', default='http://192.168.0.138/build',
                    help='设备访问本服务器的基址（要包含 HFS 里那个虚拟目录前缀）')
    ap.add_argument('--version', default=None,
                    help='版本号；不给就取**固件内嵌的**版本（推荐留空）')
    ap.add_argument('--notes', default='', help='更新说明（不能含竖线）')
    ap.add_argument('--allow-stale', action='store_true',
                    help='版本号与固件内嵌值不一致时仍然继续（默认拒绝）')
    args = ap.parse_args(argv)

    # ---- 源文件先确认存在（后面要读它的版本号）----
    if not os.path.exists(BIN_SRC):
        print('错误：找不到 %s' % BIN_SRC, file=sys.stderr)
        print('  先跑一次 idf.py build。', file=sys.stderr)
        return 1

    # ---- 版本号：**以编译产物为准** ----
    #
    # 这是本脚本最重要的一条设计。清单里的版本号必须等于设备下载到的那个固件
    # 内嵌的版本，否则设备侧的回读校验会拒绝切换分区（报「固件版本与清单不符」）。
    # 从产物读，就消除了"忘了重新编译"这一类错误。
    bin_version = version_in_binary(BIN_SRC)
    file_version = read_version_file()

    if bin_version is None:
        print('错误：%s 里读不出 app 描述符（magic 不对或文件损坏）。' % BIN_SRC,
              file=sys.stderr)
        print('  先确认 idf.py build 成功过。', file=sys.stderr)
        return 1

    if args.version:
        version = args.version
    else:
        version = bin_version

    # ---- 和源码里的 version.txt 对一下 ----
    # 不一致 = "改了 version.txt 但没重新编译"，几乎总是笔误。
    # 因为 ESP-IDF 把 version.txt 登记成了 CMAKE_CONFIGURE_DEPENDS，
    # 正常情况下重跑 idf.py build 就会自动同步，所以这里不一致就是真的忘了编。
    if file_version and file_version != bin_version and not args.allow_stale:
        print('=' * 68, file=sys.stderr)
        print('错误：版本号不一致 —— 固件和源码说的不是同一件事', file=sys.stderr)
        print('=' * 68, file=sys.stderr)
        print('  工程根 version.txt        : %s   （源码"打算"是这一版）' % file_version, file=sys.stderr)
        print('  固件里实际嵌的            : %s   （设备会报告这一版）' % bin_version, file=sys.stderr)
        print('', file=sys.stderr)
        print('  几乎可以肯定是：改了 version.txt，但没有重新编译。', file=sys.stderr)
        print('  请重新编译一次，脚本再来发布：', file=sys.stderr)
        print('', file=sys.stderr)
        print('      idf.py build', file=sys.stderr)
        print('      python tools\\publish_ota.py --notes "..."', file=sys.stderr)
        print('', file=sys.stderr)
        print('  （如果确实要用源码里的版本号发布，就加 --allow-stale —— 但那样', file=sys.stderr)
        print('    设备的回读校验会拒绝升级，除非 --version 显式给的就是固件里的值。）', file=sys.stderr)
        return 1

    if file_version and file_version != version:
        print('警告：清单版本(%s) 与 工程根 version.txt(%s) 不一致。' % (version, file_version),
              file=sys.stderr)

    if '|' in args.notes:
        print('错误：--notes 里不能有竖线（它是 version.txt 的字段分隔符）。', file=sys.stderr)
        return 1
    if '|' in version:
        print('错误：版本号里不能有竖线。', file=sys.stderr)
        return 1

    out_dir = os.path.abspath(args.out)
    os.makedirs(out_dir, exist_ok=True)

    # ---- 复制 ----
    # 发布目录 == build 目录时源和目标是同一个文件，**必须跳过复制** ——
    # shutil.copyfile 对同一路径会抛 SameFileError。
    # 这种情况下连"复制完整性"都不用验：本来就是它自己。
    dst = os.path.join(out_dir, BIN_NAME)
    same_file = (os.path.abspath(dst) == os.path.abspath(BIN_SRC))
    if not same_file:
        shutil.copyfile(BIN_SRC, dst)

    size = os.path.getsize(dst)
    digest = sha256_of(dst)

    # ---- 复制完整性自检 ----
    # 比的是**复制过去的那个文件**和源文件，不是"我以为复制对了"。
    if not same_file:
        src_digest = sha256_of(BIN_SRC)
        if src_digest != digest:
            print('错误：复制后的文件哈希与源文件不一致！', file=sys.stderr)
            print('  源  : %s' % src_digest, file=sys.stderr)
            print('  目标: %s' % digest, file=sys.stderr)
            return 1

    # ---- 清单 ----
    url = '%s/%s' % (args.base_url.rstrip('/'), BIN_NAME)
    line = '%s|%d|%s|%s|%s' % (version, size, url, digest, args.notes)
    manifest = os.path.join(out_dir, MANIFEST_NAME)
    with io.open(manifest, 'w', encoding='utf-8', newline='\n') as f:
        f.write(line + '\n')

    print('发布目录 : %s' % out_dir)
    print('版本     : %s   （读自固件内嵌值）' % version)
    print('大小     : %d 字节 (%.2f MB)' % (size, size / 1048576.0))
    print('SHA256   : %s' % digest)
    print('清单     : %s' % manifest)
    print()
    print('version.txt 内容：')
    print('  ' + line)
    print()
    print('设备侧要访问的两个地址（**在设备上验证过再写进代码**）：')
    print('  清单 : %s/%s' % (args.base_url.rstrip('/'), MANIFEST_NAME))
    print('  固件 : %s' % url)
    print()
    print('验证服务器真的发出了这个文件（在 PC 上跑）：')
    print('  curl -s %s | Select-String .' % ('%s/%s' % (args.base_url.rstrip('/'), MANIFEST_NAME)))
    print('  curl -o %%TEMP%%\\ota.bin %s' % url)
    print('  (Get-FileHash %%TEMP%%\\ota.bin -Algorithm SHA256).Hash')
    print('  # 应当等于上面的 SHA256')
    return 0


if __name__ == '__main__':
    sys.exit(main())
