#pragma once
#include <mutex>
#include <vector>
#include <cstdint>
#include <string>

struct Marker {
    std::uint64_t timestamp_ms;
    double pre_sec;
    double post_sec;
};

class MarkerManager {
public:
    void add(Marker m);
    std::vector<Marker> list() const;
private:
    mutable std::mutex mtx_;
    std::vector<Marker> markers_;
};
