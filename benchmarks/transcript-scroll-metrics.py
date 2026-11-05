#!/usr/bin/env python3
"""Reduce a KETPLUS_CHAT_LAYOUT_TRACE file to the transcript scroll metrics.

M3 -- path-to-net ratio.  THE SUCCESS CRITERION.  Per continuous gesture run,
the total distance the content travelled divided by the distance it actually
got.  A hand-driven capture of the reported symptom scrolled 9,521 px of
content while moving it 381,302 px: a ratio of 40, meaning 97.5% of the motion
the reader saw was wasted.  It needs no threshold, it has a physical meaning,
and the target is 1.

M1 -- counter-gesture frames.  Over each contiguous run of frames in which a
gesture with a known direction is active, count the frames whose content offset
moves *opposite* to that direction by more than the settle threshold.  A useful
complement to M3: it localises the reversals rather than aggregating them.  The
momentum phase after the gesture ends is reported separately, because a fling
that reverses is exactly as visible to the reader as a gesture that does but has
a different cause.

M2 -- row re-entry.  DEMOTED; NOT THE SUCCESS CRITERION.  Counts how many times
each messageKey's row rect goes from not-intersecting to intersecting the
viewport rect.  It was written as the literal encoding of "the same passage
passes the viewport several times" and it does not work: on the hand capture of
the symptom it reads 2, the same as a clean synthetic run, because content that
oscillates around a position never takes the row off screen, so the row never
re-enters.  It is retained only as a secondary signal.  Do not re-promote it.

Reverse drift by collapsed-row density.  A collapsed activity-group member keeps
its index but has height 0, so no pixel-based metric can see it, and such rows
are the bulk of projected rows.  Per frame, motion that opposes its own scroll's
heading is bucketed by how many collapsed rows are around, because the defect in
collapsed-dense regions is not a large jump but a steady few-pixels-a-frame drift
that cancels the gesture and holds the reader in place.  RANK BY SIGN, NOT BY
MAGNITUDE: magnitude ranking reported the exact opposite of the truth here, and
lost M2 the same way.

Usage:
    transcript-scroll-metrics.py TRACE [TRACE ...] [--json OUT] [--threshold PX]
"""

import argparse
import json
import sys
from collections import Counter

# Below this, a reversal is sub-pixel settle rather than motion the reader sees.
DEFAULT_THRESHOLD_PX = 2.0


# "offset" is contentY; "readerOffset" is the accumulated screen motion of the
# row being read, which excludes position holds the reader never sees.
OFFSET_FIELD = "offset"


def read_events(path):
    """Yield trace records. A run killed mid-write can leave a partial last line."""
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            try:
                yield json.loads(line)
            except json.JSONDecodeError:
                continue


def split_runs(events):
    """Group the metric events into contiguous scroll runs.

    A run ends when a frame is idle (no gesture-frame emitted for it) or when the
    gesture direction flips, because the user turning around is not the defect we
    are counting.
    """
    runs = []
    current = None
    frame_had_gesture = False
    idle = True
    session = 0
    for event in events:
        name = event.get("event")
        if name == "frame-presented":
            # A frame that emitted no gesture-frame was idle, and idle is what
            # separates one scroll from the next.
            if not frame_had_gesture:
                current = None
                idle = True
            frame_had_gesture = False
        elif name == "gesture-frame":
            frame_had_gesture = True
            if idle:
                session += 1
                idle = False
            direction = int(event.get("direction") or 0)
            # M1 and M2 need a direction to be meaningful, so a direction change
            # starts a fresh run. M3 deliberately does NOT use this split: within
            # one uninterrupted scroll, a reversal is the defect, not a new scroll.
            if current is None or current["direction"] != direction:
                current = {"direction": direction, "session": session, "frames": [], "visibility": []}
                runs.append(current)
            current["frames"].append(
                {
                    "phase": event.get("phase"),
                    "offset": float(event.get(OFFSET_FIELD, event.get("offset", 0.0))),
                    "content_height": float(event.get("contentHeight") or 0.0),
                    "collapsed_realized": None,
                    "collapsed_in_span": None,
                    "collapsed_in_viewport": None,
                    "index_span": None,
                }
            )
        elif name == "row-visibility" and current is not None:
            current["visibility"].append(
                {"phase": event.get("phase"), "keys": event.get("keys") or []}
            )
            # Emitted after the gesture-frame of the same presented frame.
            if current["frames"]:
                frame = current["frames"][-1]
                frame["collapsed_realized"] = event.get("collapsedRealized")
                frame["collapsed_in_span"] = event.get("collapsedInSpan")
                frame["collapsed_in_viewport"] = event.get("collapsedInViewport")
                frame["index_span"] = event.get("indexSpan")
    for run in runs:
        run["direction"] = run["direction"] or inferred_direction(run)
    return runs


def inferred_direction(run):
    """Direction of a run the view could not name.

    A programmatic flick carries no wheel event, so the WheelHandler has no
    direction to report. Net displacement over the run is then the only honest
    statement of which way the reader was travelling; a run that nets out to
    nothing stays undirected and is excluded from both metrics.
    """
    if len(run["frames"]) < 2:
        return 0
    net = run["frames"][-1]["offset"] - run["frames"][0]["offset"]
    if abs(net) <= DEFAULT_THRESHOLD_PX:
        return 0
    # Scrolling up decreases the offset from the top of the content.
    return 1 if net < 0 else -1


def measure_m3(runs):
    """Path travelled against distance gained, per uninterrupted scroll.

    The unit is one scroll as the reader performs it -- every frame between two
    idle frames -- and NOT the direction-split runs M1 and M2 use. Splitting on
    direction would hide exactly the motion being measured, since a reversal
    inside one scroll is the defect rather than the start of a new scroll.

    No threshold and no direction convention: every pixel of movement counts
    towards the path, and the net is where the scroll ended relative to where it
    started. A perfect scroll has ratio 1.
    """
    sessions = {}
    for run in runs:
        sessions.setdefault(run["session"], []).extend(run["frames"])
    per_run = []
    total_path = 0.0
    total_forward = 0.0
    total_backward = 0.0
    total_net = 0.0
    for frames in sessions.values():
        if len(frames) < 2:
            continue
        offsets = [frame["offset"] for frame in frames]
        heading = 1.0 if offsets[-1] >= offsets[0] else -1.0
        path = 0.0
        forward = 0.0
        backward = 0.0
        for previous, current in zip(offsets, offsets[1:]):
            delta = current - previous
            path += abs(delta)
            # Forward is whichever way the scroll as a whole ended up going.
            if delta * heading > 0:
                forward += abs(delta)
            else:
                backward += abs(delta)
        net = offsets[-1] - offsets[0]
        total_path += path
        total_forward += forward
        total_backward += backward
        total_net += net
        per_run.append(
            {
                "frames": len(offsets),
                "net_px": round(net, 1),
                "path_px": round(path, 1),
                "forward_px": round(forward, 1),
                "backward_px": round(backward, 1),
                "ratio": round(path / abs(net), 2) if abs(net) > 1 else None,
            }
        )
    # The longest scroll dominates what the reader experienced, so it is named as
    # well as folded into the total.
    longest = max(per_run, key=lambda r: r["path_px"], default=None)
    # The aggregate ratio divides total path by total net, so it grows without
    # bound as forward and backward motion cancel. That is the honest headline --
    # cancelling motion is the defect -- but the median per scroll is reported
    # beside it so a single near-zero net cannot be mistaken for the whole story.
    ratios = [r["ratio"] for r in per_run if r["ratio"] is not None]
    return {
        "median_scroll_ratio": round(median(ratios), 2) if ratios else None,
        "scrolls_measured": len(per_run),
        "net_px": round(total_net, 1),
        "path_px": round(total_path, 1),
        "forward_px": round(total_forward, 1),
        "backward_px": round(total_backward, 1),
        "ratio": round(total_path / abs(total_net), 2) if abs(total_net) > 1 else None,
        "longest_scroll": longest,
        "per_scroll": per_run,
    }


# Collapsed-row density buckets. A collapsed activity-group member keeps its
# model index but has height 0, so these rows are invisible to every pixel-based
# metric while still occupying the index range ListView averages over.
COLLAPSED_BUCKETS = ((0, 5), (6, 20), (21, 60), (61, None))

# Teleports are a different defect (a single huge extent correction) and they
# dominate any sum they appear in, so per-frame drift is measured without them.
# The cap is reported, because the picture is genuinely sensitive to it.
TELEPORT_PX = 2000.0


def bucket_label(bucket):
    return "%d-%s" % (bucket[0], "+" if bucket[1] is None else bucket[1])


def measure_reverse_drift(runs, field, teleport=TELEPORT_PX):
    """Frames whose motion opposes the scroll they belong to, by collapsed density.

    THE RANKING TRAP, WRITTEN DOWN SO IT IS NOT WALKED INTO A FOURTH TIME.
    Ranking frames by the MAGNITUDE of backward movement hides this defect
    completely, and did: magnitude-ranked slicing of a hand capture reported
    collapsed density as *inversely* related to backward movement, the exact
    opposite of the truth. Large jumps live in text-dense regions; collapsed-dense
    regions fail by drifting backwards a few pixels at a time, every frame, which
    no magnitude ranking will ever surface. Look at the SIGN, per frame, bucketed
    by the population you suspect. M2 was lost to the same class of error.

    The scroll's own heading (its net displacement) is the reference, not the
    WheelHandler's reported direction: that direction persists across gestures and
    mixes downward frames into an upward scroll, which is enough on its own to
    invert the net of a bucket.
    """
    buckets = {}
    for bucket in COLLAPSED_BUCKETS:
        buckets[bucket] = {
            "frames": 0,
            "path_px": 0.0,
            "progress_px": 0.0,
            "reverse_frames": 0,
            "reverse_px": 0.0,
            "height_swings": [],
        }
    sessions = {}
    for run in runs:
        sessions.setdefault(run["session"], []).extend(run["frames"])
    teleports = 0
    for frames in sessions.values():
        if len(frames) < 2:
            continue
        net = frames[-1]["offset"] - frames[0]["offset"]
        if net == 0:
            continue
        heading = -1.0 if net < 0 else 1.0
        for previous, current in zip(frames, frames[1:]):
            density = current.get(field)
            if density is None:
                continue
            delta = current["offset"] - previous["offset"]
            if abs(delta) > teleport:
                teleports += 1
                continue
            for bucket in COLLAPSED_BUCKETS:
                if density < bucket[0] or (bucket[1] is not None and density > bucket[1]):
                    continue
                entry = buckets[bucket]
                entry["frames"] += 1
                entry["path_px"] += abs(delta)
                # Signed against the scroll's own heading, so that downward and
                # upward scrolls can share a bucket: positive is progress in the
                # direction the reader asked for, negative is ground lost.
                entry["progress_px"] += delta * heading
                entry["height_swings"].append(
                    abs(current["content_height"] - previous["content_height"])
                )
                # Opposing the scroll's own heading is the defect signature.
                if delta * heading < 0:
                    entry["reverse_frames"] += 1
                    entry["reverse_px"] += abs(delta)
                break

    rows = []
    for bucket in COLLAPSED_BUCKETS:
        entry = buckets[bucket]
        if not entry["frames"]:
            continue
        frames = entry["frames"]
        rows.append(
            {
                "bucket": bucket_label(bucket),
                "frames": frames,
                "path_px": round(entry["path_px"], 1),
                "progress_px": round(entry["progress_px"], 1),
                "progress_per_frame": round(entry["progress_px"] / frames, 1),
                # M3 restricted to this bucket: motion spent per pixel gained.
                "waste": (
                    round(entry["path_px"] / abs(entry["progress_px"]), 2)
                    if abs(entry["progress_px"]) > 1
                    else None
                ),
                "reverse_frames": entry["reverse_frames"],
                "reverse_frame_share": round(entry["reverse_frames"] / frames, 4),
                "reverse_px": round(entry["reverse_px"], 1),
                "height_swing_mean": round(
                    sum(entry["height_swings"]) / len(entry["height_swings"]), 1
                ),
            }
        )
    return {"field": field, "teleports_excluded": teleports, "teleport_px": teleport, "buckets": rows}


def drift_verdict(drift):
    """What the bucket table says, in words, so the numbers are not re-misread."""
    rows = drift["buckets"]
    if len(rows) < 2:
        return "not enough populated buckets to compare"
    shares = [row["reverse_frame_share"] for row in rows]
    sparsest, densest = rows[0], rows[-1]
    notes = []
    if densest["reverse_frame_share"] >= 2 * max(sparsest["reverse_frame_share"], 0.005):
        notes.append(
            "reverse-drift frames rise with collapsed density (%.1f%% sparsest -> %.1f%% densest)"
            % (100 * sparsest["reverse_frame_share"], 100 * densest["reverse_frame_share"])
        )
    elif max(shares) - min(shares) < 0.02:
        notes.append("reverse-drift frames do not separate by collapsed density")
    else:
        notes.append("reverse-drift frames vary but not monotonically with collapsed density")
    wastes = [row["waste"] for row in rows if row["waste"] is not None]
    if len(wastes) >= 2 and wastes[-1] >= 2 * wastes[0]:
        notes.append(
            "waste rises with collapsed density (%.1fx sparsest -> %.1fx densest)"
            % (wastes[0], wastes[-1])
        )
    # A bucket that nets backwards over a whole scroll is not jitter.
    for row in rows:
        if row["progress_px"] < 0:
            notes.append(
                "bucket %s NETS BACKWARDS (%+.1f px/frame over %d frames): the reader is"
                " held in place there" % (row["bucket"], row["progress_per_frame"], row["frames"])
            )
    return "; ".join(notes)



def median(values):
    ordered = sorted(values)
    if not ordered:
        return 0
    middle = len(ordered) // 2
    if len(ordered) % 2:
        return ordered[middle]
    return (ordered[middle - 1] + ordered[middle]) / 2


def measure_m1(runs, threshold):
    """Counter-gesture movement, split by phase, over runs with a known direction."""
    out = {}
    for phase in ("gesture", "momentum", "idle"):
        episodes = 0
        backward_px = 0.0
        worst = 0.0
        frames = 0
        in_episode = False
        for run in runs:
            if run["direction"] == 0:
                continue
            previous = None
            for frame in run["frames"]:
                if frame["phase"] != phase:
                    # Frames of the other phase break the chain rather than
                    # contributing a delta that straddles the phase boundary.
                    previous = None
                    in_episode = False
                    continue
                if previous is not None:
                    frames += 1
                    # direction > 0 is an upward gesture, which must *decrease*
                    # the offset; anything increasing it is counter-gesture.
                    delta = frame["offset"] - previous
                    backward = delta if run["direction"] > 0 else -delta
                    if backward > threshold:
                        backward_px += backward
                        worst = max(worst, backward)
                        if not in_episode:
                            episodes += 1
                            in_episode = True
                    else:
                        in_episode = False
                previous = frame["offset"]
            in_episode = False
        out[phase] = {
            "frames_compared": frames,
            "counter_episodes": episodes,
            "counter_pixels_total": round(backward_px, 1),
            "largest_single_backward_px": round(worst, 1),
        }
    return out


def measure_m2(runs):
    """Viewport entries per messageKey, over runs that scroll upward.

    Secondary only. See the module docstring: this reads the same on a capture of
    the symptom as on a clean run, because oscillation around a position never
    takes a row off screen. M3 is the success criterion.
    """
    # Counted per run, never across runs: a key that is on screen when a new
    # scroll starts has not re-entered, it never left.
    counts = []
    runs_measured = 0
    repeated = 0
    worst_key = None
    worst_count = 0
    for run in runs:
        if run["direction"] <= 0 or not run["visibility"]:
            continue
        runs_measured += 1
        entries = Counter()
        on_screen = set()
        for sample in run["visibility"]:
            keys = set(sample["keys"])
            for key in keys - on_screen:
                entries[key] += 1
            on_screen = keys
        for key, n in entries.items():
            counts.append(n)
            repeated += n > 1
            if n > worst_count:
                worst_key, worst_count = key, n
    counts.sort(reverse=True)
    return {
        "upward_runs_measured": runs_measured,
        "key_run_pairs": len(counts),
        "keys_entering_more_than_once": repeated,
        "max_entries_per_key": counts[0] if counts else 0,
        "p99_entries_per_key": counts[int(0.01 * (len(counts) - 1))] if counts else 0,
        "worst_key": worst_key,
        "worst_key_entries": worst_count,
        "distribution": {str(n): c for n, c in sorted(Counter(counts).items())},
    }


def reduce_trace(path, threshold):
    events = list(read_events(path))
    runs = split_runs(events)
    return {
        "trace": path,
        "events": len(events),
        "runs": len(runs),
        "m3_path_to_net": measure_m3(runs),
        "m1_counter_gesture": measure_m1(runs, threshold),
        "m2_row_reentry": measure_m2(runs),
        "reverse_drift_in_span": measure_reverse_drift(runs, "collapsed_in_span"),
        "reverse_drift_realized": measure_reverse_drift(runs, "collapsed_realized"),
    }


def format_report(result):
    lines = ["%s  (%d events, %d runs)" % (result["trace"], result["events"], result["runs"])]
    m3 = result["m3_path_to_net"]
    lines.append(
        "  M3 ratio=%s  net=%.0fpx path=%.0fpx (forward %.0f / backward %.0f) over %d scrolls"
        % (
            "n/a" if m3["ratio"] is None else "%.1fx" % m3["ratio"],
            m3["net_px"],
            m3["path_px"],
            m3["forward_px"],
            m3["backward_px"],
            m3["scrolls_measured"],
        )
    )
    lines.append(
        "  M3 median ratio per scroll=%s"
        % ("n/a" if m3["median_scroll_ratio"] is None else "%.1fx" % m3["median_scroll_ratio"])
    )
    longest = m3["longest_scroll"]
    if longest:
        lines.append(
            "  M3 longest scroll: ratio=%s net=%.0fpx path=%.0fpx over %d frames"
            % (
                "n/a" if longest["ratio"] is None else "%.1fx" % longest["ratio"],
                longest["net_px"],
                longest["path_px"],
                longest["frames"],
            )
        )
    for phase, m1 in result["m1_counter_gesture"].items():
        lines.append(
            "  M1 %-8s episodes=%-4d backward_px=%-10.1f worst=%-8.1f frames=%d"
            % (
                phase,
                m1["counter_episodes"],
                m1["counter_pixels_total"],
                m1["largest_single_backward_px"],
                m1["frames_compared"],
            )
        )
    m2 = result["m2_row_reentry"]
    lines.append(
        "  M2 (secondary) upward_runs=%d key_runs=%d with_reentry=%d max=%d p99=%d"
        % (
            m2["upward_runs_measured"],
            m2["key_run_pairs"],
            m2["keys_entering_more_than_once"],
            m2["max_entries_per_key"],
            m2["p99_entries_per_key"],
        )
    )
    lines.append("  M2 (secondary) distribution (entries -> keys): %s" % m2["distribution"])
    if m2["worst_key"]:
        lines.append(
            "  M2 (secondary) worst key entered %d times" % m2["worst_key_entries"]
        )
    for key, title in (
        ("reverse_drift_in_span", "collapsed in index span"),
        ("reverse_drift_realized", "collapsed realized"),
    ):
        drift = result[key]
        if not drift["buckets"]:
            continue
        lines.append(
            "  reverse drift by %s (teleports >%.0fpx excluded: %d)"
            % (title, drift["teleport_px"], drift["teleports_excluded"])
        )
        lines.append(
            "    %-8s %7s %10s %10s %9s %8s %10s"
            % ("bucket", "frames", "path_px", "progress", "waste", "reverse", "swing_mean")
        )
        for row in drift["buckets"]:
            lines.append(
                "    %-8s %7d %10.0f %+10.0f %8.1fx %5d/%3.0f%% %11.0f"
                % (
                    row["bucket"],
                    row["frames"],
                    row["path_px"],
                    row["progress_px"],
                    row["waste"] or 0.0,
                    row["reverse_frames"],
                    100 * row["reverse_frame_share"],
                    row["height_swing_mean"],
                )
            )
        lines.append("    %s" % drift_verdict(drift))
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("traces", nargs="+")
    parser.add_argument("--json", dest="json_out")
    parser.add_argument("--threshold", type=float, default=DEFAULT_THRESHOLD_PX)
    parser.add_argument("--offset-field", default="offset", choices=["offset", "readerOffset"])
    args = parser.parse_args()
    global OFFSET_FIELD
    OFFSET_FIELD = args.offset_field

    results = [reduce_trace(path, args.threshold) for path in args.traces]
    for result in results:
        print(format_report(result))
    if args.json_out:
        with open(args.json_out, "w", encoding="utf-8") as handle:
            json.dump(results, handle, indent=2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
