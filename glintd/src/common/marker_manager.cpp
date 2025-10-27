#include "marker_manager.h"

void MarkerManager::add(Marker m) {
    std::lock_guard<std::mutex> lk(mtx_);
    markers_.push_back(m);
}

std::vector<Marker> MarkerManager::list() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return markers_;
}
