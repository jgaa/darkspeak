"""Run isolated peer scenarios without pytest/AI; retain only sanitized, bounded results."""
import argparse
import asyncio
import json
from pathlib import Path
import sys
import uuid

from harness import Runtime, secure_process
from scenario import Results, happy_path


async def run(instance, chutney, artifacts, scenario="happy_path"):
    from error_scenarios import SCENARIOS
    from file_error_scenarios import FILE_SCENARIOS
    from preview_scenarios import PREVIEW_SCENARIOS
    actions = {"happy_path": happy_path, **SCENARIOS, **FILE_SCENARIOS, **PREVIEW_SCENARIOS}
    action = actions[scenario]
    secure_process()
    results = Results()
    runtime = Runtime(instance, chutney)
    try:
        try:
            await asyncio.wait_for(action(runtime, results), 600)
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
                    bio_wire=[{key: item.get("bio_wire", {}).get(key, 0) for key in
                               ("responses", "content_frames", "hash_frames", "forbidden_frames")}
                              for item in state["identities"]],
                    contact_states=[dict(state=c["state"], online=c["online"]) for c in state["contacts"]],
                    transfer_states=[dict(state=f["state"], direction=f["direction"], bytes=f.get("bytes", 0),
                                          resumed_from=f.get("resumed_from", 0)) for f in state["transfers"]],
                    pending_approvals=state["pending"], journal_enabled=state["journal_enabled"],
                    diagnostics=state.get("diagnostics", [])))
            except Exception:
                diagnostics.append(dict(peer=peer.label, unavailable=True))
        try:
            await asyncio.wait_for(runtime.close(), 20)
            results.checks.append(dict(check="cleanup", outcome="passed"))
        except Exception as error:
            results.checks.append(dict(check="cleanup", outcome="fatal", code=type(error).__name__))
    report = dict(version=1, scenario="P2-001" if scenario == "happy_path" else scenario, checks=results.checks, diagnostics=diagnostics,
                  process_exit_codes=[p.returncode for p in runtime.processes])
    directory = Path(artifacts)
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    (directory / (scenario + "-" + uuid.uuid4().hex + ".json")).write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--instance", required=True)
    parser.add_argument("--chutney", required=True)
    parser.add_argument("--artifacts", required=True)
    parser.add_argument("--scenario", choices=("happy_path", "privacy_multi", "resume_crash", "receiver_crash", "delete_contacts", "delete_sender", "file_decisions", "corrupt_partial", "image_previews", "image_previews_disabled", "video_previews", "video_previews_disabled"), default="happy_path")
    args = parser.parse_args()
    try:
        report = asyncio.run(run(args.instance, args.chutney, args.artifacts, args.scenario))
        return int(any(c["outcome"] != "passed" for c in report["checks"]))
    except BaseException as error:
        print("Scenario runner failed: " + type(error).__name__, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
