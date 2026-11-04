// job.cpp -- see job.h. Follows engine/analyzer.py's Job step for step:
//   1. coarse pass: sample each segment at a low rate, downscaled, for smile strength
//   2. moments: smile peaks at least min_gap apart
//   3. refine: every frame within +-window of each peak, cropped to the face at
//      native resolution, for sharpness, motion and open eyes
//   4. rank: normalise sharpness across the job and combine into one score
//   5. thumbnails: the chosen frames, written as JPEGs into the job folder
#include "job.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

#include "face.h"
#include "frame_reader.h"
#include "jpeg_writer.h"

extern "C" {
#include <libavutil/log.h>
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

extern "C" {
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

namespace fs = std::filesystem;

namespace saycheese {
namespace {

constexpr int kCoarseMaxWidth = 960;
constexpr int kRefineMaxSide = 1280;
constexpr int kThumbWidth = 384;  // panel cards are 188 px wide; 2x covers HiDPI
constexpr int kFaceThumb = 320;
constexpr int kJpegQuality = 3;   // MJPEG qscale, roughly JPEG quality 88
constexpr size_t kKeepJobs = 5;

using Clock = std::chrono::steady_clock;

struct Weights {
    double expression, sharpness, stillness, frontal;
};

// Presets for the "prioritise" option. Eyes-open and mouth shape are applied
// as multipliers on top, since a blink ruins a thumbnail regardless.
Weights weightsFor(const std::string& priority) {
    if (priority == "sharpness") return {0.30, 0.42, 0.20, 0.08};
    if (priority == "smile") return {0.60, 0.20, 0.10, 0.10};
    return {0.45, 0.30, 0.15, 0.10};
}

double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

struct Cancelled {};

struct Sample {
    double t, smile;
    BBox boxN;  // face box, normalised to the frame
};

struct FrameScore {
    double t = 0, lap = 0, eyeOpen = 1, motion = 0, facePx = 0;
    bool hasMotion = false;
    BBox boxN;
    Expression ex;
    double smile = 0, sharp = 0, still = 0, eyes = 0, mouth = 0, frontal = 0, total = 0;
};

struct Moment {
    const Segment* seg;
    double t, coarseSmile;
    BBox boxN;
    std::vector<FrameScore> frames;
    int best = -1;
};

struct Job {
    int id = 0;
    JobOptions opt;
    std::vector<Segment> segments;
    std::string dir;
    std::atomic<bool> cancelRequested{false};

    std::mutex lock;  // guards everything below
    std::string status = "queued";
    std::string message = "Queued";
    double progress = 0;
    std::vector<std::string> warnings;
    std::string results = "[]";
    size_t resultCount = 0;
    double openMs = 0, coarseMs = 0, refineMs = 0, thumbMs = 0;  // reported for tuning
    bool started = false, finished = false;
    Clock::time_point startTime, endTime;

    void set(double p, const std::string& msg) {
        std::lock_guard<std::mutex> g(lock);
        progress = clamp01(p);
        message = msg;
    }
    void warn(const std::string& w) {
        std::lock_guard<std::mutex> g(lock);
        warnings.push_back(w);
    }
    void check() const {
        if (cancelRequested) throw Cancelled();
    }
};

// One face analyzer per worker: MediaPipe runs each landmarker on one core,
// so the coarse scan and the refine pass split their work across these.
std::vector<std::unique_ptr<FaceAnalyzer>> pool;
std::string workDir;
std::mutex jobsLock;
std::map<int, std::shared_ptr<Job>> jobs;
int nextId = 1;
std::mutex runLock;  // one job at a time

std::string fmtNum(double v, int decimals) {
    if (!std::isfinite(v)) v = 0;
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

std::string jsonStr(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            o += '\\';
            o += static_cast<char>(c);
        } else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            o += buf;
        } else {
            o += static_cast<char>(c);
        }
    }
    return o + "\"";
}

std::string baseName(const std::string& path) {
    size_t i = path.find_last_of("\\/");
    return i == std::string::npos ? path : path.substr(i + 1);
}

/** numpy.percentile's default linear interpolation. */
double percentile(std::vector<double> v, double p) {
    std::sort(v.begin(), v.end());
    double idx = p / 100.0 * (v.size() - 1);
    size_t lo = static_cast<size_t>(std::floor(idx)), hi = std::min(v.size() - 1, lo + 1);
    return v[lo] + (v[hi] - v[lo]) * (idx - lo);
}

/** Decoder threads per reader when `readers` decode at once. */
int decoderThreads(size_t readers) {
    unsigned cores = std::max(2u, std::thread::hardware_concurrency());
    return std::max(2, static_cast<int>(cores / std::max<size_t>(1, readers)));
}

int evenHeight(int srcW, int srcH, int outW) {
    return std::max(2, static_cast<int>(std::lround(double(srcH) * outW / srcW)) & ~1);
}

// ---------------------------------------------------------------------------
// The passes
// ---------------------------------------------------------------------------

/** Decodes and scales on a second thread while this one runs face detection. */
std::vector<Sample> coarse(Job& job, const Segment& seg, double srcIn, double srcOut, FaceAnalyzer& analyzer,
                           int decoderThreads, const std::function<void(double)>& report) {
    struct Item {
        double t;
        Image img;
    };
    std::mutex qLock;
    std::condition_variable qCv;
    std::deque<Item> queue;
    bool producerDone = false, stop = false;
    std::string producerError;
    const double rate = job.opt.sampleFps;

    std::thread producer([&] {
        FrameReader reader;
        std::string err;
        auto push = [&](Item&& item) {
            std::unique_lock<std::mutex> g(qLock);
            qCv.wait(g, [&] { return queue.size() < 4 || stop; });
            if (stop) return false;
            queue.push_back(std::move(item));
            qCv.notify_all();
            return true;
        };
        if (reader.open(seg.mediaPath, err, decoderThreads)) {
            reader.setFastDecode(true);
            const double half = 0.5 / reader.fps();
            auto emit = [&](const AVFrame* f, double t) {
                int ow = std::min(kCoarseMaxWidth, f->width);
                Item item{t, {}};
                reader.toRGB(f, 0, 0, f->width, f->height, ow, evenHeight(f->width, f->height, ow), SWS_BICUBIC, item.img);
                return push(std::move(item));
            };
            double t = 0;
            reader.seek(srcIn);
            double next = srcIn;
            while (const AVFrame* f = reader.next(t)) {
                if (t >= srcOut) break;
                if (t + half < next) continue;  // between samples
                while (next <= t + half) next += 1 / rate;
                if (!emit(f, t)) break;
            }
        } else {
            producerError = err;
        }
        std::lock_guard<std::mutex> g(qLock);
        producerDone = true;
        qCv.notify_all();
    });
    // Stop and join the producer however this function exits.
    struct Joiner {
        std::thread& th;
        std::mutex& m;
        std::condition_variable& cv;
        bool& stop;
        ~Joiner() {
            {
                std::lock_guard<std::mutex> g(m);
                stop = true;
            }
            cv.notify_all();
            th.join();
        }
    } joiner{producer, qLock, qCv, stop};

    std::vector<Sample> samples;
    BBox hint;
    bool haveHint = false;
    int count = 0;
    const double dur = srcOut - srcIn;
    for (;;) {
        Item item;
        {
            std::unique_lock<std::mutex> g(qLock);
            qCv.wait(g, [&] { return !queue.empty() || producerDone; });
            if (queue.empty()) break;
            item = std::move(queue.front());
            queue.pop_front();
            qCv.notify_all();
        }
        job.check();
        Face face;
        haveHint = analyzer.find(item.img, haveHint ? &hint : nullptr, face);
        if (haveHint) {
            hint = face.box;
            const double w = item.img.w, h = item.img.h;
            samples.push_back({item.t, face.expression().smile,
                               {face.box.x0 / w, face.box.y0 / h, face.box.x1 / w, face.box.y1 / h}});
        }
        if (++count % 8 == 0) report(std::min(dur, item.t - srcIn));
    }
    if (!producerError.empty()) throw std::runtime_error(producerError);
    if (count == 0) throw std::runtime_error("no frames could be decoded");
    return samples;
}

std::vector<Moment> pickMoments(const Job& job, const Segment& seg, std::vector<Sample> samples) {
    std::stable_sort(samples.begin(), samples.end(), [](const Sample& a, const Sample& b) { return a.smile > b.smile; });
    std::vector<Moment> picked;
    for (const Sample& s : samples) {
        if (s.smile < job.opt.minSmile) break;
        bool farEnough = std::all_of(picked.begin(), picked.end(),
                                     [&](const Moment& m) { return std::abs(s.t - m.t) >= job.opt.minGap; });
        if (farEnough) picked.push_back({&seg, s.t, s.smile, s.boxN, {}, -1});
    }
    return picked;
}

void refine(Job& job, FrameReader& reader, FaceAnalyzer& analyzer, Moment& m) {
    const Segment& seg = *m.seg;
    const double fps = reader.fps();
    double start = std::max(seg.srcIn, m.t - job.opt.window);
    double end = std::min(seg.srcOut, m.t + job.opt.window);
    start = std::ceil(start * fps - 1e-6) / fps;  // land on a frame boundary
    if (end - start < 1 / fps) end = start + 1 / fps;

    // Crop around the face (generously, it may move) before converting:
    // refining at native resolution is only cheap this way.
    const BBox& b = m.boxN;
    double cx = (b.x0 + b.x1) / 2, cy = (b.y0 + b.y1) / 2;
    double hw = (b.x1 - b.x0) * 1.25, hh = (b.y1 - b.y0) * 1.25;
    double cx0 = clamp01(cx - hw), cy0 = clamp01(cy - hh), cx1 = clamp01(cx + hw), cy1 = clamp01(cy + hh);

    reader.seek(start);
    double t = 0;
    BBox hint;
    bool haveHint = false;
    const Face* prev = nullptr;
    Face prevFace, face;
    Image img;
    while (const AVFrame* f = reader.next(t)) {
        job.check();
        if (t >= end - 0.25 / fps) break;
        const int W = f->width, H = f->height;
        int px = static_cast<int>(W * cx0) & ~1, py = static_cast<int>(H * cy0) & ~1;
        int cw = std::min(W - px, (static_cast<int>(W * (cx1 - cx0)) / 2) * 2);
        int ch = std::min(H - py, (static_cast<int>(H * (cy1 - cy0)) / 2) * 2);
        if (cw < 16 || ch < 16) break;
        int ow = std::min(kRefineMaxSide, cw);
        int oh = ow == cw ? ch : evenHeight(cw, ch, ow);
        reader.toRGB(f, px, py, px + cw, py + ch, ow, oh, SWS_BICUBIC, img);

        if (!analyzer.find(img, haveHint ? &hint : nullptr, face)) {
            haveHint = false;
            prev = nullptr;
            continue;
        }
        haveHint = true;
        hint = face.box;
        FrameScore fs;
        fs.t = t;
        fs.ex = face.expression();
        if (prev) {
            double sum = 0;
            size_t n = std::min(face.x.size(), prev->x.size());
            for (size_t i = 0; i < n; i++) sum += std::hypot(face.x[i] - prev->x[i], face.y[i] - prev->y[i]);
            fs.motion = n ? sum / n / face.height() : 0;
            fs.hasMotion = true;
        }
        double eyeR, eyeL;
        analyzer.eyesOpen(img, face, eyeR, eyeL);
        fs.eyeOpen = (eyeR + eyeL) / 2;
        fs.lap = sharpness(img, face);
        fs.boxN = {(px + face.box.x0 / ow * cw) / W, (py + face.box.y0 / oh * ch) / H,
                   (px + face.box.x1 / ow * cw) / W, (py + face.box.y1 / oh * ch) / H};
        fs.facePx = face.height();
        m.frames.push_back(fs);
        prevFace = std::move(face);
        prev = &prevFace;
    }
    // Motion is measured backwards; the first frame borrows its neighbour's.
    for (size_t i = 0; i + 1 < m.frames.size(); i++) {
        if (!m.frames[i].hasMotion && m.frames[i + 1].hasMotion) {
            m.frames[i].motion = m.frames[i + 1].motion;
            m.frames[i].hasMotion = true;
        }
    }
}

std::vector<Moment*> rank(const Job& job, std::vector<Moment>& moments) {
    std::vector<double> laps;
    for (auto& m : moments)
        for (auto& f : m.frames) laps.push_back(std::log1p(f.lap));
    std::vector<Moment*> ranked;
    if (laps.empty()) return ranked;
    double lo = percentile(laps, 5), hi = percentile(laps, 95);
    double span = std::max(hi - lo, 1e-6);
    Weights w = weightsFor(job.opt.priority);

    std::vector<Moment*> scored;
    for (auto& m : moments) {
        if (m.frames.empty()) continue;
        for (auto& f : m.frames) {
            f.sharp = clamp01((std::log1p(f.lap) - lo) / span);
            double motion = f.hasMotion ? f.motion : 0.01;
            f.still = 1.0 / (1.0 + std::pow(motion / 0.012, 2));
            // No bonus for cheek/eye squint: it favours the deepest point of a
            // laugh, where eyes are narrowest, over the open-eyed smile just
            // before it that editors tend to pick.
            f.smile = clamp01(f.ex.smile);
            // The eye classifier catches eyes shut by laughter; the blink
            // blendshape backs it up on winks in wide shots, where eyes are too
            // small for the classifier (winks read ~0.6, open smiles <= ~0.4).
            f.eyes = std::min(clamp01((f.eyeOpen - 0.2) / 0.7), clamp01(1 - (f.ex.blink - 0.45) / 0.3));
            f.mouth = 1 - 0.6 * clamp01((f.ex.pucker - 0.25) / 0.4);
            if (f.ex.jaw > 0.4 && f.ex.smile < 0.45) f.mouth *= 0.7;  // mid-word rather than a smile
            f.frontal = clamp01(1 - (std::abs(f.ex.yaw - 0.5) - 0.15) / 0.3);
            double quality = w.expression * f.smile + w.sharpness * f.sharp + w.stillness * f.still + w.frontal * f.frontal;
            f.total = 100 * quality * (0.4 + 0.6 * f.eyes) * (0.75 + 0.25 * f.mouth);
        }
        m.best = static_cast<int>(std::max_element(m.frames.begin(), m.frames.end(),
                                                   [](const FrameScore& a, const FrameScore& b) { return a.total < b.total; }) -
                                  m.frames.begin());
        scored.push_back(&m);
    }
    std::stable_sort(scored.begin(), scored.end(),
                     [](const Moment* a, const Moment* b) { return a->frames[a->best].total > b->frames[b->best].total; });
    for (Moment* m : scored) {
        double st = m->seg->seqTime(m->frames[m->best].t);
        bool farEnough = std::all_of(ranked.begin(), ranked.end(), [&](const Moment* r) {
            return std::abs(st - r->seg->seqTime(r->frames[r->best].t)) >= job.opt.minGap;
        });
        if (farEnough) ranked.push_back(m);
        if (static_cast<int>(ranked.size()) >= job.opt.maxResults) break;
    }
    return ranked;
}

/** Writes <id>.jpg (whole frame) and <id>_face.jpg; returns their names. */
void thumbnails(Job& job, FrameReader& reader, double t, const BBox& boxN, const std::string& cid,
                std::string& thumbName, std::string& faceName) {
    thumbName.clear();
    faceName.clear();
    reader.seek(t);
    double ft;
    const AVFrame* f = reader.next(ft);
    if (!f) return;
    const int W = f->width, H = f->height;
    std::string err;
    Image img;
    reader.toRGB(f, 0, 0, W, H, kThumbWidth, std::max(1, static_cast<int>(std::lround(double(H) * kThumbWidth / W))),
                 SWS_AREA, img);
    if (writeJpeg(img, job.dir + "/" + cid + ".jpg", kJpegQuality, err)) thumbName = cid + ".jpg";
    else job.warn("Thumbnail failed: " + err);
    BBox px = {boxN.x0 * W, boxN.y0 * H, boxN.x1 * W, boxN.y1 * H};
    int x0, y0, x1, y1;
    if (!squareRegion(W, H, px, 1.5, x0, y0, x1, y1)) return;
    reader.toRGB(f, x0, y0, x1, y1, kFaceThumb, kFaceThumb, SWS_AREA, img);
    if (writeJpeg(img, job.dir + "/" + cid + "_face.jpg", kJpegQuality, err)) faceName = cid + "_face.jpg";
}

std::string resultJson(int rankNo, const Moment& m, const std::string& thumb, const std::string& face) {
    const Segment& seg = *m.seg;
    const FrameScore& f = m.frames[m.best];
    char cid[16];
    std::snprintf(cid, sizeof cid, "c%03d", rankNo);
    std::ostringstream o;
    o << "{\"id\":" << jsonStr(cid) << ",\"rank\":" << rankNo << ",\"score\":" << fmtNum(f.total, 1)
      << ",\"scores\":{\"smile\":" << fmtNum(f.smile, 3) << ",\"sharpness\":" << fmtNum(f.sharp, 3)
      << ",\"stillness\":" << fmtNum(f.still, 3) << ",\"eyes\":" << fmtNum(f.eyes, 3) << ",\"mouth\":" << fmtNum(f.mouth, 3)
      << ",\"frontal\":" << fmtNum(f.frontal, 3) << "}"
      << ",\"raw\":{\"smile\":" << fmtNum(f.ex.smile, 3) << ",\"squint\":" << fmtNum(f.ex.squint, 3)
      << ",\"blink\":" << fmtNum(f.ex.blink, 3) << ",\"eye_open\":" << fmtNum(f.eyeOpen, 3)
      << ",\"jaw\":" << fmtNum(f.ex.jaw, 3) << ",\"pucker\":" << fmtNum(f.ex.pucker, 3) << ",\"yaw\":" << fmtNum(f.ex.yaw, 3)
      << ",\"lap\":" << fmtNum(f.lap, 3) << "}"
      << ",\"srcTime\":" << fmtNum(f.t, 4) << ",\"seqTime\":" << fmtNum(seg.seqTime(f.t), 4)
      << ",\"segmentId\":" << jsonStr(seg.id) << ",\"clipName\":" << jsonStr(seg.clipName)
      << ",\"mediaPath\":" << jsonStr(seg.mediaPath) << ",\"track\":" << seg.track
      << ",\"facePx\":" << std::lround(f.facePx)
      << ",\"thumb\":" << (thumb.empty() ? "null" : jsonStr(thumb))
      << ",\"face\":" << (face.empty() ? "null" : jsonStr(face)) << "}";
    return o.str();
}

void run(Job& job) {
    fs::create_directories(fs::u8path(job.dir));
    // One reader per media file for the refine and thumbnail passes.
    std::map<std::string, std::unique_ptr<FrameReader>> readers;
    std::vector<const Segment*> segments;
    for (const Segment& seg : job.segments) {
        auto it = readers.find(seg.mediaPath);
        if (it == readers.end()) {
            auto r = std::make_unique<FrameReader>();
            std::string err;
            if (!r->open(seg.mediaPath, err)) {
                job.warn((seg.clipName.empty() ? seg.mediaPath : seg.clipName) + ": " + err);
                readers[seg.mediaPath] = nullptr;
                continue;
            }
            it = readers.emplace(seg.mediaPath, std::move(r)).first;
        }
        if (!it->second) continue;
        // Keep segments inside the file, so chunks split real footage.
        Segment& s = const_cast<Segment&>(seg);
        if (it->second->duration() > 0) s.srcOut = std::min(s.srcOut, it->second->duration());
        if (seg.srcOut - seg.srcIn > 0.05) segments.push_back(&seg);
    }

    auto since = [](Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); };
    auto phase = Clock::now();
    job.openMs = since(job.startTime);
    double total = 0, done = 0;
    for (const Segment* s : segments) total += s->srcOut - s->srcIn;
    if (total <= 0) total = 1;
    std::vector<Moment> moments;
    for (size_t i = 0; i < segments.size(); i++) {
        const Segment& seg = *segments[i];
        std::string label = "Scanning " + (seg.clipName.empty() ? baseName(seg.mediaPath) : seg.clipName) + " (" +
                            std::to_string(i + 1) + "/" + std::to_string(segments.size()) + ")";
        job.set(0.75 * done / total, label);
        // Split the segment into one chunk per worker (none shorter than 20 s).
        const double len = seg.srcOut - seg.srcIn;
        const size_t chunks = std::max<size_t>(1, std::min(pool.size(), static_cast<size_t>(len / 20)));
        std::vector<double> chunkDone(chunks, 0.0);
        std::vector<std::vector<Sample>> chunkSamples(chunks);
        std::vector<std::string> errors(chunks);
        std::mutex progressLock;
        std::vector<std::thread> threads;
        for (size_t c = 0; c < chunks; c++) {
            threads.emplace_back([&, c] {
                double a = seg.srcIn + len * c / chunks, b = seg.srcIn + len * (c + 1) / chunks;
                try {
                    chunkSamples[c] = coarse(job, seg, a, b, *pool[c], decoderThreads(chunks), [&](double p) {
                        std::lock_guard<std::mutex> g(progressLock);
                        chunkDone[c] = p;
                        double sum = 0;
                        for (double d : chunkDone) sum += d;
                        job.set(0.75 * (done + sum) / total, label);
                    });
                } catch (const Cancelled&) {
                    // seen by job.check() below
                } catch (const std::exception& e) {
                    errors[c] = e.what();
                }
            });
        }
        for (auto& th : threads) th.join();
        job.check();
        std::vector<Sample> samples;
        for (auto& cs : chunkSamples) samples.insert(samples.end(), cs.begin(), cs.end());
        for (const auto& e : errors) {
            if (!e.empty()) {
                job.warn((seg.clipName.empty() ? baseName(seg.mediaPath) : seg.clipName) + ": " + e);
                break;
            }
        }
        auto picked = pickMoments(job, seg, std::move(samples));
        moments.insert(moments.end(), picked.begin(), picked.end());
        done += len;
    }

    job.coarseMs = since(phase);
    phase = Clock::now();
    // Keep the strongest peaks overall; refining is the expensive part.
    std::stable_sort(moments.begin(), moments.end(),
                     [](const Moment& a, const Moment& b) { return a.coarseSmile > b.coarseSmile; });
    size_t keep = std::max<size_t>(job.opt.maxResults * 3, 30);
    if (moments.size() > keep) moments.resize(keep);
    if (moments.empty()) {
        job.warn("No smiling faces were found.");
        return;
    }

    // Refine the moments in parallel, one worker per analyzer, each with its own readers.
    std::atomic<size_t> nextMoment{0}, refined{0};
    const size_t workers = std::min(pool.size(), moments.size());
    std::vector<std::thread> threads;
    for (size_t w = 0; w < workers; w++) {
        threads.emplace_back([&, w] {
            std::map<std::string, std::unique_ptr<FrameReader>> own;
            for (size_t i; (i = nextMoment++) < moments.size();) {
                if (job.cancelRequested) return;
                Moment& m = moments[i];
                try {
                    auto& r = own[m.seg->mediaPath];
                    if (!r) {
                        r = std::make_unique<FrameReader>();
                        std::string err;
                        if (!r->open(m.seg->mediaPath, err, decoderThreads(workers))) throw std::runtime_error(err);
                    }
                    refine(job, *r, *pool[w], m);
                } catch (const Cancelled&) {
                    return;
                } catch (const std::exception& e) {
                    job.warn("Refine failed at " + fmtNum(m.t, 2) + "s in " + m.seg->clipName + ": " + e.what());
                }
                size_t n = ++refined;
                job.set(0.75 + 0.20 * n / moments.size(),
                        "Checking sharpness " + std::to_string(n) + "/" + std::to_string(moments.size()));
            }
        });
    }
    for (auto& th : threads) th.join();
    job.check();

    job.refineMs = since(phase);
    phase = Clock::now();
    std::vector<Moment*> ranked = rank(job, moments);
    job.set(0.95, "Grabbing thumbnails");
    std::string results = "[";
    for (size_t i = 0; i < ranked.size(); i++) {
        job.check();
        job.set(0.95 + 0.05 * i / std::max<size_t>(1, ranked.size()), "Grabbing thumbnails");
        Moment& m = *ranked[i];
        char cid[16];
        std::snprintf(cid, sizeof cid, "c%03d", static_cast<int>(i + 1));
        std::string thumb, face;
        thumbnails(job, *readers[m.seg->mediaPath], m.frames[m.best].t, m.frames[m.best].boxN, cid, thumb, face);
        if (i) results += ",";
        results += resultJson(static_cast<int>(i + 1), m, thumb, face);
    }
    results += "]";
    job.thumbMs = since(phase);
    std::lock_guard<std::mutex> g(job.lock);
    job.results = results;
    job.resultCount = ranked.size();
}

void runThread(std::shared_ptr<Job> job) {
    std::lock_guard<std::mutex> one(runLock);
    {
        std::lock_guard<std::mutex> g(job->lock);
        job->status = "running";
        job->started = true;
        job->startTime = Clock::now();
    }
    std::string status = "done", message;
    try {
        if (job->cancelRequested) throw Cancelled();
        run(*job);
    } catch (const Cancelled&) {
        status = "cancelled";
        message = "Cancelled";
    } catch (const std::exception& e) {
        status = "error";
        message = e.what();
    } catch (...) {
        status = "error";
        message = "unexpected error";
    }
    std::lock_guard<std::mutex> g(job->lock);
    job->status = status;
    if (status == "done") {
        job->progress = 1;
        job->message = "Found " + std::to_string(job->resultCount) + " smiles";
    } else {
        job->message = message;
    }
    job->finished = true;
    job->endTime = Clock::now();
}

std::shared_ptr<Job> findJob(int id) {
    std::lock_guard<std::mutex> g(jobsLock);
    auto it = jobs.find(id);
    return it == jobs.end() ? nullptr : it->second;
}

} // namespace

std::string moduleDir() {
#ifdef _WIN32
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&moduleDir), &self);
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(self, buf, MAX_PATH * 2);
    std::wstring w(buf, n);
    w = w.substr(0, w.find_last_of(L"\\/"));
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], len, nullptr, nullptr);
    return s;
#else
    Dl_info info;
    dladdr(reinterpret_cast<void*>(&moduleDir), &info);
    std::string s = info.dli_fname;
    return s.substr(0, s.find_last_of('/'));
#endif
}

bool init(const std::string& modelPath, const std::string& dir, std::string& error) {
    std::lock_guard<std::mutex> g(jobsLock);
    if (pool.empty()) {
        av_log_set_level(AV_LOG_ERROR);  // FFmpeg's warnings would only be noise here
        // Leave a few cores for decoding and for Premiere itself.
        unsigned cores = std::max(2u, std::thread::hardware_concurrency());
        size_t n = std::clamp<size_t>(cores / 3, 1, 6);
        for (size_t i = 0; i < n; i++) {
            auto a = std::make_unique<FaceAnalyzer>();
            if (!a->init(moduleDir(), modelPath, error)) {
                if (pool.empty()) return false;
                break;  // fewer workers, still fine
            }
            pool.push_back(std::move(a));
        }
    }
    if (workDir != dir) {
        workDir = dir;
        // Thumbnails from earlier sessions. Only our own job-N folders.
        std::error_code ec;
        fs::create_directories(fs::u8path(workDir), ec);
        for (const auto& e : fs::directory_iterator(fs::u8path(workDir), ec)) {
            std::string name = e.path().filename().u8string();
            if (e.is_directory() && name.rfind("job-", 0) == 0) fs::remove_all(e.path(), ec);
        }
    }
    return true;
}

int createJob(const JobOptions& options) {
    auto job = std::make_shared<Job>();
    job->opt = options;
    if (job->opt.sampleFps <= 0) job->opt.sampleFps = 4;
    job->opt.maxResults = std::max(1, job->opt.maxResults);
    std::lock_guard<std::mutex> g(jobsLock);
    job->id = nextId++;
    job->dir = workDir + "/job-" + std::to_string(job->id);
    jobs[job->id] = job;
    // Keep the last few finished jobs (and their thumbnails).
    while (jobs.size() > kKeepJobs) {
        auto oldest = jobs.begin();
        {
            std::lock_guard<std::mutex> jg(oldest->second->lock);
            if (!oldest->second->finished && oldest->second->started) break;
        }
        std::error_code ec;
        fs::remove_all(fs::u8path(oldest->second->dir), ec);
        jobs.erase(oldest);
    }
    return job->id;
}

void addSegment(int id, const Segment& segment) {
    if (auto job = findJob(id)) {
        Segment s = segment;
        if (!(s.speed > 0)) s.speed = 1;
        job->segments.push_back(s);
    }
}

void runJob(int id) {
    auto job = findJob(id);
    if (!job) return;
    if (pool.empty()) {
        std::lock_guard<std::mutex> g(job->lock);
        job->status = "error";
        job->message = "The analysis engine isn't initialised";
        job->finished = true;
        return;
    }
    std::thread(runThread, job).detach();
}

std::string poll(int id) {
    auto job = findJob(id);
    if (!job) return "{\"status\":\"unknown\",\"message\":\"Unknown job\",\"warnings\":[],\"results\":[]}";
    std::lock_guard<std::mutex> g(job->lock);
    double elapsed = 0;
    if (job->started)
        elapsed = std::chrono::duration<double>((job->finished ? job->endTime : Clock::now()) - job->startTime).count();
    std::ostringstream o;
    o << "{\"id\":" << job->id << ",\"status\":" << jsonStr(job->status) << ",\"progress\":" << fmtNum(job->progress, 4)
      << ",\"message\":" << jsonStr(job->message) << ",\"warnings\":[";
    for (size_t i = 0; i < job->warnings.size(); i++) o << (i ? "," : "") << jsonStr(job->warnings[i]);
    o << "],\"elapsed\":" << fmtNum(elapsed, 1) << ",\"timings\":{\"open\":" << fmtNum(job->openMs / 1000, 1)
      << ",\"coarse\":" << fmtNum(job->coarseMs / 1000, 1) << ",\"refine\":" << fmtNum(job->refineMs / 1000, 1)
      << ",\"thumbnails\":" << fmtNum(job->thumbMs / 1000, 1) << "}"
      << ",\"dir\":" << jsonStr(job->dir) << ",\"results\":" << job->results << "}";
    return o.str();
}

void cancel(int id) {
    if (auto job = findJob(id)) job->cancelRequested = true;
}

std::string thumbsFromFrame(const std::string& framePath, const std::string& outDir, const std::string& name,
                            std::string& error) {
    if (pool.empty()) {
        error = "The analysis engine isn't initialised";
        return "";
    }
    FrameReader reader;
    if (!reader.open(framePath, error)) return "";
    double t;
    const AVFrame* f = reader.next(t);
    if (!f) {
        error = "can't read " + framePath;
        return "";
    }
    const int W = f->width, H = f->height;
    Image img;
    reader.toRGB(f, 0, 0, W, H, kThumbWidth, std::max(1, static_cast<int>(std::lround(double(H) * kThumbWidth / W))),
                 SWS_AREA, img);
    std::string thumb = name + ".jpg", faceName;
    if (!writeJpeg(img, outDir + "/" + thumb, kJpegQuality, error)) return "";
    // The face is found again in this frame, so reframing in the sequence doesn't matter.
    Image full;
    reader.toRGB(f, 0, 0, W, H, W, H, SWS_BICUBIC, full);
    Face face;
    int x0, y0, x1, y1;
    if (pool[0]->find(full, nullptr, face) && squareRegion(W, H, face.box, 1.5, x0, y0, x1, y1)) {
        reader.toRGB(f, x0, y0, x1, y1, kFaceThumb, kFaceThumb, SWS_AREA, img);
        std::string err;
        if (writeJpeg(img, outDir + "/" + name + "_face.jpg", kJpegQuality, err)) faceName = name + "_face.jpg";
    }
    return "{\"thumb\":" + jsonStr(thumb) + ",\"face\":" + (faceName.empty() ? "null" : jsonStr(faceName)) + "}";
}

} // namespace saycheese
