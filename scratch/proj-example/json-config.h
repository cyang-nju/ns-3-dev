
#include "ns3/core-module.h"
#include <optional>
#include <sstream>

namespace JsonConfig {

constexpr auto NS3_DEFAULTS_KEY = "ConfigDefault";

using ConfigPath = const std::vector<std::string_view> &;

/// Initialize from a list of json files.
/// ns3::Config::SetDefault is called for values under path "ConfigDefault".
void Init(const std::vector<std::string>& fileList, const std::vector<std::string>& extraKVs = {});
void EnableLog();
void Print(std::ostream &os);

bool Contains(ConfigPath path);
bool GetBool(ConfigPath path);
uint64_t GetUInt(ConfigPath path);
double GetDouble(ConfigPath path);
std::string GetString(ConfigPath path);

/// Construct value from string.
/// Required: operator >> (ostream &, T &)
template <class T>
T Get(ConfigPath path) {
    std::string s = GetString(path);
    std::istringstream iss{s};
    T ret;
    iss >> ret;
    return ret;
}

// Returns std::nullopt if path not exist
std::optional<bool> GetBoolOrNull(ConfigPath path);
std::optional<uint64_t> GetUIntOrNull(ConfigPath path);
std::optional<double> GetDoubleOrNull(ConfigPath path);
std::optional<std::string> GetStringOrNull(ConfigPath path);

template <class T>
std::optional<T> GetOrNull(ConfigPath path) {
    std::optional<std::string> s = GetStringOrNull(path);
    if (!s.has_value()) {
        return std::nullopt;
    }
    std::istringstream iss{s.value()};
    T ret;
    iss >> ret;
    return ret;
}



inline bool Contains(std::string_view key) { return Contains({key}); }
inline bool GetBool(std::string_view key) { return GetBool({key}); }
inline uint64_t GetUInt(std::string_view key) { return GetUInt({key}); }
inline double GetDouble(std::string_view key) { return GetDouble({key}); }
inline std::string GetString(std::string_view key) { return GetString({key}); }
template <class T> T Get(std::string_view key) { return Get<T>({key}); }

inline std::optional<bool> GetBoolOrNull(std::string_view key) { return GetBoolOrNull({key}); }
inline std::optional<uint64_t> GetUIntOrNull(std::string_view key) { return GetUIntOrNull({key}); }
inline std::optional<double> GetDoubleOrNull(std::string_view key) { return GetDoubleOrNull({key}); }
inline std::optional<std::string> GetStringOrNull(std::string_view key) { return GetStringOrNull({key}); }
template <class T> std::optional<T> GetOrNull(std::string_view key) { return GetOrNull<T>({key}); }


inline ns3::BooleanValue GetBoolValue(ConfigPath path)  { return ns3::BooleanValue{GetBool(path)}; }
inline ns3::UintegerValue GetUIntValue(ConfigPath path) { return ns3::UintegerValue{GetUInt(path)}; }
inline ns3::DoubleValue GetDoubleValue(ConfigPath path) { return ns3::DoubleValue{GetDouble(path)}; }
inline ns3::StringValue GetStringValue(ConfigPath path) { return ns3::StringValue{GetString(path)}; }

inline ns3::BooleanValue GetBoolValue(std::string_view key)  { return ns3::BooleanValue{GetBool({key})}; }
inline ns3::UintegerValue GetUIntValue(std::string_view key) { return ns3::UintegerValue{GetUInt({key})}; }
inline ns3::DoubleValue GetDoubleValue(std::string_view key) { return ns3::DoubleValue{GetDouble({key})}; }
inline ns3::StringValue GetStringValue(std::string_view key) { return ns3::StringValue{GetString({key})}; }

} // namespace JsonConfig