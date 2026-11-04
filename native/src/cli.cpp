// cli.cpp -- runs the addon's engine outside Premiere, like `analyzer.py`'s CLI.
//   saycheese-cli <video> <start> <end> [results] [fps] [priority] [outDir]
// Expects libmediapipe.dll and face_landmarker.task next to the exe. Prints
// the job snapshot JSON; thumbnails land in <outDir>/job-1.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "eye_model.h"
#include "job.h"

int main(int argc, char** argv) {
    // --eye <file>: eye-open probability for each 32x32 grey crop in a raw file,
    // to check the C++ eye network against OpenCV's run of the ONNX model.
    if (argc == 3 && std::string(argv[1]) == "--eye") {
        FILE* f = std::fopen(argv[2], "rb");
        if (!f) return 1;
        unsigned char crop[32 * 32];
        while (std::fread(crop, 1, sizeof crop, f) == sizeof crop) std::printf("%.6f\n", saycheese::eyeOpenProbability(crop));
        std::fclose(f);
        return 0;
    }
    // --thumbs <frame.jpg> <outDir> <name>: the panel's graded-thumbnail step.
    if (argc == 5 && std::string(argv[1]) == "--thumbs") {
        std::string error, dir = saycheese::moduleDir();
        if (!saycheese::init(dir + "/face_landmarker.task", dir + "/cli-out", error)) {
            std::fprintf(stderr, "init failed: %s\n", error.c_str());
            return 1;
        }
        std::string json = saycheese::thumbsFromFrame(argv[2], argv[3], argv[4], error);
        std::printf("%s\n", json.empty() ? error.c_str() : json.c_str());
        return json.empty() ? 1 : 0;
    }
    if (argc < 4) {
        std::fprintf(stderr, "usage: saycheese-cli <video> <start> <end> [results] [fps] [priority] [outDir]\n");
        return 2;
    }
    std::string dir = saycheese::moduleDir();
    std::string out = argc > 7 ? argv[7] : dir + "/cli-out";
    std::string error;
    if (!saycheese::init(dir + "/face_landmarker.task", out, error)) {
        std::fprintf(stderr, "init failed: %s\n", error.c_str());
        return 1;
    }
    saycheese::JobOptions o;
    if (argc > 4) o.maxResults = std::atoi(argv[4]);
    if (argc > 5) o.sampleFps = std::atof(argv[5]);
    if (argc > 6) o.priority = argv[6];
    int job = saycheese::createJob(o);
    saycheese::Segment s;
    s.id = "s0";
    s.mediaPath = argv[1];
    s.srcIn = std::atof(argv[2]);
    s.srcOut = std::atof(argv[3]);
    s.seqStart = s.srcIn;
    s.clipName = s.mediaPath.substr(s.mediaPath.find_last_of("\\/") + 1);
    saycheese::addSegment(job, s);
    saycheese::runJob(job);
    std::string last;
    for (;;) {
        std::string snap = saycheese::poll(job);
        bool running = snap.find("\"status\":\"running\"") != std::string::npos ||
                       snap.find("\"status\":\"queued\"") != std::string::npos;
        if (!running) {
            std::printf("%s\n", snap.c_str());
            return snap.find("\"status\":\"done\"") != std::string::npos ? 0 : 1;
        }
        size_t m = snap.find("\"message\":");
        std::string msg = m == std::string::npos ? "" : snap.substr(m, snap.find(',', m) - m);
        if (msg != last) std::fprintf(stderr, "%s\n", msg.c_str());
        last = msg;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}
