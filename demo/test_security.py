"""Exercise the compiled endpoint policy on genuine BPv7/BCB wire bundles."""
import ctypes
import os
from pathlib import Path
import sys
import unittest
import cbor2

LIBRARY = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else Path('demo/build/integration/libbplib_bsl_security.so').resolve()


def wire(blocks):
    """Encode the BPv7 outer indefinite array used by BPLib and BSL."""
    return b'\x9f' + b''.join(cbor2.dumps(block) for block in blocks) + b'\xff'


class EndpointSecurity(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lib = ctypes.CDLL(str(LIBRARY))
        cls.lib.BPLib_BSL_SecurityInit.argtypes = [ctypes.c_void_p]
        cls.lib.BPLib_BSL_SecurityTransform.argtypes = [ctypes.c_bool, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]

    def setUp(self):
        self.key = os.urandom(32)
        self.assertEqual(self.lib.BPLib_BSL_SecurityInit(self.key), 0)
        self.addCleanup(self.lib.BPLib_BSL_SecurityClose)
        self.plain = wire([[7, 4, 0, [2, [50, 1]], [2, [10, 1]], [2, [10, 0]], [1000, 1], 3600000], [1, 1, 0, 0, b'authenticated video chunk']])

    def transform(self, protect, data, success=True):
        output = ctypes.create_string_buffer(b'x' * 17000)
        length = ctypes.c_size_t(99)
        result = self.lib.BPLib_BSL_SecurityTransform(protect, data, len(data), output, 17000, ctypes.byref(length))
        if success:
            self.assertEqual(result, 0)
            return output.raw[:length.value]
        self.assertNotEqual(result, 0)
        self.assertEqual(length.value, 0)
        self.assertEqual(output.raw[:17000], b'x' * 17000)

    def test_roundtrip_and_fresh_ivs(self):
        first = self.transform(True, self.plain)
        second = self.transform(True, self.plain)
        self.assertNotEqual(first, second)
        self.assertNotIn(b'authenticated video chunk', first)
        self.assertEqual(cbor2.loads(self.transform(False, first)), cbor2.loads(self.plain))
        self.assertEqual([block[0] for block in cbor2.loads(first)[1:]], [12, 1])

    def test_wrong_key_missing_bcb_and_tampering(self):
        protected = self.transform(True, self.plain)
        self.transform(False, self.plain, False)
        blocks = cbor2.loads(protected)
        blocks[-1][4] = bytes([blocks[-1][4][0] ^ 1]) + blocks[-1][4][1:]
        self.transform(False, wire(blocks), False)
        blocks = cbor2.loads(protected); blocks[0][6][1] += 1
        self.transform(False, wire(blocks), False)
        self.lib.BPLib_BSL_SecurityClose()
        self.assertEqual(self.lib.BPLib_BSL_SecurityInit(os.urandom(32)), 0)
        self.transform(False, protected, False)

    def test_wrong_endpoints_and_repeated_protection(self):
        blocks = cbor2.loads(self.plain); blocks[0][3] = [2, [40, 1]]
        self.transform(True, wire(blocks), False)
        protected = self.transform(True, self.plain)
        self.transform(True, protected, False)

    def test_mutable_relay_blocks_preserve_endpoint_security(self):
        blocks = cbor2.loads(self.plain)
        blocks.insert(1, [7, 2, 0, 0, cbor2.dumps([2, [20, 0]])])
        blocks.insert(2, [10, 3, 0, 0, cbor2.dumps([10, 1])])
        protected = cbor2.loads(self.transform(True, wire(blocks)))
        for block in protected[1:]:
            if block[0] == 7: block[4] = cbor2.dumps([2, [40, 0]])
            if block[0] == 10: block[4] = cbor2.dumps([10, 4])
        accepted = cbor2.loads(self.transform(False, wire(protected)))
        self.assertEqual(accepted[-1][4], b'authenticated video chunk')

    def test_truncation_and_capacity_fail_closed(self):
        self.transform(False, b'\x9f', False)
        output = ctypes.create_string_buffer(1); length = ctypes.c_size_t()
        self.assertNotEqual(self.lib.BPLib_BSL_SecurityTransform(True, self.plain, len(self.plain), output, 1, ctypes.byref(length)), 0)
        self.assertEqual(length.value, 0)

if __name__ == '__main__':
    unittest.main(verbosity=2)
