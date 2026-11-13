# Public-domain peer scenario fixtures

These bytes are checked in for offline, repeatable regression tests. Runtime
tests must not download or update them. `manifest.json` version 1 records the
exact size, SHA-256, media type, provenance and public-domain basis for each file.
The source files are unchanged apart from local names. Retrieved 2026-11-13.

| File | Creator and source | Public-domain basis | Scenario use |
|---|---|---|---|
| `earth.jpg` | NASA, Blue Marble eastern hemisphere (2002); [source and rights](https://commons.wikimedia.org/wiki/File:Earth_Eastern_Hemisphere.jpg) | NASA federal-government work, marked PD-USGov-NASA (United States) | A's avatar input; JPEG attachment in both directions |
| `apollo11.jpg` | NASA Apollo 11 team, lunar surface photograph (1969); [source and rights](https://commons.wikimedia.org/wiki/File:Apollo-11_nasa_535.jpg) | NASA federal-government work, marked PD-USGov-NASA (United States) | B's avatar input; grayscale JPEG attachment in both directions |
| `sos.ogg` | Hydrargyrum, synthesized Morse audio; [source and rights](https://commons.wikimedia.org/wiki/File:SOS_morse_code.ogg) | Author's worldwide public-domain dedication, with unrestricted-use fallback | Ogg Vorbis audio attachment in both directions |

The corpus is under 1 MiB and includes color/grayscale images and non-image
media. Both JPEGs are square and import through DarkSpeak's normal avatar
normalization (maximum 128 × 128). Compare remote avatars with normalized sender
pixels and the protocol avatar hash; compare file attachments with the manifest's
SHA-256 of the original encoded bytes. Do not compare avatar pixels with JPEG
file checksums. No externally edited avatar derivatives are needed.

Use copies under the isolated test root. Do not mutate these files or open
received attachments in external viewers. Source URLs are provenance only;
fixture updates require deliberate review of rights, bytes and manifest hashes.
The asset rights above are separate from the project's source-code license.
