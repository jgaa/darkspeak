"""Explicit transfer decisions and corruption of an isolated stopped receiver."""
import asyncio
from contextlib import closing
import os
from pathlib import Path
import signal
import sqlite3
import stat
import time
import uuid

from error_scenarios import (completed, connect, expect, message, offer,
                             partial, setup, transfer_state)
from harness import CheckFailure, FatalFailure, wait_for


async def storage(peer, transfer):
    return await peer.controller.call("controller_storage", dict(transfer=transfer))


async def states(a, b, transfer, expected):
    async def reached():
        snapshots = [await transfer_state(peer, transfer) for peer in (a, b)]
        return snapshots if all(f and f["state"] == expected for f in snapshots) else False
    return await wait_for(reached, 25)


async def unpublished(peer, transfer, *, no_pending=False):
    async def inspect():
        result = await storage(peer, transfer)
        expect(not result["published"])
        return not result["pending"] if no_pending else True
    await wait_for(inspect, 15)


async def file_decisions(runtime, results):
    a, b = await setup(runtime, results)
    ac, bc = await results.check("accepted_pair", lambda: connect(a, b), fatal=True)
    records = []
    for sender, receiver, sc, rc in ((a, b, ac, bc), (b, a, bc, ac)):
        async def reject_offer(sender=sender, receiver=receiver, sc=sc, rc=rc):
            transfer = await offer(sender, receiver, sc, rc)
            await receiver.approved("file_reject", contact=rc, transfer=transfer)
            snapshots = await states(sender, receiver, transfer, 8)
            expect(all(f["bytes"] == 0 for f in snapshots))
            await unpublished(receiver, transfer, no_pending=True)
            records.append((sender, receiver, transfer, 8))
        await results.check(f"{sender.label}_offer_rejected_without_content_or_storage", reject_offer)

    for cancel_sender in (False, True):
        label = "sender" if cancel_sender else "receiver"
        transfer, _, _ = await results.check(label + "_cancel_nonzero_active_transfer",
            lambda: partial(a, b, ac, bc), fatal=True)
        async def cancel(cancel_sender=cancel_sender, transfer=transfer):
            try:
                if cancel_sender:
                    os.kill(b.process.pid, signal.SIGSTOP)
                    os.kill(a.process.pid, signal.SIGCONT)
                    await a.approved("file_cancel_outgoing", contact=ac, transfer=transfer)
                else:
                    await b.approved("file_cancel_incoming", contact=bc, transfer=transfer)
            finally:
                os.kill((b if cancel_sender else a).process.pid, signal.SIGCONT)
            await states(a, b, transfer, 9)
            records.append((a, b, transfer, 9))
        await results.check(label + "_cancel_reaches_both_peers", cancel)
        await results.check(label + "_cancel_removes_unpublished_partial",
            lambda transfer=transfer: unpublished(b, transfer, no_pending=True))
        await results.check(label + "_cancel_preserves_messaging",
            lambda label=label: message(a, b, ac, bc, 0, "Message after " + label + " cancellation"))

    await results.check("restart_after_transfer_decisions", a.restart, fatal=True)
    await results.check("restart_receiver_after_transfer_decisions", b.restart, fatal=True)
    async def persisted():
        for sender, receiver, transfer, expected in records:
            await states(sender, receiver, transfer, expected)
            await unpublished(receiver, transfer, no_pending=True)
        expect(len(records) == 4)
    await results.check("rejected_and_cancelled_files_do_not_resurrect", persisted)


def damage_stopped_partial(peer, transfer):
    """Flip one byte through owned, nonsymlink descriptors in this test profile."""
    if peer.process.returncode is None:
        raise FatalFailure("FaultRequiresStoppedApplication")
    profile = peer.profile
    root = peer.runtime.root
    marker = profile / ".darkspeak-test-profile-v1"
    if (root.parent != Path("/tmp") or not root.name.startswith("ds-mcp-") or
            root.resolve() != root or profile.name not in ("A", "B") or
            profile.resolve() != profile or profile.parent != root or marker.is_symlink()):
        raise FatalFailure("UnsafeFaultProfile")
    try:
        if marker.read_bytes() != b"darkspeak-mcp-test-v1\n":
            raise FatalFailure("UnsafeFaultProfile")
        database = profile / "data/application.sqlite"
        if database.resolve() != database:
            raise FatalFailure("UnsafeFaultProfile")
        transfer_bytes = bytes.fromhex(transfer)
        if len(transfer_bytes) != 16:
            raise ValueError
        with closing(sqlite3.connect(f"file:{database}?mode=ro", uri=True)) as db:
            row = db.execute("SELECT pending_id,bytes_transferred FROM file WHERE file_id=? AND direction=1",
                             (transfer_bytes,)).fetchone()
        if not row or row[1] < 8192:
            raise FatalFailure("MissingDurablePartial")
        pending = uuid.UUID(row[0]) if isinstance(row[0], str) else uuid.UUID(bytes=row[0])
        flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
        descriptor = os.open(profile, flags)
        try:
            for component in ("data", "downloads", "Managed-v1", ".filemanager", "staging"):
                info = os.fstat(descriptor)
                if info.st_uid != os.getuid() or info.st_mode & 0o077:
                    raise FatalFailure("UnsafeFaultDirectory")
                child = os.open(component, flags, dir_fd=descriptor)
                os.close(descriptor)
                descriptor = child
            file_descriptor = os.open(str(pending), os.O_RDWR | os.O_NOFOLLOW | os.O_CLOEXEC,
                                      dir_fd=descriptor)
            try:
                info = os.fstat(file_descriptor)
                if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or
                        info.st_nlink != 1 or info.st_mode & 0o077 or info.st_size < 8192):
                    raise FatalFailure("UnsafeFaultFile")
                original = os.pread(file_descriptor, 1, 0)
                if len(original) != 1 or os.pwrite(file_descriptor, bytes([original[0] ^ 0xff]), 0) != 1:
                    raise FatalFailure("FaultWriteFailed")
                os.fsync(file_descriptor)
                if os.fstat(file_descriptor).st_size != info.st_size:
                    raise FatalFailure("FaultChangedFileSize")
            finally:
                os.close(file_descriptor)
        finally:
            os.close(descriptor)
    except FatalFailure:
        raise
    except Exception:
        raise FatalFailure("PartialFaultSetupFailed") from None


async def corrupt_partial(runtime, results):
    a, b = await setup(runtime, results)
    ac, bc = await results.check("accepted_pair", lambda: connect(a, b), fatal=True)
    transfer, _, _ = await results.check("nonzero_durable_partial", lambda: partial(a, b, ac, bc), fatal=True)
    await results.check("stop_receiver_before_external_fault", b.stop, fatal=True)
    try:
        async def damage():
            damage_stopped_partial(b, transfer)
        await results.check("corrupt_one_staging_byte_without_changing_size", damage, fatal=True)
    finally:
        os.kill(a.process.pid, signal.SIGCONT)
    async def sender_failed():
        file = await transfer_state(a, transfer)
        return file and file["state"] == 7
    await results.check("sender_retains_interrupted_record", lambda: wait_for(sender_failed), fatal=True)
    await results.check("restart_corrupted_receiver", b.start, fatal=True)
    await b.controller.call("controller_online")
    async def connected():
        for peer, contact in ((a, ac), (b, bc)):
            if not any(c["contact"] == contact and c["online"] for c in (await peer.state())["contacts"]):
                return False
        return True
    await results.check("reconnect_after_external_fault", lambda: wait_for(connected, 90), fatal=True)
    await results.check("corrupted_partial_is_not_published", lambda: unpublished(b, transfer))
    await results.check("attempt_resume_with_original_metadata", lambda: b.approved("file_resume", contact=bc, transfer=transfer), fatal=True)
    async def rejected_corruption():
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            inspected = await storage(b, transfer)
            expect(not inspected["published"] and inspected["state"] != 6)
            file = await transfer_state(b, transfer)
            if file and file["state"] in (7, 9) and file["resumed_from"] > 0:
                return
            await asyncio.sleep(0.2)
        # A backend may detect a corrupted prefix before negotiating a channel.
        # Observe it for the full bounded window and require a stable failed state.
        file = await transfer_state(b, transfer)
        expect(file and file["state"] == 7 and file["resumed_from"] == 0)
    await results.check("corruption_never_becomes_readable_during_resume", rejected_corruption)
    await results.check("messaging_survives_integrity_failure", lambda: message(a, b, ac, bc, 0, "Message after rejected corruption"))
    await results.check("restart_after_integrity_failure", b.restart, fatal=True)
    await results.check("corrupted_file_stays_unpublished_after_restart", lambda: unpublished(b, transfer))
    await b.controller.call("controller_online")
    await results.check("reconnect_for_fresh_valid_file", lambda: wait_for(connected, 90), fatal=True)
    async def good_transfer():
        valid = await offer(a, b, ac, bc)
        await b.approved("file_accept", contact=bc, transfer=valid)
        asset = runtime.assets[0]
        await completed(a, b, valid, asset["sha256"], asset["size_bytes"])
    await results.check("fresh_valid_transfer_after_corruption", good_transfer)


FILE_SCENARIOS = {"file_decisions": file_decisions, "corrupt_partial": corrupt_partial}
