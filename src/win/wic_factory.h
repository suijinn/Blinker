#pragma once

struct IWICImagingFactory;

/**
 * @file wic_factory.h
 * @brief スレッドローカルな WIC ファクトリの取得。
 */

namespace blinker {

/**
 * @brief 呼び出しスレッド用の WIC ファクトリを返す。
 *
 * スレッドごとに COM と WIC ファクトリを初期化して使い回す。
 * decoder_wic (ワーカースレッド) と encoder_wic (UI スレッド) で共用する。
 *
 * @return WIC ファクトリ。所有権は移らない(解放しないこと)。初期化に失敗した場合は nullptr。
 */
IWICImagingFactory* wicFactoryForThisThread();

} // namespace blinker
