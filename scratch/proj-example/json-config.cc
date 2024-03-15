#include "json-config.h"
#include "nlohmann-json.hpp"
#include "utils.h"
#include <cctype>
#include <filesystem>
#include <string>

using namespace ns3;
namespace fs = std::filesystem;
using json = nlohmann::json;




namespace JsonConfig {

static json configObj;

static bool logEnabled = false;

void EnableLog() {
    logEnabled = true;
}

static std::ostream& operator<< (std::ostream& os, ConfigPath path) {
    bool flag = false;
    for (auto key : path) {
        if (flag) {
            os << ".";
        }
        flag = true;
        os << key;
    }
    return os;
}

void ConfigSetDefault() {
    if (!configObj.contains(NS3_DEFAULTS_KEY)) {
        return;
    }
    
    const auto& configs = configObj[NS3_DEFAULTS_KEY];
    for (const auto& [className, values] : configs.items()) {
        for (const auto& [attr, value] : values.items()) {
            std::ostringstream oss;
            oss << className << "::" << attr;
            std::string path = oss.str();
            
            auto valueType = value.type();
            switch(valueType) {
            using value_t = decltype(valueType);
            case value_t::string:
                Config::SetDefault(path, StringValue{value.get<std::string>()});
                break;
            case value_t::boolean:
                Config::SetDefault(path, BooleanValue{value.get<bool>()});
                break;
            case value_t::number_integer:
                Config::SetDefault(path, IntegerValue{value.get<int64_t>()});
                break;
            case value_t::number_unsigned:
                Config::SetDefault(path, StringValue{std::to_string(value.get<uint64_t>())});
                break;
            case value_t::number_float:
                Config::SetDefault(path, DoubleValue{value.get<double>()});
                break;
            default:
                NS_ABORT_MSG(
                    "Unexpected value at "
                    << NS3_DEFAULTS_KEY << "." << className << "." << attr
                    << ": " << value
                );
            }
        }
    }
}

static void MergeJsonObj(json& obj, json&& other) {
    obj.merge_patch(other);
}

static void MergeConfigJson(json&& newConfigObj) {
    if (configObj.empty()) {
        configObj = std::move(newConfigObj);
    } else {
        MergeJsonObj(configObj, std::move(newConfigObj));
    }
}

// convert single line comments and trailing commas to space
static void removeCommentsAndTrailingCommas(std::string& content) {
    bool inStr = false;
    bool inComment = false;
    int commaPos = 0;
    for (std::size_t i = 0; i < content.size(); i++) {
        char c = content[i];
        if (inStr) {
            if (c == '\\') {
                i++;
            } else if (c == '"') {
                inStr = false;
            }
            continue;
        }

        if (inComment) {
            if (c == '\n') {
                inComment = false;
            } else {
                content[i] = ' ';
            }
            continue;
        }

        switch (c) {
        case '"':
            inStr = true;
            break;
        case '/':
            if (i + 1 < content.size() && content[i + 1] == '/') {
                inComment = true;
                content[i] = ' ';
            }
            break;
        case ',':
            commaPos = (int)i;
            break;
        case '}':
        case ']':
            if (commaPos != -1) {
                content[commaPos] = ' ';
            }
            break;
        default:
            if (!std::isspace(c)) {
                commaPos = -1;
            }
        }
    }
}

void Init(const std::vector<std::string>& fileList, const std::vector<std::string>& extraKVs) {
    constexpr auto search = [](const std::string &filename) -> fs::path {
        fs::path path{filename};
        if (path.is_absolute()) {
            return path;
        }
        fs::path searchPaths[] = {
            // CURR_SRC_DIR_ABSOLUTE is set to ${CMAKE_CURRENT_SOURCE_DIR} in CMakeLists.txt
            fs::path{CURR_SRC_DIR_ABSOLUTE}.append("config"),
            fs::path{CURR_SRC_DIR_ABSOLUTE},

#ifdef ROOT_SRC_DIR
            // ROOT_SRC_DIR is set to ${CMAKE_SOURCE_DIR} i.e. ns3 home in CMakeLists.txt
            fs::path{ROOT_SRC_DIR}.append("config"),
            fs::path{ROOT_SRC_DIR}
#endif
        };
        for (auto& path : searchPaths) {
            path.append(filename);
            if (fs::exists(path)) {
                return path;
            }
        }
        NS_ABORT_MSG("failed to find " << filename);
        return {};
    };


    for (auto& filename : fileList) {
        fs::path path = search(filename);
        if (logEnabled) {
            std::clog << "Reading config file " << path << std::endl;
        }
        std::ifstream configFile{path};
        if (!configFile) {
            NS_ABORT_MSG("Failed to open config file " << filename);
        }

        std::stringstream buf;
        buf << configFile.rdbuf();
        std::string content = buf.str();
        removeCommentsAndTrailingCommas(content);
        auto config = json::parse(content);
        MergeConfigJson(std::move(config));
    }
    
    for (const auto& kvStr : extraKVs) {
        auto splitPos = kvStr.find('=');
        if (splitPos == std::string::npos) {
            continue;
        }
        auto key = kvStr.substr(0, splitPos);
        auto val = kvStr.substr(splitPos + 1);
        auto keyPath = SplitString(key, '.');
        if (keyPath[0] == "ns3") {
            keyPath[0] = NS3_DEFAULTS_KEY;
        }
        json* jsonval = &configObj;
        for (std::string_view k : keyPath) {
            if (jsonval->is_object()) {
                jsonval = &(*jsonval)[k];
            } else {
                *jsonval = json::object();
                jsonval = &(*jsonval)[k];
            }
        }
        *jsonval = val;
    }

    ConfigSetDefault();

    if (logEnabled) {
        std::clog << "config: " << configObj << std::endl;
    }
}

void Print(std::ostream& os) {
    os << configObj << std::endl;
}

bool Contains(ConfigPath path) {
    json* val = &configObj;
    for (auto key : path) {
        if (!val->contains(key)) {
            return false;
        }
        val = &(*val)[key];
    }
    return true;
}

const json* GetConfigValueSafe(ConfigPath path) {
    json* val = &configObj;
    for (auto key : path) {
        if (!val->contains(key)) {
            return nullptr;
        }
        val = &(*val)[key];
    }
    return val;
}

static const json& GetConfigValue(ConfigPath path) {
    const json* val = GetConfigValueSafe(path);
    if (val == nullptr) {
        NS_ABORT_MSG("KeyError: "<< path);
    }
    return *val;
}

/// @param path used to provide info in NS_ABORT_MSG when exception raised
static bool toBool(const json& v, ConfigPath path) {
    if (v.is_boolean()) {
        return v.get<bool>();
    } else if (v.is_number_integer()) {
        return v.get<int64_t>() != 0;
    } else {
        NS_ASSERT_MSG(v.is_string(), "ValueTypeError: " << path);
        std::string s{v.get<std::string>()};
        // to lower case
        for (auto& c : s) {
            if (c >= 'A' && c <= 'Z') {
                c = 'a' + (c - 'A');
            }
        }
        if (s == "true" || s == "1" || s == "yes" || s == "y") {
            return true;
        } else if (s == "false" || s == "0" || s == "no" || s == "n") {
            return false;
        }
        NS_ABORT_MSG("unexpected value for bool: " << path);
    }
    return false;
}

/// @param path used to provide info in NS_ABORT_MSG when exception raised
static uint64_t toUInt(const json& v, ConfigPath path) {
    if (v.is_number_integer()) {
        return v.get<uint64_t>();
    } else {
        NS_ASSERT_MSG(v.is_string(), "ValueTypeError: " << path);
        uint64_t u;
        std::istringstream iss{std::string{v.get<std::string>()}};
        iss >> u;
        return u;
    }
}

/// @param path used to provide info in NS_ABORT_MSG when exception raised
static double toDouble(const json& v, ConfigPath path) {
    if (v.is_number_float()) {
        return v.get<double>();
    } else if (v.is_number_integer()) {
        return v.get<int64_t>();
    } else {
        NS_ASSERT_MSG(v.is_string(), "ValueTypeError: " << path);
        double d;
        std::istringstream iss{std::string{v.get<std::string>()}};
        iss >> d;
        return d;
    }
}

static std::string toString(const json& v, ConfigPath path) {
    NS_ASSERT_MSG(v.is_string(), "ValueTypeError: " << path);
    return std::string{v.get<std::string>()};
}

bool GetBool(ConfigPath path) {
    return toBool(GetConfigValue(path), path);
}

uint64_t GetUInt(ConfigPath path) {
    return toUInt(GetConfigValue(path), path);
}

double GetDouble(ConfigPath path) {
    return toDouble(GetConfigValue(path), path);
}

std::string GetString(ConfigPath path) {
    return toString(GetConfigValue(path), path);
}



std::optional<bool> GetBoolOrNull(ConfigPath path) {
    const auto *val = GetConfigValueSafe(path);
    return (val != nullptr ? toBool(*val, path) : std::optional<bool>{});
}

std::optional<uint64_t> GetUIntOrNull(ConfigPath path) {
    const auto *val = GetConfigValueSafe(path);
    return (val != nullptr ? toUInt(*val, path) : std::optional<uint64_t>{});
}

std::optional<double> GetDoubleOrNull(ConfigPath path) {
const auto *val = GetConfigValueSafe(path);
    return (val != nullptr ? toDouble(*val, path) : std::optional<double>{});
}

std::optional<std::string> GetStringOrNull(ConfigPath path) {
    const auto *val = GetConfigValueSafe(path);
    return (val != nullptr ? toString(*val, path) : std::optional<std::string>{});
}

} // namespace JsonConfig