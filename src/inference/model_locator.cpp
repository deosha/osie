// SPDX-License-Identifier: Apache-2.0
#include "osie/inference/model_locator.hpp"

#include <unistd.h>

#include <climits>
#include <cstdlib>
#include <filesystem>
#include <system_error>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace osie::inference {
namespace fs = std::filesystem;

std::string_view to_string(ModelSource s) noexcept {
    switch (s) {
        case ModelSource::explicit_path: return "explicit path (--model)";
        case ModelSource::environment: return "OSIE_MODEL";
        case ModelSource::user_data_dir: return "user data directory";
        case ModelSource::bundled: return "installed with the binary";
        case ModelSource::none: return "none";
    }
    return "?";
}

namespace {

bool readable_file(const std::string& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec) && ::access(p.c_str(), R_OK) == 0;
}

std::optional<std::string> non_empty_env(const char* name) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') return std::nullopt;
    return std::string(v);
}

}  // namespace

ModelLocation locate_model(const ModelSearch& s) {
    ModelLocation loc;
    // (1) and (2) are returned even when the file is missing, so the caller
    // can report the user's own path instead of silently ignoring it.
    if (s.explicit_path) {
        loc.searched.push_back(*s.explicit_path);
        loc.path = *s.explicit_path;
        loc.source = ModelSource::explicit_path;
        return loc;
    }
    if (s.env_path) {
        loc.searched.push_back(*s.env_path);
        loc.path = *s.env_path;
        loc.source = ModelSource::environment;
        return loc;
    }
    auto try_candidate = [&loc](const fs::path& p, ModelSource src) {
        loc.searched.push_back(p.string());
        if (!readable_file(p.string())) return false;
        loc.path = p.string();
        loc.source = src;
        return true;
    };
    if (s.data_dir && try_candidate(fs::path(*s.data_dir) / "models" / kModelFileName, ModelSource::user_data_dir))
        return loc;
    if (s.executable) {
        const fs::path dir = fs::path(*s.executable).parent_path();
        // Installed layout (<prefix>/bin/osie, <prefix>/share/osie/models) and
        // the build tree, where CMake copies the model to <build>/share/osie/models.
        for (const auto& cand : {dir / ".." / "share" / "osie" / "models" / kModelFileName,
                                 dir / "share" / "osie" / "models" / kModelFileName}) {
            if (try_candidate(cand.lexically_normal(), ModelSource::bundled)) return loc;
        }
    }
    loc.path.clear();
    loc.source = ModelSource::none;
    return loc;
}

std::optional<std::string> executable_path() {
#if defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return std::nullopt;
    buf.resize(std::char_traits<char>::length(buf.c_str()));
#else
    std::string buf(PATH_MAX, '\0');
    const ssize_t n = ::readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (n <= 0) return std::nullopt;
    buf.resize(static_cast<std::size_t>(n));
#endif
    std::error_code ec;
    auto canon = fs::weakly_canonical(buf, ec);  // resolves e.g. Homebrew's bin/ symlink
    return ec ? buf : canon.string();
}

ModelSearch default_model_search(std::optional<std::string> explicit_path) {
    ModelSearch s;
    s.explicit_path = std::move(explicit_path);
    s.env_path = non_empty_env("OSIE_MODEL");
#if defined(__APPLE__)
    if (auto home = non_empty_env("HOME")) s.data_dir = *home + "/Library/Application Support/osie";
#else
    // The XDG spec requires relative values to be ignored.
    if (auto xdg = non_empty_env("XDG_DATA_HOME"); xdg && xdg->front() == '/') s.data_dir = *xdg + "/osie";
    else if (auto home = non_empty_env("HOME")) s.data_dir = *home + "/.local/share/osie";
#endif
    s.executable = executable_path();
    return s;
}

}  // namespace osie::inference
