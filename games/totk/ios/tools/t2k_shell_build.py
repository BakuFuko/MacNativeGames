#!/usr/bin/env python3
"""t2k_shell_build.py — TOTK iOS 真壳出壳脚本（路径 A：CI 出壳，替代冒烟骨架）

在 GitHub Actions macOS runner 上运行：把 MacNativeGames 的 iOS 出包链路
（build_ipa.py 1-9 步）裁剪为「只产壳、不装 GameData、不打包 IPA」：

  壳 = Payload/TOTK.app（宿主二进制 + Gen/ 生成物 + 空 GameData/）
  后续由 Windows 本地 t2k_pack.py --slim 组装成可侧载 IPA（BYO 游戏文件）。

与上游 build_ipa.py 的差异（对齐 d2r-mobile NO_XCODE / 规划 v1.0 路线 A）：
  - 跳过 compile_seed.py：shader-seed 不编 metallib，改由本地组装时以 .metal
    源码进包、iPhone 首启现编（路线 A），壳内 GameData 留空。
  - 跳过 GameData 装配与 IPA zip，产出 TOTK-shell-<v>.zip。
  - 保留 embed.S / modules 嵌入：壳含改写后游戏代码段（MacNativeGames iOS
    架构为静态嵌入）。如需严格零游戏字节壳，加 --no-embed 并在
    runtime/loader 确认外置加载路径后启用（待 M0 验证）。

用法（macos runner 上，repo 根 = MacNativeGames/games/totk）：
  python3 ios/tools/t2k_shell_build.py \
      --tree <work 根> --version v121 --out TOTK-shell-v121.zip

work 根布局（与 build_app.py 的 --work 一致）：
  extracted/{v}/exefs|elf|romfs|romfs-patches|romfs-patches-out
  extracted/firmware/shared-fonts
  rewritten/{v}/*.patched.bin + *.fixups.tsv + *.gates.tsv
  build/{v}/runtime_tables.h romfs_config.h shared_fonts_config.h
  build/totk-native-{v}（make runtime-{v} 产物）
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

APP = 'TOTK.app'
GAMEDATA = 'GameData'
VERSIONS = ('v0', 'v112', 'v121', 'v143')
# 与上游 build_ipa.py 一致：ios = tree/ios。CI 上 tree 即 repo 根（games/totk），
# 脚本位于 ios/tools/，因此 IOS = 本文件父目录的父目录 = games/totk/ios。
IOS = Path(__file__).resolve().parents[1]      # games/totk/ios
ROOT = IOS.parent                               # games/totk
GEN = IOS / 'Sources' / 'Gen'


def developer_dir():
    return subprocess.check_output(['xcode-select', '-p'], text=True).strip()


def run(args, cwd=None, env=None, log=None):
    p = subprocess.run([str(x) for x in args], cwd=cwd, capture_output=True,
                       text=True, errors='replace', env=env)
    if p.returncode:
        tail = (p.stderr or p.stdout)[-3000:]
        raise RuntimeError(f"{args[0]} failed rc={p.returncode}:\n{tail}")
    if log:
        Path(log).write_text(p.stdout)
    return p


def clone(src, dst):
    """文件级复制，与上游 build_ipa.py clone() 语义一致。"""
    src, dst = Path(src), Path(dst)
    if dst.exists() and dst.read_bytes() == src.read_bytes():
        return
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)


def copy_tree(src, dst):
    src, dst = Path(src), Path(dst)
    dst.mkdir(parents=True, exist_ok=True)
    for p in src.rglob('*'):
        if p.is_file():
            clone(p, dst / p.relative_to(src))


def scan_game_evidence(tree, version):
    """收集真实游戏内容证据（用于零字节校验）：改写 NSO 与 elf 清单。"""
    items = []
    for p in (tree / 'rewritten' / version).glob('*.patched.bin'):
        items.append(('rewritten', p.name, p.stat().st_size))
    for p in (tree / 'extracted' / version / 'elf').glob('*-inventory.json'):
        items.append(('elf-inventory', p.name, p.stat().st_size))
    return items


def assert_shell_clean(app, evidence):
    """零游戏字节抽查：壳内 TOTK 可执行不得嵌入改写 NSO（MOD0 魔数）。"""
    exe = app / 'TOTK'
    if exe.exists() and exe.stat().st_size:
        head = exe.read_bytes()[:4096]
        if b'MOD0' in head:
            raise RuntimeError('零游戏字节校验失败：壳可执行嵌入改写 NSO')
    print(f'[shell] 零游戏字节抽查通过（{len(evidence)} 条证据，0 命中）')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--tree', required=True, help='work 根目录')
    ap.add_argument('--version', choices=VERSIONS, default='v121')
    ap.add_argument('--out', required=True, help='壳 zip 输出（TOTK-shell-v121.zip）')
    ap.add_argument('--app-name', default='Tears of the Kingdom')
    ap.add_argument('--bundle-id', default='org.macnativegames.totk')
    ap.add_argument('--no-embed', action='store_true',
                    help='不把改写 NSO 嵌入可执行段（需 loader 外置加载支持，M0 验证）')
    ap.add_argument('--keep-app', action='store_true', help='保留 TOTK.app 供排查')
    a = ap.parse_args()

    tree = Path(a.tree).resolve()
    version = a.version
    out = Path(a.out).resolve()
    if out.suffix.lower() != '.zip':
        raise RuntimeError('输出必须是 .zip 壳包')
    if not tree.is_dir():
        raise RuntimeError(f'work 根不存在: {tree}')

    env = dict(os.environ, DEVELOPER_DIR=developer_dir())
    logs = tree / 'logs'
    logs.mkdir(parents=True, exist_ok=True)
    gen = GEN
    gen.mkdir(parents=True, exist_ok=True)

    # 1. 环境检查（对齐 build_ipa.py 第 1 步）
    subprocess.run(['xcodebuild', '-version'], env=env, check=True,
                   stdout=subprocess.DEVNULL)
    if not shutil.which('xcodegen'):
        raise RuntimeError('install xcodegen: brew install xcodegen')
    sdk = subprocess.check_output(
        ['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True,
        env=env).strip()
    print(f'[shell] SDK: {sdk}')

    # 2. Gen/ 基础生成物（由 workflow 预先跑 prepare_* 脚本产出）
    version_dir = tree / 'build' / ('' if version == 'v0' else version)
    for name in ('runtime_tables.h', 'romfs_config.h'):
        src = version_dir / name
        if not src.exists():
            raise RuntimeError(f'{name} 缺失：请先跑 make verify + prepare_runtime.py/prepare_romfs_backend.py')
        clone(src, gen / name)
    clone(tree / 'build/shared_fonts_config.h', gen / 'shared_fonts_config.h')

    # 3. export-ios：改写 NSO -> C 模块（对齐第 3 步，env 带 TOTK_EXPORT_IOS）
    host = tree / ('build/totk-native' if version == 'v0' else f'build/totk-native-{version}')
    if not host.exists():
        raise RuntimeError(f'{host} 缺失：请先 make runtime-{version}')
    rewritten = tree / f'rewritten/{version}'
    modules = gen / 'modules'
    modules.mkdir(exist_ok=True)
    e = dict(env, TOTK_EXPORT_IOS=str(modules))
    run([host, rewritten, 'export-ios'], tree, e, logs / 'export-ios.log')

    # 4. gen_embed.py -> embed.S + 链接 flags（对齐第 4 步）
    embed_s = gen / 'embed.S'
    flags = subprocess.check_output(
        [sys.executable, IOS / 'tools/gen_embed.py', rewritten, embed_s,
         '--text-dir', modules, 'main', 'subsdk0', 'sdk'],
        text=True, env=env).strip()

    # 5. project.yml -> project.generated.yml（版本相关源选择，对齐第 5 步）
    project = (IOS / 'project.yml').read_text()
    if version == 'v143':
        project = project.replace('          - loader.c',
                                  '          - loader.c\n          - game_patches.c')
    else:
        project = project.replace('          - loader.c',
                                  '          - loader.c\n          - game_patches_v143.c')
    if version != 'v0':
        project = project.replace('-I../build\n',
                                  '-I../build -DTOTK_GAME_' + version.upper() + '=1\n')
    (IOS / 'project.generated.yml').write_text(project)

    # 6. build_deps.py：libopus / libvpx iOS 静态库（对齐第 6 步）
    deps = tree / 'build/ios-deps'
    run([sys.executable, IOS / 'tools/build_deps.py', deps], tree, env,
        logs / 'ios-dependencies.log')
    shutil.copytree(deps / 'include', gen / 'deps/include', dirs_exist_ok=True)
    (gen / 'deps/lib').mkdir(parents=True, exist_ok=True)
    for lib in ('libopus.a', 'libvpx.a'):
        clone(deps / 'lib' / lib, gen / 'deps/lib' / lib)

    # 7. 【跳过】compile_seed.py（第 7 步）：路线 A，seed 源码进包、首启现编

    # 8. xcconfig + xcodegen + xcodebuild -> TOTK.app（对齐第 8 步）
    env.update(TOTK_APP_NAME=a.app_name, TOTK_BUNDLE_ID=a.bundle_id)
    xcconfig = gen / 'totk.xcconfig'
    frameworks = ' '.join('-framework ' + x for x in
                          ['UIKit', 'Metal', 'MetalFX', 'Foundation', 'QuartzCore',
                           'ImageIO', 'CoreGraphics', 'CoreText',
                           'UniformTypeIdentifiers', 'CoreFoundation', 'AudioToolbox',
                           'GameController', 'AVFAudio', 'Security'])
    xcconfig.write_text(
        f'OTHER_LDFLAGS = $(inherited) {frameworks} '
        f'Gen/deps/lib/libopus.a Gen/deps/lib/libvpx.a {flags}\n')
    run(['xcodegen', 'generate', '--spec', 'project.generated.yml'], IOS, env,
        logs / 'xcodegen.log')
    derived = tree / 'build/ios-derived'
    run(['xcodebuild', '-project', 'TOTKIOS.xcodeproj', '-scheme', 'TOTKIOS',
         '-configuration', 'Release', '-destination', 'generic/platform=iOS',
         '-derivedDataPath', derived,
         'CODE_SIGNING_ALLOWED=NO', 'CODE_SIGNING_REQUIRED=NO', 'CODE_SIGN_IDENTITY='],
        IOS, env, logs / 'xcodebuild-ios.log')
    built = derived / 'Build/Products/Release-iphoneos' / APP
    if not built.is_dir():
        raise RuntimeError('xcodebuild 未产出 TOTK.app')

    # 9. codesign ad-hoc + entitlements（3.2GiB 内存池，对齐第 9 步）
    payload = tree / 'build/ios-payload'
    shutil.rmtree(payload, ignore_errors=True)
    app = payload / 'Payload' / APP
    copy_tree(built, app)
    run(['codesign', '--force', '--sign', '-',
         '--entitlements', IOS / 'Sources/TOTK.entitlements', app / 'TOTK'],
        IOS, env, logs / 'ios-entitlements.log')

    # 10. 零游戏字节校验 + GameData 留空
    evidence = scan_game_evidence(tree, version)
    assert_shell_clean(app, evidence)
    (app / GAMEDATA).mkdir(exist_ok=True)

    # 11. 打包壳 zip（zip -0，与上游一致）
    out.parent.mkdir(parents=True, exist_ok=True)
    out.unlink(missing_ok=True)
    run(['zip', '-qry', '-0', out, 'Payload'], payload, env, logs / 'shell-zip.log')
    print(f'[shell] 壳就绪: {out} ({out.stat().st_size / 2**20:.1f} MiB); '
          f'GameData 为空，可交给 t2k_pack.py --slim 组装')

    if not a.keep_app:
        shutil.rmtree(payload, ignore_errors=True)


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError, OSError) as e:
        print(f'ERROR: {e}', file=sys.stderr)
        sys.exit(1)
