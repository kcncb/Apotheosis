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
    // Threat scoring telemetry. `threat` is the [0,1] blended threat score
    // produced by compute_threat_score (depth + head-class confidence). The
    // raw inputs are exposed too: `confidence` is the latest matched detection
    // confidence; `depth_at_pivot` is the normalized depth (0..1, 1 = closest)
    // sampled at the track pivot, or -1 when depth inference is unavailable.
    float threat = 0.5f;
    float confidence = 0.0f;
    float depth_at_pivot = -1.0f;
};

// Inputs for MultiTargetTracker::update(). The tracker only considers
// detections whose class_id is in `aim_class_ids` (populated from the active
// HotkeyProfile). The matching `y_offsets` map supplies per-class pivot Y
// offsets in the range [0, 1] (0 = top of box, 1 = bottom).
struct TrackerUpdate
{
    const std::vector<cv::Rect>* boxes = nullptr;
    const std::vector<int>* classes = nullptr;
    // Per-detection confidence aligned with `boxes`/`classes`. Optional: when
    // null or shorter than boxes, missing entries are treated as 0. Used by
    // the threat-scoring formula's head-confidence term.
    const std::vector<float>* confidences = nullptr;
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

    // Optional normalized depth map (CV_8UC1, 255 = closest, 0 = farthest)
    // from depth_anything in the same coord space as detection boxes. The
    // tracker samples this at each track pivot to produce the "depth" term of
    // the threat-scoring formula. Pass nullptr / empty when depth inference
    // is disabled — the tracker falls back to a neutral 0.5 depth score.
    const cv::Mat* depth_normalized = nullptr;

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

    // Aim origin for target-selection distance: the point candidates are
    // ranked by proximity to. Set to the crosshair-colour pivot when live,
    // otherwise the detection-image centre. A negative value is a sentinel
    // meaning "use screen centre".
    double aim_origin_x = -1.0;
    double aim_origin_y = -1.0;

    // Threat-weighted priority. When enabled, each track gets a [0,1] threat
    // score blended from (a) normalized depth at the track pivot (closer =
    // higher threat) and (b) head-class detection confidence (higher conf =
    // higher threat). `threat_depth_head_ratio` linearly mixes the two
    // signals: 0 = full depth, 1 = full head-conf. `threat_weight` is the
    // overall multiplier into the lock-candidate score (0 = ignore, 1 = full
    // multiplicative effect). `threat_head_class_id` may be -1 when the user
    // hasn't selected a head class — the head-conf term then falls back to a
    // neutral 0.5.
    bool  threat_priority_enabled = false;
    float threat_weight = 0.50f;
    int   threat_head_class_id = -1;
    float threat_depth_head_ratio = 0.5f;

    // 近距离瞄头(body→head pivot 吸附)。body 候选框高 ≥
    // close_range_trigger_height_frac × screen_height(近距离),且其内部上半区存在一个
    // close_range_head_class_id 的 head 框时,把该候选 pivot 吸附到 head。head 类无需在
    // aim_class_ids 中;close_range_head_class_id < 0 时关闭。只移动 pivot、不改变 track。
    bool  close_range_head_aim_enabled = false;
    int   close_range_head_class_id = -1;
    float close_range_trigger_height_frac = 0.30f;

    // 多人锁切（kill-detect）。当 trigger 在最近 trigger_fresh_ms 内打出，且锁定目标紧接
    // 失观 ≥ kill_suspicion_missed_frames 帧（叠加 bbox 急剧塌缩判定），跳过 lock_hold
    // 倒计时、立即把锁切到本帧可见的次目标。kill_suspicion_bbox_shrink ∈ [0,1] 是
    // bbox 面积塌缩比例阈值（0.55 = 缩到原来 55% 以下视为击杀征兆），<=0 关闭塌缩判定
    // （仅靠失观计数）。
    bool  kill_detect_enabled = true;
    int   kill_suspicion_missed_frames = 2;
    int   kill_trigger_fresh_ms = 250;
    float kill_suspicion_bbox_shrink = 0.55f;

    // Coasting-lock fast handoff 的 grace 帧数。锁定目标连续未观测 ≥ 此值时,
    // 立刻把锁交给本帧最佳可见目标。低值=灵活(目标稍丢就换),高值=守锁
    // (允许短暂遮挡/掉帧不切走)。原版硬编 1,与 lock_hold/kill_detect 配合时会产
    // 生切换乒乓:加大此值可彻底消除乒乓。
    int   coast_grace_frames = 1;

    // 击杀回收成功的"次目标快速接管"参数。次目标接管后短时间内允许同 rank 直接夺锁
    // （绕过 lock_switch hysteresis），让连杀更顺滑。0 关闭。
    int   kill_followup_grace_frames = 6;
    // 距上次 smart_trigger 击发的毫秒数。由 mouse_thread_loop 从 g_smart_trigger_ready
    // 的下沿事件维护；INT_MAX 表示从未击发或太久没击发。tracker 内部用此值与
    // kill_trigger_fresh_ms 比较，判定击杀检测窗口是否新鲜。
    int   ms_since_last_fire = 0x7fffffff;
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
        // Latest matched-detection confidence, persisted across coast frames
        // so phantom-rejection blips don't zero out the head-conf threat term.
        float confidence = 0.0f;
        // 击杀检测：记录最近一次观测时 bbox 面积，下一帧失观时与之比较判塌缩。
        float lastObservedArea = 0.0f;
        // 当前帧 bbox 面积相对 lastObservedArea 的比例（[0,1]，1=未塌缩）。
        // 失观时不更新，所以 staleness 自带在 missed 里。
        float lastAreaRatio = 1.0f;
        std::chrono::steady_clock::time_point lastUpdate{};
    };

    struct DetectionCandidate
    {
        cv::Rect2f box{};
        int classId = -1;
        float confidence = 0.0f;
        double pivotX = 0.0;
        double pivotY = 0.0;
    };

    static float iou(const cv::Rect2f& a, const cv::Rect2f& b);
    int findTrackIndexById(int id) const;
    int allowedMissedFrames(const TrackState& t) const;
    void pruneDeadTracks();

    // Pick best track honouring the priority list (lower index = higher
    // priority), tie-broken by distance to screen center. When `observedOnly`
    // is set, tracks that were not observed this frame (coasting on
    // prediction) are skipped — used to hand a stale lock over to a target
    // that genuinely exists right now.
    int chooseBestTrack(const std::vector<int>& priority,
                        int screenWidth,
                        int screenHeight,
                        bool observedOnly = false) const;

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
                       float threat_depth_head_ratio,
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
    // same weighting consistently. `lastDepthNormalized_` holds a refcounted
    // shallow copy of the depth map so the pointer can't dangle between
    // chained calls inside update().
    bool  lastThreatEnabled_ = false;
    float lastThreatWeight_ = 0.0f;
    int   lastThreatHeadClassId_ = -1;
    float lastThreatDepthHeadRatio_ = 0.5f;
    cv::Mat lastDepthNormalized_{};

    // Dynamic FOV gate cached the same way (0 = no gate).
    float lastFovRadiusXpx_ = 0.0f;
    float lastFovRadiusYpx_ = 0.0f;

    // Aim origin (selection distance reference) cached from the latest
    // TrackerUpdate. Negative = use screen centre.
    double lastAimOriginX_ = -1.0;
    double lastAimOriginY_ = -1.0;

    // 击杀检测 / 多人锁切状态。
    //   killSuspectedTrackId_ = 上一次被判定击杀的 track id，用于触发连杀 grace。
    //   killGraceRemaining_   = 击杀后允许同 rank 快速接管的剩余帧数；> 0 时
    //                           lock_switch hysteresis 被绕过，新挑战者立即夺锁。
    int killSuspectedTrackId_ = -1;
    int killGraceRemaining_ = 0;
};

#endif // AIMBOTTARGET_H
