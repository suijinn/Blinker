// stb 単一ヘッダライブラリの実装を集約する翻訳単位。
// 警告はライブラリ由来のため、この TU のみ警告を抑止してビルドする(CMake 参照)。

#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#define STBI_WINDOWS_UTF8  // Windows で動作確認する場合に UTF-8 パスを扱えるように
#include "stb/stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBIW_WINDOWS_UTF8
#include "stb/stb_image_write.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb/stb_truetype.h"
