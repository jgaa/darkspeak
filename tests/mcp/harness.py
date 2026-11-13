"""Isolated application lifecycle, MCP approval helpers and bounded waits."""
import asyncio
from contextlib import AsyncExitStack
import hashlib
import json
import logging
import os
from pathlib import Path
import resource
import secrets
import signal
import shutil
import socket
import tempfile
import time

import httpx2
from mcp import ClientSession
from mcp.client.streamable_http import streamable_http_client

from tor_network import tor_ready

logging.disable(logging.CRITICAL)
ASSETS = Path(__file__).resolve().parents[1] / "assets" / "mcp"


class CheckFailure(Exception):
    """Only fixed local codes may appear in this exception's message."""


class FatalFailure(CheckFailure):
    pass


async def wait_for(predicate, timeout=30):
    deadline = time.monotonic() + timeout
    delay = 0.1
    while time.monotonic() < deadline:
        try:
            result = await predicate()
            if result:
                return result
        except (OSError, TimeoutError, httpx2.TransportError):
            pass
        await asyncio.sleep(min(delay, max(0, deadline - time.monotonic())))
        delay = min(1, delay * 1.5)
    raise CheckFailure("DeadlineExceeded")


class Client:
    def __init__(self, port, secret):
        self.port, self.secret = port, secret
        self.closed = asyncio.Event()
        self.admission = asyncio.Lock()
        self.last_call = 0.0

    async def admit(self):
        # Stay below the production per-key budget. A polling harness must not
        # accidentally turn the happy path into an overload test.
        async with self.admission:
            delay = self.last_call + 0.7 - time.monotonic()
            if delay > 0:
                await asyncio.sleep(delay)
            self.last_call = time.monotonic()

    async def open(self):
        self.ready = asyncio.get_running_loop().create_future()
        self.worker = asyncio.create_task(self._lifetime())
        await self.ready
        return self

    async def _lifetime(self):
        # anyio contexts must enter and leave in the same task, including failure
        # cleanup after a scenario's wait_for cancels its separate task.
        try:
            async with AsyncExitStack() as stack:
                http = await stack.enter_async_context(httpx2.AsyncClient(
                    headers={"Authorization": "Bearer " + self.secret}, timeout=70))
                streams = await stack.enter_async_context(streamable_http_client(
                    f"http://127.0.0.1:{self.port}/mcp", http_client=http))
                self.session = await stack.enter_async_context(ClientSession(*streams))
                discovery = await self.session.discover()
                if "2026-07-28" not in discovery.supported_versions:
                    raise FatalFailure("IncompatibleMcpVersion")
                self.ready.set_result(True)
                await self.closed.wait()
        except BaseException:
            if not self.ready.done():
                self.ready.set_exception(FatalFailure("McpClientStartupFailed"))

    async def close(self):
        self.closed.set()
        await self.worker
        self.secret = ""

    async def call(self, name, args=None):
        try:
            await self.admit()
            result = await asyncio.wait_for(self.session.call_tool(name, args or {}), 70)
            if result.is_error:
                # Only source-defined codes may be retained, never arbitrary SDK text.
                text = " ".join(getattr(item, "text", "") for item in result.content)
                for code in ("PersistenceFailed", "InvalidArguments", "InvalidSharingId", "ContactCreationFailed",
                             "ApprovalUnavailable", "AccessDenied", "RateLimited", "PeerPolicyDenied",
                             "TestUnavailable", "OperationNotCompleted", "InvalidTransferState", "StorageInspectionFailed", "ResultLimit"):
                    if code in text:
                        raise CheckFailure(code)
                raise CheckFailure("McpToolRejected")
            if not isinstance(result.structured_content, dict):
                raise CheckFailure("InvalidMcpResult")
            return result.structured_content
        except CheckFailure:
            raise
        except Exception as error:
            raise CheckFailure("McpTransportFailed:" + type(error).__name__) from None


class Peer:
    def __init__(self, runtime, label, node):
        self.runtime, self.label, self.node = runtime, label, node
        self.profile = runtime.root / label
        self.process = None
        self.ordinary = self.controller = None
        self.number = 0
        self.bootstrap_options = {}
        self.identity_clients = []

    async def start(self):
        self.profile.mkdir(mode=0o700, exist_ok=True)
        inputs = self.profile / "inputs"
        inputs.mkdir(mode=0o700, exist_ok=True)
        for asset in self.runtime.assets:
            shutil.copyfile(ASSETS / asset["path"], inputs / asset["path"])
        (self.profile / ".darkspeak-test-profile-v1").write_text("darkspeak-mcp-test-v1\n")
        config = dict(version=1, label=self.label, mcp_port=self.runtime.port(),
                      controller_port=self.runtime.port(), tor_control_port=self.node["control"],
                      tor_socks_port=self.node["socks"], vault_passphrase=secrets.token_hex(32))
        config.update(self.bootstrap_options)
        if (self.profile / "bootstrap.json").exists():
            config["vault_passphrase"] = json.loads((self.profile / "bootstrap.json").read_text())["vault_passphrase"]
        (self.profile / "bootstrap.json").write_text(json.dumps(config))
        provision = self.profile / "provision.json"
        provision.unlink(missing_ok=True)
        env = self.runtime.child_env()
        env["QT_QPA_PLATFORM"] = "offscreen"
        self.process = await self.runtime.launch(self.runtime.instance, "--activate-mcp-test",
                                                 "--mcp-test-profile", str(self.profile), env=env)

        async def provisioned():
            if self.process.returncode is not None:
                raise FatalFailure("ApplicationExitedDuringBootstrap")
            return provision.exists()
        await wait_for(provisioned, 30)
        data = json.loads(provision.read_text())
        provision.unlink()  # Secrets stay in client memory, not retained artifacts.
        if data.get("version") != 1:
            raise FatalFailure("UnsupportedProvisionVersion")
        self.provision_fields = set(data)
        self.identity = data["identity"]
        self.ordinary = await Client(data["mcp_port"], data["ordinary_secret"]).open()
        self.identities = [item["identity"] for item in data["identities"]]
        self.identity_clients = [self.ordinary]
        for item in data["identities"][1:]:
            self.identity_clients.append(await Client(data["mcp_port"], item["secret"]).open())
        self.controller = await Client(data["controller_port"], data["controller_secret"]).open()
        async def application_ready():
            return (await self.controller.call("controller_state"))["application_ready"]
        await wait_for(application_ready, 30)
        self.epoch = (await self.call("status"))["retry_epoch"]
        return self

    async def crash(self):
        # Kill only the owned application, preserving its isolated profile/Tor.
        self.process.kill()
        await asyncio.wait_for(self.process.wait(), 5)
        for client in (self.controller, *reversed(self.identity_clients)):
            if client:
                await client.close()
        self.controller = self.ordinary = None
        self.identity_clients = []

    async def restart(self):
        await self.stop()
        return await self.start()

    async def stop(self):
        if self.controller:
            try:
                await asyncio.wait_for(self.controller.call("controller_shutdown"), 2)
            except Exception:
                pass
        # Async context stacks must be closed in reverse order in their owning task.
        for client in (self.controller, *reversed(self.identity_clients)):
            if client:
                await client.close()
        self.controller = self.ordinary = None
        self.identity_clients = []
        if self.process:
            try:
                await asyncio.wait_for(self.process.wait(), 2)
            except TimeoutError:
                pass
            await self.runtime.stop_process(self.process)

    async def call(self, name, *, identity=None, **args):
        identity = identity or self.identity
        client = self.identity_clients[self.identities.index(identity)]
        return await client.call(name, dict(identity=identity, **args))

    async def approved(self, tool, *, expected_request_handle=None, identity=None, **args):
        self.number += 1
        identity = identity or self.identity
        client = self.identity_clients[self.identities.index(identity)]
        expected = dict(identity=identity, idempotency_key=f"{self.epoch}:{self.number}", **args)
        task = asyncio.create_task(client.call(tool, expected))
        completed = False
        try:
            async def pending():
                if task.done():
                    await task  # Surface an early rejection without waiting for approval.
                    raise CheckFailure("ExpectedApprovalMissing")
                rows = (await self.controller.call("controller_pending"))["items"]
                if not rows:
                    return False
                if len(rows) != 1:
                    raise FatalFailure("UnexpectedApprovals")
                row = rows[0]
                actual = json.loads(row["arguments"])
                if row["tool"] != tool or any(actual.get(k) != v for k, v in expected.items()):
                    raise FatalFailure("ApprovalArgumentMismatch")
                if tool == "contact_approve" and (not expected_request_handle or
                        actual.get("_request_details", {}).get("handle") != expected_request_handle):
                    raise FatalFailure("UnexpectedContactRequester")
                targets = args.get("contacts", [args["contact"]] if "contact" in args else None)
                if targets is not None and sorted(row["contacts"]) != sorted(targets):
                    raise FatalFailure("ApprovalTargetMismatch")
                return row
            row = await wait_for(pending, 10)
            await self.controller.call("controller_decide", dict(operation=row["id"],
                decision="Allow once", scope="Selected contacts"))
            result = await task
            if result.get("state") != "Completed":
                raise CheckFailure("OperationNotCompleted")
            completed = True
            return result
        finally:
            if not task.done():
                task.cancel()
            await asyncio.gather(task, return_exceptions=True)
            if not completed:
                # A transport error can leave an admitted approval alive. Resolve
                # only this exact request before moving to another check.
                try:
                    rows = (await self.controller.call("controller_pending"))["items"]
                    for row in rows:
                        actual = json.loads(row["arguments"])
                        if row["tool"] == tool and all(actual.get(k) == v for k, v in expected.items()):
                            await self.controller.call("controller_decide", dict(operation=row["id"], decision="Deny", scope="Selected contacts"))
                except Exception:
                    pass

    async def state(self):
        if self.process.returncode is not None:
            raise FatalFailure("ApplicationExited")
        return await self.controller.call("controller_state")


class Runtime:
    def __init__(self, instance, chutney):
        self.instance, self.chutney = str(Path(instance).resolve()), Path(chutney).resolve()
        self.root = Path(tempfile.mkdtemp(prefix="ds-mcp-", dir="/tmp"))
        self.processes, self.peers, self.ports = [], [], set()
        self.assets = json.loads((ASSETS / "manifest.json").read_text())["assets"]

    def port(self):
        while True:
            with socket.socket() as stream:
                stream.bind(("127.0.0.1", 0))
                port = stream.getsockname()[1]
            if port not in self.ports:
                self.ports.add(port)
                return port

    def child_env(self):
        # Inherit only runtime/library discovery; never inherit a user's profile,
        # vault, Tor, proxy, MCP credentials or Qt user configuration.
        return {k: v for k, v in os.environ.items() if k in (
            "PATH", "LD_LIBRARY_PATH", "QT_PLUGIN_PATH", "QML_IMPORT_PATH", "LANG", "LC_ALL")}

    async def launch(self, *args, env=None):
        process = await asyncio.create_subprocess_exec(*map(str, args), env=env or self.child_env(),
            stdout=asyncio.subprocess.DEVNULL, stderr=asyncio.subprocess.DEVNULL,
            start_new_session=True, cwd=self.root)
        self.processes.append(process)
        return process

    async def configure_tor(self):
        for asset in self.assets:
            data = (ASSETS / asset["path"]).read_bytes()
            if len(data) != asset["size_bytes"] or hashlib.sha256(data).hexdigest() != asset["sha256"]:
                raise FatalFailure("FixtureIntegrityFailed")
        network = self.root / "tor"
        network.mkdir(mode=0o700)
        env = self.child_env()
        env["HOME"] = str(self.root)
        process = await self.launch(os.sys.executable, Path(__file__).with_name("tor_network.py"),
                                    self.chutney, network, env=env)
        if await asyncio.wait_for(process.wait(), 60) != 0:
            raise FatalFailure("PrivateTorConfigurationFailed")
        self.nodes = json.loads((network / "endpoints.json").read_text())
        for node in self.nodes:
            self.ports.update(node["ports"])

    async def start_peers(self):
        clients = [node for node in self.nodes if node["client"]]
        if len(clients) != 2:
            raise FatalFailure("InvalidPrivateTorTopology")
        for label, node in zip(("A", "B"), clients):
            peer = Peer(self, label, node)
            self.peers.append(peer)
            await peer.start()
        return self.peers

    async def start_tor(self):
        for node in self.nodes:
            await self.launch("tor", "-f", node["torrc"])
        async def ready():
            for process in self.processes:
                if process.returncode not in (None, 0):
                    raise FatalFailure("PrivateTorProcessExited")
            states = await asyncio.gather(*(tor_ready(node) for node in self.nodes), return_exceptions=True)
            # All authorities and both clients must bootstrap. Onion circuits
            # require at least five ready servers; spare relays may still be
            # learning descriptors and are not application readiness gates.
            return (all(states[index] is True for index in (0, 1, 2, 3, 7, 8))
                    and sum(state is True for state in states[:7]) >= 5)
        await wait_for(ready, 120)

    async def stop_process(self, process):
        if process.returncode is not None:
            return
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            await asyncio.wait_for(process.wait(), 2)
        except TimeoutError:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            await process.wait()

    async def close(self):
        try:
            for peer in reversed(self.peers):
                try:
                    await asyncio.wait_for(peer.stop(), 4)
                except Exception:
                    pass
            await asyncio.gather(*(self.stop_process(p) for p in self.processes), return_exceptions=True)
        finally:
            shutil.rmtree(self.root)


def secure_process():
    os.umask(0o077)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
