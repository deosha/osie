// SPDX-License-Identifier: Apache-2.0
// Deterministic lookup of the intent model, independent of the working
// directory and of the source checkout (docs/INSTALL.md).
//
// Order: (1) an explicit path (--model), (2) $OSIE_MODEL, (3) the user data
// directory, (4) the model installed next to the binary. If none exists the
// caller uses the deterministic rule parser. The current working directory is
// deliberately never searched: a model file in an arbitrary directory must not
// silently change how OSIE interprets questions.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace osie::inference {

enum class ModelSource { explicit_path, environment, user_data_dir, bundled, none };
std::string_view to_string(ModelSource s) noexcept;

struct ModelSearch {
    std::string file_name{"intent.osiem"};     // looked up as <dir>/models/<file_name>
    std::optional<std::string> explicit_path;  // --model
    std::optional<std::string> env_path;       // $OSIE_MODEL
    std::optional<std::string> data_dir;       // e.g. ~/.local/share/osie
    std::optional<std::string> executable;     // absolute path of the running binary
};

struct ModelLocation {
    std::string path;  // empty when source == none
    ModelSource source{ModelSource::none};
    /// True for (1) and (2): the user asked for this file, so a failure to
    /// load it is an error rather than a silent fallback to the rules.
    [[nodiscard]] bool user_requested() const noexcept {
        return source == ModelSource::explicit_path || source == ModelSource::environment;
    }
    std::vector<std::string> searched;  // candidates in the order tried
};

inline constexpr std::string_view kModelFileName = "intent.osiem";

/// Pure apart from checking which candidate files are readable.
ModelLocation locate_model(const ModelSearch& search);

/// The search inputs for this process: environment, platform data directory
/// ($XDG_DATA_HOME/osie or ~/.local/share/osie on Linux,
/// ~/Library/Application Support/osie on macOS) and the executable's path.
/// `env_var` names the override variable ($OSIE_MODEL for the intent model,
/// $OSIE_WORLD_MODEL for the CPU world model).
ModelSearch default_model_search(std::optional<std::string> explicit_path, std::string file_name = "intent.osiem",
                                 const char* env_var = "OSIE_MODEL");

/// Absolute path of the running executable (/proc/self/exe or
/// _NSGetExecutablePath), symlinks resolved.
std::optional<std::string> executable_path();

}  // namespace osie::inference
