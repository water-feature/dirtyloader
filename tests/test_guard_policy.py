"""Exercise the shared boot decision without stock firmware or an ARM compiler."""

from pathlib import Path
import shutil
import subprocess
from tempfile import TemporaryDirectory
import unittest


class GuardPolicyTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("cc"), "requires a host C compiler")
    def test_guard_skip_reasons(self):
        include = Path(__file__).resolve().parents[1] / "dirtyloader/core/c"
        cases = """\
#include <assert.h>
#include "guard_policy.h"

int main(void)
{
    assert(guard_skip_reason(0, 0, 0) == SKIP_NONE);
    assert(guard_skip_reason(G_GOOD, 0, 0) == SKIP_NONE);
    assert(guard_skip_reason(G_ARMD, 0, 0) == SKIP_NONE);
    assert(guard_skip_reason(G_SKIP, 0, 0) == SKIP_GUARD);
    assert(guard_skip_reason(G_GOOD, 1, 0) == SKIP_CRASH);
    assert(guard_skip_reason(G_GOOD, 0, 1) == SKIP_HOST);
    assert(guard_skip_reason(G_SKIP, 0, 1) == SKIP_HOST);
    assert(guard_skip_reason(G_SKIP, 1, 1) == SKIP_CRASH);
}
"""
        with TemporaryDirectory() as temp:
            executable = Path(temp) / "guard-policy"
            subprocess.run(
                ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-I", str(include),
                 "-x", "c", "-", "-o", str(executable)],
                input=cases,
                text=True,
                check=True,
            )
            subprocess.run([str(executable)], check=True)
