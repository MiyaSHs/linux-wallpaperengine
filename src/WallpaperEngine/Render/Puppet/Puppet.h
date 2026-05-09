#pragma once

// Puppet warp runtime: bone hierarchy, animation keyframes, and per-frame layer blending.
//
// Ported from catsout/wallpaper-scene-renderer (GPLv2-or-later) and waywallen's open-
// wallpaper-engine, retargeted from Eigen to GLM. The math follows WPE's Windows
// implementation: each bone has a bind transform; animations are arrays of (position, euler
// angles, scale) keyframes per bone; the engine blends multiple animation layers each frame
// using quaternion delta blending against the layer's first frame as the rest pose.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace WallpaperEngine::Render::Puppet {

class PuppetLayer;

class Puppet {
public:
    enum class PlayMode { Loop, Mirror, Single };

    struct Bone {
	/** 4x4 bind transform read from the .mdl (4x3 in file, expanded to 4x4 here). */
	glm::mat4 transform { 1.0f };
	/** Inverse of the world-space accumulated transform; baked in prepared(). */
	glm::mat4 offsetTrans { 1.0f };
	/** Index of parent bone in the bones vector. 0xFFFFFFFF = root. */
	uint32_t parent { 0xFFFFFFFFu };

	[[nodiscard]] bool noParent () const { return parent == 0xFFFFFFFFu; }
    };

    struct BoneFrame {
	glm::vec3 position { 0.0f };
	glm::vec3 angle { 0.0f }; // euler radians (XYZ)
	glm::vec3 scale { 1.0f };
	/** Cached quaternion from the euler angles, computed in prepared(). */
	glm::dquat quaternion { 1.0, 0.0, 0.0, 0.0 };
    };

    struct InterpolationInfo {
	uint32_t frameA { 0 };
	uint32_t frameB { 0 };
	double t { 0.0 };
    };

    struct Animation {
	int32_t id { 0 };
	double fps { 30.0 };
	int32_t length { 1 };
	PlayMode mode { PlayMode::Loop };
	std::string name;

	struct BoneFrames {
	    std::vector<BoneFrame> frames;
	};
	std::vector<BoneFrames> bframesArray;

	/** Computed in prepared(). */
	double maxTime { 0.0 };
	double frameTime { 1.0 / 30.0 };

	[[nodiscard]] InterpolationInfo getInterpolationInfo (double* curTime) const;
    };

    std::vector<Bone> bones;
    std::vector<Animation> anims;

    /** Bake derived fields after parse: offsetTrans, frame quaternions, frame_time, max_time. */
    void prepared ();

    /** Compute the final per-bone affine transform for the given layer config and time.
     *  Returns a span over an internal buffer that the caller can upload as the g_Bones uniform.
     *  The matrices are stored as 4x4 (top 3 rows are the meaningful 4x3 part for skinning). */
    std::span<const glm::mat4> genFrame (PuppetLayer& puppetLayer, double time) noexcept;

private:
    std::vector<glm::mat4> m_finalAffines;
};

class PuppetLayer {
    friend class Puppet;

public:
    /** Per-layer config the parser/scene gets from the wallpaper's `animationlayers` array.
     *  Note: `id` here is the WPE animation id (the JSON `animation` field), not the layer's
     *  own id — Puppet::Animation::id is the same id space. */
    struct AnimationLayer {
	int32_t id { 0 };
	double rate { 1.0 };
	double blend { 1.0 };
	bool visible { true };
	double curTime { 0.0 };
    };

    PuppetLayer ();
    explicit PuppetLayer (std::shared_ptr<Puppet> puppet);
    ~PuppetLayer ();

    [[nodiscard]] bool hasPuppet () const { return static_cast<bool> (m_puppet); }

    /** Build per-layer state. Must be called before genFrame(). */
    void prepared (std::span<AnimationLayer> animLayers);

    /** Advance per-layer cur_time by dtSeconds*rate and recompute interpolation indices. */
    void updateInterpolation (double dtSeconds) noexcept;

    /** Convenience: delegates to the underlying Puppet. */
    std::span<const glm::mat4> genFrame (double dtSeconds) noexcept;

private:
    struct Layer {
	AnimationLayer animLayer;
	double blend { 0.0 };
	const Puppet::Animation* anim { nullptr };
	Puppet::InterpolationInfo interpInfo {};
    };

    double m_globalBlend { 1.0 };
    double m_totalBlend { 0.0 };

    std::vector<Layer> m_layers;
    std::shared_ptr<Puppet> m_puppet;
};

using PuppetSharedPtr = std::shared_ptr<Puppet>;

} // namespace WallpaperEngine::Render::Puppet
