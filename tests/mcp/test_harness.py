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


class TransferPeer:
    """Bounded state snapshots for the resume-admission race regression."""
    def __init__(self, states):
        self.states = list(states)
        self.controller = self
        self.hashed = False

    async def state(self):
        state, offset = self.states[0]
        if len(self.states) > 1:
            self.states.pop(0)
        return dict(transfers=[dict(transfer="a" * 32, state=state, resumed_from=offset)])

    async def call(self, name, args):
        assert name == "controller_hash" and args == dict(transfer="a" * 32)
        self.hashed = True
        return dict(sha256="d" * 64, size=16384)


@pytest.mark.asyncio
async def test_resume_admission_waits_for_remote_failed_state_to_advance():
    from error_scenarios import completed
    sender = TransferPeer([(7, 0), (5, 8192), (6, 8192)])
    receiver = TransferPeer([(5, 8192), (5, 8192), (6, 8192)])
    await completed(sender, receiver, "a" * 32, "d" * 64, 16384, resume=True)
    assert receiver.hashed


@pytest.mark.asyncio
@pytest.mark.parametrize("resume,offset", [(False, 0), (True, 8192)])
async def test_real_transfer_failure_is_reported_immediately(resume, offset):
    from error_scenarios import completed
    sender = TransferPeer([(7, offset)])
    receiver = TransferPeer([(5, 8192)])
    with pytest.raises(CheckFailure, match="^TransferFailed$"):
        await completed(sender, receiver, "a" * 32, "d" * 64, 16384, resume=resume)
    assert not receiver.hashed


@pytest.mark.asyncio
@pytest.mark.parametrize("kind", ["valid", "running", "symlink", "hardlink", "shared_directory", "missing_marker"])
async def test_partial_fault_is_confined_to_a_stopped_private_test_profile(kind, tmp_path):
    import os
    import sqlite3
    import uuid
    from types import SimpleNamespace
    from file_error_scenarios import damage_stopped_partial
    runtime = Runtime(sys.executable, Path(__file__).parent)
    profile = runtime.root / "B"
    directory = profile / "data/downloads/Managed-v1/.filemanager/staging"
    try:
        profile.mkdir(mode=0o700)
        current = profile
        for part in ("data", "downloads", "Managed-v1", ".filemanager", "staging"):
            current = current / part
            current.mkdir(mode=0o700)
        marker = profile / ".darkspeak-test-profile-v1"
        marker.write_text("darkspeak-mcp-test-v1\n")
        pending = uuid.uuid4()
        stage = directory / str(pending)
        expected = b"x" * 8192
        stage.write_bytes(expected)
        stage.chmod(0o600)
        with sqlite3.connect(profile / "data/application.sqlite") as db:
            db.execute("CREATE TABLE file(file_id BLOB, direction INTEGER, pending_id TEXT, bytes_transferred INTEGER)")
            db.execute("INSERT INTO file VALUES(?,?,?,?)", (bytes.fromhex("a" * 32), 1, str(pending), 8192))
        external = tmp_path / "outside-fixture"
        external.write_bytes(expected)
        external.chmod(0o600)
        peer = SimpleNamespace(profile=profile, runtime=runtime, process=SimpleNamespace(returncode=0))
        if kind == "running":
            peer.process.returncode = None
        elif kind == "symlink":
            stage.unlink()
            stage.symlink_to(external)
        elif kind == "hardlink":
            os.link(stage, tmp_path / "alias")
        elif kind == "shared_directory":
            (profile / "data").chmod(0o750)
        elif kind == "missing_marker":
            marker.unlink()
        if kind == "valid":
            damage_stopped_partial(peer, "a" * 32)
            assert stage.read_bytes() == bytes([ord("x") ^ 0xff]) + expected[1:]
        else:
            with pytest.raises(FatalFailure):
                damage_stopped_partial(peer, "a" * 32)
            assert stage.read_bytes() == expected
        assert external.read_bytes() == expected
    finally:
        await runtime.close()
