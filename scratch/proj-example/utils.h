#pragma once

#include <memory>
#include "ns3/callback.h"

using std::unique_ptr;
using std::make_unique;
using std::pair;

namespace std {
template <class T>
std::ostream& operator << (std::ostream &os, const std::vector<T> &vec) {
    os << '[';
    for (std::size_t i = 0; i < vec.size(); i++) {
        if (i != 0) {
            os << ", ";
        }
        os << vec[i];
    }
    os << ']';
    return os;
}

template <class T1, class T2>
std::ostream& operator << (std::ostream &os, const std::pair<T1, T2> &p) {
    os << '(' << p.first << ',' << p.second << ')';
    return os;
}
}


template <class Lambda>
auto LambdaToCallback(Lambda &&lambda) {
    // This creates an object that will not be deleted (until the process exits).
    // The lifetime of Callback is expected to span the whole execution.
    auto lambdaPtr = new Lambda{std::forward<Lambda>(lambda)};
    return ns3::MakeCallback(&Lambda::operator(), lambdaPtr);
}


inline std::vector<std::string> SplitString(const std::string &s, char splitChar) {
    std::vector<std::string> ret;
    if (s.empty()) {
        return ret;
    }
    std::size_t pos = 0;
    while (true) {
        std::size_t endPos = s.find(splitChar, pos);
        ret.push_back(s.substr(pos, endPos - pos));
        if (endPos == std::string::npos) {
            break;
        }
        pos = endPos + 1;
    }
    return ret;
}


constexpr int64_t operator ""_KiB(unsigned long long kib) {
    return kib * (1 << 10);
}

constexpr int64_t operator ""_MiB(unsigned long long mib) {
    return mib * (1 << 20);
}

constexpr int64_t operator ""_GiB(unsigned long long gib) {
    return gib * (1 << 30);
}

constexpr int64_t operator ""_KiB(long double kib) {
    return static_cast<int64_t>(kib * (1 << 10));
}

constexpr int64_t operator ""_MiB(long double mib) {
    return static_cast<int64_t>(mib * (1 << 20));
}

constexpr int64_t operator ""_GiB(long double gib) {
    return static_cast<int64_t>(gib * (1 << 30));
}
