# Blinker のリリース。
#
#   .\release.ps1 x.y.z      (または release.bat x.y.z)
#
# やることは CMakeLists.txt の project(Blinker VERSION x.y.z) を書き換えて
# commit・push するだけ。バージョンの正はその 1 行だけで、タグ (vx.y.z) も
# GitHub Release も .github/workflows/{tag,release}.yml が push を検知して作る。
#
# このスクリプトはタグを打たない。手で打つとバージョンの正が二か所に増え、
# CMakeLists.txt と食い違ったときに release.yml が失敗する
# (「タグ vX と CMakeLists.txt の Y が食い違っている」)。
#
# main 以外のブランチで実行してもよい。その場合はバージョン変更をコミットして
# push するところまでで、タグと Release は main へマージされた時点で作られる。

[CmdletBinding()]
param(
    # 新しいバージョン (x.y.z)
    [Parameter(Position = 0)]
    [string]$Version
)

$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

function Fail([string]$message) {
    Write-Host "エラー: $message" -ForegroundColor Red
    exit 1
}

$cmakeLists = Join-Path $PSScriptRoot 'CMakeLists.txt'
$versionPattern = '(?m)^project\(Blinker VERSION (\d+\.\d+\.\d+)'

$text = [IO.File]::ReadAllText($cmakeLists)
$match = [regex]::Match($text, $versionPattern)
if (-not $match.Success) {
    Fail "CMakeLists.txt の project(Blinker VERSION x.y.z ...) を解析できない"
}
$current = $match.Groups[1].Value

if (-not $Version) {
    Write-Host "現在のバージョン: $current"
    Write-Host "使い方: .\release.ps1 <新しいバージョン>   例: .\release.ps1 x.y.z"
    exit 1
}
if ($Version -notmatch '^\d+\.\d+\.\d+$') {
    Fail "バージョンは x.y.z 形式で指定すること (指定値: '$Version')"
}
if ($Version -eq $current) {
    Fail "CMakeLists.txt は既に $current。バージョンを上げずにリリースはできない"
}

# 無関係な変更を巻き込んでコミットしないよう、作業ツリーが綺麗なことを確かめる
if (git status --porcelain --untracked-files=no) {
    git status --short --untracked-files=no  # 未追跡ファイルは commit されないので無視
    Fail "コミットされていない変更がある。先に commit / stash すること"
}

# タグが既にあると tag.yml は「作成済み」と判断して何もせず、Release も作られない。
# (手で打ったタグが残っている場合がこれに当たる)
git fetch --tags --quiet
if (git tag --list "v$Version") {
    Fail @"
タグ v$Version は既に存在する。手で打ったタグが残っていると、このリリースは
自動化の経路に乗らない。消してからやり直すこと:

    gh release delete v$Version --yes   # Release がある場合のみ
    git push --delete origin v$Version
    git tag -d v$Version
"@
}

$branch = git rev-parse --abbrev-ref HEAD
Write-Host "ブランチ : $branch"
Write-Host "バージョン: $current -> $Version"
if ($branch -ne 'main') {
    Write-Host "(main 以外なので、タグと Release は main へマージされた時点で作られる)"
}
if ((Read-Host "この内容で commit / push する? [y/N]") -notmatch '^[yY]') {
    Write-Host "中止した"
    exit 1
}

$updated = [regex]::Replace($text, $versionPattern, "project(Blinker VERSION $Version")
[IO.File]::WriteAllText($cmakeLists, $updated)

git add CMakeLists.txt
git commit -m "バージョンを $Version に上げる"
if ($LASTEXITCODE -ne 0) { Fail "commit に失敗した" }
git push origin $branch
if ($LASTEXITCODE -ne 0) { Fail "push に失敗した" }

Write-Host ""
Write-Host "push した。以降は GitHub Actions が処理する:" -ForegroundColor Green
Write-Host "  tag.yml     : main への push を検知して v$Version を作成し、"
Write-Host "                続けて release.yml を呼ぶ"
Write-Host "  release.yml : ビルド・テストして Release に"
Write-Host "                blinker.exe を添付(版番号は付けない。上書き更新のため)"
Write-Host ""
Write-Host "進行の確認: gh run watch / gh release view v$Version"
Write-Host "失敗して作り直したいとき: gh workflow run release.yml -f tag=v$Version"
