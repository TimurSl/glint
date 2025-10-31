#include "config.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include <nlohmann/json.hpp>

#include "logger.h"

namespace {
using json = nlohmann::json;

std::string sanitize_device_name(std::string value) {
    auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](unsigned char ch) { return !is_space(ch); }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [&](unsigned char ch) { return !is_space(ch); }).base(), value.end());
    if (value.empty()) {
        return "default";
    }
    return value;
}


AppConfig make_default_config() {
    AppConfig cfg;
    ProfileConfig base{};
    base.video.width = 1920;
    base.video.height = 1080;
    base.video.fps = 60;
    base.video.bitrate_kbps = 18000;
    base.video.codec = "h264";
    base.video.encoder = "auto";

    base.audio.sample_rate = 48000;
    base.audio.channels = 2;
    base.audio.codec = "aac";
    base.audio.bitrate_kbps = 192;
    base.audio.enable_system = true;
    base.audio.enable_microphone = true;
    base.audio.enable_applications = false;
    base.audio.device = "default";

    base.buffer.enabled = true;
    base.buffer.rolling_mode = true;
    base.buffer.size_limit_bytes = 100ull * 1024ull * 1024ull;
    base.buffer.segment_directory = "buffer";
    base.buffer.output_directory = "recordings";
    base.buffer.segment_prefix = "seg_";
    base.buffer.segment_extension = ".mkv";
    base.buffer.container = "matroska";

    cfg.general.temp_path = "temp";
    cfg.general.db_path = "glintd.db";
    cfg.general.log_path = "glintd.log";
    cfg.general.file_logging = true;
    cfg.general.log_level = "info";

    cfg.profiles["default"] = base;

    ProfileConfig high = base;
    high.video.bitrate_kbps = 35000;
    high.video.codec = "hevc";
    high.video.encoder = "nvenc";
    cfg.profiles["high_quality"] = high;

    ProfileConfig low = base;
    low.video.fps = 120;
    low.video.bitrate_kbps = 12000;
    low.buffer.rolling_mode = false;
    cfg.profiles["low_latency"] = low;

    cfg.active_profile = "default";
    return cfg;
}

json profile_to_json(const ProfileConfig& profile) {
    json j;
    j["video"] = {
        {"width", profile.video.width},
        {"height", profile.video.height},
        {"fps", profile.video.fps},
        {"bitrate_kbps", profile.video.bitrate_kbps},
        {"codec", profile.video.codec},
        {"encoder", profile.video.encoder}
    };

    j["audio"] = {
        {"sample_rate", profile.audio.sample_rate},
        {"channels", profile.audio.channels},
        {"codec", profile.audio.codec},
        {"bitrate_kbps", profile.audio.bitrate_kbps},
        {"enable_system", profile.audio.enable_system},
        {"enable_microphone", profile.audio.enable_microphone},
        {"enable_applications", profile.audio.enable_applications},
        {"device", profile.audio.device}
    };

    j["buffer"] = {
        {"enabled", profile.buffer.enabled},
        {"rolling_mode", profile.buffer.rolling_mode},
        {"size_limit_bytes", profile.buffer.size_limit_bytes},
        {"segment_directory", profile.buffer.segment_directory.string()},
        {"output_directory", profile.buffer.output_directory.string()},
        {"segment_prefix", profile.buffer.segment_prefix},
        {"segment_extension", profile.buffer.segment_extension},
        {"container", profile.buffer.container}
    };
    return j;
}

ProfileConfig json_to_profile(const json& j, const ProfileConfig& fallback) {
    ProfileConfig profile = fallback;
    if (j.contains("video")) {
        const auto& v = j["video"];
        profile.video.width = v.value("width", profile.video.width);
        profile.video.height = v.value("height", profile.video.height);
        profile.video.fps = v.value("fps", profile.video.fps);
        profile.video.bitrate_kbps = v.value("bitrate_kbps", profile.video.bitrate_kbps);
        profile.video.codec = v.value("codec", profile.video.codec);
        profile.video.encoder = v.value("encoder", profile.video.encoder);
    }
    if (j.contains("audio")) {
        const auto& a = j["audio"];
        profile.audio.sample_rate = a.value("sample_rate", profile.audio.sample_rate);
        profile.audio.channels = a.value("channels", profile.audio.channels);
        profile.audio.codec = a.value("codec", profile.audio.codec);
        profile.audio.bitrate_kbps = a.value("bitrate_kbps", profile.audio.bitrate_kbps);
        profile.audio.enable_system = a.value("enable_system", profile.audio.enable_system);
        profile.audio.enable_microphone = a.value("enable_microphone", profile.audio.enable_microphone);
        profile.audio.enable_applications = a.value("enable_applications", profile.audio.enable_applications);
        if (a.contains("device") && a["device"].is_string()) {
            profile.audio.device = sanitize_device_name(a.at("device").get<std::string>());
        } else {
            profile.audio.device = sanitize_device_name(profile.audio.device);
        }
    }
    if (j.contains("buffer")) {
        const auto& b = j["buffer"];
        profile.buffer.enabled = b.value("enabled", profile.buffer.enabled);
        profile.buffer.rolling_mode = b.value("rolling_mode", profile.buffer.rolling_mode);
        profile.buffer.size_limit_bytes = b.value("size_limit_bytes", profile.buffer.size_limit_bytes);
        if (b.contains("segment_directory")) {
            profile.buffer.segment_directory = b.at("segment_directory").get<std::string>();
        }
        if (b.contains("output_directory")) {
            profile.buffer.output_directory = b.at("output_directory").get<std::string>();
        }
        profile.buffer.segment_prefix = b.value("segment_prefix", profile.buffer.segment_prefix);
        profile.buffer.segment_extension = b.value("segment_extension", profile.buffer.segment_extension);
        profile.buffer.container = b.value("container", profile.buffer.container);
    }
    return profile;
}

json config_to_json(const AppConfig& config) {
    json j;
    j["active_profile"] = config.active_profile;
    json profiles = json::object();
    for (const auto& [name, profile] : config.profiles) {
        profiles[name] = profile_to_json(profile);
    }
    j["profiles"] = profiles;
    j["general"] = {
        {"temp_path", config.general.temp_path.string()},
        {"db_path", config.general.db_path.string()},
        {"log_path", config.general.log_path.string()},
        {"file_logging", config.general.file_logging},
        {"log_level", config.general.log_level}
    };
    return j;
}

AppConfig json_to_config(const json& j) {
    AppConfig cfg = make_default_config();
    if (j.contains("general")) {
        const auto& g = j["general"];
        if (g.contains("temp_path")) cfg.general.temp_path = g.at("temp_path").get<std::string>();
        if (g.contains("db_path")) cfg.general.db_path = g.at("db_path").get<std::string>();
        if (g.contains("log_path")) cfg.general.log_path = g.at("log_path").get<std::string>();
        cfg.general.file_logging = g.value("file_logging", cfg.general.file_logging);
        cfg.general.log_level = g.value("log_level", cfg.general.log_level);
    }
    cfg.active_profile = j.value("active_profile", cfg.active_profile);
    if (j.contains("profiles") && j["profiles"].is_object()) {
        std::map<std::string, ProfileConfig> parsed;
        for (const auto& [name, value] : j["profiles"].items()) {
            parsed[name] = json_to_profile(value, cfg.profiles.contains(name) ? cfg.profiles.at(name) : cfg.profiles.at("default"));
        }
        cfg.profiles = parsed;
    }
    if (!cfg.profiles.contains(cfg.active_profile) && !cfg.profiles.empty()) {
        cfg.active_profile = cfg.profiles.begin()->first;
    }
    return cfg;
}

std::string trim(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::string strip_quotes(const std::string& s) {
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') || (s.front() == '\'' && s.back() == '\''))) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

json parse_scalar(const std::string& value) {
    const std::string lowered = [&]() {
        std::string tmp = value;
        for (auto& ch : tmp) ch = static_cast<char>(::tolower(ch));
        return tmp;
    }();
    if (lowered == "true") return true;
    if (lowered == "false") return false;
    try {
        size_t idx = 0;
        const long long i = std::stoll(value, &idx, 10);
        if (idx == value.size()) return json(i);
    } catch (...) {
    }
    try {
        size_t idx = 0;
        const double d = std::stod(value, &idx);
        if (idx == value.size()) return json(d);
    } catch (...) {
    }
    return json(strip_quotes(value));
}

void assign_dotted(json& root, const std::string& dotted, const std::string& value) {
    if (dotted.empty()) return;
    std::stringstream ss(dotted);
    std::string item;
    json* node = &root;
    std::vector<std::string> parts;
    while (std::getline(ss, item, '.')) {
        if (!item.empty()) parts.push_back(item);
    }
    if (parts.empty()) return;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        node = &((*node)[parts[i]]);
    }
    (*node)[parts.back()] = parse_scalar(value);
}

json parse_ini_or_toml(std::istream& in, bool /*toml*/) {
    json root;
    std::string line;
    std::string section;
    while (std::getline(in, line)) {
        auto trimmed = trim(line);
        if (trimmed.empty() || trimmed.front() == '#' || trimmed.front() == ';') continue;
        if (trimmed.front() == '[' && trimmed.back() == ']') {
            section = trimmed.substr(1, trimmed.size() - 2);
            section = trim(section);
            continue;
        }
        auto delim = trimmed.find('=');
        if (delim == std::string::npos) continue;
        std::string key = trim(trimmed.substr(0, delim));
        std::string value = trim(trimmed.substr(delim + 1));
        std::string dotted = section.empty() ? key : section + "." + key;
        assign_dotted(root, dotted, value);
    }
    return root;
}

json parse_yaml(std::istream& in) {
    json root;
    std::vector<std::pair<int, std::string>> stack;
    std::string line;
    while (std::getline(in, line)) {
        if (line.find_first_not_of(" \t\r\n") == std::string::npos) continue;
        const size_t indent = line.find_first_not_of(' ');
        std::string trimmed_line = trim(line);
        if (trimmed_line.empty() || trimmed_line.front() == '#') continue;
        while (!stack.empty() && indent <= static_cast<size_t>(stack.back().first)) {
            stack.pop_back();
        }
        if (trimmed_line.back() == ':') {
            std::string key = trim(trimmed_line.substr(0, trimmed_line.size() - 1));
            std::string prefix = stack.empty() ? std::string{} : stack.back().second;
            std::string dotted = prefix.empty() ? key : prefix + "." + key;
            stack.emplace_back(static_cast<int>(indent), dotted);
        } else {
            auto pos = trimmed_line.find(':');
            if (pos == std::string::npos) continue;
            std::string key = trim(trimmed_line.substr(0, pos));
            std::string value = trim(trimmed_line.substr(pos + 1));
            std::string prefix = stack.empty() ? std::string{} : stack.back().second;
            std::string dotted = prefix.empty() ? key : prefix + "." + key;
            assign_dotted(root, dotted, value);
        }
    }
    return root;
}

std::map<std::string, json> flatten(const json& j, const std::string& prefix = "") {
    std::map<std::string, json> result;
    if (j.is_object()) {
        for (const auto& [key, value] : j.items()) {
            std::string dotted = prefix.empty() ? key : prefix + "." + key;
            auto sub = flatten(value, dotted);
            result.insert(sub.begin(), sub.end());
        }
    } else {
        result[prefix] = j;
    }
    return result;
}

void write_ini_or_toml(std::ostream& os, const json& j, bool toml) {
    (void)toml;
    auto flat = flatten(j);
    std::map<std::string, std::vector<std::pair<std::string, json>>> sections;
    for (const auto& [path, value] : flat) {
        auto pos = path.rfind('.');
        std::string section = pos == std::string::npos ? "" : path.substr(0, pos);
        std::string key = pos == std::string::npos ? path : path.substr(pos + 1);
        sections[section].push_back({key, value});
    }
    if (sections.contains("")) {
        for (const auto& [key, value] : sections[""]) {
            std::string rendered;
            if (value.is_string()) {
                rendered = '"' + value.get<std::string>() + '"';
            } else {
                rendered = value.dump();
            }
            os << key << ' ' << '=' << ' ' << rendered << "\n";
        }
        os << "\n";
    }
    for (auto& [section, kvs] : sections) {
        if (section.empty()) continue;
        if (!section.empty()) {
            os << '[' << section << "]\n";
        }
        for (const auto& [key, value] : kvs) {
            std::string rendered;
            if (value.is_string()) {
                rendered = '"' + value.get<std::string>() + '"';
            } else {
                rendered = value.dump();
            }
            os << key << ' ' << '=' << ' ' << rendered << "\n";
        }
        os << "\n";
    }
}

void write_yaml(std::ostream& os, const json& j, int indent = 0) {
    if (!j.is_object()) {
        os << std::string(indent, ' ') << j.dump() << "\n";
        return;
    }
    for (auto it = j.begin(); it != j.end(); ++it) {
        os << std::string(indent, ' ') << it.key() << ":";
        if (it.value().is_object()) {
            os << "\n";
            write_yaml(os, it.value(), indent + 2);
        } else {
            if (it.value().is_string()) {
                os << ' ' << '"' << it.value().get<std::string>() << '"' << "\n";
            } else {
                os << ' ' << it.value().dump() << "\n";
            }
        }
    }
}

std::string extension_of(const std::filesystem::path& path) {
    auto ext = path.extension().string();
    for (auto& ch : ext) ch = static_cast<char>(::tolower(ch));
    return ext;
}

} // namespace

const ProfileConfig& AppConfig::activeProfile() const {
    auto it = profiles.find(active_profile);
    if (it != profiles.end()) {
        return it->second;
    }
    if (!profiles.empty()) {
        return profiles.begin()->second;
    }
    static ProfileConfig fallback{};
    return fallback;
}

AppConfig load_config(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path().empty() ? std::filesystem::path{"."} : path.parent_path());
    AppConfig cfg = make_default_config();
    if (!std::filesystem::exists(path)) {
        save_config(path, cfg);
        return cfg;
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        Logger::instance().warn("Config: failed to open file " + path.string() + ", using defaults");
        return cfg;
    }
    const auto ext = extension_of(path);
    try {
        json parsed;
        if (ext == ".json") {
            parsed = json::parse(file, nullptr, true, true);
        } else if (ext == ".toml" || ext == ".ini") {
            parsed = parse_ini_or_toml(file, ext == ".toml");
        } else if (ext == ".yaml" || ext == ".yml") {
            parsed = parse_yaml(file);
        } else {
            Logger::instance().warn("Config: unsupported extension " + ext + ", defaulting to JSON parser");
            file.clear();
            file.seekg(0);
            parsed = json::parse(file, nullptr, true, true);
        }
        cfg = json_to_config(parsed);
    } catch (const std::exception& ex) {
        Logger::instance().error(std::string("Config: parse error: ") + ex.what());
    }
    return cfg;
}

void save_config(const std::filesystem::path& path, const AppConfig& config) {
    std::filesystem::create_directories(path.parent_path().empty() ? std::filesystem::path{"."} : path.parent_path());
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) {
        Logger::instance().error("Config: failed to write to " + path.string());
        return;
    }
    json j = config_to_json(config);
    const auto ext = extension_of(path);
    if (ext == ".json") {
        file << j.dump(4);
    } else if (ext == ".toml" || ext == ".ini") {
        write_ini_or_toml(file, j, ext == ".toml");
    } else if (ext == ".yaml" || ext == ".yml") {
        write_yaml(file, j);
    } else {
        file << j.dump(4);
    }
}

ConfigHotReloader::ConfigHotReloader(std::filesystem::path path, AppConfig initial, Callback callback)
    : path_(std::move(path)), callback_(std::move(callback)), current_(std::move(initial)) {
    serialized_ = config_to_json(current_).dump();
    if (std::filesystem::exists(path_)) {
        std::error_code ec;
        last_write_ = std::filesystem::last_write_time(path_, ec);
        if (ec) {
            last_write_.reset();
        }
    }
}

ConfigHotReloader::~ConfigHotReloader() { stop(); }

void ConfigHotReloader::start() {
    if (running_.exchange(true)) return;
    worker_ = std::thread(&ConfigHotReloader::watchLoop, this);
}

void ConfigHotReloader::stop() {
    if (!running_.exchange(false)) return;
    if (worker_.joinable()) {
        worker_.join();
    }
}

AppConfig ConfigHotReloader::current() const {
    std::scoped_lock lock(mutex_);
    return current_;
}

void ConfigHotReloader::watchLoop() {
    while (running_.load()) {
        reloadIfNeeded();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void ConfigHotReloader::reloadIfNeeded() {
    std::error_code ec;
    if (!std::filesystem::exists(path_)) {
        return;
    }
    auto write_time = std::filesystem::last_write_time(path_, ec);
    if (ec) {
        return;
    }
    bool should_reload = !last_write_ || (*last_write_ != write_time);
    if (!should_reload) {
        return;
    }
    AppConfig updated = load_config(path_);
    write_time = std::filesystem::last_write_time(path_, ec);
    if (ec) {
        return;
    }
    auto serialized = config_to_json(updated).dump();
    {
        std::scoped_lock lock(mutex_);
        if (serialized == serialized_) {
            last_write_ = write_time;
            return;
        }
        current_ = updated;
        serialized_ = serialized;
        last_write_ = write_time;
    }
    if (callback_) {
        callback_(current());
    }
}