"""Pytest entry point; the standalone runner is also used by CTest."""
import os
from pathlib import Path

import pytest

from run_scenario import run


@pytest.mark.asyncio
async def test_two_peer_happy_path(tmp_path):
    instance = os.environ.get("DARKSPEAK_TEST_INSTANCE")
    chutney = os.environ.get("DARKSPEAK_TEST_CHUTNEY")
    if not instance or not chutney:
        pytest.fail("Set DARKSPEAK_TEST_INSTANCE and DARKSPEAK_TEST_CHUTNEY", pytrace=False)
    artifacts = os.environ.get("DARKSPEAK_TEST_ARTIFACTS", str(tmp_path / "artifacts"))
    report = await run(instance, chutney, artifacts)
    failures = [item["check"] for item in report["checks"] if item["outcome"] != "passed"]
    if failures:
        pytest.fail("P2-001 failed checks: " + ", ".join(failures), pytrace=False)
