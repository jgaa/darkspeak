"""Pinned SDK contract test against the actual DarkSpeak service.

The C++ test provisions a disposable engine and supplies connection details over
stdin. Never print requests, responses, credentials, or SDK exception messages.
"""
import asyncio
import json
import logging
import sys

import httpx2
from mcp import ClientSession
from mcp.client.streamable_http import streamable_http_client


async def check(config):
    async with httpx2.AsyncClient(headers={"Authorization": "Bearer " + config["secret"]}) as http:
        async with streamable_http_client(config["url"], http_client=http) as streams:
            async with ClientSession(*streams) as session:
                discovery = await session.discover()
                assert "2026-07-28" in discovery.supported_versions
                catalog = await session.list_tools()
                names = [tool.name for tool in catalog.tools]
                assert names == sorted(names)
                assert "message_send" in names and "operation_status" in names
                assert not any("controller" in name or "test" in name for name in names)
                status = await session.call_tool("status", {"identity": config["identity"]})
                assert not status.is_error
                epoch = status.structured_content["retry_epoch"]
                invalid = await session.call_tool("status", {"identity": config["identity"], "extra": 1})
                assert invalid.is_error
                profile = await session.call_tool("profile", {
                    "identity": config["identity"], "contact": config["contact"],
                    "idempotency_key": epoch + ":sdk-profile",
                })
                assert not profile.is_error
                assert profile.structured_content["state"] == "Completed"
                assert "bio" in profile.structured_content
                retry = await session.call_tool("profile", {
                    "identity": config["identity"], "contact": config["contact"],
                    "idempotency_key": epoch + ":sdk-profile",
                })
                assert not retry.is_error
                assert retry.structured_content["operation"] == profile.structured_content["operation"]
                assert "bio" not in retry.structured_content
                help_result = await session.call_tool("darkspeak_help", {"tool": "message_send", "topic": "retries"})
                assert not help_result.is_error
        # Header/body mismatch and matching unsupported versions are distinct errors.
        for body_version, header_version, code in (
            ("2026-07-28", "invalid", -32020),
            ("invalid", "invalid", -32022),
        ):
            response = await http.post(config["url"], headers={
                "MCP-Protocol-Version": header_version, "Mcp-Method": "tools/list",
                "Accept": "application/json, text/event-stream",
            }, json={"jsonrpc": "2.0", "id": 99, "method": "tools/list", "params": {
                "_meta": {"io.modelcontextprotocol/protocolVersion": body_version,
                          "io.modelcontextprotocol/clientCapabilities": {}},
            }})
            assert response.status_code == 400
            assert response.json()["error"]["code"] == code
        oversized = await http.post(config["url"], headers={"Content-Type": "application/json"}, content=b" " * (256 * 1024 + 1))
        assert oversized.status_code == 413
        compressed = await http.post(config["url"], headers={"Content-Encoding": "gzip", "Content-Type": "application/json"}, content=b"not a gzip stream")
        assert compressed.status_code == 415
    print("SDK compatibility passed", flush=True)


if __name__ == "__main__":
    logging.disable(logging.CRITICAL)
    try:
        config = json.loads(sys.stdin.readline())
        asyncio.run(asyncio.wait_for(check(config), timeout=15))
    except BaseException as exc:
        # SDK errors can contain request/response payloads. Retain only the type.
        print("SDK check failed: " + type(exc).__name__, file=sys.stderr)
        sys.exit(1)
