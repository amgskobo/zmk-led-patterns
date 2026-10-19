# SPDX-License-Identifier: MIT
"""Recognize a C assertion abort from the canonical POSIX SDK, not any crash."""
import re
import signal


_ASSERTION = re.compile(
    r"(?m)^[^\r\n]+: [^\r\n]+:\d+: [^\r\n]+: Assertion [\x60'][^\r\n]+['\x60] failed\.$"
)


def is_c_assertion_abort(returncode, stderr):
    return returncode == -signal.SIGABRT and _ASSERTION.search(stderr) is not None
