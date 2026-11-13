# P2-007/008 — Image previews

Both cases use two fresh, isolated application instances, one identity each,
private real Tor, checked-in public-domain images, ordinary scoped credentials
and the production approval manager. No AI service or public Tor is used.

## P2-007: previews enabled

Enable image previews for both identities. Establish and approve their contact
relationship. In each direction, offer the original image without accepting it.
Verify that the signed manifest produces exactly one related JPEG transfer,
bounded to 48 KiB, which completes and becomes locally renderable while the
original remains offered with zero received bytes, no pending storage and no
published content. Inspect the completed thumbnail's actual managed bytes using
the existing controller integrity helper.

Accept each original separately and verify its complete SHA-256 and length.
Verify messaging still works. Restart the receiver and verify that its completed
original and preference-gated cached preview remain available.

## P2-008: previews disabled at receiver

Enable previews only at the sender. Offer an image with a thumbnail. Verify that
the receiver admits no thumbnail transfer and renders no preview; the sender
observes thumbnail rejection without sending thumbnail content. The original
remains independently offered. Accept the original and verify its complete
SHA-256 and length; its receiver-side preview remains disabled after download.

## Failure and cleanup

Bootstrap, credential, private-network and contact/offer prerequisites are fatal.
Independent preview, original-integrity, messaging and restart assertions are
recoverable; continue them when feasible. The shared runner aborts after three
consecutive failed checks, has a 600-second scenario deadline and a separate
20-second cleanup deadline, reaps owned processes and removes isolated profiles.
Retained JSON contains only sanitized checks, bounded diagnostic categories,
numeric state and exit codes. It contains no images, data URIs, physical paths,
credentials, raw logs, profiles or database copies.

Each case is registered with CTest, runs sequentially, and is selectable through
`run_scenario.py --scenario image_previews` or `--scenario image_previews_disabled`.
