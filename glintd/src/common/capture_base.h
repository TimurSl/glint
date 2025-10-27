#pragma once

class CaptureBase {
public:
    virtual ~CaptureBase() = default;
    virtual bool init() = 0;
    virtual bool start() = 0;
    virtual void stop() = 0;
};
