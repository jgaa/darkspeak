"""P2-001: deterministic checks against real isolated application processes."""
import asyncio
import time
import httpx2
from harness import CheckFailure, FatalFailure, wait_for

# Domain enums from Contact::ContactState, File::State and Message::State.
CONTACT_ACCEPTED = 2
FILE_OFFERED = 3
FILE_DONE = 6
FILE_FAILURES = (7, 8, 9)


def expect(condition):
    if not condition:
        raise CheckFailure("ExpectationFailed")


class Results:
    def __init__(self):
        self.checks = []
        self.consecutive_failures = 0

    async def check(self, label, action, *, fatal=False):
        started = time.monotonic()
        try:
            result = await action()
        except asyncio.CancelledError:
            raise
        except Exception as error:
            # Exception payloads can contain private state or SDK request dumps.
            # CheckFailure messages are fixed codes produced by this test library.
            code = str(error) if isinstance(error, CheckFailure) else type(error).__name__
            self.consecutive_failures += 1
            is_fatal = fatal or isinstance(error, FatalFailure) or self.consecutive_failures >= 3
            self.checks.append(dict(check=label, outcome="fatal" if is_fatal else "failed", code=code,
                                    elapsed_ms=round((time.monotonic() - started) * 1000)))
            print(f"{'FATAL' if is_fatal else 'FAIL'} {label}: {code}", flush=True)
            if is_fatal:
                raise FatalFailure("PrerequisiteOrRepeatedFailure") from None
            return None
        self.consecutive_failures = 0
        self.checks.append(dict(check=label, outcome="passed", elapsed_ms=round((time.monotonic() - started) * 1000)))
        print(f"PASS {label}", flush=True)
        return result

    async def assertion(self, label, condition):
        async def action():
            expect(condition)
        return await self.check(label, action)


async def happy_path(runtime, results):
    await results.check("fixtures_and_private_tor_configuration", runtime.configure_tor, fatal=True)
    a, b = await results.check("independent_application_readiness", runtime.start_peers, fatal=True)
    states = [await a.state(), await b.state()]
    await results.assertion("one_distinct_identity_per_instance", all(len(s["identities"]) == 1 for s in states)
                            and a.identity != b.identity
                            and states[0]["identities"][0]["public_key"] != states[1]["identities"][0]["public_key"])
    for peer in (a, b):
        async def catalog(peer=peer):
            discovery = await peer.ordinary.session.discover()
            names = [tool.name for tool in (await peer.ordinary.session.list_tools()).tools]
            expect("2026-07-28" in discovery.supported_versions)
            expect("message_send" in names and not any("controller" in name for name in names))
            controller_names = [tool.name for tool in (await peer.controller.session.list_tools()).tools]
            expect("controller_decide" in controller_names and "message_send" not in controller_names)
        await results.check(f"{peer.label}_transport_and_catalog_isolation", catalog, fatal=True)
    async def credential_boundaries():
        attempts = ((a.controller, a.ordinary), (a.ordinary, a.controller),
                    (a.controller, b.controller), (a.ordinary, b.ordinary))
        async with httpx2.AsyncClient(timeout=5) as http:
            for target, credential in attempts:
                response = await http.post(f"http://127.0.0.1:{target.port}/mcp",
                    headers={"Authorization": "Bearer " + credential.secret}, json={})
                expect(response.status_code == 401)
    await results.check("credentials_cannot_cross_catalogs_or_instances", credential_boundaries, fatal=True)
    await results.check("private_tor_readiness", runtime.start_tor, fatal=True)
    for peer in (a, b):
        await peer.controller.call("controller_online")

    async def onions():
        sa, sb = await a.state(), await b.state()
        aa, bb = sa["identities"][0], sb["identities"][0]
        return (aa, bb) if sa["online"] and sb["online"] and aa["online"] and bb["online"] and aa["sharing_id"] and bb["sharing_id"] else False
    aid, bid = await results.check("both_onion_services_ready", lambda: wait_for(onions, 90), fatal=True)
    await results.assertion("distinct_onion_sharing_information", aid["sharing_id"] != bid["sharing_id"])

    async def preview(reader, subject, text):
        await reader.approved("bio_probe", sharing_id=subject["sharing_id"])
        async def received():
            return (await reader.approved("bio_preview", sharing_id=subject["sharing_id"]))["bio"] == text
        await wait_for(received)

    await results.check("A_previews_B_public_bio", lambda: preview(a, bid, "Peer B — Lunar explorer."))
    await results.assertion("preview_creates_no_contacts_or_conversations",
        not (await a.call("contacts"))["items"] and not (await b.call("contacts"))["items"]
        and not (await a.call("conversations"))["items"] and not (await b.call("conversations"))["items"]
        and not (await b.call("contact_requests"))["items"])
    added = await results.check("A_sends_connection_request", lambda: a.approved("contact_add",
        sharing_id=bid["sharing_id"], name="Peer B", message="P2-001 connection greeting"), fatal=True)
    a.contact = added["reference"]

    async def incoming():
        requests = (await b.call("contact_requests"))["items"]
        return requests if len(requests) == 1 else False
    requests = await results.check("B_observes_one_pending_request", lambda: wait_for(incoming), fatal=True)
    await results.check("B_previews_A_public_bio", lambda: preview(b, aid, "Peer A — Earth observer."))
    await results.check("B_accepts_connection_request", lambda: b.approved("contact_approve",
        expected_request_handle=aid["handle"], request=requests[0]["request"]), fatal=True)

    async def established():
        sa, sb = await a.state(), await b.state()
        return (sa, sb) if all(len(s["contacts"]) == 1 and s["contacts"][0]["state"] == CONTACT_ACCEPTED and s["contacts"][0]["online"] for s in (sa, sb)) else False
    connected = await results.check("both_connections_established", lambda: wait_for(established), fatal=True)
    b.contact = connected[1]["contacts"][0]["contact"]
    await results.assertion("pending_requests_cleared", not (await a.call("contact_requests"))["items"] and not (await b.call("contact_requests"))["items"])

    for peer, remote, text in ((a, bid, "Peer B — Lunar explorer."), (b, aid, "Peer A — Earth observer.")):
        async def profile(peer=peer, text=text):
            async def available():
                data = await peer.approved("profile", contact=peer.contact)
                return data.get("available") and data.get("bio") == text
            await wait_for(available)
        await results.check(f"{peer.label}_receives_remote_bio", profile)
        async def avatars(peer=peer, remote=remote):
            async def available():
                contact = (await peer.state())["contacts"][0]
                return contact["avatar_hash"] == remote["avatar_hash"] and contact["avatar_width"] > 0 and contact["avatar_width"] <= 128 and contact["avatar_height"] <= 128
            await wait_for(available)
        await results.check(f"{peer.label}_receives_normalized_remote_avatar", avatars)

    for sender, receiver, text in ((a, b, "Hello B — P2-001/A"), (b, a, "Hello A — P2-001/B")):
        async def message(sender=sender, receiver=receiver, text=text):
            sent = await sender.approved("message_send", contacts=[sender.contact], text=text)
            async def arrived():
                conversations = (await receiver.call("conversations"))["items"]
                if len(conversations) != 1:
                    return False
                receiver.conversation = conversations[0]["conversation"]
                messages = (await receiver.approved("messages", conversation=receiver.conversation))["items"]
                return any(m["text"] == text and m["direction"] == 1 and m["message"] in sent["messages"] for m in messages)
            await wait_for(arrived)
        await results.check(f"{sender.label}_to_{receiver.label}_text_received", message)

    for sender, receiver in ((a, b), (b, a)):
        for asset in runtime.assets:
            async def transfer(sender=sender, receiver=receiver, asset=asset):
                offered = await sender.approved("file_offer", contact=sender.contact, asset=asset["path"])
                transfer_id = offered["reference"]
                async def incoming_file():
                    outgoing = [f for f in (await sender.state())["transfers"] if f["transfer"] == transfer_id]
                    if outgoing and outgoing[0]["state"] in FILE_FAILURES:
                        raise CheckFailure("OutgoingTransferFailed")
                    state = await receiver.state()
                    return any(f["transfer"] == transfer_id and f["direction"] == 1 and f["state"] == FILE_OFFERED for f in state["transfers"])
                try:
                    await wait_for(incoming_file, 20)
                except CheckFailure as error:
                    if str(error) == "DeadlineExceeded":
                        raise CheckFailure("FileOfferDeadlineExceeded") from None
                    raise
                await receiver.approved("file_accept", contact=receiver.contact, transfer=transfer_id)
                async def finished():
                    for peer in (sender, receiver):
                        matching = [f for f in (await peer.state())["transfers"] if f["transfer"] == transfer_id]
                        if not matching:
                            return False
                        if matching[0]["state"] in FILE_FAILURES:
                            raise CheckFailure("TransferFailed")
                        if matching[0]["state"] != FILE_DONE:
                            return False
                    return True
                try:
                    await wait_for(finished, 40)
                except CheckFailure as error:
                    if str(error) == "DeadlineExceeded":
                        raise CheckFailure("FileCompletionDeadlineExceeded") from None
                    raise
                details = (await receiver.approved("files", contact=receiver.contact))["items"]
                file = next(f for f in details if f["transfer"] == transfer_id)
                expect(file["direction"] == 1 and file["size"] == asset["size_bytes"])
                expect(file["mime"] == asset["media_type"])
                content = await receiver.controller.call("controller_hash", dict(transfer=transfer_id))
                expect(content["sha256"] == asset["sha256"] and content["size"] == asset["size_bytes"])
            await results.check(f"{sender.label}_to_{receiver.label}_{asset['id']}_media_integrity", transfer)

    for peer in (a, b):
        async def final(peer=peer):
            state = await peer.state()
            expect(not state["journal_enabled"] and state["pending"] == 0)
            expect(len(state["contacts"]) == 1 and state["contacts"][0]["online"])
            expect(len(state["transfers"]) == 6 and all(f["state"] == FILE_DONE for f in state["transfers"]))
            expect(sum(f["direction"] == 1 for f in state["transfers"]) == 3)
            conversations = (await peer.call("conversations"))["items"]
            expect(len(conversations) == 1 and conversations[0]["contact"] == peer.contact)
            messages = (await peer.approved("messages", conversation=conversations[0]["conversation"]))["items"]
            expect(len(messages) == 2 and sum(m["direction"] for m in messages) == 1)
            expect(all(m["state"] == 3 for m in messages))
            expect(not (await peer.call("contact_requests"))["items"])
            events = await peer.call("events")
            expect(bool(events["items"]))
        await results.check(f"{peer.label}_final_state_and_events", final)
