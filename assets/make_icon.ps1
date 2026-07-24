# blinker.ico を生成する。
#
# アイコンの正はこのスクリプト。.ico はここからの生成物なので、
# デザインを変えるときは .ico を直接編集せず、ここを直して再実行すること。
#
#   pwsh -File assets\make_icon.ps1
#
# デザイン: 水色グラデーションの角丸正方形に、白い「欠けたリング」。

[CmdletBinding()]
param(
  [string]$OutPath = (Join-Path $PSScriptRoot "blinker.ico"),
  # 各サイズは 4 倍で描いてから縮小する(小サイズの輪郭を滑らかにするため)
  [int]$Supersample = 4
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

# Windows のシェルが使うサイズ一式
$sizes = @(16, 20, 24, 32, 48, 64, 128, 256)

# --- デザイン定数(256px 基準。他サイズへは比例させる) ---
$BASE        = 256.0
$COLOR_TOP   = "#7FE0FF"   # 左上
$COLOR_BOT   = "#2BB8F0"   # 右下
$BG_MARGIN   = 4.0         # 角丸正方形の余白
$BG_RADIUS   = 56.0
$RING_BOX    = 62.0        # リングの外接矩形(正方形)の左上
$RING_SIZE   = 132.0       # 同 一辺
$RING_WIDTH  = 34.0        # 線幅
$RING_START  = 345.0       # 開始角(度、3時方向から時計回り)
$RING_SWEEP  = 300.0       # 掃引角。360 に満たない分が「欠け」になる

function New-RoundedRectPath([double]$x, [double]$y, [double]$w, [double]$h, [double]$r) {
  $path = New-Object System.Drawing.Drawing2D.GraphicsPath
  $d = $r * 2
  $path.AddArc($x,          $y,          $d, $d, 180, 90)
  $path.AddArc($x + $w - $d, $y,          $d, $d, 270, 90)
  $path.AddArc($x + $w - $d, $y + $h - $d, $d, $d,   0, 90)
  $path.AddArc($x,          $y + $h - $d, $d, $d,  90, 90)
  $path.CloseFigure()
  $path
}

# 一辺 $size のアイコン画像を描く
function New-IconBitmap([int]$size) {
  $k = $size / $BASE
  $bmp = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  try {
    $g.SmoothingMode     = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.PixelOffsetMode   = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic

    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
      (New-Object System.Drawing.PointF(0, 0)),
      (New-Object System.Drawing.PointF([single]$size, [single]$size)),
      [System.Drawing.ColorTranslator]::FromHtml($COLOR_TOP),
      [System.Drawing.ColorTranslator]::FromHtml($COLOR_BOT))
    $bg = New-RoundedRectPath ($BG_MARGIN * $k) ($BG_MARGIN * $k) `
                              (($BASE - $BG_MARGIN * 2) * $k) (($BASE - $BG_MARGIN * 2) * $k) `
                              ($BG_RADIUS * $k)
    $g.FillPath($brush, $bg)
    $bg.Dispose(); $brush.Dispose()

    $pen = New-Object System.Drawing.Pen([System.Drawing.Color]::White, [single]($RING_WIDTH * $k))
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap   = [System.Drawing.Drawing2D.LineCap]::Round
    $g.DrawArc($pen, [single]($RING_BOX * $k), [single]($RING_BOX * $k),
                     [single]($RING_SIZE * $k), [single]($RING_SIZE * $k),
                     [single]$RING_START, [single]$RING_SWEEP)
    $pen.Dispose()
  } finally {
    $g.Dispose()
  }
  $bmp
}

# 4 倍で描いてから縮小した $size ピクセルの画像を返す
function New-IconBitmapSmooth([int]$size) {
  if ($Supersample -le 1) { return (New-IconBitmap $size) }
  $big = New-IconBitmap ($size * $Supersample)
  try {
    $bmp = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    try {
      $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
      $g.PixelOffsetMode   = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
      $g.CompositingMode   = [System.Drawing.Drawing2D.CompositingMode]::SourceCopy
      $g.DrawImage($big, (New-Object System.Drawing.Rectangle(0, 0, $size, $size)))
    } finally {
      $g.Dispose()
    }
    return $bmp
  } finally {
    $big.Dispose()
  }
}

# 注: PowerShell は byte[] をパイプラインで 1 要素ずつに展開してしまうため、
# 戻り値も入れ物も [byte[]] を明示して保持すること
function Get-PngBytes([System.Drawing.Bitmap]$bmp) {
  $ms = New-Object System.IO.MemoryStream
  try {
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    [byte[]]$ms.ToArray()
  } finally {
    $ms.Dispose()
  }
}

# --- 各サイズを PNG にする ---
$pngs = New-Object 'System.Collections.Generic.List[byte[]]'
foreach ($size in $sizes) {
  $bmp = New-IconBitmapSmooth $size
  try { $pngs.Add([byte[]](Get-PngBytes $bmp)) } finally { $bmp.Dispose() }
}

# --- ICO を組み立てる(全エントリ PNG 圧縮。Vista 以降が読める) ---
$out = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter($out)
try {
  $w.Write([uint16]0)             # reserved
  $w.Write([uint16]1)             # type: 1 = icon
  $w.Write([uint16]$sizes.Count)

  # 画像データは全エントリのディレクトリの直後から並べる
  $offset = 6 + 16 * $sizes.Count
  for ($i = 0; $i -lt $sizes.Count; $i++) {
    $size = $sizes[$i]
    $w.Write([byte]($(if ($size -ge 256) { 0 } else { $size })))  # 256 は 0 で表す
    $w.Write([byte]($(if ($size -ge 256) { 0 } else { $size })))
    $w.Write([byte]0)             # パレット色数(トゥルーカラーは 0)
    $w.Write([byte]0)             # reserved
    $w.Write([uint16]1)           # プレーン数
    $w.Write([uint16]32)          # bpp
    $w.Write([uint32]$pngs[$i].Length)
    $w.Write([uint32]$offset)
    $offset += $pngs[$i].Length
  }
  foreach ($png in $pngs) { $w.Write([byte[]]$png, 0, $png.Length) }
  $w.Flush()

  [System.IO.File]::WriteAllBytes($OutPath, $out.ToArray())
} finally {
  $w.Dispose()
  $out.Dispose()
}

$total = (Get-Item $OutPath).Length
Write-Host ("{0} ({1} sizes, {2} bytes)" -f $OutPath, $sizes.Count, $total)
