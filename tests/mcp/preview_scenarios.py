"""Opt-in image previews over signed manifests and ordinary file channels."""
import hashlib
from pathlib import Path
from harness import wait_for
from scenario import expect
from error_scenarios import setup, connect, offer, transfer_state, completed, message


def provision_asset(runtime, peers, video):
    if not video:
        asset = runtime.assets[0]
        return asset["path"], asset["sha256"], asset["size_bytes"]
    source = Path(__file__).resolve().parents[1] / "fixtures/media-preview-v1/blue.mkv"
    data = source.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    expect(digest == "bf6dc4e2dd82e6f5c3cbfe0472fde2d064b174c5ace2e430dd116bc49cd0f6b2")
    for peer in peers:
        with (peer.profile / "inputs/blue.mkv").open("xb") as output:
            output.write(data)
    return "blue.mkv", digest, len(data)


async def image_previews(runtime, results, *, video=False):
    a, b = await setup(runtime, results, previews=(not video, not video), video_previews=(video, video))
    asset, digest, size = provision_asset(runtime, (a, b), video)
    ac, bc = await results.check("approved_preview_peers", lambda: connect(a, b), fatal=True)
    for sender, receiver, sc, rc in ((a, b, ac, bc), (b, a, bc, ac)):
        transfer = await results.check(sender.label + "_primary_offered",
            lambda: offer(sender, receiver, sc, rc, asset), fatal=True)
        async def preview_before_accept():
            async def ready():
                state = await receiver.state()
                thumbnails = [f for f in state["transfers"] if f.get("thumbnail_for") == transfer]
                primary = next(f for f in state["transfers"] if f["transfer"] == transfer)
                expect(primary["state"] == 3 and primary["bytes"] == 0)
                return thumbnails[0] if len(thumbnails) == 1 and thumbnails[0]["state"] == 6 and primary["preview_available"] else False
            thumbnail = await wait_for(ready, 45)
            expect(0 < thumbnail["size"] <= 48 * 1024)
            digest = await receiver.controller.call("controller_hash", dict(transfer=thumbnail["transfer"]))
            expect(digest["size"] == thumbnail["size"])
            storage = await receiver.controller.call("controller_storage", dict(transfer=transfer))
            expect(not storage["published"] and not storage["pending"])
        await results.check(sender.label + "_thumbnail_arrives_without_primary_download", preview_before_accept)
        async def original():
            await receiver.approved("file_accept", identity=receiver.identity, contact=rc, transfer=transfer)
            await completed(sender, receiver, transfer, digest, size)
        await results.check(sender.label + "_original_remains_independently_acceptable", original)
    await results.check("text_after_preview_transfers", lambda: message(a, b, ac, bc, 0, "Preview transfers preserve messaging"))
    async def restart():
        await b.restart()
        async def cached():
            files = [f for f in (await b.state())["transfers"] if f["direction"] == 1 and not f["thumbnail_for"]]
            return len(files) == 1 and files[0]["state"] == 6 and files[0]["preview_available"]
        await wait_for(cached, 15)
    await results.check("private_preview_cache_survives_restart", restart)


async def image_previews_disabled(runtime, results, *, video=False):
    a, b = await setup(runtime, results, previews=(not video, False), video_previews=(video, False))
    asset, digest, size = provision_asset(runtime, (a, b), video)
    ac, bc = await results.check("approved_preview_peers", lambda: connect(a, b), fatal=True)
    transfer = await results.check("primary_offered", lambda: offer(a, b, ac, bc, asset), fatal=True)
    async def rejected_preview():
        async def rejected():
            rows = [f for f in (await a.state())["transfers"] if f.get("thumbnail_for") == transfer]
            return len(rows) == 1 and rows[0]["state"] == 8 and rows[0]["bytes"] == 0
        await wait_for(rejected, 20)
        state = await b.state()
        expect(not any(f.get("thumbnail_for") == transfer for f in state["transfers"]))
        primary = await transfer_state(b, transfer)
        expect(primary["state"] == 3 and primary["bytes"] == 0 and not primary["preview_available"])
    await results.check("disabled_previews_are_neither_fetched_nor_rendered", rejected_preview)
    async def original():
        await b.approved("file_accept", identity=b.identity, contact=bc, transfer=transfer)
        await completed(a, b, transfer, digest, size)
        expect(not (await transfer_state(b, transfer))["preview_available"])
    await results.check("preview_rejection_does_not_reject_original", original)


async def video_previews(runtime, results):
    await image_previews(runtime, results, video=True)


async def video_previews_disabled(runtime, results):
    await image_previews_disabled(runtime, results, video=True)


PREVIEW_SCENARIOS = {"image_previews": image_previews, "image_previews_disabled": image_previews_disabled,
                     "video_previews": video_previews, "video_previews_disabled": video_previews_disabled}
