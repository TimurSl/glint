#include "capture_base.h"
#include "../common/logger.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

class DxgiCapture : public IVideoCapture {
public:
    explicit DxgiCapture(int targetFps, bool withCursor);
    bool start(VideoCallback cb) override;
    void stop() override;
private:

};
