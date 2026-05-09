#pragma once

// Parser for Wallpaper Engine's puppet .mdl files (header "MDLV<n>").
// The format is a single binary stream: header → mesh (vertices + indices) → bone hierarchy
// → optional MDAT attachments → MDLA animation table.
//
// Ported from catsout/wallpaper-scene-renderer + waywallen/open-wallpaper-engine, both
// GPLv2+ — version coverage is mdlv 4, 13, 14, 16, 17 and mdla 0/1/3.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <glm/mat4x4.hpp>

#include "Puppet.h"
#include "WallpaperEngine/Assets/AssetLocator.h"

namespace WallpaperEngine::Render::Puppet {

struct Mdl {
    int32_t mdlv { 13 };
    int32_t mdls { 1 };
    int32_t mdla { 1 };

    /** Path to the material JSON the puppet's mesh references (e.g. "materials/foo.json"). */
    std::string matJsonFile;

    /** Per-vertex skinned attributes — what we'll feed to the GPU. */
    struct Vertex {
	std::array<float, 3> position;
	std::array<uint32_t, 4> blendIndices;
	std::array<float, 4> weight;
	std::array<float, 2> texcoord;
    };
    std::vector<Vertex> vertexs;
    std::vector<std::array<uint16_t, 3>> indices;

    /** Bone hierarchy + animations, fully baked (prepared() already called). */
    PuppetSharedPtr puppet;

    /** Named attachment points read from the file's MDAT section. WPE wallpapers
     *  use these to pin a child image (with `attachment: <name>` in scene.json)
     *  to a specific bone in the parent puppet's skeleton. The MDAT record stores
     *  a `boneIndex` (which bone in the parent puppet the attachment hangs off
     *  of) plus a 4x4 transform expressing the attachment's offset from that
     *  bone's local frame. To position the child image, accumulate the bone's
     *  world transform (its bind transform times its parent chain) and post-
     *  multiply by `transform` — the result's translation column is where the
     *  attachment lands in puppet-local coordinates. */
    struct Attachment {
	uint32_t boneIndex { 0 };
	std::string name;
	glm::mat4 transform { 1.0f };
    };
    std::vector<Attachment> attachments;
};

class MdlParser {
public:
    /** Read and decode the .mdl from the wallpaper's asset locator (which transparently
     *  maps to the appropriate scene.pkg / loose-file location). Returns true on success.
     *  Logs sLog.error and returns false for any unrecoverable parse failure — the caller
     *  must handle that by falling back to non-puppet rendering. */
    static bool parse (const Assets::AssetLocator& locator, const std::string& path, Mdl& out);
};

} // namespace WallpaperEngine::Render::Puppet
