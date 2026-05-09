#include "Puppet.h"

#include <algorithm>
#include <cmath>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include "WallpaperEngine/Logging/Log.h"

namespace WallpaperEngine::Render::Puppet {

namespace {

// WPE applies euler angles in Z*Y*X order (intrinsic). Mirror that with glm so the same
// .mdl produces the same pose as Windows. Use double precision because the catsout
// reference does — keeps small angles from drifting under repeated multiplication.
glm::dquat eulerToQuat (const glm::vec3& euler) {
    const double cx = std::cos (euler.x * 0.5);
    const double sx = std::sin (euler.x * 0.5);
    const double cy = std::cos (euler.y * 0.5);
    const double sy = std::sin (euler.y * 0.5);
    const double cz = std::cos (euler.z * 0.5);
    const double sz = std::sin (euler.z * 0.5);

    const glm::dquat qx (cx, sx, 0.0, 0.0);
    const glm::dquat qy (cy, 0.0, sy, 0.0);
    const glm::dquat qz (cz, 0.0, 0.0, sz);

    return qz * qy * qx;
}

// Slerp dquat→dquat with a falloff factor [0,1] toward identity. Used to scale a quaternion's
// "amount" of rotation (1.0 = full, 0.0 = identity).
glm::dquat slerpToIdentity (const glm::dquat& q, double towardIdentity) {
    const glm::dquat ident (1.0, 0.0, 0.0, 0.0);
    return glm::slerp (q, ident, towardIdentity);
}

void genInterpolationInfo (
    Puppet::InterpolationInfo& info, double& cur, uint32_t length, double frameTime, double maxTime
) {
    cur = std::fmod (cur, maxTime);
    if (cur < 0.0) cur += maxTime;

    const double rate = cur / frameTime;
    info.frameA = static_cast<uint32_t> (rate) % length;
    info.frameB = (info.frameA + 1) % length;
    info.t = rate - static_cast<double> (info.frameA);
}

} // namespace

Puppet::InterpolationInfo Puppet::Animation::getInterpolationInfo (double* curTime) const {
    InterpolationInfo info;
    auto& cur = *curTime;

    if (mode == PlayMode::Loop || mode == PlayMode::Single) {
	genInterpolationInfo (info, cur, static_cast<uint32_t> (length), frameTime, maxTime);
    } else if (mode == PlayMode::Mirror) {
	// Walk forward then back: total cycle is 2*length frames. Map back into [0,length).
	const auto getFrame = [this] (uint32_t f) -> uint32_t {
	    return f >= static_cast<uint32_t> (length) ? (length - 1) - (f - length) : f;
	};
	genInterpolationInfo (info, cur, static_cast<uint32_t> (length) * 2, frameTime, maxTime * 2.0);
	info.frameA = getFrame (info.frameA);
	info.frameB = getFrame (info.frameB);
    }

    return info;
}

void Puppet::prepared () {
    // Walk bones in declared order. The .mdl format guarantees parents precede children,
    // so we can build the world-space transform array in one pass.
    std::vector<glm::mat4> combinedTrans (bones.size (), glm::mat4 (1.0f));
    for (size_t i = 0; i < bones.size (); ++i) {
	auto& b = bones[i];
	combinedTrans[i] = b.noParent () ? b.transform : combinedTrans[b.parent] * b.transform;
	b.offsetTrans = glm::inverse (combinedTrans[i]);
    }

    for (auto& anim : anims) {
	anim.frameTime = (anim.fps > 0.0) ? 1.0 / anim.fps : (1.0 / 30.0);
	anim.maxTime = static_cast<double> (anim.length) * anim.frameTime;
	for (auto& bf : anim.bframesArray) {
	    for (auto& f : bf.frames) {
		f.quaternion = eulerToQuat (f.angle);
	    }
	}
    }

    m_finalAffines.assign (bones.size (), glm::mat4 (1.0f));
}

std::span<const glm::mat4> Puppet::genFrame (PuppetLayer& puppetLayer, double dtSeconds) noexcept {
    const double globalBlend = puppetLayer.m_globalBlend;
    puppetLayer.updateInterpolation (dtSeconds);

    const glm::dquat ident (1.0, 0.0, 0.0, 0.0);

    for (size_t i = 0; i < m_finalAffines.size (); ++i) {
	const auto& bone = bones[i];

	const glm::mat4 parent = bone.noParent () ? glm::mat4 (1.0f) : m_finalAffines[bone.parent];

	// Start from bone's bind translation (scaled by global blend) and accumulate per-layer
	// offsets relative to that layer's first frame.
	glm::vec3 trans = glm::vec3 (bone.transform[3]) * static_cast<float> (globalBlend);
	glm::vec3 scale = glm::vec3 (1.0f) * static_cast<float> (globalBlend);
	glm::dquat quat = ident;

	for (auto& layer : puppetLayer.m_layers) {
	    const auto& alayer = layer.animLayer;
	    if (layer.anim == nullptr || !alayer.visible) continue;
	    if (i >= layer.anim->bframesArray.size ()) continue;

	    const auto& info = layer.interpInfo;
	    const auto& boneFrames = layer.anim->bframesArray[i].frames;
	    if (boneFrames.empty ()) continue;

	    const auto& frameBase = boneFrames[0];
	    const auto& frameA = boneFrames[std::min<size_t> (info.frameA, boneFrames.size () - 1)];
	    const auto& frameB = boneFrames[std::min<size_t> (info.frameB, boneFrames.size () - 1)];

	    const double t = info.t;
	    const double oneT = 1.0 - t;

	    // Quaternion: blend the (frame_a, frame_b) interpolated rotation in the
	    // base-relative frame, then re-apply the base orientation. Each step is also
	    // attenuated by the layer's blend factor so partial-strength layers
	    // partially deflect the bone.
	    const glm::dquat frameAdelta = frameA.quaternion * glm::conjugate (frameBase.quaternion);
	    const glm::dquat frameBdelta = frameB.quaternion * glm::conjugate (frameBase.quaternion);
	    const glm::dquat blendedDelta
		= slerpToIdentity (glm::slerp (frameAdelta, frameBdelta, t), 1.0 - alayer.blend);
	    const glm::dquat blendedBase = slerpToIdentity (frameBase.quaternion, 1.0 - layer.blend);
	    quat = quat * blendedDelta * blendedBase;

	    // Position: linear interpolation between frame A and frame B along the layer's
	    // delta-from-base axis, weighted by both the layer's normalized stack weight
	    // (`layer.blend`) and the user-set strength (`alayer.blend`). The catsout/owe
	    // reference applied alayer.blend to the delta WITHOUT layer.blend, which made
	    // the delta over-applied N× when N layers normalized to 1/N each — visible as
	    // doubled keyframe motion on multi-layer puppets like 3363252053. Both factors
	    // multiplied keeps the sum independent of layer count.
	    const glm::vec3 frameAposDelta = frameA.position - frameBase.position;
	    const glm::vec3 frameBposDelta = frameB.position - frameBase.position;
	    trans += static_cast<float> (layer.blend)
		* (frameBase.position
		   + static_cast<float> (alayer.blend)
		       * (frameAposDelta * static_cast<float> (oneT) + frameBposDelta * static_cast<float> (t)));

	    // Scale: same shape as position.
	    const glm::vec3 frameAscaleDelta = frameA.scale - frameBase.scale;
	    const glm::vec3 frameBscaleDelta = frameB.scale - frameBase.scale;
	    scale += static_cast<float> (layer.blend)
		* (frameBase.scale
		   + static_cast<float> (alayer.blend)
		       * (frameAscaleDelta * static_cast<float> (oneT) + frameBscaleDelta * static_cast<float> (t)));
	}

	const glm::dquat blendedRot = glm::slerp (quat, ident, globalBlend);
	glm::mat4 affine (1.0f);
	affine = glm::translate (affine, trans);
	affine = affine * glm::mat4_cast (glm::quat (blendedRot));
	affine = glm::scale (affine, scale);
	m_finalAffines[i] = parent * affine;
    }

    // Apply each bone's offset so the skinning matrix maps from rest-pose into the deformed
    // pose (standard skin transform).
    for (size_t i = 0; i < m_finalAffines.size (); ++i) {
	m_finalAffines[i] = m_finalAffines[i] * bones[i].offsetTrans;
    }

    return { m_finalAffines };
}

PuppetLayer::PuppetLayer () = default;
PuppetLayer::PuppetLayer (std::shared_ptr<Puppet> puppet) : m_puppet (std::move (puppet)) { }
PuppetLayer::~PuppetLayer () = default;

void PuppetLayer::prepared (std::span<AnimationLayer> alayers) {
    if (!m_puppet) return;

    m_layers.assign (alayers.size (), Layer {});
    m_globalBlend = 1.0;
    m_totalBlend = 0.0;

    for (const auto& a : alayers) {
	if (a.visible) m_totalBlend += a.blend;
    }

    // Walk in reverse: WPE blends "from top to bottom" of the layer stack, with each layer
    // taking some fraction of the remaining blend budget. `m_globalBlend` is the residual
    // weight on the bind pose — when the layer stack fully covers the bone (`totalBlend
    // >= 1`), bind contributes 0; otherwise the leftover budget multiplies bind. The
    // catsout/owe reference holds `blend` as a reference to `m_global_blend` so the loop
    // body's writes propagate; we mirror that here by writing through to m_globalBlend.
    const auto& anims = m_puppet->anims;
    for (size_t r = 0; r < alayers.size (); ++r) {
	const auto& alayer = alayers[alayers.size () - 1 - r];
	auto it = std::find_if (anims.begin (), anims.end (), [&] (const auto& a) {
	    return a.id == alayer.id;
	});

	const bool ok = it != anims.end () && alayer.visible;
	double curBlend = 0.0;
	if (ok) {
	    if (m_totalBlend > 1.0) {
		curBlend = alayer.blend / m_totalBlend;
		m_globalBlend = 0.0;
	    } else {
		curBlend = m_globalBlend * alayer.blend;
		m_globalBlend *= 1.0 - alayer.blend;
		m_globalBlend = std::max (m_globalBlend, 0.0);
	    }
	}

	m_layers[alayers.size () - 1 - r] = Layer {
	    .animLayer = alayer, .blend = curBlend, .anim = ok ? &(*it) : nullptr, .interpInfo = {},
	};
    }
}

void PuppetLayer::updateInterpolation (double dtSeconds) noexcept {
    for (auto& layer : m_layers) {
	if (!layer.anim) continue;
	layer.animLayer.curTime += dtSeconds * layer.animLayer.rate;
	layer.interpInfo = layer.anim->getInterpolationInfo (&layer.animLayer.curTime);
    }
}

std::span<const glm::mat4> PuppetLayer::genFrame (double dtSeconds) noexcept {
    if (!m_puppet) return {};
    return m_puppet->genFrame (*this, dtSeconds);
}

} // namespace WallpaperEngine::Render::Puppet
