#include "flashlight_tracker.h"

#include <algorithm>

namespace crosshair
{

std::vector<FlashlightTrackVerdict> FlashlightTracker::update(
    const std::vector<cv::Point2f>& spots,
    const std::vector<float>&       radii,
    const FlashlightTemporal&       p)
{
    for (auto& tr : tracks_)
        tr.seen_this = false;

    std::vector<FlashlightTrackVerdict> out(spots.size());
    std::vector<char> taken(tracks_.size(), 0);

    const float max_jump2 = p.max_jump_px * p.max_jump_px;
    const int   confirm_k = std::max(1, p.confirm_frames);

    for (size_t i = 0; i < spots.size(); ++i)
    {
        // Greedy nearest-free-track association within the jump radius.
        int   best  = -1;
        float bestd2 = max_jump2;
        for (size_t k = 0; k < tracks_.size(); ++k)
        {
            if (taken[k]) continue;
            const float dx = tracks_[k].center.x - spots[i].x;
            const float dy = tracks_[k].center.y - spots[i].y;
            const float d2 = dx * dx + dy * dy;
            if (d2 <= bestd2) { bestd2 = d2; best = static_cast<int>(k); }
        }

        FlashlightTrackVerdict v;
        if (best >= 0)
        {
            Track& tr = tracks_[static_cast<size_t>(best)];
            taken[static_cast<size_t>(best)] = 1;
            tr.center    = spots[i];
            tr.radius    = (i < radii.size()) ? radii[i] : tr.radius;
            tr.age       = std::min(tr.age + 1, 100000);
            tr.unseen    = 0;
            tr.seen_this = true;
            v.age        = tr.age;
            v.onset      = false;
            v.confirmed  = (tr.age >= confirm_k);
        }
        else
        {
            Track tr;
            tr.center    = spots[i];
            tr.radius    = (i < radii.size()) ? radii[i] : 0.0f;
            tr.age       = 1;
            tr.unseen    = 0;
            tr.seen_this = true;
            tracks_.push_back(tr);
            taken.push_back(1); // keep `taken` parallel; new track already claimed
            v.age       = 1;
            v.onset     = true;
            v.confirmed = (1 >= confirm_k); // K==1 → confirmed on first sight
        }
        out[i] = v;
    }

    // Age unseen tracks; a single miss resets the consecutive-age counter (so a
    // winking glint can never accumulate to confirmed) but the track lingers for
    // drop_after ticks so a briefly-occluded real halo re-associates cleanly.
    for (auto& tr : tracks_)
    {
        if (!tr.seen_this)
        {
            tr.unseen += 1;
            tr.age = 0;
        }
    }
    tracks_.erase(
        std::remove_if(tracks_.begin(), tracks_.end(),
                       [&](const Track& tr) { return tr.unseen > p.drop_after; }),
        tracks_.end());

    return out;
}

void FlashlightTracker::reset()
{
    tracks_.clear();
}

} // namespace crosshair
