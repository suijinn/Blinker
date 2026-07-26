#include "core/print_layout.h"

#include <algorithm>
#include <cmath>

namespace blinker {

PrintPlacement layoutPrintImage(const int imageWidth, const int imageHeight, const int areaWidth,
                                const int areaHeight, const bool autoRotate) {
    PrintPlacement placement;
    if (imageWidth <= 0 || imageHeight <= 0 || areaWidth <= 0 || areaHeight <= 0) {
        return placement;  // width / height = 0 のまま返す(配置できない)
    }

    const double w = imageWidth;
    const double h = imageHeight;
    const double aw = areaWidth;
    const double ah = areaHeight;
    // そのまま置いた場合と、90 度回して置いた場合の収まる倍率
    const double scale = std::min(aw / w, ah / h);
    const double scaleRotated = std::min(aw / h, ah / w);

    placement.rotated = autoRotate && scaleRotated > scale;
    const double placedScale = placement.rotated ? scaleRotated : scale;
    const double placedWidth = (placement.rotated ? h : w) * placedScale;
    const double placedHeight = (placement.rotated ? w : h) * placedScale;

    // 丸めで 1px はみ出すことがあるので領域内へ収める(0px にはしない)
    placement.width = std::clamp(static_cast<int>(std::lround(placedWidth)), 1, areaWidth);
    placement.height = std::clamp(static_cast<int>(std::lround(placedHeight)), 1, areaHeight);
    placement.x = (areaWidth - placement.width) / 2;
    placement.y = (areaHeight - placement.height) / 2;
    return placement;
}

} // namespace blinker
