"""The build cache must notice changes to included SDK dependencies."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('reference_build', ROOT / 'scripts/build_reference.py')
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


class DependencyCacheTests(unittest.TestCase):
    def test_sdk_headers_and_packaged_libc_invalidate_cache(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            core = root / 'core'
            for relative in builder.INPUT_DIRS:
                (core / relative).mkdir(parents=True, exist_ok=True)
            for relative in ('platform.txt', 'tools/elf2bin.py'):
                (core / relative).write_text('fixture')
            header = core / 'tools/sdk/ssl/bearssl/inc/bearssl.h'
            header.write_text('original')
            libc = core / 'tools/sdk/libc/xtensa-lx106-elf/include/stdlib.h'
            libc.parent.mkdir(parents=True)
            libc.write_text('original')
            with patch.object(builder, 'run', return_value='fixture compiler'):
                initial = builder.input_digest(core, root / 'toolchain/bin')
                header.write_text('changed')
                ssl_changed = builder.input_digest(core, root / 'toolchain/bin')
                self.assertNotEqual(initial, ssl_changed)
                header.write_text('original')
                self.assertEqual(initial, builder.input_digest(core, root / 'toolchain/bin'))
                libc.write_text('changed')
                self.assertNotEqual(initial, builder.input_digest(core, root / 'toolchain/bin'))


if __name__ == '__main__':
    unittest.main()
