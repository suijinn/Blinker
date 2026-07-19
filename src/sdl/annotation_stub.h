#pragma once

#include "platform/annotation.h"

namespace blinker {

// SDL バックエンドは注釈編集を未サポート(コンテキストメニュー未実装のため
// 編集メニューに到達しない)。万一呼ばれても失敗を返し、App 側は
// 「描画に失敗しました」の通知で安全に継続する。
class AnnotationStub final : public IAnnotationRasterizer {
public:
    AnnotationOverlay rasterize(const AnnotationSpec&) override { return {}; }
};

} // namespace blinker
