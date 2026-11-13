# P2-009/010 — Video previews

Use the same two-instance, isolated-Tor setup and assertions as
[P2-007/008](004-image-previews.md), with video previews enabled separately from
image previews. Provision the fixed generated `blue.mkv` fixture into the
protected input directory; no client-supplied path or runtime download is used.
Qt may identify Matroska by its generic container MIME; the normal MIME policy
still governs the original and the bounded FFmpeg decoder must find a video
stream before producing a preview.

`video_previews` verifies both directions: exactly one related JPEG completes
and becomes available while the original remains unaccepted, unpublished and at
zero bytes. Accept each original independently and verify its complete pinned
SHA-256 and size. Verify subsequent messaging and preview availability after
receiver restart. `video_previews_disabled` enables video previews only for the
sender, verifies no thumbnail admission or rendering at the receiver, then
accepts and verifies the original without enabling previews.

The ordinary production authorization manager, file channels, hashing, quotas,
privacy settings and managed storage remain authoritative. The shared runner's
fatal/recoverable distinction, consecutive-failure abort, 600-second deadline,
separate cleanup deadline and sanitized diagnostic projection are unchanged.
The scenarios are registered with CTest, run sequentially, and are included by
the full test entry point and MCP CI. They require FFmpeg, Landlock and seccomp,
and use neither public Tor nor AI services.
