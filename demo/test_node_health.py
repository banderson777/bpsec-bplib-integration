"""Regression checks for native-host versus BPNode application health."""
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from five_node import check_nodes, wait_for_nodes


class NodeHealth(unittest.TestCase):
    def test_live_host_with_failed_application(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'node1.log').write_text('Error initializing BPLib, RC = -336\n')
            host = SimpleNamespace(poll=lambda: None)
            with self.assertRaisesRegex(RuntimeError, 'application initialization failed'):
                check_nodes({1: host}, root)

    def test_restart_requires_current_application_readiness(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            log = root / 'node1.log'
            previous = b'DEMO node=1 recovered 2 custody records\n'
            log.write_bytes(previous)
            host = SimpleNamespace(poll=lambda: None, log_offset=len(previous))
            self.assertFalse(check_nodes({1: host}, root))
            with self.assertRaisesRegex(RuntimeError, 'startup timed out'):
                wait_for_nodes({1: host}, root, timeout=0)
            log.write_bytes(previous + b'DEMO node=1 recovered 1 custody records\n')
            self.assertTrue(check_nodes({1: host}, root))
            wait_for_nodes({1: host}, root, timeout=0)
            host.poll = lambda: 1
            host.returncode = 1
            with self.assertRaisesRegex(RuntimeError, 'node 1 exited'):
                check_nodes({1: host}, root)


if __name__ == '__main__':
    unittest.main(verbosity=2)
