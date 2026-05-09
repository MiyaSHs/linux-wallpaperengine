#include "MdlParser.h"

#include <cassert>
#include <cstring>

#include "WallpaperEngine/Data/Utils/BinaryReader.h"
#include "WallpaperEngine/Logging/Log.h"

namespace WallpaperEngine::Render::Puppet {

using Data::Utils::BinaryReader;

namespace {

// Bytes per per-vertex record in the *standard* MDL format:
//   pos(3) + blendIndices(4) + weights(4) + uv(2) → 13 floats × 4 bytes = 52
constexpr uint32_t kStdVertexBytes = 4 * (3 + 4 + 4 + 2);

// "Alt" format adds 7 extra 32-bit chunks between position and blend indices.
constexpr uint32_t kAltVertexBytes = 4 * (3 + 4 + 4 + 2 + 7);

constexpr uint32_t kIndexBytes = 2 * 3; // u16[3] per triangle

// Bytes per bone-frame keyframe: pos(3) + euler(3) + scale(3) = 9 floats.
constexpr uint32_t kBoneFrameBytes = 4 * 9;

// "Herald" values placed in the file just before the vertex section. They identify which
// vertex layout follows. Discovered by reverse-engineering — names match catsout's parser.
constexpr uint32_t kStdVertexHerald = 0x01800009;
constexpr uint32_t kAltVertexHerald = 0x0180000F;

// MDAT attachments carry 64 bytes of data after their name string.
constexpr uint32_t kMdatAttachmentDataBytes = 64;

Puppet::PlayMode parsePlayMode (std::string_view m) {
    if (m == "loop" || m.empty ()) return Puppet::PlayMode::Loop;
    if (m == "mirror") return Puppet::PlayMode::Mirror;
    if (m == "single") return Puppet::PlayMode::Single;
    sLog.error ("Unknown puppet animation play mode: \"", m, "\", defaulting to Loop");
    return Puppet::PlayMode::Loop;
}

// MDL version is stored as ASCII "MDLV<digits>" — a fixed 8-byte prefix for the top-level
// header, then "MDLS<digits>" / "MDLA<digits>" inside. We also need to read these mid-stream
// for inner sections, hence a function instead of just slicing the header bytes.
int32_t readVersionTag (const BinaryReader& f, std::string_view expectedPrefix) {
    char tag[8];
    f.next (tag, 8);
    const std::string prefix (tag, 4);
    if (prefix != expectedPrefix) {
	sLog.error ("MDL version tag mismatch: expected ", expectedPrefix, " got ", prefix);
	return -1;
    }
    return std::stoi (std::string (tag + 4, 4));
}

} // namespace

bool MdlParser::parse (const Assets::AssetLocator& locator, const std::string& path, Mdl& mdl) {
    Data::Utils::ReadStreamSharedPtr stream;
    try {
	stream = locator.read (path);
    } catch (const std::exception& e) {
	sLog.error ("MdlParser: cannot open '", path, "': ", e.what ());
	return false;
    }
    if (!stream) {
	sLog.error ("MdlParser: cannot open '", path, "'");
	return false;
    }

    BinaryReader f (stream);

    mdl.mdlv = readVersionTag (f, "MDLV");
    if (mdl.mdlv < 0) return false;

    bool altFormat = false;
    uint32_t curr = 0;

    // Peek at the first u32 after the header. Newer .mdl files (verified for files marked
    // mdlv 16 and mdlv 23) start with the magic 0x80000900 — that's our cue to use the
    // new layout: 13 fixed bytes, NULL-terminated matJson, filler, then vertex herald.
    // Older files (per catsout/owe, mdlv 4–17 fixtures) start with the int32 mdlFlag
    // value — usually small (e.g. 0, 1, 9).
    const uint32_t firstU32 = f.nextUInt32 ();
    f.base ().seekg (-4, std::ios::cur);

    if (firstU32 == 0x80000900u) {
	// New header layout. Skip the 13-byte fixed struct, read NULL-terminated matJson,
	// then scan for the std/alt-vertex herald (whichever comes first). The filler region
	// before the herald is variable-length and not 4-byte-aligned, so we slide a 4-byte
	// window byte-by-byte until we hit the herald magic.
	for (int i = 0; i < 13; ++i) (void) f.nextUInt8 ();
	mdl.matJsonFile = f.nextNullTerminatedString ();

	uint32_t window = 0;
	uint32_t scanned = 0;
	bool found = false;
	while (scanned < 256) {  // bound: in practice the gap is < 32 bytes
	    if (!f.base ()) {
		sLog.error ("MdlParser: EOF scanning for vertex herald in new-format file");
		return false;
	    }
	    const uint8_t b = f.nextUInt8 ();
	    window = (window >> 8) | (static_cast<uint32_t> (b) << 24);
	    ++scanned;
	    if (scanned < 4) continue;
	    if (window == kAltVertexHerald) { altFormat = true; found = true; break; }
	    if (window == kStdVertexHerald) { altFormat = false; found = true; break; }
	}
	if (!found) {
	    sLog.error ("MdlParser: vertex herald not found within 256 bytes (corrupt file?)");
	    return false;
	}
	curr = f.nextUInt32 ();  // vertex_size follows the herald
    } else {
	// Older format (mdlv 4..17). Layout per catsout/owe.
	const int32_t mdlFlag = f.nextInt32 ();
	if (mdlFlag == 9) {
	    sLog.error ("MdlParser: puppet '", path, "' is incomplete (mdlFlag=9)");
	    return false;
	}
	(void) f.nextInt32 (); // unk, observed 1
	(void) f.nextInt32 (); // unk, observed 1
	mdl.matJsonFile = f.nextSizedString ();
	(void) f.nextInt32 (); // 0

	curr = f.nextUInt32 ();
	if (curr == 0) {
	    altFormat = true;
	    while (curr != kAltVertexHerald) {
		if (!f.base ()) {
		    sLog.error ("MdlParser: EOF looking for alt-format vertex herald");
		    return false;
		}
		curr = f.nextUInt32 ();
	    }
	    curr = f.nextUInt32 ();
	} else if (curr == kStdVertexHerald) {
	    curr = f.nextUInt32 ();
	}
    }

    const uint32_t vertexSize = curr;
    const uint32_t bytesPerVertex = altFormat ? kAltVertexBytes : kStdVertexBytes;
    if (vertexSize % bytesPerVertex != 0) {
	sLog.error (
	    "MdlParser: unsupported vertex_size=", vertexSize, " (expected multiple of ",
	    bytesPerVertex, ", alt=", altFormat, ")"
	);
	return false;
    }

    const uint32_t vertexNum = vertexSize / bytesPerVertex;
    mdl.vertexs.resize (vertexNum);
    for (auto& v : mdl.vertexs) {
	for (auto& x : v.position) x = f.nextFloat ();
	if (altFormat) {
	    for (int i = 0; i < 7; ++i) (void) f.nextUInt32 (); // alt-only padding
	}
	for (auto& x : v.blendIndices) x = f.nextUInt32 ();
	for (auto& x : v.weight) x = f.nextFloat ();
	for (auto& x : v.texcoord) x = f.nextFloat ();
    }

    const uint32_t indicesSize = f.nextUInt32 ();
    if (indicesSize % kIndexBytes != 0) {
	sLog.error ("MdlParser: unsupported indices_size=", indicesSize);
	return false;
    }
    mdl.indices.resize (indicesSize / kIndexBytes);
    for (auto& tri : mdl.indices) {
	for (auto& x : tri) x = f.nextUInt16 ();
    }

    // MDLV0023+ inserts a new section between indices and MDLS containing extra per-vertex
    // skin/normal/etc data we don't currently decode. Its size and layout vary per file
    // (and aren't documented in the catsout/owe parsers). Scan forward byte-by-byte for the
    // next "MDLS" tag — that's the next section marker we DO understand.
    if (firstU32 == 0x80000900u) {
	std::string foundTag;
	while (foundTag != "MDLS") {
	    if (!f.base ()) {
		sLog.error ("MdlParser: EOF scanning for MDLS tag in new-format file");
		return false;
	    }
	    char c = f.next ();
	    foundTag.push_back (c);
	    if (foundTag.size () > 4) foundTag.erase (foundTag.begin ());
	}
	// Read the 4-digit version that follows the MDLS tag (consume the next 4 bytes).
	char vbuf[4];
	f.next (vbuf, 4);
	mdl.mdls = std::stoi (std::string (vbuf, 4));
    } else {
	mdl.mdls = readVersionTag (f, "MDLS");
	if (mdl.mdls < 0) return false;
    }

    uint16_t bonesNum;

    if (firstU32 == 0x80000900u) {
	// New layout: 10 bytes of header before first bone:
	//   u8 pad = 0
	//   u32 mdla_offset (absolute file offset of the MDLA tag)
	//   u16 bones_num
	//   3 bytes of unknown metadata (sometimes 0x00 0x00 0x01 in newer wallpapers)
	(void) f.nextUInt8 ();           // pad
	(void) f.nextUInt32 ();          // mdla_offset
	bonesNum = f.nextUInt16 ();
	for (int i = 0; i < 3; ++i) (void) f.nextUInt8 ();
    } else {
	(void) f.nextUInt32 ();          // bones_file_end
	bonesNum = f.nextUInt16 ();
	(void) f.nextUInt16 ();          // unk
    }

    mdl.puppet = std::make_shared<Puppet> ();
    auto& bones = mdl.puppet->bones;
    auto& anims = mdl.puppet->anims;

    bones.resize (bonesNum);
    for (uint32_t i = 0; i < bonesNum; ++i) {
	auto& bone = bones[i];

	if (firstU32 == 0x80000900u) {
	    // New layout drops the bone name but keeps the int32 unk before parent.
	    (void) f.nextInt32 ();       // unk
	} else {
	    // Old layout: sized-string name + int32 unk
	    (void) f.nextSizedString (); // name
	    (void) f.nextInt32 ();       // unk
	}

	bone.parent = f.nextUInt32 ();
	if (bone.parent != 0xFFFFFFFFu && bone.parent >= i) {
	    sLog.error ("MdlParser: invalid bone parent index ", bone.parent, " for bone ", i);
	    return false;
	}

	const uint32_t size = f.nextUInt32 ();
	if (size != 64) {
	    sLog.error ("MdlParser: unsupported bone matrix size: ", size);
	    return false;
	}
	// File stores 16 floats column-major (4x4). glm::mat4 is also column-major in memory,
	// so a straight read into glm::value_ptr works.
	float matBuf[16];
	for (auto& x : matBuf) x = f.nextFloat ();
	std::memcpy (&bone.transform[0][0], matBuf, sizeof (matBuf));

	if (firstU32 == 0x80000900u) {
	    // New format: null-terminated sim_json followed by an extra null/pad byte that
	    // separates this bone from the next one.
	    (void) f.nextNullTerminatedString ();
	    (void) f.nextUInt8 ();
	} else {
	    // Old format: sized sim_json
	    (void) f.nextSizedString ();
	}
    }

    std::string mdType;
    std::string mdVersion;

    if (firstU32 == 0x80000900u) {
	// New format: byte-scan for the next section tag (MDAT or MDLA). When we hit
	// MDAT, decode its named attachments (each is a u16-prefixed null-terminated
	// name followed by a 64-byte transform), since wallpapers like 3363252053 use
	// these to pin a child image (with `attachment: <name>` in scene.json) to a
	// fixed offset in the parent puppet's local frame. Once we hit MDLA, fall
	// through to the animation parsing below.
	std::string foundTag;
	while (foundTag.size () < 4 || (foundTag.substr (foundTag.size () - 4) != "MDLA"
					&& foundTag.substr (foundTag.size () - 4) != "MDAT")) {
	    if (!f.base ()) {
		mdType = "MDLA";
		mdVersion = "0";
		break;
	    }
	    foundTag.push_back (static_cast<char> (f.nextUInt8 ()));
	    if (foundTag.size () > 8) foundTag.erase (foundTag.begin ());
	}
	if (mdType.empty () && foundTag.substr (foundTag.size () - 4) == "MDAT") {
	    // Consume the 4-character version digits after the tag.
	    char vbuf[4];
	    f.next (vbuf, 4);
	    // New-format MDAT layout: u8 pad, u32 next_section_offset, u16 count,
	    // then per attachment: u16 nameLen, nameLen bytes (name + NUL), 64-byte mat.
	    (void) f.nextUInt8 ();      // pad
	    (void) f.nextUInt32 ();     // next-section offset (= file pos of MDLA)
	    const uint16_t numAttachments = f.nextUInt16 ();
	    mdl.attachments.reserve (numAttachments);
	    for (uint16_t i = 0; i < numAttachments; ++i) {
		// Layout per attachment: u16 bone_index, NULL-terminated name, 64-byte
		// matrix (16 floats column-major). The bone_index identifies which bone
		// in the puppet skeleton this attachment hangs off of; the matrix is the
		// offset from that bone's local frame to the attachment point.
		const uint16_t boneIndex = f.nextUInt16 ();
		std::string name = f.nextNullTerminatedString ();
		float matBuf[16];
		for (auto& x : matBuf) x = f.nextFloat ();
		Mdl::Attachment a;
		a.boneIndex = boneIndex;
		a.name = std::move (name);
		std::memcpy (&a.transform[0][0], matBuf, sizeof (matBuf));
		mdl.attachments.push_back (std::move (a));
	    }
	    // Continue scanning for the MDLA tag.
	    foundTag.clear ();
	    while (foundTag.size () < 4 || foundTag.substr (foundTag.size () - 4) != "MDLA") {
		if (!f.base ()) {
		    mdType = "MDLA";
		    mdVersion = "0";
		    break;
		}
		foundTag.push_back (static_cast<char> (f.nextUInt8 ()));
		if (foundTag.size () > 8) foundTag.erase (foundTag.begin ());
	    }
	}
	if (mdType.empty ()) {
	    mdType = "MDLA";
	    char vbuf[4];
	    f.next (vbuf, 4);
	    mdVersion = std::string (vbuf, 4);
	}
    } else {
	// Old format path — keep catsout/owe logic unchanged.
	if (mdl.mdls > 1) {
	    (void) f.nextInt16 (); // unk
	    const uint8_t hasTrans = f.nextUInt8 ();
	    if (hasTrans) {
		for (uint32_t i = 0; i < bonesNum; ++i)
		    for (uint32_t j = 0; j < 16; ++j) (void) f.nextFloat ();
	    }
	    const uint32_t sizeUnk = f.nextUInt32 ();
	    for (uint32_t i = 0; i < sizeUnk; ++i)
		for (int j = 0; j < 3; ++j) (void) f.nextUInt32 ();
	    (void) f.nextUInt32 (); // unk
	    const uint8_t hasOffsetTrans = f.nextUInt8 ();
	    if (hasOffsetTrans) {
		for (uint32_t i = 0; i < bonesNum; ++i) {
		    for (uint32_t j = 0; j < 3; ++j) (void) f.nextFloat ();
		    for (uint32_t j = 0; j < 16; ++j) (void) f.nextFloat ();
		}
	    }
	    const uint8_t hasIndex = f.nextUInt8 ();
	    if (hasIndex) {
		for (uint32_t i = 0; i < bonesNum; ++i) (void) f.nextUInt32 ();
	    }
	}

	while (mdType != "MDLA") {
	    const std::string mdPrefix = f.nextSizedString ();
	    if (mdPrefix.length () != 8) continue;
	    mdType = mdPrefix.substr (0, 4);
	    mdVersion = mdPrefix.substr (4, 4);
	    if (mdType == "MDAT") {
		(void) f.nextUInt32 ();
		const uint16_t numAttachments = f.nextUInt16 ();
		mdl.attachments.reserve (numAttachments);
		for (uint16_t i = 0; i < numAttachments; ++i) {
		    const uint16_t boneIndex = f.nextUInt16 ();
		    Mdl::Attachment a;
		    a.boneIndex = boneIndex;
		    a.name = f.nextSizedString ();
		    static_assert (kMdatAttachmentDataBytes == 64, "attachment data size must match 4x4 matrix");
		    float matBuf[16];
		    for (auto& x : matBuf) x = f.nextFloat ();
		    std::memcpy (&a.transform[0][0], matBuf, sizeof (matBuf));
		    mdl.attachments.push_back (std::move (a));
		}
	    }
	}
    }

    if (mdType == "MDLA" && !mdVersion.empty ()) {
	mdl.mdla = std::stoi (mdVersion);
	if (mdl.mdla != 0 && firstU32 != 0x80000900u) {
	    // Old-format MDLA parsing — keep catsout/owe behavior.
	    (void) f.nextUInt32 (); // end_size
	    const uint32_t animNum = f.nextUInt32 ();
	    anims.resize (animNum);
	    for (auto& anim : anims) {
		// A variable number of zero u32s can sit between animations; skip them.
		anim.id = 0;
		while (anim.id == 0) {
		    anim.id = f.nextInt32 ();
		}
		if (anim.id < 0) {
		    sLog.error ("MdlParser: invalid animation id ", anim.id);
		    return false;
		}

		(void) f.nextInt32 ();
		anim.name = f.nextSizedString ();
		if (anim.name.empty ()) {
		    anim.name = f.nextSizedString ();
		}
		anim.mode = parsePlayMode (f.nextSizedString ());
		anim.fps = f.nextFloat ();
		anim.length = f.nextInt32 ();
		(void) f.nextInt32 ();

		const uint32_t bNum = f.nextUInt32 ();
		anim.bframesArray.resize (bNum);
		for (auto& bframes : anim.bframesArray) {
		    (void) f.nextInt32 ();
		    const uint32_t byteSize = f.nextUInt32 ();
		    if (byteSize % kBoneFrameBytes != 0) {
			sLog.error ("MdlParser: invalid bone-frame size ", byteSize);
			return false;
		    }
		    const uint32_t num = byteSize / kBoneFrameBytes;
		    bframes.frames.resize (num);
		    for (auto& frame : bframes.frames) {
			frame.position = { f.nextFloat (), f.nextFloat (), f.nextFloat () };
			frame.angle = { f.nextFloat (), f.nextFloat (), f.nextFloat () };
			frame.scale = { f.nextFloat (), f.nextFloat (), f.nextFloat () };
		    }
		}

		// Trailer between animations differs by format.
		if (altFormat) {
		    (void) f.nextUInt8 ();
		    (void) f.nextUInt8 ();
		} else if (mdl.mdla == 3) {
		    (void) f.nextUInt8 (); // single zero
		} else {
		    const uint32_t unkExtra = f.nextUInt32 ();
		    for (uint32_t i = 0; i < unkExtra; ++i) {
			(void) f.nextFloat ();
			(void) f.nextSizedString ();
		    }
		}
	    }
	} else if (mdl.mdla != 0 && firstU32 == 0x80000900u) {
	    // New-format MDLA parsing. Verified for MDLA0003 (Makima 3011284844) and
	    // MDLA0006 (Miku 3363252053, bikini 3704273480). Layout differs from old
	    // format in two important ways:
	    //   1. All embedded strings are NULL-terminated, not size-prefixed.
	    //   2. Each anim is followed by a variable-length trailer (alternating
	    //      `u32 byte_count + byte_count bytes` blocks) whose total size and
	    //      stop condition aren't documented. Rather than decode it, after
	    //      finishing each anim's bone-frame tracks we byte-scan the remaining
	    //      stream for the next anim header (u32 plausible_id, u32 == 0,
	    //      printable/UTF-8 first byte) and seek directly to it.
	    //
	    // The pattern-scan stop condition is conservative: we cap the scan at
	    // ~16 MiB and abort the rest of the anim list if we don't find a header.
	    // For all four test wallpapers we successfully recover all declared anims.
	    (void) f.nextUInt8 ();    // pad (always 0 observed)
	    (void) f.nextUInt32 ();   // endsize
	    const uint32_t animNum = f.nextUInt32 ();
	    anims.reserve (animNum);

	    auto parseOneAnim = [&] (Puppet::Animation& anim) -> bool {
		anim.id = f.nextInt32 ();
		if (anim.id <= 0) {
		    sLog.error ("MdlParser: invalid new-format animation id ", anim.id);
		    return false;
		}
		(void) f.nextInt32 (); // unk
		anim.name = f.nextNullTerminatedString ();
		anim.mode = parsePlayMode (f.nextNullTerminatedString ());
		anim.fps = f.nextFloat ();
		anim.length = f.nextInt32 ();
		(void) f.nextInt32 (); // unk
		const uint32_t bNum = f.nextUInt32 ();
		if (bNum != bonesNum) {
		    sLog.error (
			"MdlParser: new-format anim has ", bNum, " bone-frame tracks but puppet has ",
			bonesNum, " bones"
		    );
		    return false;
		}
		anim.bframesArray.resize (bNum);
		for (auto& bframes : anim.bframesArray) {
		    (void) f.nextUInt32 (); // bone unk
		    const uint32_t byteSize = f.nextUInt32 ();
		    if (byteSize % kBoneFrameBytes != 0) {
			sLog.error ("MdlParser: invalid bone-frame size ", byteSize);
			return false;
		    }
		    const uint32_t num = byteSize / kBoneFrameBytes;
		    bframes.frames.resize (num);
		    for (auto& frame : bframes.frames) {
			frame.position = { f.nextFloat (), f.nextFloat (), f.nextFloat () };
			frame.angle = { f.nextFloat (), f.nextFloat (), f.nextFloat () };
			frame.scale = { f.nextFloat (), f.nextFloat (), f.nextFloat () };
		    }
		}
		return true;
	    };

	    // Scan forward for the next anim header. Returns the absolute file offset
	    // of the start, or -1 if none found within the scan budget. Validates the
	    // candidate by attempting to walk the full anim header (name, mode, fps,
	    // length, unk, bnum) and rejecting unless bnum == bonesNum. The trailer
	    // contains 1.0-float runs (`00 00 80 3f`) that produce false-positive
	    // (aid, unk) tuples at unaligned offsets, so we can't rely on that alone.
	    auto findNextAnimHeader = [&f, bonesNum] () -> std::streamoff {
		auto& s = f.base ();
		s.clear ();
		const std::streamoff start = s.tellg ();
		s.seekg (0, std::ios::end);
		const std::streamoff fileEnd = s.tellg ();
		constexpr std::streamoff kMaxScan = 16 * 1024 * 1024;
		const std::streamoff scanEnd = std::min<std::streamoff> (start + kMaxScan, fileEnd - 12);

		auto validateAt = [&s, fileEnd, bonesNum] (std::streamoff p) -> bool {
		    // Read enough to walk through name+mode+(4*u32). Cap reads to avoid
		    // false positives that would seek far ahead.
		    s.clear ();
		    s.seekg (p, std::ios::beg);
		    uint8_t buf[256];
		    const std::streamoff maxRead = std::min<std::streamoff> (sizeof (buf), fileEnd - p);
		    s.read (reinterpret_cast<char*> (buf), maxRead);
		    const auto got = s.gcount ();
		    if (got < 32) return false;

		    // u32 aid, u32 unk
		    size_t off = 8;
		    // null-term name
		    size_t nameEnd = off;
		    while (nameEnd < static_cast<size_t> (got) && buf[nameEnd] != 0) ++nameEnd;
		    if (nameEnd >= static_cast<size_t> (got)) return false;
		    const size_t nameLen = nameEnd - off;
		    if (nameLen > 64) return false;
		    // Name must be valid UTF-8-ish: only printable / multi-byte UTF-8 starts (>=0x20 or >=0x80)
		    for (size_t i = off; i < nameEnd; ++i) {
			if (buf[i] < 0x20 && buf[i] != 0x09) return false;
		    }
		    off = nameEnd + 1;
		    // null-term mode
		    size_t modeEnd = off;
		    while (modeEnd < static_cast<size_t> (got) && buf[modeEnd] != 0) ++modeEnd;
		    if (modeEnd >= static_cast<size_t> (got)) return false;
		    const std::string mode (reinterpret_cast<const char*> (buf + off), modeEnd - off);
		    if (mode != "loop" && mode != "mirror" && mode != "single") return false;
		    off = modeEnd + 1;
		    // u32 fps (float), u32 length, u32 unk, u32 bnum — total 16 bytes
		    if (off + 16 > static_cast<size_t> (got)) return false;
		    // bnum at off + 12
		    const uint32_t bnum = static_cast<uint32_t> (buf[off + 12])
					 | (static_cast<uint32_t> (buf[off + 13]) << 8)
					 | (static_cast<uint32_t> (buf[off + 14]) << 16)
					 | (static_cast<uint32_t> (buf[off + 15]) << 24);
		    return bnum == bonesNum;
		};

		std::streamoff pos = start;
		uint8_t hdr[12];
		while (pos <= scanEnd) {
		    s.clear ();
		    s.seekg (pos, std::ios::beg);
		    s.read (reinterpret_cast<char*> (hdr), sizeof (hdr));
		    if (s.gcount () != sizeof (hdr)) {
			return -1;
		    }
		    const uint32_t aid = static_cast<uint32_t> (hdr[0]) | (static_cast<uint32_t> (hdr[1]) << 8)
				       | (static_cast<uint32_t> (hdr[2]) << 16) | (static_cast<uint32_t> (hdr[3]) << 24);
		    const uint32_t unk = static_cast<uint32_t> (hdr[4]) | (static_cast<uint32_t> (hdr[5]) << 8)
				       | (static_cast<uint32_t> (hdr[6]) << 16) | (static_cast<uint32_t> (hdr[7]) << 24);
		    if (aid > 0 && aid < 100000u && unk == 0 && (hdr[8] == 0 || hdr[8] >= 0x20)) {
			if (validateAt (pos)) {
			    s.clear ();
			    s.seekg (pos, std::ios::beg);
			    return pos;
			}
		    }
		    ++pos;
		}
		return -1;
	    };

	    for (uint32_t ai = 0; ai < animNum; ++ai) {
		if (ai > 0) {
		    // Skip the previous anim's trailer by pattern-scanning to the next header.
		    const std::streamoff nextHdr = findNextAnimHeader ();
		    if (nextHdr < 0) {
			sLog.error ("MdlParser: could not locate next anim header after anim ", ai - 1);
			break;
		    }
		    f.base ().clear ();
		    f.base ().seekg (nextHdr, std::ios::beg);
		}
		Puppet::Animation anim;
		if (!parseOneAnim (anim)) {
		    break;
		}
		anims.push_back (std::move (anim));
	    }
	}
    }

    mdl.puppet->prepared ();

    sLog.out (
	"Puppet '", path, "': mdlv=", mdl.mdlv, " mdls=", mdl.mdls, " mdla=", mdl.mdla,
	" bones=", mdl.puppet->bones.size (), " anims=", mdl.puppet->anims.size (),
	" verts=", mdl.vertexs.size (), " tris=", mdl.indices.size ()
	);
    return true;
}

} // namespace WallpaperEngine::Render::Puppet
