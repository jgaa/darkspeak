"""Real isolated application services; Tor need not be started for these checks."""
import asyncio
import os
import json
from pathlib import Path

import pytest

from harness import Peer, Runtime, secure_process, wait_for


def instance_path():
    value = os.environ.get("DARKSPEAK_TEST_INSTANCE")
    if not value:
        pytest.fail("DARKSPEAK_TEST_INSTANCE is required", pytrace=False)
    return value


@pytest.mark.asyncio
async def test_readiness_without_tor_and_restart_preserves_identity_and_avatar():
    secure_process()
    runtime = Runtime(instance_path(), Path(__file__).parent)
    peer = Peer(runtime, "A", dict(control=runtime.port(), socks=runtime.port()))
    runtime.peers.append(peer)
    try:
        await peer.start()
        fixture = json.loads((Path(__file__).resolve().parents[1] / "fixtures/mcp/test-bootstrap-v1.json").read_text())
        assert fixture["version"] == 1
        assert (peer.profile / ".darkspeak-test-profile-v1").read_text() == fixture["marker"]
        assert set(json.loads((peer.profile / "bootstrap.json").read_text())) == set(fixture["bootstrap_fields"])
        assert peer.provision_fields == set(fixture["provision_fields"])
        assert {tool.name for tool in (await peer.controller.session.list_tools()).tools} == set(fixture["controller_tools"])
        state = await peer.state()
        assert state["application_ready"] and not state["online"]
        identity = state["identities"][0]
        assert identity["avatar_hash"] and identity["avatar_width"] == 128
        await peer.restart()
        async def restored():
            current = (await peer.state())["identities"][0]
            return current if current["avatar_hash"] else False
        current = await wait_for(restored, 5)
        assert current["identity"] == identity["identity"]
        assert current["public_key"] == identity["public_key"]
        assert current["avatar_hash"] == identity["avatar_hash"]
    finally:
        await runtime.close()
    assert not runtime.root.exists()


@pytest.mark.asyncio
@pytest.mark.parametrize("kind", ["missing_activation", "missing_marker", "shared_root", "symlink", "production_profile"])
async def test_unsafe_bootstrap_is_rejected_before_services(kind, tmp_path):
    secure_process()
    runtime = Runtime(instance_path(), Path(__file__).parent)
    profile = runtime.root / "A"
    profile.mkdir(mode=0o700)
    if kind == "shared_root":
        profile.chmod(0o755)
    if kind == "symlink":
        link = runtime.root / "link"
        link.symlink_to(profile, target_is_directory=True)
        profile = link
    if kind == "production_profile":
        profile = tmp_path
    arguments = [instance_path(), "--activate-mcp-test", "--mcp-test-profile", str(profile)]
    if kind == "missing_activation":
        arguments.remove("--activate-mcp-test")
    try:
        process = await runtime.launch(*arguments)
        assert await asyncio.wait_for(process.wait(), 5) == 2
        assert not (profile / "data").exists()
    finally:
        await runtime.close()


@pytest.mark.asyncio
@pytest.mark.parametrize("options", [
    {"identity_count": 0}, {"identity_count": 3}, {"identity_count": 1.5},
    {"identity_count": "2"}, {"public_bio": "false"}, {"preview_images": "false"},
    {"preview_videos": "false"},
])
async def test_fixture_configuration_is_bounded_and_typed(options):
    from harness import FatalFailure
    secure_process()
    runtime = Runtime(instance_path(), Path(__file__).parent)
    peer = Peer(runtime, "A", dict(control=runtime.port(), socks=runtime.port()))
    peer.bootstrap_options = options
    runtime.peers.append(peer)
    try:
        with pytest.raises(FatalFailure, match="ApplicationExitedDuringBootstrap"):
            await peer.start()
        assert peer.process.returncode == 2
    finally:
        await runtime.close()
