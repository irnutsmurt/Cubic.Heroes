#!/usr/bin/env python3
"""M8.2's picture comparison (docs/FFMPEG_MIGRATION_PLAN.md section 5): the planes of each frame a run decoded in the
game (DOTHEROES_MOVIE_PLANES=planes.txt on a lift with the movie trace's observers: Bink's own decode, or the native
player's), hashed as tests/native/ffmpeg_decode_check hashes FFmpeg's decode of the same file, against that offline
decode's hashes (<movie>.frames.txt).

  tools/m8_frames_compare.py <offline hashes folder> <run or planes.txt>...

Per run and movie: the frames recorded (a movie may be decoded more than once in a run), how many have the same
planes as FFmpeg's decode of that frame, the first that differs. Reads hashes only (the player's movies stay where
they are).
"""
import collections
import os
import sys


def offline(folder):
    out = {}
    for f in os.listdir(folder):
        if f.endswith(".frames.txt"):
            movie = f[:-len(".frames.txt")]
            with open(os.path.join(folder, f)) as h:   # numbered from 0 there; the game's frames from 1
                out[movie] = {int(a) + 1: b for a, b in (line.split() for line in h if line.strip())}
    return out


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    ref = offline(sys.argv[1])
    bad = 0
    for arg in sys.argv[2:]:
        path = os.path.join(arg, "planes.txt") if os.path.isdir(arg) else arg
        if not os.path.exists(path):
            print("%s: no planes.txt" % arg)
            bad += 1
            continue
        per = collections.OrderedDict()
        with open(path) as h:
            for line in h:
                p = line.split()
                if len(p) != 3:
                    continue
                movie, frame, digest = p[0], int(p[1]), p[2]
                s = per.setdefault(movie, {"n": 0, "same": 0, "first_diff": None, "frames": set(), "unknown": 0})
                s["n"] += 1
                s["frames"].add(frame)
                want = ref.get(movie, {}).get(frame)
                if want is None:
                    s["unknown"] += 1
                elif want == digest:
                    s["same"] += 1
                elif s["first_diff"] is None:
                    s["first_diff"] = frame
        print("%s:" % arg)
        for movie, s in per.items():
            total = len(ref.get(movie, {}))
            print("  %s: %d frames recorded (%d different frames of %d), %d the same as FFmpeg's decode%s%s" % (
                movie, s["n"], len(s["frames"]), total, s["same"],
                "" if s["first_diff"] is None else ", the first different: frame %d" % s["first_diff"],
                "" if not s["unknown"] else ", %d with no offline hash" % s["unknown"]))
            if s["same"] != s["n"]:
                bad += 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
