import unittest

import pr_checks as pc

GOOD_BODY = """## Summary
Adds a retry to the picture worker so one failed download is tried again later.

## Why
Players kept their shirt numbers after a single network error.

## How I tested it
Added a sim test and ran the whole suite; flashed env app on a V2 board and watched the log.

- [x] This change contains no secrets, tokens or Wi-Fi passwords.
"""


class Title(unittest.TestCase):
    def test_good(self):
        self.assertEqual(pc.check_title("feat(ui): retry failed pictures"), [])
        self.assertEqual(pc.check_title("fix: stop the clock freezing at boot"), [])

    def test_bad(self):
        for t in ("update stuff", "Feat: Big change here", "feat(ui):x", "wip", "fix(ui) missing colon",
                  "feat: " + "x" * 120):
            self.assertTrue(pc.check_title(t), t)


class Body(unittest.TestCase):
    def test_good(self):
        self.assertEqual(pc.check_body(GOOD_BODY), [])

    def test_template_left_empty(self):
        template = "## Summary\n<!-- what -->\n\n## Why\n<!-- why -->\n\n## How I tested it\n<!-- how -->\n- [ ] no secrets\n"
        self.assertEqual(len(pc.check_body(template)), 4)

    def test_missing_section(self):
        self.assertTrue(any("Why" in e for e in pc.check_body(GOOD_BODY.replace("## Why", "## Reason"))))

    def test_secrets_box_required(self):
        self.assertTrue(pc.check_body(GOOD_BODY.replace("[x]", "[ ]")))


def run(files, labels=(), body=GOOD_BODY, lines=10, size=lambda h, p: 100):
    return pc.check_files(files, set(labels), body, lines, "HEAD", size_of=size)


class Files(unittest.TestCase):
    def test_ui_change_needs_test(self):
        e, _ = run([("M", "ui/src/store.cpp")])
        self.assertEqual(len(e), 1)
        e, _ = run([("M", "ui/src/store.cpp"), ("A", "sim/tests/store_test.cpp")])
        self.assertEqual(e, [])

    def test_label_waives_tests(self):
        e, _ = run([("M", "ui/src/store.cpp")], labels=["no-tests-needed"])
        self.assertEqual(e, [])

    def test_helper_change_needs_test(self):
        e, _ = run([("M", "helper/src/colors.ts")])
        self.assertEqual(len(e), 1)
        e, _ = run([("M", "helper/src/colors.ts"), ("M", "helper/test/colors.test.ts")])
        self.assertEqual(e, [])

    def test_docs_only_is_fine(self):
        self.assertEqual(run([("M", "README.md")]), ([], []))

    def test_forbidden_files(self):
        for p in ("firmware/app/secrets.ini", ".env", "helper/.env.local", "key.pem", "build/sim/x", "helper.env"):
            e, _ = run([("A", p)])
            self.assertTrue(e, p)

    def test_deleting_a_forbidden_file_is_fine(self):
        self.assertEqual(run([("D", "firmware/app/secrets.ini")])[0], [])

    def test_big_new_file(self):
        e, _ = run([("A", "docs/images/huge.png")], size=lambda h, p: 5_000_000)
        self.assertEqual(len(e), 1)
        e, _ = run([("A", "ui/fonts/big.c")], size=lambda h, p: 5_000_000)
        self.assertEqual(e, [])

    def test_firmware_needs_hardware_note(self):
        _, w = run([("M", "firmware/app/src/main.cpp")], body="## Summary\nx")
        self.assertEqual(len(w), 1)
        _, w = run([("M", "firmware/app/src/main.cpp")])
        self.assertEqual(w, [])

    def test_size_limits(self):
        e, w = run([("M", "README.md")], lines=2000)
        self.assertEqual((e, len(w)), ([], 1))
        e, _ = run([("M", "README.md")], lines=7000)
        self.assertEqual(len(e), 1)
        e, _ = run([("M", "README.md")], lines=7000, labels=["large-change"])
        self.assertEqual(e, [])


if __name__ == "__main__":
    unittest.main()
