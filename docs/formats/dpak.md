# `.dpak` pack archive format

The shipping archive of cooked assets (SPEC §11.7). This file is the byte-level reference. It is
kept in the repo, next to the code, because the code must match it byte for byte:

- Format header (structs, constants, little-endian encoders and decoders):
  `src/asset/dpak/dpak_format.h`. It is internal, header-only, and shared by the writer and
  the reader.
- Writer: `tools/pack/lib/dpak_writer.{h,c}` (target `de_pack_lib`, DENG-211).
- Reader: `src/asset/dpak/dpak_reader.{h,c}` (DENG-212).
- Mount stack (priorities, overrides, bundle lookup): `src/asset/mount.c` and
  `src/asset/mount_internal.h`; pack loads in the pipeline: `src/asset/pack_stage.c` (DENG-212).
  A packed (Shipping) engine mounts every `.dpak` of `game:/` at create: `src/host/host_packs.c`
  (DENG-213). Packs are not a VFS root: `pack:/` is reserved and never mounted (DENG-393).

Current version: **2**. Readers also accept version 1 (see "Version 1").

## Conventions

- All integers are little-endian. Records have fixed sizes and no implicit padding.
- Offsets are byte offsets from the start of the file.
- Every padding byte is zero.
- Hashes are `de_hash64(bytes, len, 0)`: XXH3-64 with seed 0 over the exact bytes, with no NUL.
  An asset ID is the hash of the asset path (`"player/model#Body"`). A type hash is the hash of
  the type name (`"texture"`). A bundle name hash is the hash of the bundle name. See the Trama
  page "Decision: Names and asset IDs are XXH3-64 of the exact bytes".

## File layout

```
offset 0      header (256 bytes)
              sections, in the order of the table below, each on an 8-byte boundary
              dictionary blobs, each on an 8-byte boundary
              asset data: per asset, its chunks back to back; each asset starts on `alignment`
file_size     end of the last asset's data
```

Empty sections take no space. Their descriptor has offset, size and count 0. An asset with no
data (raw size 0) has no chunks and `data_offset` 0.

## Header (256 bytes)

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | u8[4] | magic | `D` `P` `A` `K` |
| 4 | u32 | version | 2 (1 for an old pack) |
| 8 | u32 | header_size | 256 |
| 12 | u32 | platform | 0 any, 1 Windows, 2 Linux, 3 macOS |
| 16 | u8[16] | build_id | opaque, chosen by the cooker |
| 32 | u64 | content_hash | `de_hash64` of bytes `[header_size, file_size)` |
| 40 | u64 | file_size | total bytes |
| 48 | u32 | flags | bit 0 `HAS_NAMES`: the NAMES section is present |
| 52 | u32 | chunk_size | raw bytes per full chunk, 1024 to 65536 (default 65536) |
| 56 | u32 | alignment | data alignment, a power of two, 8 to 1 MiB (default 4096) |
| 60 | u32 | reserved | 0 |
| 64 | desc[8] | sections | one 24-byte descriptor per section, indexed as below |

Section descriptor (24 bytes): `u64 offset`, `u64 size`, `u32 count` (records; bytes for
STRINGS), `u32 reserved` (0). `size == count * record size`.

## Sections

| Index | Section | Record | Size | Order and contents |
|---|---|---|---|---|
| 0 | TOC | toc entry | 64 | One per asset, sorted by `id`. IDs are unique. |
| 1 | CHUNKS | chunk entry | 32 | Every chunk of every asset, in TOC order. |
| 2 | DEPS | u64 | 8 | Asset IDs. Each asset's range is sorted and has no duplicates. |
| 3 | BUNDLES | bundle entry | 40 | Sorted by `name_hash`. Name hashes are unique. |
| 4 | BUNDLE_IDS | u64 | 8 | Per bundle: its member asset IDs, then the name hashes of the bundles it depends on. Each list is sorted and has no duplicates. |
| 5 | DICTS | dict entry | 32 | Sorted by `type_hash`. At most one per type. |
| 6 | NAMES | name entry | 8 | Present only with `HAS_NAMES`. Count equals the TOC count; entry `i` is the path of TOC entry `i`. |
| 7 | STRINGS | bytes | 1 | Asset paths (TOC order), then bundle names (BUNDLES order). Each string is followed by a NUL. |

### TOC entry (64 bytes)

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | u64 | id | asset ID |
| 8 | u64 | type_hash | hash of the type name |
| 16 | u64 | data_offset | file offset of the first chunk; a multiple of `alignment`; 0 for an empty asset |
| 24 | u64 | raw_size | bytes after decompression |
| 32 | u64 | stored_size | sum of the chunks' stored sizes |
| 40 | u32 | first_chunk | index into CHUNKS |
| 44 | u32 | chunk_count | `ceil(raw_size / chunk_size)` |
| 48 | u32 | dep_first | index into DEPS |
| 52 | u32 | dep_count | |
| 56 | u32 | flags | bit 0 `SUB_ASSET` (the path has a `#` part), bit 1 `UNCOMPRESSED` (every chunk is raw) |
| 60 | u32 | reserved | 0 |

Dependencies may name assets that are not in this pack (they may be in another mounted pack).

### Chunk entry (32 bytes)

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | u64 | file_offset | where the stored bytes start |
| 8 | u32 | stored_size | bytes in the file |
| 12 | u32 | raw_size | bytes after decompression; equals `chunk_size` except for an asset's last chunk |
| 16 | u16 | dict_index | index into DICTS, or `0xFFFF` for no dictionary |
| 18 | u16 | flags | bit 0 `RAW`: stored as is (`stored_size == raw_size`, no zstd frame) |
| 20 | u32 | reserved | 0 |
| 24 | u64 | hash | `de_hash64` of the `stored_size` stored bytes (version 2) |

An asset's chunks are contiguous: chunk `k + 1` starts where chunk `k` ends. Each chunk that is
not `RAW` is one complete zstd frame that decompresses alone (with its dictionary, if any) to
`raw_size` bytes. The frame header has the content size, no zstd checksum (the chunk entry's
`hash` covers the stored bytes) and no dictionary ID (the chunk entry names the dictionary).

### Bundle entry (40 bytes)

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | u64 | name_hash | hash of the bundle name |
| 8 | u32 | name_offset | into STRINGS |
| 12 | u32 | name_len | bytes, without the NUL |
| 16 | u32 | member_first | index into BUNDLE_IDS |
| 20 | u32 | member_count | |
| 24 | u32 | depend_first | index into BUNDLE_IDS (equals `member_first + member_count`) |
| 28 | u32 | depend_count | |
| 32 | u32 | flags | bit 0 `RESIDENT` (never evicted) |
| 36 | u32 | priority | 0 critical, 1 high, 2 normal, 3 low, 4 background |

Bundle names are always stored, also in a stripped pack, so a packed game can load bundles by
name without `assets.toml`. Members and depends are not checked against this pack: a patch
pack's bundle may name assets or bundles from another pack.

### Dictionary entry (32 bytes)

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | u64 | type_hash | the type whose chunks may use it |
| 8 | u64 | file_offset | the dictionary bytes (zstd dictionary format) |
| 16 | u32 | size | bytes |
| 20 | u32 | zstd_id | `ZDICT_getDictID()` of the dictionary |
| 24 | u64 | hash | `de_hash64` of the `size` dictionary bytes (version 2) |

### Name entry (8 bytes)

`u32 offset` into STRINGS and `u32 len` (bytes, without the NUL). `len` 0 means that the writer
had no path for that asset.

## Hashes (version 2)

Every chunk entry and every dictionary entry holds the XXH3-64 (seed 0) of the bytes it points
to, exactly as stored in the file. A reader can then say which asset a damaged byte belongs to
(the asset that owns the chunk, or every asset with a chunk that uses the dictionary) without
decompressing anything, for raw and compressed chunks alike. The header's `content_hash` still
covers every byte after the header, the metadata and the padding included.

A pack carries no zstd frame checksums. They would not cover raw chunks or dictionaries, would
need a full decode to check, and are 32 bits. See the Trama page "Decision: .dpak v2 stores an
XXH3-64 of every chunk and dictionary".

## How the writer fills it

- Inputs are sorted (entries by ID, bundles by name hash; dependency, member and depend lists
  sorted and deduplicated), so the output depends only on the set of inputs and the settings,
  never on the order of the add calls, the host or the time. Two entries with one ID, or two
  bundles with one name hash, are an error.
- Each chunk is compressed on its own with zstd, single-threaded, at a fixed level (default 19).
  A chunk that does not get smaller is stored raw.
- With dictionaries on, the writer trains one dictionary per type that has at least 8 chunks
  (`DPAK_DICT_MIN_SAMPLES`). It uses `ZDICT_trainFromBuffer` on that type's chunks, in TOC order
  (at most 64 MiB of samples). The capacity is a tenth of the sample bytes, at most 110 KiB; a
  type whose capacity would be under 1 KiB gets no dictionary. A chunk uses the dictionary only if
  that makes it smaller. The dictionary is kept only if the bytes it saves are more than its own
  size.
- Without `include_names` the NAMES section is absent and no asset path is in the file.
- Each chunk entry's `hash` is the hash of the chunk's stored bytes (raw or compressed), and
  each dictionary entry's `hash` that of the dictionary bytes.
- With `format_version = 1` (for compatibility tests and fuzz seeds only) the writer writes the
  version 1 layout. The bytes are the same as those of the version 1 writer.

## Reader checks

A reader must not trust a pack. Before use it checks: the magic, `version` (1 or 2) and
`header_size`;
`file_size` against the real size; every section inside the file and `size == count * record size`,
and every empty section (`count` 0) with `offset` and `size` 0 (DENG-216); TOC IDs strictly
increasing; every chunk range, dependency range, bundle range, string range and dictionary range
inside its section or the file; `chunk_count` against `raw_size` and `chunk_size`; `dict_index`
against the DICTS count; and, for a RAW chunk, `stored_size == raw_size`. `content_hash` detects
corruption; checking it means reading the whole file once, so the reader may do it only on request
(for example, `de-pack verify`).

In a version 2 pack a reader may also check a chunk's `hash` before it decodes the chunk, and a
dictionary's `hash` before it uses the dictionary. A mismatch means the bytes are damaged. The
runtime does this in Debug and Dev builds (`check_chunks`). `de-pack verify` checks every chunk
and dictionary and names every damaged asset.

## Version 1

Version 1 is version 2 without the two `hash` fields: the chunk entry and the dictionary entry
are 24 bytes (the first 24 bytes of the version 2 records), so the CHUNKS and DICTS sections are
`count * 24` bytes. Everything else is the same. A reader takes the record sizes from the
header's `version`. A version 1 pack can say only through `content_hash` that it is damaged, and
it names the damaged asset only when a chunk no longer decodes.

## Version history

| Version | Change | Issue |
|---|---|---|
| 1 | First version: header, sorted TOC, 64 KiB zstd chunks, alignment, per-type dictionaries, bundles, optional names. | DENG-211 |
| 2 | A `u64 hash` (XXH3-64 of the stored bytes) at the end of the chunk entry and of the dictionary entry; both grow from 24 to 32 bytes. Readers accept 1 and 2. | DENG-392 |

Bump `DPAK_VERSION` (and add a row here) for any change to the layout or to the meaning of a
field. Also bump it if the asset ID hash or the type hash changes. `DPAK_VERSION_MIN` is the
oldest version readers still accept.
