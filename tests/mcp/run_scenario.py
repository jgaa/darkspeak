"""Run P2-001 without pytest/AI; retain only sanitized, bounded results."""
import argparse
import asyncio
import json
from pathlib import Path
import sys
import uuid

from harness import Runtime, secure_process
from scenario import Results, happy_path


async def run(instance, chutney, artifacts):
    secure_process()
    results = Results()
    runtime = Runtime(instance, chutney)
    try:
        try:
            await asyncio.wait_for(happy_path(runtime, results), 600)
        except Exception as error:
            if not results.checks or results.checks[-1]["outcome"] != "fatal":
                # No exception messages or tracebacks: these may contain SDK data.
                results.checks.append(dict(check="scenario", outcome="fatal", code=type(error).__name__))
    finally:
        # Retain a projection assembled from known-safe fields, never raw SDK
        # objects, SQL databases, profiles, process environments or log files.
        diagnostics = []
        for peer in runtime.peers:
            try:
                state = await asyncio.wait_for(peer.state(), 2)
                diagnostics.append(dict(peer=peer.label,
                    application_ready=state["application_ready"], online=state["online"],
                    identity_count=len(state["identities"]),
                    contact_states=[dict(state=c["state"], online=c["online"]) for c in state["contacts"]],
                    transfer_states=[dict(state=f["state"], direction=f["direction"]) for f in state["transfers"]],
                    pending_approvals=state["pending"], journal_enabled=state["journal_enabled"],
                    diagnostics=state.get("diagnostics", [])))
            except Exception:
                diagnostics.append(dict(peer=peer.label, unavailable=True))
        try:
            await asyncio.wait_for(runtime.close(), 20)
            results.checks.append(dict(check="cleanup", outcome="passed"))
        except Exception as error:
            results.checks.append(dict(check="cleanup", outcome="fatal", code=type(error).__name__))
    report = dict(version=1, scenario="P2-001", checks=results.checks, diagnostics=diagnostics,
                  process_exit_codes=[p.returncode for p in runtime.processes])
    directory = Path(artifacts)
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    (directory / ("p2-001-" + uuid.uuid4().hex + ".json")).write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--instance", required=True)
    parser.add_argument("--chutney", required=True)
    parser.add_argument("--artifacts", required=True)
    args = parser.parse_args()
    try:
        report = asyncio.run(run(args.instance, args.chutney, args.artifacts))
        return int(any(c["outcome"] != "passed" for c in report["checks"]))
    except BaseException as error:
        print("Scenario runner failed: " + type(error).__name__, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
