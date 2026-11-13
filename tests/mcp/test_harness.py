"""Checks of failure policy, bounded waits and cleanup, without Tor/GUI."""
import asyncio
from pathlib import Path
import sys

import pytest

from harness import CheckFailure, FatalFailure, Runtime, wait_for
from scenario import Results


@pytest.mark.asyncio
async def test_recoverable_failure_continues_and_success_resets_streak():
    results = Results()
    async def fail():
        raise CheckFailure("SyntheticFailure")
    async def succeed():
        return 42
    assert await results.check("first", fail) is None
    assert await results.check("second", succeed) == 42
    assert results.consecutive_failures == 0
    assert [item["outcome"] for item in results.checks] == ["failed", "passed"]


@pytest.mark.asyncio
async def test_three_consecutive_failures_abort():
    results = Results()
    async def fail():
        raise CheckFailure("SyntheticFailure")
    await results.check("first", fail)
    await results.check("second", fail)
    with pytest.raises(FatalFailure):
        await results.check("third", fail)
    assert results.checks[-1]["outcome"] == "fatal"


@pytest.mark.asyncio
async def test_fatal_prerequisite_aborts_immediately():
    results = Results()
    async def fail():
        raise CheckFailure("SyntheticFailure")
    with pytest.raises(FatalFailure):
        await results.check("prerequisite", fail, fatal=True)
    assert len(results.checks) == 1


@pytest.mark.asyncio
async def test_exception_payload_is_not_retained():
    results = Results()
    async def fail():
        raise RuntimeError("credential=secret-test-marker; private message")
    await results.check("redaction", fail)
    assert results.checks[0]["code"] == "RuntimeError"
    assert "secret-test-marker" not in str(results.checks)


@pytest.mark.asyncio
async def test_wait_deadline_and_cancellation():
    async def never():
        return False
    with pytest.raises(CheckFailure, match="DeadlineExceeded"):
        await wait_for(never, 0.02)
    task = asyncio.create_task(wait_for(never, 30))
    await asyncio.sleep(0)
    task.cancel()
    with pytest.raises(asyncio.CancelledError):
        await task


@pytest.mark.asyncio
async def test_cleanup_reaps_real_process_and_removes_isolated_root():
    runtime = Runtime(sys.executable, Path(__file__).parent)
    process = await runtime.launch(sys.executable, "-c", "import time; time.sleep(60)")
    await runtime.close()
    assert process.returncode is not None
    assert not runtime.root.exists()
