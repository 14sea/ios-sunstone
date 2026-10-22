"""Offline archive/payload/extraction checks; no firmware, root image or device needed."""
from pathlib import Path
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import adt
import mkpayload


class ToolTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='sunstone-tools-')
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)
        self.src = self.dir / 'source'
        self.src.write_bytes(b'canary')

    def run_tool(self, tool, *args):
        return subprocess.run([sys.executable, str(ROOT / 'tools' / tool), *map(str, args)],
                              capture_output=True, timeout=5)

    def drop(self, *entries):
        out = self.dir / 'drop'
        result = self.run_tool('mkdrop.py', out, *entries)
        return result, out

    def test_drop_memory_and_utf8_boundary(self):
        path = '/' + '測' * 42  # 127 bytes, with a byte left for NUL.
        result, out = self.drop(f'{self.src}:{path}:mem')
        self.assertEqual(result.returncode, 0, result.stderr)
        blob = out.read_bytes()
        self.assertEqual(struct.unpack_from('<8sII', blob), (b'SWEETDRP', 1, 1))
        name, mode, size, offset, _ = struct.unpack_from('<128sIIII', blob, 16)
        self.assertEqual(name.rstrip(b'\0').decode(), path)
        self.assertEqual(mode, 0x80000000 | 0o444)
        self.assertEqual(offset % 64, 0)
        self.assertEqual(blob[offset:offset + size], b'canary')

    def test_drop_rejects_long_utf8_path_without_output(self):
        result, out = self.drop(f'{self.src}:/' + '測' * 43 + ':mem')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'UTF-8', result.stderr)
        self.assertFalse(out.exists())

    def test_drop_rejects_duplicate_destinations(self):
        result, out = self.drop(f'{self.src}:/same:mem', f'{self.src}:/same:644')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'duplicate destination', result.stderr)
        self.assertFalse(out.exists())

    def test_drop_directory_and_entry_limit(self):
        result, out = self.drop('--mem-dir', f'{self.dir}:/vendor/config')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(struct.unpack_from('<I', out.read_bytes(), 12)[0], 1)
        out.unlink()
        result, out = self.drop(*(f'{self.src}:/file-{i}:mem' for i in range(257)))
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(out.exists())

    def super_image(self, truncated=False):
        # LP geometry, primary metadata and one linear vendor_a extent.
        blob = bytearray(16384 if truncated else 16896)
        struct.pack_into('<II32sIII', blob, 4096, 0x616c4467, 52, b'\0' * 32, 4096, 2, 4096)
        struct.pack_into('<IHHI', blob, 12288, 0x414c5030, 10, 0, 128)
        struct.pack_into('<I', blob, 12288 + 44, 76)
        struct.pack_into('<III', blob, 12288 + 80, 0, 1, 52)
        struct.pack_into('<III', blob, 12288 + 92, 52, 1, 24)
        struct.pack_into('<36sIIII', blob, 12416, b'vendor_a', 0, 0, 1, 0)
        struct.pack_into('<QIQI', blob, 12416 + 52, 1, 0, 32, 0)
        if not truncated:
            blob[16384:] = b'V' * 512
        path = self.dir / 'super'
        path.write_bytes(blob)
        return path

    def test_lpextract_lists_and_extracts(self):
        src = self.super_image()
        listed = self.run_tool('lpextract.py', src)
        self.assertEqual(listed.returncode, 0, listed.stderr)
        self.assertIn(b'vendor_a', listed.stdout)
        out = self.dir / 'vendor'
        result = self.run_tool('lpextract.py', src, 'vendor_a', out)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(out.read_bytes(), b'V' * 512)

    def test_lpextract_truncation_terminates(self):
        result = self.run_tool('lpextract.py', self.super_image(True), 'vendor_a', self.dir / 'vendor')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'truncated super image', result.stderr)

    def payload_inputs(self):
        va = 0xfffffff007004000
        kernel = bytearray(4096)
        struct.pack_into('<8I', kernel, 0, 0xfeedfacf, 0x100000c, 0, 2, 2, 360, 0, 0)
        struct.pack_into('<II16s4Q4I', kernel, 32, 0x19, 72, b'__TEXT', va, 0x4000, 0, 4096, 5, 5, 0, 0)
        struct.pack_into('<4I', kernel, 104, 5, 288, 6, 68)
        struct.pack_into('<Q', kernel, 104 + 16 + 32 * 8, va + 0x100)
        kpath = self.dir / 'kernel'
        kpath.write_bytes(kernel)
        root, chosen, mmap = adt.Node(), adt.Node(), adt.Node()
        root.props = [('name', 0, b'device-tree\0')]
        chosen.props = [('name', 0, b'chosen\0'), ('dram-base', 0, struct.pack('<Q', 0x180000000)),
                        ('dram-size', 0, struct.pack('<Q', 0x100000000))]
        mmap.props = [('name', 0, b'memory-map\0')]
        root.children = [chosen]
        chosen.children = [mmap]
        apath = self.dir / 'adt'
        apath.write_bytes(adt.serialize(root))
        return kpath, apath

    def test_payload_preserves_boot_args_and_ramdisk(self):
        kernel, tree = self.payload_inputs()
        line = 'x' * 607
        blob, info = mkpayload.build(kernel, tree, line, self.src)
        self.assertEqual(blob[0x48:0x48 + 608], line.encode() + b'\0')
        self.assertEqual(struct.unpack_from('<8sII', blob), (b'SWEETXNU', 1, 2))
        va, size, offset, length = struct.unpack_from('<4Q', blob, 0x2a8 + 32)
        self.assertEqual(blob[offset:offset + length], b'canary')
        self.assertEqual(size, 0x4000)
        adt_offset, adt_size = struct.unpack_from('<2Q', blob, 0x20)
        parsed, _ = adt.parse(blob[adt_offset:adt_offset + adt_size])
        rd = parsed.children[0].children[0].prop('RAMDisk')
        self.assertEqual(struct.unpack('<2Q', rd), (int(info['ramdisk'][0], 16), size))

    def test_payload_rejects_truncated_or_embedded_nul_args(self):
        kernel, tree = self.payload_inputs()
        for line in ('x' * 608, '測' * 203, 'cpus=8\0sweetgpu=0'):
            with self.subTest(line_length=len(line)), self.assertRaises(ValueError):
                mkpayload.build(kernel, tree, line)

    def test_log_wrap_and_truncation(self):
        data = struct.pack('<8sIIQQ', b'SWEETLOG', 1, 8, 11, 3).ljust(4096, b'\0') + b'ijkdefgh'
        path = self.dir / 'log'
        path.write_bytes(data)
        result = self.run_tool('sweet-log.py', path)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, b'defghijk')
        for broken in (data[:32], data[:-1]):
            path.write_bytes(broken)
            result = self.run_tool('sweet-log.py', path)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b'truncated', result.stderr)

    def test_kbuild_propagates_packer_failure(self):
        # No network or real GitHub commands: isolate the complete sh script.
        tools = self.dir / 'tools'
        tools.mkdir()
        shutil.copyfile(ROOT / 'tools/kbuild.sh', tools / 'kbuild.sh')
        packer = tools / 'mkboot.sh'
        packer.write_text('#!/bin/sh\necho "packer rejected input" >&2\nexit 23\n')
        packer.chmod(0o755)
        fakebin = self.dir / 'bin'
        fakebin.mkdir()
        mocks = {
            'git': 'case "$*" in\nremote*) echo https://github.com/example/audit.git;;\n'
                   '*--abbrev-ref*) echo sunstone;;\n*) echo abc;;\nesac\n',
            'gh': 'case "$*" in\n*headSha*) echo 123;;\napi*) echo xnu-kernel;;\nesac\n',
            'sleep': 'exit 0\n',
        }
        for name, body in mocks.items():
            path = fakebin / name
            path.write_text('#!/bin/sh\n' + body)
            path.chmod(0o755)
        env = dict(os.environ, PATH=str(fakebin) + os.pathsep + os.environ['PATH'])
        result = subprocess.run(['sh', str(tools / 'kbuild.sh'), 'audit', 'test=cpus=1'],
                                env=env, capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 23)
        self.assertIn(b'packer rejected input', result.stderr)


if __name__ == '__main__':
    unittest.main()
