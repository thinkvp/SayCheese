// job.h -- the analysis job, ported from engine/analyzer.py (Job) and
// engine/server.py (job registry). Jobs run on a worker thread one at a time;
// the panel polls a JSON snapshot shaped like the old engine's /jobs/<id>.
#pragma once
#include <string>

namespace saycheese {

struct Segment {
    std::string id, mediaPath, clipName;
    double srcIn = 0, srcOut = 0, seqStart = 0, speed = 1;
    int track = 0;

    double seqTime(double srcT) const { return seqStart + (srcT - srcIn) / speed; }
};

struct JobOptions {
    double sampleFps = 4;
    int maxResults = 24;
    double minGap = 3;
    double minSmile = 0.15;
    double window = 0.75;
    std::string priority = "balanced";
};

/** Directory holding the module (addon or exe) this code is linked into. */
std::string moduleDir();

/** Loads MediaPipe from next to the module. Thumbnails go in job-N folders
 *  under `workDir`; leftovers from earlier sessions are removed. */
bool init(const std::string& modelPath, const std::string& workDir, std::string& error);

int createJob(const JobOptions& options);
void addSegment(int job, const Segment& segment);
void runJob(int job);
/** {id, status, progress, message, warnings, elapsed, dir, results}. */
std::string poll(int job);
void cancel(int job);

/** Card and face thumbnails cut from a frame Premiere exported, so they show
 *  the grade, effects and reframing: writes <name>.jpg and <name>_face.jpg into
 *  `outDir` and returns {"thumb", "face"} (face null when none is found), or
 *  "" with `error` set. */
std::string thumbsFromFrame(const std::string& framePath, const std::string& outDir, const std::string& name,
                            std::string& error);

} // namespace saycheese
