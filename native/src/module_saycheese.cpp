/*
 * SayCheese UXP addon -- the analysis engine, inside the panel.
 *   info() -> { dir }                                   where the addon lives
 *   init(modelPath, workDir) -> true, or throws         loads libmediapipe next to the addon
 *   createJob(sampleFps, maxResults, minGap, priority) -> job id
 *   addSegment(job, id, mediaPath, srcIn, srcOut, seqStart, speed, clipName, track)
 *   runJob(job)                                         starts it on a worker thread
 *   poll(job) -> JSON string                            {status, progress, message, warnings, elapsed, dir, results}
 *   cancel(job)
 *   thumbsFromFrame(framePath, outDir, name) -> JSON string  {thumb, face} cut from a
 *                                                     frame Premiere exported (graded)
 *
 * Every call returns at once; the panel polls, as it used to poll the Python
 * engine. Glue follows Adobe's UXP addon C API (UxpAddonShared.h), as in Syncitol.
 */
#include <string>

#include "job.h"
#include "utilities/UxpAddon.h"

namespace {

struct Args {
    addon_env env;
    size_t argc = 9;
    addon_value argv[9] = {};

    Args(addon_env e, addon_callback_info info) : env(e) {
        Check(UxpAddonApis.uxp_addon_get_cb_info(env, info, &argc, argv, nullptr, nullptr));
    }
    std::string str(size_t i) const {
        std::string out;
        size_t len = 0;
        if (i >= argc || UxpAddonApis.uxp_addon_get_value_string_utf8(env, argv[i], nullptr, 0, &len) != addon_ok) return out;
        out.resize(len + 1);
        size_t copied = 0;
        UxpAddonApis.uxp_addon_get_value_string_utf8(env, argv[i], &out[0], len + 1, &copied);
        out.resize(copied);
        return out;
    }
    double num(size_t i) const {
        double d = 0;
        if (i < argc) UxpAddonApis.uxp_addon_get_value_double(env, argv[i], &d);
        return d;
    }
    int integer(size_t i) const { return static_cast<int>(num(i)); }
};

addon_value makeString(addon_env env, const std::string& s) {
    addon_value v = nullptr;
    UxpAddonApis.uxp_addon_create_string_utf8(env, s.c_str(), s.size(), &v);
    return v;
}

addon_value makeInt(addon_env env, int32_t i) {
    addon_value v = nullptr;
    UxpAddonApis.uxp_addon_create_int32(env, i, &v);
    return v;
}

addon_value Info(addon_env env, addon_callback_info /*info*/) {
    addon_value o = nullptr;
    UxpAddonApis.uxp_addon_create_object(env, &o);
    UxpAddonApis.uxp_addon_set_named_property(env, o, "dir", makeString(env, saycheese::moduleDir()));
    return o;
}

addon_value Init(addon_env env, addon_callback_info info) {
    try {
        Args a(env, info);
        std::string error;
        if (a.argc < 2 || !saycheese::init(a.str(0), a.str(1), error)) {
            UxpAddonApis.uxp_addon_throw_error(env, nullptr, error.empty() ? "init(modelPath, workDir)" : error.c_str());
            return nullptr;
        }
        addon_value t = nullptr;
        UxpAddonApis.uxp_addon_get_boolean(env, true, &t);
        return t;
    } catch (...) {
        return CreateErrorFromException(env);
    }
}

addon_value CreateJob(addon_env env, addon_callback_info info) {
    try {
        Args a(env, info);
        saycheese::JobOptions o;
        if (a.argc > 0) o.sampleFps = a.num(0);
        if (a.argc > 1) o.maxResults = a.integer(1);
        if (a.argc > 2) o.minGap = a.num(2);
        if (a.argc > 3) o.priority = a.str(3);
        return makeInt(env, saycheese::createJob(o));
    } catch (...) {
        return CreateErrorFromException(env);
    }
}

addon_value AddSegment(addon_env env, addon_callback_info info) {
    try {
        Args a(env, info);
        if (a.argc < 9) {
            UxpAddonApis.uxp_addon_throw_error(env, nullptr,
                                               "addSegment(job, id, mediaPath, srcIn, srcOut, seqStart, speed, clipName, track)");
            return nullptr;
        }
        saycheese::Segment s;
        s.id = a.str(1);
        s.mediaPath = a.str(2);
        s.srcIn = a.num(3);
        s.srcOut = a.num(4);
        s.seqStart = a.num(5);
        s.speed = a.num(6);
        s.clipName = a.str(7);
        s.track = a.integer(8);
        saycheese::addSegment(a.integer(0), s);
        return nullptr;
    } catch (...) {
        return CreateErrorFromException(env);
    }
}

addon_value RunJob(addon_env env, addon_callback_info info) {
    try {
        Args a(env, info);
        saycheese::runJob(a.integer(0));
        return nullptr;
    } catch (...) {
        return CreateErrorFromException(env);
    }
}

addon_value Poll(addon_env env, addon_callback_info info) {
    try {
        Args a(env, info);
        return makeString(env, saycheese::poll(a.integer(0)));
    } catch (...) {
        return CreateErrorFromException(env);
    }
}

addon_value Cancel(addon_env env, addon_callback_info info) {
    try {
        Args a(env, info);
        saycheese::cancel(a.integer(0));
        return nullptr;
    } catch (...) {
        return CreateErrorFromException(env);
    }
}

addon_value ThumbsFromFrame(addon_env env, addon_callback_info info) {
    try {
        Args a(env, info);
        std::string error;
        std::string json = a.argc < 3 ? "" : saycheese::thumbsFromFrame(a.str(0), a.str(1), a.str(2), error);
        if (json.empty()) {
            UxpAddonApis.uxp_addon_throw_error(env, nullptr, error.empty() ? "thumbsFromFrame(framePath, outDir, name)" : error.c_str());
            return nullptr;
        }
        return makeString(env, json);
    } catch (...) {
        return CreateErrorFromException(env);
    }
}

addon_value InitModule(addon_env env, addon_value exports, const addon_apis& api) {
    const struct {
        const char* name;
        addon_callback fn;
    } fns[] = {
        {"info", Info}, {"init", Init}, {"createJob", CreateJob}, {"addSegment", AddSegment},
        {"runJob", RunJob}, {"poll", Poll}, {"cancel", Cancel}, {"thumbsFromFrame", ThumbsFromFrame},
    };
    for (const auto& f : fns) {
        addon_value v = nullptr;
        api.uxp_addon_create_function(env, nullptr, 0, f.fn, nullptr, &v);
        api.uxp_addon_set_named_property(env, exports, f.name, v);
    }
    return exports;
}

void Terminate(addon_env /*env*/) {}

} // namespace

UXP_ADDON_INIT(InitModule)
UXP_ADDON_TERMINATE(Terminate)
