"""Private, loopback-only real Tor network. Chutney is a pinned build input."""
import asyncio
import json
import os
from pathlib import Path
import socket
import sys


def configure(chutney, root):
    # Called in a disposable child; importing Chutney must not alter the harness.
    sys.path.insert(0, str(Path(chutney) / "lib"))
    os.environ.update(CHUTNEY_DATA_DIR=str(root), CHUTNEY_TOR="tor",
                      CHUTNEY_TOR_GENCERT="tor-gencert", CHUTNEY_DISABLE_IPV6="1",
                      CHUTNEY_TOR_SANDBOX="0", CHUTNEY_DNS_CONF="/dev/null",
                      CHUTNEY_CONTROLLING_PID=str(os.getppid()))
    from chutney.TorNet import Network
    network = Network.from_network_script_contents("""
parent = Node(launch_phase=1)
Authority = parent.specialize(tag='a', authority=1, relay=1)
Relay = parent.specialize(tag='r', relay=1)
Client = parent.specialize(tag='c', client=1)
ConfigureNodes(Authority.getN(4) + Relay.getN(3) + Client.getN(2))
""")
    # Reserve independent blocks while configuring, then fail if Tor cannot bind.
    # Sequential execution is deliberate; startup errors never reuse a daemon.
    reservations = []
    for attr in ("orport_base", "dirport_base", "controlport_base", "socksport_base",
                 "extorport_base", "ptport_base", "dnsport_base"):
        for attempt in range(100):
            base = 12000 + int.from_bytes(os.urandom(2)) % 40000
            block = []
            try:
                for offset in range(9):
                    stream = socket.socket()
                    block.append(stream)
                    stream.bind(("127.0.0.1", base + offset))
                reservations.extend(block)
                setattr(network, attr, base)
                break
            except OSError:
                for stream in block:
                    stream.close()
        else:
            raise RuntimeError("PortReservationFailed")
    network.init(root)
    network.configure(config_phase=1)
    nodes = []
    for node in network.nodes:
        torrc = node.dir / "torrc"
        lines = torrc.read_text().splitlines()
        # No extra local log capture, public fallbacks, public exits or DNS.
        lines = [line for line in lines if not line.lstrip().startswith(
            ("Log ", "SafeLogging ", "ExitPolicy ", "__OwningControllerProcess "))]
        lines.extend(["SafeLogging 1", "Log err stderr", "UseDefaultFallbackDirs 0",
                      "ExitPolicy reject *:*",
                      f"__OwningControllerProcess {os.getppid()}"])
        # Tor forbids ReachableAddresses on relays. Their only authorities and
        # consensus members belong to this generated loopback network.
        if node._config.client:
            lines.append("ReachableAddresses 127.0.0.1:*")
        authorities = [line for line in lines if line.startswith(("DirAuthority ", "AlternateDirAuthority "))]
        if len(authorities) != 4 or any("127.0.0.1:" not in line for line in authorities):
            raise RuntimeError("NonPrivateTorConfiguration")
        torrc.write_text("\n".join(lines) + "\n")
        nodes.append(dict(torrc=str(torrc), control=network.controlport_base + node.nodenum,
                          socks=network.socksport_base + node.nodenum,
                          cookie=str(node.dir / "control_auth_cookie"), client=node._config.client,
                          ports=[getattr(network, attr) + node.nodenum for attr in
                                 ("orport_base", "dirport_base", "controlport_base", "socksport_base",
                                  "extorport_base", "ptport_base", "dnsport_base")]))
    (root / "endpoints.json").write_text(json.dumps(nodes))
    for stream in reservations:
        stream.close()


async def tor_ready(node):
    """Authenticated, bounded control query. Credentials never leave this function."""
    reader, writer = await asyncio.wait_for(asyncio.open_connection("127.0.0.1", node["control"]), 2)
    try:
        cookie = Path(node["cookie"]).read_bytes()
        if len(cookie) != 32:
            return False
        writer.write(b"AUTHENTICATE " + cookie.hex().encode() + b"\r\n")
        await writer.drain()
        if not (await asyncio.wait_for(reader.readline(), 2)).startswith(b"250 "):
            return False
        writer.write(b"GETINFO status/bootstrap-phase\r\n")
        await writer.drain()
        for _ in range(8):
            line = await asyncio.wait_for(reader.readline(), 2)
            if b"PROGRESS=100" in line:
                return True
            if line.startswith((b"250 ", b"5")):
                return False
        return False
    finally:
        writer.close()
        await writer.wait_closed()


if __name__ == "__main__":
    try:
        os.umask(0o077)
        configure(sys.argv[1], Path(sys.argv[2]))
    except BaseException as error:
        # Child diagnostics never include Chutney's exceptions/config/credentials.
        print("Tor configuration failed: " + type(error).__name__, file=sys.stderr)
        sys.exit(1)
