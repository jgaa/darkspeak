# Media previews

## Image previews, version 1

Previews are optional media, controlled independently for each local identity.
Image and video previews remain disabled by default. Enabling image previews
allows local thumbnail generation for sent images and automatic fetching of
small offered image thumbnails. It never accepts the original file. Downloading
an original without previews enabled does not authorize decoding it.

The cache representation is a fresh, opaque JPEG, maximum 320 pixels per edge
and 48 KiB encoded. Aspect ratio is preserved, small images are not enlarged,
and transparency is flattened onto white. Only pixels survive: source EXIF,
GPS, text, filenames, comments, profiles and animation are not copied. Qt's
existing JPEG image plugin is used; no additional Qt module is required.
Presentation density is fixed rather than derived from the source or desktop.

Generation uses a bounded worker and the immutable managed snapshot, not the
external import path. Source limits are 32 MiB encoded, 16,777,216 declared
pixels and 32,768 pixels per dimension. The first frame of supported raster
formats is used. Unsupported plugins, active formats (including SVG), invalid
headers, decoding failures and oversized inputs leave the ordinary file card
usable without a preview. File-transfer size limits do not authorize decoding.
For an incoming original, detected content MIME must match its declared type
before selecting a decoder; a false JPEG claim cannot select a different parser.

One generation/cache job runs at a time, with at most sixteen admitted calls.
The private managed cache contains at most 512 JPEGs (24 MiB of encoded image
payload); oldest inserted entries are evicted. Keys include the preview format
version, local file row and complete source SHA-256. Reads verify the managed
asset digest and strict JPEG dimensions before reuse. Cache contents persist
across restart and follow file, contact and identity deletion through the
existing managed-media cleanup mechanism. They do not enter the desktop's
shared thumbnail cache, export directories, logs or MCP results.

## Offers and rendering

Generated previews are separate ordinary managed file snapshots. Their offers
are linked to the original through the existing protocol-v3 signed
`MessageAttachment.thumbnail_for_attachment_index` relation. No inline binary
field, protobuf change, extra network service or implicit original download is
introduced. The application authenticates the complete signed manifest before
admitting any attachment. It supports at most sixteen entries and eight primary
attachments, rejects duplicate transfer IDs, and validates unique, in-range,
non-chained thumbnail relationships.

The complete message/manifest is stored atomically; versioned
`message_attachment_v1` and `file_thumbnail_v1` tables retain the signed metadata
and local transfer relationship. The existing application schema is extended
with these explicitly versioned tables; fixture `tests/fixtures/media-preview-v1.json`
records their fields and resource limits. A thumbnail and primary belong to the
same contact, identity, conversation and transfer direction. Deleting a primary
also deletes its thumbnail transfer and schedules managed-content cleanup.

The sender waits for current peer file/MIME/filename preferences. When JPEG
cannot be offered, it offers the original normally. The receiver separately
checks original media type, its local preview preference, `allow_files`, JPEG
MIME permission and ordinary file/queue/storage quotas before admitting a
thumbnail. Previews are limited to JPEG and 48 KiB before allocating transfer
state. They use normal file acceptance, channels, integrity checks and storage
publication. Rendering additionally requires a valid JPEG of at most 320 by
320 pixels; authenticated peers remain untrusted.

The conversation shows one card for the original, including its local preview
when available. Thumbnail transfers and empty manifest-only messages do not
produce additional conversation rows. Disabling the identity preference clears
the visible preview. Received thumbnails are normalized before entering the
cache. Completed incoming images can also generate a local thumbnail when no
sender thumbnail is available. The reusable `File.previewSource` property is
preference-gated and exposes only a bounded generated data URI, never a managed
filesystem path.

## Video generation and startup dependency

FFmpeg is the Linux video backend. Enabling video previews allows generation
for sent videos, generation from complete downloaded videos, and fetching of
sender-provided JPEG previews. All use the existing private JPEG cache and
independent original-file acceptance. It extracts the first video frame from
supported local containers (MP4/MOV, Matroska/WebM, AVI, Ogg, MPEG and YUV4MPEG).
Display aspect ratio, including non-square source pixels, is normalized before
metadata removal. Qt's generic `application/x-matroska` content type is a video
candidate without changing the declared MIME or inferring it from a filename;
audio-only containers produce no frame.
Missing codecs, corrupt inputs and unsupported formats leave the ordinary card
usable. Cached previews do not require FFmpeg to be decoded again.

Video input is limited to 128 MiB. One decoder runs at a time. Each subprocess
has a ten-second wall deadline, five-second CPU limit, 768 MiB address-space
limit, 64 MiB single-allocation limit, bounded probing, one decoder/filter
thread, and at most 48 KiB output. Input is an immutable sealed anonymous memfd;
a fixed `/proc/self/fd/0` alias provides seeking even on older distribution
FFmpeg versions. There is no source filename, media export or plaintext disk
spool. Output is strictly validated and normalized to the canonical JPEG.

The child receives a minimal environment without application credentials, closes
unrelated descriptors, disables core dumps and dies when its parent dies.
Landlock restricts filesystem access to the executable and runtime libraries;
seccomp denies networking, child processes, parent-memory access and truncation.
Anonymous input remains accessible through its preopened descriptor. Isolation
is required: unavailable kernel support or an unsupported Linux syscall ABI
produces no preview instead of an unsandboxed fallback. Shutdown cancels the
active helper and drops queued generation jobs. Helper stderr is discarded;
retained diagnostics contain only fixed categories, numeric status and sizes.

On native Linux, startup checks for the executable. If missing, a local dialog
explains that FFmpeg is optional and required to generate video previews. It
shows a fixed package command for Debian/Ubuntu families, Arch derivatives,
Fedora or Alpine when identified from bounded local os-release data; otherwise
it recommends installing FFmpeg on the user's platform. No command executes
and no distribution information is transmitted. The unchecked-by-default
“Don’t show again” checkbox persists `mediaPreview/dontShowFfmpegWarning` in the
current profile. It suppresses the notice, not executable checks or previews.

Flatpak uses its runtime FFmpeg and does not show native package-manager advice.
The packaging workflow verifies that the actual KDE Platform runtime supplies
the CLI, separately from the SDK. Frame generation and isolation tests have
also been exercised with an SDK-matched test build inside KDE Platform 6.10.
Code comments mark Windows, Android and macOS backends/deployment as future work;
no additional Qt module is introduced.

## MCP impact

There is no new production MCP tool or media-content result. Existing file and
message capabilities and approval rules remain authoritative. Test-only file
inspection includes `thumbnail_for` and the boolean `preview_available`; it
never returns a thumbnail URI, encoded image or physical path. Isolated bootstrap
accepts strict optional `preview_images` and `preview_videos` booleans, default false. The fixed test input catalog additionally permits `blue.mkv`. Production
builds exclude these test controls.
The headless observer retains at most 64 file objects so asynchronous availability
can be observed independently of the application's small ordinary file-object LRU.

## Verification

`media_preview` exercises compressed output, metadata removal, transparency,
format restrictions and allocation bounds. `protocol_v3` checks signed thumbnail
relationships and tampering. `managed_storage` checks cache persistence/reuse,
identity privacy, partial-file refusal, pre-acceptance rendering, atomic manifest
persistence and deletion cleanup. The private-Tor `mcp_peer_image_previews` and
`mcp_peer_image_previews_disabled` scenarios exercise two real instances through
ordinary approvals and real file channels. They are sequential, bounded and
included by `tests/run-all-tests.sh` and MCP CI. Video equivalents are
`mcp_peer_video_previews` and `mcp_peer_video_previews_disabled`. The codec suite
also probes filesystem/network denial, secret-environment and descriptor
isolation, immutable input, resource bounds, timeout and cancellation.
`video_preview_support` and Qt Quick tests cover startup advice and suppression.
