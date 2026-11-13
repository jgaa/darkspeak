"""Faults and privacy against production services over private real Tor."""
import asyncio
import hashlib
import os
import signal
import time

from harness import ASSETS, CheckFailure, wait_for
from scenario import expect, FILE_DONE, FILE_OFFERED


async def setup(runtime, results, *, identities=1, private=False, previews=(False, False), video_previews=(False, False)):
    await results.check("private_network_configuration", runtime.configure_tor, fatal=True)
    # Configure before launch, without introducing a production management API.
    from harness import Peer
    clients = [node for node in runtime.nodes if node["client"]]
    for label, node, preview, video in zip(("A", "B"), clients, previews, video_previews):
        peer = Peer(runtime, label, node)
        peer.bootstrap_options = dict(identity_count=identities, public_bio=not private, preview_images=preview, preview_videos=video)
        runtime.peers.append(peer)
        await results.check(label + "_application_ready", peer.start, fatal=True)
    a, b = runtime.peers
    await results.check("private_tor_ready", runtime.start_tor, fatal=True)
    for peer in (a, b):
        await peer.controller.call("controller_online")
    async def ready():
        states = [await p.state() for p in (a, b)]
        return states if all(len(s["identities"]) == identities and all(
            i["online"] and i["sharing_id"] for i in s["identities"]) for s in states) else False
    await results.check("all_identity_onions_ready", lambda: wait_for(ready, 90), fatal=True)
    return a, b


async def identity(peer, index):
    return next(i for i in (await peer.state())["identities"] if i["identity"] == peer.identities[index])


async def connect(a, b, index=0, *, before_accept=None):
    aid, bid = await identity(a, index), await identity(b, index)
    added = await a.approved("contact_add", identity=aid["identity"], sharing_id=bid["sharing_id"],
                             name="Regression peer", message="Regression connection request")
    async def pending():
        rows = (await b.call("contact_requests", identity=bid["identity"]))["items"]
        return rows if len(rows) == 1 else False
    rows = await wait_for(pending)
    if before_accept:
        await before_accept(added["reference"])
    await b.approved("contact_approve", identity=bid["identity"], expected_request_handle=aid["handle"], request=rows[0]["request"])
    async def connected():
        contacts = []
        for peer, owner in ((a, aid), (b, bid)):
            matching = [c for c in (await peer.state())["contacts"] if c["identity"] == owner["identity"]]
            if len(matching) != 1 or matching[0]["state"] != 2 or not matching[0]["online"]:
                return False
            contacts.append(matching[0]["contact"])
        return contacts
    return await wait_for(connected)


async def rejected(action):
    try:
        await action()
    except CheckFailure as error:
        expect(str(error) == "AccessDenied")
    else:
        raise CheckFailure("ExpectedRejectionMissing")


async def transfer_state(peer, transfer):
    return next((f for f in (await peer.state())["transfers"] if f["transfer"] == transfer), None)


async def offer(a, b, ac, bc, asset="earth.jpg", index=0):
    sent = await a.approved("file_offer", identity=a.identities[index], contact=ac, asset=asset)
    transfer = sent["reference"]
    async def offered():
        f = await transfer_state(b, transfer)
        return f and f["state"] == FILE_OFFERED
    await wait_for(offered, 30)
    return transfer


async def completed(a, b, transfer, digest, size, *, resume=False):
    async def done():
        files = [await transfer_state(p, transfer) for p in (a, b)]
        # Resume admission completes before the remote peer processes the
        # request. Its original FAILED state is expected until negotiation.
        # A nonzero observed resume offset proves that negotiation occurred.
        if any(f and (f["state"] in (8, 9) or (f["state"] == 7 and
                   (not resume or f.get("resumed_from", 0) > 0))) for f in files):
            raise CheckFailure("TransferFailed")
        return all(f and f["state"] == FILE_DONE for f in files)
    await wait_for(done, 90)
    result = await b.controller.call("controller_hash", dict(transfer=transfer))
    expect(result["sha256"] == digest and result["size"] == size)


async def message(a, b, ac, bc, index, text):
    sent = await a.approved("message_send", identity=a.identities[index], contacts=[ac], text=text)
    async def received():
        rows = (await b.call("conversations", identity=b.identities[index]))["items"]
        conversation = next((c for c in rows if c["contact"] == bc), None)
        if not conversation:
            return False
        items = (await b.approved("messages", identity=b.identities[index], conversation=conversation["conversation"]))["items"]
        return any(m["message"] in sent["messages"] and m["text"] == text and m["direction"] == 1 for m in items)
    await wait_for(received)


async def privacy_multi(runtime, results):
    a, b = await setup(runtime, results, identities=2, private=True)
    async def independent():
        items = [i for p in (a, b) for i in (await p.state())["identities"]]
        expect(len({i["identity"] for i in items}) == 4 and len({i["public_key"] for i in items}) == 4)
        for p in (a, b):
            # Credentials are independently scoped, not just filtered by the caller.
            await rejected(lambda p=p: p.identity_clients[0].call("contacts", dict(identity=p.identities[1])))
    await results.check("four_independent_identities_and_scoped_keys", independent, fatal=True)
    pairs = []
    for index in range(2):
        for reader, subject in ((a, b), (b, a)):
            async def hidden(reader=reader, subject=subject, index=index):
                subject_id = await identity(subject, index)
                before = subject_id["bio_wire"]
                await reader.approved("bio_probe", identity=reader.identities[index], sharing_id=subject_id["sharing_id"])
                async def probe_connected():
                    data = await reader.approved("bio_preview", identity=reader.identities[index], sharing_id=subject_id["sharing_id"])
                    expect(data["bio"] == "")
                    return data["has_avatar"]
                # Public avatar receipt proves a real authenticated profile exchange.
                await wait_for(probe_connected, 30)
                deadline = time.monotonic() + 3
                while time.monotonic() < deadline:
                    data = await reader.approved("bio_preview", identity=reader.identities[index], sharing_id=subject_id["sharing_id"])
                    expect(data["bio"] == "")
                wire = (await identity(subject, index))["bio_wire"]
                expect(wire["responses"] > before["responses"])
                expect(wire["content_frames"] == before["content_frames"])
                expect(wire["hash_frames"] == before["hash_frames"])
                expect(wire["forbidden_frames"] == 0)
            await results.check(f"{reader.label}{index}_restricted_bio_hidden_before_approval", hidden)
        async def pending_hidden(contact, index=index):
            data = await a.approved("profile", identity=a.identities[index], contact=contact)
            expect(not data["available"] and not data["bio"])
        pair = await results.check(f"pair{index}_pending_bio_hidden_then_accept", lambda index=index: connect(a, b, index, before_accept=pending_hidden), fatal=True)
        pairs.append(pair)
        for peer, contact, remote in ((a, pair[0], "Peer B — Lunar explorer."), (b, pair[1], "Peer A — Earth observer.")):
            async def visible(peer=peer, contact=contact, remote=remote, index=index):
                async def read():
                    data = await peer.approved("profile", identity=peer.identities[index], contact=contact)
                    return data["available"] and data["bio"] == remote + f" Identity {index + 1}."
                await wait_for(read)
            await results.check(f"{peer.label}{index}_restricted_bio_visible_after_approval", visible)
        await results.check(f"pair{index}_message_routing", lambda index=index, pair=pair: message(a, b, *pair, index, f"Private route {index}"))
        async def media(index=index, pair=pair):
            transfer = await offer(a, b, *pair, index=index)
            await b.approved("file_accept", identity=b.identities[index], contact=pair[1], transfer=transfer)
            asset = runtime.assets[0]
            await completed(a, b, transfer, asset["sha256"], asset["size_bytes"])
            expect((await transfer_state(b, transfer))["identity"] == b.identities[index])
        await results.check(f"pair{index}_file_routing_and_integrity", media)
    async def isolation():
        for peer, side in ((a, 0), (b, 1)):
            for index in range(2):
                rows = (await peer.call("contacts", identity=peer.identities[index]))["items"]
                expect(len(rows) == 1 and rows[0]["contact"] == pairs[index][side])
                await rejected(lambda peer=peer, index=index, side=side: peer.approved("profile", identity=peer.identities[index], contact=pairs[1-index][side]))
                expect((await peer.state())["pending"] == 0)
    await results.check("contacts_profiles_and_transfers_cannot_cross_identities", isolation)
    async def wire_privacy():
        for peer in (a, b):
            for owner in (await peer.state())["identities"]:
                stats = owner["bio_wire"]
                expect(stats["forbidden_frames"] == 0)
                expect(stats["content_frames"] > 0 and stats["hash_frames"] > 0)
    await results.check("restricted_bio_wire_content_and_hash_only_after_approval", wire_privacy)

    async def persisted():
        for peer, side in ((a, 0), (b, 1)):
            original = {i["identity"]: i["public_key"] for i in (await peer.state())["identities"]}
            await peer.restart()
            current = {i["identity"]: i["public_key"] for i in (await peer.state())["identities"]}
            expect(current == original and len(current) == 2)
            for index in range(2):
                rows = (await peer.call("contacts", identity=peer.identities[index]))["items"]
                expect(len(rows) == 1 and rows[0]["contact"] == pairs[index][side])
    await results.check("multiple_identity_contacts_survive_restart", persisted)


def large_asset(peer):
    # Reproducible JPEG with trailing deterministic bytes; no runtime downloads.
    # 16 MiB gives the real Tor transfer enough work to interrupt after progress.
    data = (ASSETS / "earth.jpg").read_bytes()
    block = hashlib.sha256(b"DarkSpeak resume regression fixture v1").digest() * 2048
    size = 16 * 1024 * 1024
    with (peer.profile / "inputs/resume.jpg").open("wb") as output:
        output.write(data)
        remaining = size - len(data)
        while remaining:
            chunk = block[:min(remaining, len(block))]
            output.write(chunk)
            remaining -= len(chunk)
    return hashlib.sha256((peer.profile / "inputs/resume.jpg").read_bytes()).hexdigest(), size


async def partial(a, b, ac, bc):
    digest, size = large_asset(a)
    transfer = await offer(a, b, ac, bc, "resume.jpg")
    # Start accepting in parallel so polling can observe progress before the
    # approval HTTP reply returns. SIGSTOP is external to the application.
    acceptance = asyncio.create_task(b.approved("file_accept", contact=bc, transfer=transfer))
    try:
        async def progressed():
            file = await transfer_state(b, transfer)
            if file and file["state"] == FILE_DONE:
                raise CheckFailure("InterruptionMissedTransferCompleted")
            return file if file and 0 < file["bytes"] < size else False
        progress = await wait_for(progressed, 45)
        os.kill(a.process.pid, signal.SIGSTOP)
        await acceptance
        expect(progress["bytes"] > 0)
        return transfer, digest, size
    finally:
        if not acceptance.done():
            acceptance.cancel()
        await asyncio.gather(acceptance, return_exceptions=True)


async def resume_crash(runtime, results, *, receiver_crash=False):
    a, b = await setup(runtime, results)
    ac, bc = await results.check("accepted_pair", lambda: connect(a, b), fatal=True)
    transfer, digest, size = await results.check("interrupt_after_nonzero_file_progress", lambda: partial(a, b, ac, bc), fatal=True)
    victim = b if receiver_crash else a
    await results.check("crash_preserves_isolated_profile", victim.crash, fatal=True)
    if receiver_crash:
        os.kill(a.process.pid, signal.SIGCONT)
    async def failed():
        f = await transfer_state(a if receiver_crash else b, transfer)
        return f if f and f["state"] == 7 else False
    saved = await results.check("survivor_retains_resumable_record", lambda: wait_for(failed), fatal=True)
    if not receiver_crash:
        await results.assertion("receiver_keeps_nonzero_partial", 0 < saved["bytes"] < size)
    original = victim.identity
    await results.check("crashed_process_restart", victim.start, fatal=True)
    await victim.controller.call("controller_online")
    await results.assertion("restart_preserves_identity", victim.identity == original)
    async def connected():
        for p, target in ((a, ac), (b, bc)):
            if not any(c["contact"] == target and c["online"] for c in (await p.state())["contacts"]):
                return False
        return True
    await results.check("reconnect_after_crash", lambda: wait_for(connected, 90), fatal=True)
    if receiver_crash:
        async def restored_partial():
            # Loading through the ordinary domain accessor performs recovery.
            files = (await b.approved("files", contact=bc))["items"]
            f = next(f for f in files if f["transfer"] == transfer)
            expect(f["state"] == 7 and 0 < f["bytes"] < size)
        await results.check("receiver_restart_preserves_partial", restored_partial, fatal=True)
    await results.check("resume_existing_incoming_transfer", lambda: b.approved("file_resume", contact=bc, transfer=transfer), fatal=True)
    await results.check("resumed_file_full_sha256", lambda: completed(a, b, transfer, digest, size, resume=True))
    async def unique():
        for p in (a, b):
            files = [f for f in (await p.state())["transfers"] if f["transfer"] == transfer]
            expect(len(files) == 1 and 0 < files[0]["resumed_from"] < size)
    await results.check("resume_does_not_duplicate_transfer", unique)
    await results.check("messaging_after_recovery", lambda: message(a, b, ac, bc, 0, "Recovered after sender crash"))


async def delete_contacts(runtime, results, *, delete_sender=False):
    a, b = await setup(runtime, results, identities=2)
    first = await results.check("active_pair", lambda: connect(a, b, 0), fatal=True)
    second = await results.check("independent_idle_pair", lambda: connect(a, b, 1), fatal=True)
    transfer, _, _ = await results.check("active_transfer_nonzero_partial", lambda: partial(a, b, *first), fatal=True)
    target, survivor = (a, b) if delete_sender else (b, a)
    side = 0 if delete_sender else 1
    async def remove_active():
        if delete_sender:
            # Freeze the receiver before unfreezing the sender so no ACK window
            # can finish the transfer while sender-side deletion is approved.
            os.kill(b.process.pid, signal.SIGSTOP)
            os.kill(a.process.pid, signal.SIGCONT)
        try:
            await target.approved("contact_remove", contact=first[side])
        finally:
            os.kill(survivor.process.pid, signal.SIGCONT)
        state = await target.state()
        expect(not any(c["contact"] == first[side] for c in state["contacts"]))
        expect(not any(f["transfer"] == transfer for f in state["transfers"]))
        expect(not (await target.call("conversations"))["items"])
        await rejected(lambda: target.approved("file_accept", contact=first[side], transfer=transfer))
    await results.check("delete_contact_during_active_transfer", remove_active)
    async def stopped():
        f = await transfer_state(survivor, transfer)
        return f and f["state"] in (7, 8, 9)
    await results.check("survivor_observes_deleted_peer_disconnect", lambda: wait_for(stopped))
    await results.check("other_identity_remains_usable", lambda: message(a, b, *second, 1, "Unaffected identity"))
    await results.check("delete_idle_contact", lambda: target.approved("contact_remove", identity=target.identities[1], contact=second[side]))
    async def absent():
        state = await target.state()
        expect(not state["contacts"] and not state["transfers"] and state["pending"] == 0)
        for owner in target.identities:
            expect(not (await target.call("conversations", identity=owner))["items"])
    await results.check("all_deleted_contact_records_removed", absent)
    await results.check("restart_after_deletion", target.restart, fatal=True)
    await results.check("deleted_contacts_do_not_resurrect", absent)


async def receiver_crash(runtime, results):
    await resume_crash(runtime, results, receiver_crash=True)


async def delete_sender(runtime, results):
    await delete_contacts(runtime, results, delete_sender=True)


SCENARIOS = {"receiver_crash": receiver_crash, "delete_sender": delete_sender, "privacy_multi": privacy_multi, "resume_crash": resume_crash, "delete_contacts": delete_contacts}
