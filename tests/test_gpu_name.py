#!/usr/bin/env python3
"""Offline contract: the PCI device gets ATY,FamilyName / ATY,DeviceName so Metal names the GPU
"AMD Radeon 660M" instead of "AMD Radeon HD GFX10 Family Unknown Prototype". Source only."""
from pathlib import Path
import unittest

SRC = (Path(__file__).resolve().parents[1] / 'src/Rembrandt660M/kern_start.cpp').read_text()


class GpuName(unittest.TestCase):
    def test_properties_set_with_model(self):
        a = SRC.index('static void syncPciDeviceDirect(IOService *svc) {')
        body = SRC[a:SRC.index('\n}\n', a)]
        self.assertIn('static const char kModel[] = "AMD Radeon 660M";', body)
        self.assertIn('static const char kFamily[] = "Radeon", kDevice[] = "660M";', body)
        self.assertIn('svc->setProperty("ATY,FamilyName", d);', body)
        self.assertIn('svc->setProperty("ATY,DeviceName", d);', body)
        self.assertIn('OSData::withBytes(kFamily, sizeof(kFamily))', body)  # NUL included, like EFI data


if __name__ == '__main__':
    unittest.main()
