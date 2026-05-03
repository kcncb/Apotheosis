#ifndef AIMBOTTARGET_H
#define AIMBOTTARGET_H

#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <opencv2/opencv.hpp>

// Basic bounding box + resolved pivot for the currently-locked target.
class AimbotTarget
{
public:
    AimbotTarget();
    AimbotTarget(int x, int y, int w, int h, int classId,
                 double pivotX = 0.0, double pivotY = 0.0);

    int x;
    int y;
    int w;
    int h;
    int classId;
    double pivotX;
    double pivotY;
};

struct LockedTargetInfo
{
    int trackId = -1;
    bool observedThisFrame = false;
    int missedFrames = 0;
    AimbotTarget target;
};

struct TrackDebugInfo
{
    int trackId = -1;
    int classId = -1;
    cv::Rect box;
    double pivotX = 0.0;
    double pivotY = 0.0;
    bool observedThisFrame = false;
    int missedFrames = 0;
    bool isLocked = false;
};

// Inputs for MultiTargetTracker::update(). The tracker only considers
// detections whose class_id is in `aim_class_ids` (populated from the active
// HotkeyProfile). The matching `y_offsets` map supplies per-class pivot Y
// offsets in the range [0, 1] (0 = top of box, 1 = bottom).
struct TrackerUpdate
{
    const std::vector<cv::Rect>* boxes = nullptr;
    const std::vector<int>* classes = nullptr;
    std::unordered_set<int> aim_class_ids{};
    std::unordered_map<int, float> y_offsets{};
    // Ordered priority list; the first class with any locked candidate wins
    // the lock. Classes not in this list are ignored.
    std::vector<int> priority_class_ids{};
    int screen_width = 0;
    int screen_height = 0;
    bool keep_current_lock = false;

    // Optional depth-derived visibility mask (CV_8UC1) in the same coordinate
    // space as the detection boxes (detection_resolution square). A pixel
    // value of 0 means "visible / not suppressed"; 255 means "suppressed by
    // depth mask (likely occluder such as a wall)". When provided, detection
    // pivots are re-computed as the centroid of visible pixels inside the
    // bbox, so that half-occluded targets aim at the exposed half instead of
    // the wall. Pass nullptr or an empty mat to fall back to the pure
    // geometric pivot.
    const cv::Mat* visibility_mask = nullptr;

    // Lock-switch hysteresis. When a hotkey is held and a track is already
    // locked, a challenger track must beat the locked track's score by at
    // least `lock_switch_score_margin` (fraction of half-diagonal) for at
    // least `lock_switch_min_frames` consecutive frames before the lock
    // transfers. Higher-priority class still preempts immediately.
    float lock_switch_score_margin = 0.15f;
    int lock_switch_min_frames = 3;

    // Minimum number of frames a lock must persist after acquisition. While
    // the hold counter is non-zero the tracker keeps the current lock
    // unconditionally (no priority preempt, no eligibility-driven hand-off).
    // 0 disables.
    int lock_hold_min_frames = 10;

    // y_offset distance/size decay. Large bboxes (close targets) suffer more
    // pivot jitter from a fixed y_offset because the same fraction maps to
    // many pixels. When enabled, the per-class y_offset is linearly blended
    // toward 0.5 (geometric center) as the bbox height grows from
    // `low_frac` × screen_height up to `high_frac` × screen_height.
    bool y_offset_size_decay_enabled = false;
    float y_offset_size_decay_low_frac = 0.10f;
    float y_offset_size_decay_high_frac = 0.40f;

    // Dynamic-FOV candidate gate. When > 0 these are detection-pixel
    // RADII (half-diameters) of an aim region centred on the crosshair.
    // Tracks whose pivot lies outside the elliptical region are excluded
    // from the lock candidate pool. 0 means "no gating" (legacy behaviour).
    // The mouse loop computes these per-frame from the locked target's
    // bbox and pivot distance — see HotkeyProfile::dynamic_fov_*.
    float fov_radius_x_px = 0.0f;
    float fov_radius_y_px = 0.0f;

    // Threat-weighted priority. When enabled, each track gets the production
    // heuristic score in [0,1] from class tier (head/body/other), motion
    // direction relative to crosshair, and hit-frame count. `threat_weight`
    // is the blending factor: 0 = ignore, 1 = full multiplicative effect.
    // Class ids may be -1 when the user hasn't selected that tier.
    bool  threat_priority_enabled = false;
    float threat_weight = 0.50f;
    int   threat_head_class_id = -1;
    int   threat_body_class_id = -1;
};

class MultiTargetTracker
{
public:
    void reset();
    void update(const TrackerUpdate& in);

    bool getLockedTarget(LockedTargetInfo& out) const;
    int getLockedTrackId() const { return lockedTrackId_; }
    std::vector<TrackDebugInfo> getDebugTracks() const;

private:
    struct TrackState
    {
        int id = -1;
        cv::Rect2f box{};
        cv::Point2f velocity{ 0.0f, 0.0f };
        int classId = -1;
        int hits = 0;
        int missed = 0;
        bool observedThisFrame = false;
        double pivotX = 0.0;
        double pivotY = 0.0;
        std::chrono::steady_clock::time_point lastUpdate{};
    };

    struct DetectionCandidate
    {
        cv::Rect2f box{};
        int classId = -1;
        double pivotX = 0.0;
        double pivotY = 0.0;
    };

    static float iou(const cv::Rect2f& a, const cv::Rect2f& b);
    int findTrackIndexById(int id) const;
    int allowedMissedFrames(const TrackState& t) const;
    void pruneDeadTracks();

    // Pick best track honouring the priority list (lower index = higher
    // priority), tie-broken by distance to screen center.
    int chooseBestTrack(const std::vector<int>& priority,
                        int screenWidth,
                        int screenHeight) const;

    // Per-track score/rank used by chooseBestTrack — exposed so the lock
    // hysteresis can compare a specific track against the current best.
    // Returns false when the track is ineligible (missed too long, class not
    // in priority list, index OOB). When `threat_enabled`, the score is
    // multiplied by (1 - weight*(threat-0.5)*2) so threat in [0,1] biases
    // the score down (better) for high-threat targets and up for low-threat
    // ones. The locked-id parameter lets us look up tracker context (only
    // velocity is used so a separate accessor isn't needed).
    bool evaluateTrack(int trackIndex,
                       const std::vector<int>& priority,
                       int screenWidth,
                       int screenHeight,
                       bool threat_enabled,
                       float threat_weight,
                       int   threat_head_class_id,
                       int   threat_body_class_id,
                       int& outRank,
                       double& outScore,
                       float& outThreat,
                       bool is_locked = false) const;

    std::vector<TrackState> tracks_;
    int nextId_ = 1;
    int lockedTrackId_ = -1;
    int maxMissedFrames_ = 6;

    // Hysteresis state for lock switching. challengerTrackId_ is the id of
    // the track that has been beating the locked track in recent frames;
    // challengerStreak_ is how many consecutive frames it has held that
    // advantage. Reset on lock change / reset() / resolution change.
    int challengerTrackId_ = -1;
    int challengerStreak_ = 0;

    // Minimum-hold counter. Set to `lock_hold_min_frames` whenever the
    // locked track id changes; decremented every update cycle. While > 0
    // the same-rank / higher-rank / eligibility logic is bypassed and the
    // current lock is preserved verbatim.
    int lockHoldRemaining_ = 0;

    // Threat-priority parameters captured from the latest TrackerUpdate so
    // chooseBestTrack (which is also called outside update()) can apply the
    // same weighting consistently.
    bool  lastThreatEnabled_ = false;
    float lastThreatWeight_ = 0.0f;
    int   lastThreatHeadClassId_ = -1;
    int   lastThreatBodyClassId_ = -1;

    // Dynamic FOV gate cached the same way (0 = no gate).
    float lastFovRadiusXpx_ = 0.0f;
    float lastFovRadiusYpx_ = 0.0f;
};

#endif // AIMBOTTARGET_H
