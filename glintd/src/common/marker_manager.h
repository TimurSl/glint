#pragma once
#include <string>
#include <vector>

struct Marker {
    int id;
    int ts_ms;
    int pre;
    int post;
};

class MarkerManager {
public:
    int addSession(const std::string& game, const std::string& container = "", const std::string& output = "");
    void stopSession(int id);
    void addMarker(int sid, int ts, int pre, int post);
    std::vector<Marker> listMarkers(int sid);
};
