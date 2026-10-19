"""Object identity must not depend on printable bytes adjacent to names."""
import unittest
from registration import REQUIRED, validate


class RegistrationTests(unittest.TestCase):
    def symbols(self, names=REQUIRED):
        return "\n".join("0005f01c r " + name for name in sorted(names))

    def test_defined_registration_symbols(self):
        validate(self.symbols())

    def test_every_device_init_and_pm_object_is_required(self):
        for missing in REQUIRED:
            with self.subTest(missing=missing), self.assertRaises(ValueError):
                validate(self.symbols(REQUIRED - {missing}))

    def test_suffixes_and_undefined_references_do_not_count(self):
        for missing in REQUIRED:
            base = self.symbols(REQUIRED - {missing})
            for substitute in ("00000001 r " + missing + "_extra", "         U " + missing,
                               "00000001 U " + missing):
                with self.subTest(missing=missing, substitute=substitute), self.assertRaises(ValueError):
                    validate(base + "\n" + substitute)

    def test_strings_and_printable_prefixes_are_not_registration_evidence(self):
        with self.assertRaises(ValueError):
            validate("led_pattern_pm\n7J?YE0R;_BL+led_pattern_pm_late\n")
        validate(self.symbols() + "\n7J?YE0R;_BL+led_pattern_pm_late\n")
