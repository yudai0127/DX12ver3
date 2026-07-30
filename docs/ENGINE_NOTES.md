# DX12ver3 エンジンノート

新しいセッションの引き継ぎ用。**まずこれを読んでから作業を始めること。**

目標は、レイトレーシングを使ってリアルな絵を出せるゲームエンジン（RE ENGINE / Unreal のような）を作ること。
検証環境は Intel Core i7-11800H / 16GB / **RTX 3050 Laptop**、出力解像度 **1280x720**。

---

## 1. 現在の構成

### レンダーモード（ImGui の Render Mode [F6]）

Framework 側は `0 = Rasterizer / 1 = Raytracing / 2 = Path Tracing`。
`RaytracingRenderer::renderMode` は **`0 = Raytracing / 1 = Path Tracing`** で、`Framework.cpp` が `m_renderMode - 1` に変換して渡す。ここを取り違えやすいので注意。

| モード | 内容 | 時間的処理 |
|---|---|---|
| Rasterizer | 従来のポリゴン描画 | なし |
| Raytracing | 直接光 + ハードシャドウ + 鋭い鏡面反射。決定論的（乱数を使わない） | **自前 TAA**（`TAA_CS.hlsl`） |
| Path Tracing | モンテカルロ GI | **DLSS Ray Reconstruction**、または自前デノイザ（`Denoise_CS.hlsl`） |

**モードごとに正解が違う。** これは計測で確定している（後述）。

### パイプライン

```
DispatchRays (m_renderWidth x m_renderHeight)   ← 出力解像度ではない
  └ PathTrace.hlsl の RayGen が mode で分岐
      mode 0: gOutput(LDR) + gAccum(linear HDR) + 全ガイドバッファ
      mode 1: gAccum(linear HDR) + 全ガイドバッファ
  ↓
時間的処理（どれか一つ）
  ├ TAA_CS.hlsl        … Raytracing かつ DLSS オフ
  ├ Denoise_CS.hlsl    … Path Tracing かつ DLSS オフ かつ Denoise オン
  └ DLSS::EvaluateRR   … DLSS オン（両モード可）
  ↓
Upscale_CS.hlsl … 出力解像度へ拡大。DLSS 経由のときだけここでトーンマップ
  ↓
CopyResource → バックバッファ
```

### バッファ（すべて出力解像度で確保し、左上の部分矩形だけ使う）

`m_output`(LDR) / `m_accum`(linear HDR) / `m_geo`(normal.xyz + hitT.w) / `m_albedo` /
`m_motion` / `m_normalRough` / `m_specAlbedo` / `m_linearDepth` /
`m_upscaled` / `m_dlssColor`(RGBA16F) / `m_histColor[2]` / `m_histGeo[2]`

**リサイズ時以外は再確保しない。** Render Scale スライダを動かしても確保はやり直さない（部分矩形を変えるだけ）。DLSS もこの形を期待している。

`m_histColor` / `m_histGeo` は **TAA とデノイザで共用**。モードを切り替えると片方の履歴がもう片方に流れ込むので、`renderMode != m_prevRenderMode` のとき履歴を無効化している。

---

## 2. 重要な決定と、その理由

ここが一番の資産。**理由を知らずに戻すと、直したバグが再発する。**

### 2.1 カメラレイは逆射影ではなく、カメラ基底から作る（最重要）

`PathTrace.hlsl` の `cameraRay()`：

```hlsl
return normalize(camFwd.xyz
               + camRight.xyz * (ndc.x * camRight.w)   // w = tan(fovY/2) * aspect
               + camUp.xyz    * (ndc.y * camUp.w));    // w = tan(fovY/2)
```

**以前は `invViewProj` で far 平面を逆射影して原点との差を取っていた。これが長期間の不具合の真犯人だった。**

`nearZ = 0.01`, `farZ = 400`（比 40000:1）では逆射影の条件数が悪く、**レイ方向の誤差がサブピクセルジッター（±0.5px）と同オーダーになる**。`motionVector()` は `MV = ppix - (pix + 0.5 + jit)` を計算するが、`worldPos` が誤差を含むレイから来るため、**カメラが完全に静止していてもモーションベクタが毎フレーム乱数になっていた**。

これで全ての症状が説明できた：

- 背景は安定（中心レイは非ジッターなので誤差が一定）
- モデルだけうねる（ジッターされたレイなので誤差が毎フレーム変わる）
- **デノイズを切るとうねりが消える**（累積はモーションベクタを使わない）
- ガイドバッファの平均化で少しマシになる（平均が誤差を隠していた）
- ラスタライザは無関係（順方向変換しか使わない）
- カメラを動かすと揺れる

`Denoise_CS.hlsl` と `TAA_CS.hlsl` の `worldPosOf()` も同じ理由で同じ形にしてある。定数バッファが行列ではなくカメラ基底（`gCamRight/gCamUp/gCamFwd/gCameraPos`）を運んでいるのはこのため。

### 2.2 モーションベクタからジッターを引く

```hlsl
float2 motionVector(float3 worldPos, uint2 pix, uint2 dim, float2 jit)
{ ... return ppix - (float2(pix) + 0.5f + jit); }
```

`worldPos` は**ジッターされたレイ**の当たり点なので、`pix + 0.5 + jit` に再投影される。中心を引くとジッターがモーションベクタに焼き込まれ、静止カメラでもゼロにならない。背景は中心レイ（`dir0`）から作るので `jit = 0` を渡す。

### 2.3 DLSS 時はガイドバッファも色も累積しない

`tAcc = dlssMode ? 1.0f : t`。RR は**色もガイドも「そのフレームの値」で、報告したジッター位置に対応している**ことを前提にしている。平均は複数のジッター位置にまたがるので、色とガイドが食い違う。特に `gLinearDepth` を平均すると、シルエット上で前景と背景の中間という**存在しない深度**が RR に渡り、輪郭にゴーストが出る。

かつてガイドを平均していたのは、うねりへの対症療法だった。真因（2.1）が直ったので根拠は消えている。

自前デノイザ側は逆で、寄る辺がないので running mean のまま。

### 2.4 Raytracing モードは TAA、Path Tracing は DLSS

**計測値（1280x720 出力、RTX 3050 Laptop、GBV オフ）**

| | 描画解像度 | GPU | 備考 |
|---|---|---|---|
| Path Tracing ネイティブ | 1280x720 | 約 28.7ms | |
| Path Tracing + DLSS DLAA | 1280x720 | 30.8ms | 29.8 fps |
| Path Tracing + DLSS Balanced | 742x418 | 15.1ms | 約 66 fps |
| Path Tracing + DLSS Ultra Perf | 427x240 | 6.2ms | 125 fps、ただし明らかに眠い |
| Raytracing ネイティブ | 1280x720 | 約 3ms | 144 fps、**AA が一切ない** |
| Raytracing + TAA | 1280x720 | 約 3.3ms | **140 fps、ギラつき解消** |

DLAA と Ultra Performance の差から逆算すると、**RR の推論コストは約 3ms、パストレーシング本体が約 27.8ms**（DLSS 無しの実測 28.7ms とほぼ一致）。

つまり **DLSS の効果はほぼ全て「描画解像度の削減」**。だから：

- **Path Tracing は元が 28ms なので DLSS が大きく効く**
- **Raytracing は元が 3ms しかないので、3ms の推論を払う価値がない。**しかも拡大でボケる

Raytracing の「ギラギラ」は**鋭い鏡面反射のスペキュラエイリアシング**。ノイズではない。TAA の時間的蓄積で解決した。

**アプリは完全に GPU bound。Debug と Release でほぼ変わらない。**

### 2.5 TAA の履歴検証は「幾何」ではなく「色」

`TAA_CS.hlsl` はデノイザの平面距離判定を**使っていない**。シルエット上の画素はジッターにより「当たるフレーム」と「背景に抜けるフレーム」が混ざるので、幾何判定は**AA が最も必要な場所で必ず履歴を捨てる**。

代わりに 3x3 近傍の `mu ± 1.25 sigma` に履歴をクランプする。

- **min/max ではなく mu±sigma**：鋭いスペキュラは 1 画素だけ周囲の数百倍になり、min/max だと箱がそれに引きずられる
- **YCoCg 空間**：RGB のまま各チャンネルを丸めると、色相がずれた履歴が生き残り、動くエッジに色の縁取りが出る

`TAA_CS.hlsl` は `Denoise_CS.hlsl` と**バインディングと定数バッファを完全に一致**させてあるので、ルートシグネチャ・ディスクリプタテーブル・定数バッファを共用し、違うのは PSO だけ。

### 2.6 環境マップはぼかさない

かつて粗さに応じて環境マップの mip をぼかす `skyColorLod()` があったが削除済み。
CAPCOM GDC2026 の資料が言っているのは「**IBL を Streaming RIS の候補集合から外して独立に評価する**」（ライトリストに載せないだけで輝度は正確に評価し続ける）ことで、ぼかす話ではない。誤って結び付けていたので撤回した。

### 2.7 ペイロードに hit position を持たない

`WorldRayOrigin() + RayTCurrent() * WorldRayDirection()` と厳密に等しいので、RayGen 側で `origin + dir * p.hitT` として再構築する。ペイロードは全レイに付いて回りレジスタ圧を決めるので、60 → 48 バイト。

---

## 3. DLSS / Streamline のはまりどころ

SDK は **プリビルドの `streamline-sdk-v2.12.0`**（ソースからのビルドは LFS ポインタ問題で不可）。`Until/streamline-sdk-v2.12.0/` は `.gitignore` 済み。

ラッパは `SourceCode/RHI/DLSS.h/.cpp`。`#if __has_include(<sl.h>)` でガードしてあるので、SDK が無くてもビルドは通る。

過去に踏んだもの（全て解決済み。**戻すと再発する**）：

| 症状 | 原因と対処 |
|---|---|
| `Missing NGX context - DLSSContext cannot run` | デバイス/ファクトリを **`sl.interposer.dll` 経由で作る必要がある**。`LoadLibraryW` + `GetProcAddress` で動的に読んでいる（`DLSS::CreateFactory/CreateDevice`）。`Device.cpp` の初期化順序に依存 |
| `Please provide correct application id` / `Failed to initialize NGX` | `applicationId == 0` のとき `sl::Preferences` に `engineVersion` と `projectId`(GUID) が必須 |
| `slSetTagForFrame ... flag is not set` | `PreferenceFlags::eUseFrameBasedResourceTagging` が必要 |
| `Error: HDR Color required` / `0xbad00005` | **RR は linear HDR しか受け付けない**。`m_accum` を色入力にし、トーンマップを DLSS の後（`Upscale_CS`）へ移した。`m_dlssColor` は RGBA16F |
| `slEvaluateFeature` 後のディスクリプタヒープ不一致 | Streamline が自分のヒープを束ね直して**戻さない**。呼び出し後にこちらのヒープを再バインドすること |

**その他の約束事**

- `f.jitter = (-jitter.x, -jitter.y)` — こちらは**サンプル位置**を `+jitter` ずらすが、Streamline が要求するのは**射影行列**に入れるオフセットで、同じサンプルに対して逆向きになる。試行錯誤の結果ではなく理由がある
- `mvecScale = {1/renderWidth, 1/renderHeight}`（符号は正。反転すると動かしたときざらつくことを確認済み）
- `linearDepth` は**ビュー空間 Z**。レイの距離ではない。混同すると DLSS が遮蔽変化を誤判定して履歴を捨てる
- Halton (2,3) の 16 サンプル周期
- DRS（動的解像度）は不可

---

## 4. 作業上の約束事

### エンコーディング

- **`SourceCode/**/*.cpp,h` は Shift-JIS。必ず維持すること。** UTF-8 で書き戻すと日本語コメントが壊れる
- `HLSL/*.hlsl` は ASCII
- このファイルは UTF-8

### 新しいファイルを足すとき

- **`.cpp` は Visual Studio のソリューションエクスプローラーで手動追加が必要**（忘れるとリンクエラー。`DLSS.cpp` で実際に踏んだ）
- **`.hlsl` は追加したら、プロパティの「項目の種類」を `Denoise_CS.hlsl` と同じにする**（構成＝すべての構成、プラットフォーム＝すべてのプラットフォーム）。HLSL は実行時に `ShaderManager::CompileFromFile` がコンパイルするので、VS にビルドさせてはいけない。放置すると `MSB6006 dxc.exe はコード 1 を伴って終了しました` になる（`TAA_CS.hlsl` で実際に踏んだ）

### シェーダーのエラーは「エラー一覧」に出ない

`PathTrace.hlsl` のコンパイルが失敗すると、**エラーは出力ウィンドウの `[RT]` にしか出ず**、アプリは「Raytracing (DXR): not available」と表示してラスタライザにフォールバックする。挙動がおかしいときはまず出力ウィンドウを見ること。

また、エラー一覧の `RAY_FLAG_*` が未宣言という類は **IntelliSense のノイズ**（プロジェクト列が空欄なら IntelliSense）。フィルタを「ビルドのみ」にすると消える。

### GPU ベースバリデーション

`SourceCode/RHI/Device.cpp` の `DX12_ENABLE_GPU_BASED_VALIDATION`。既定 0。

**ディスクリプタやリソースを新規に足したときは 1 に戻して確認すること。** Streamline のヒープ再バインド問題を見つけたのはこれ。切っていると、この種のバグはエラーではなく「画面が真っ黒」「ノイズまみれ」「TDR」としてしか現れない。

デバッグレイヤー本体は常時オンのままで、こちらは軽い。

---

## 5. 未解決 / 保留中

| 項目 | 状況 |
|---|---|
| **ファイアフライのクランプ** (`PathTrace.hlsl` の `maxLum = 10`) | 環境マップを BSDF サンプリングでしか拾えないため、明るい小領域に稀に当たって巨大な値が返る。それをクランプで潰しているので、その分だけ絵が暗い。**MIS + 環境マップ重点サンプリングが正しい直し方** |
| **mipmap + レイコーン LOD** | 一度入れて revert 済み（VRAM +33%、当時は効果が無いと判断）。遠景のテクスチャがチラつくようなら再導入する |
| `Accumulated: N spp` の表示 | DLSS 時は何も累積しないので、単なるフレームカウンタ。実害なし |
| リソースバリア | `EvaluateRR` 前の 6 個などを個別に発行している。配列で 1 回にまとめるのが作法だが、効果は測定誤差レベル |

---

## 6. ロードマップ

**次にやるのは MIS を勧める。** 理由は速度ではなく、(1) ファイアフライのクランプを正しく撤去できる、(2) Step 1 の面光源の前提条件になる、から。面光源を先に入れると、NEE と BSDF サンプリングが同じ光源を二重計上した絵を見ながらのデバッグになる。

### MIS + 環境マップ重点サンプリング

現状 `PathTrace.hlsl:429-437` が NEE（太陽方向へシャドウレイ）、`:439-472` が BSDF バウンスで、**重み付けせずに足している**。いま壊れてはいない（解析的な平行光源と環境マップが別物なので二重計上は起きていない）が、上記の 2 つの問題がある。

### Step 1: ライトシステム

**現状ライトは `Camera` が持っている**（`SourceCode/Camera/Camera.h:48-50`）。平行光源 1 個。まずカメラから引き剥がすのが出発点。

1. `Light` 構造体（POD、16 バイト整列、HLSL と共有）
   `position/range`, `direction/spotCosOuter`, `color/intensity`, `type/spotCosInner/radius`
   type: 0=平行, 1=点, 2=スポット
2. `Scene` が `std::vector<Light>` を持ち `StructuredBuffer` へ。dirty 時のみ再アップロード。グローバルルートシグネチャに SRV 追加、`SceneConstants` に `lightCount`
3. シェーダー：**Raytracing は全ライトをループ**、**Path Tracing は 1 バウンスにつき 1 個だけ確率的に選ぶ**（全ループだとバウンス数 x ライト数で爆発する）。まずは一様選択でよく、増えたら RIS に差し替える。器は同じ
4. 減衰は逆二乗 + 窓関数 `saturate(1 - (d/range)^4)^2`（単純な打ち切りだと range 境界に輪が出る）
5. **`radius`（発光体半径）を最初から入れることを勧める。** シャドウレイの着弾点を半径内で散らすだけで本物の半影が出る。デノイザ / DLSS があるので追加コストはほぼゼロで、ラスタライザが一番ごまかしている部分
6. ImGui は Objects パネルと同じ作りの Lights パネル
7. ラスタライザはライト配列の 0 番を既存の定数バッファに流して現状維持

**G-Buffer はここに含めない。** DLSS 用のガイドバッファ（albedo / normal+roughness / spec albedo / linear depth / motion）が既に G-Buffer そのもの。デファードのラスタライズパスが要るのは Step 5 のハイブリッド化のときで、ライトが 1 個しかない今作っても繋ぐ相手がいない。

### その先

- Step 2: レイトレーシングの影（面光源とセット）
- Step 3: レイトレーシングの反射
- Step 4: RTAO / RTGI
- Step 5: **ハイブリッド化** — 一次可視性をラスタライズで出し、影・反射・GI だけレイを飛ばす。ここで初めて G-Buffer が要る。**パストレーシングを構造的に速くする唯一の大きな手**（一次レイのトレースが丸ごと消える）
- Step 6: マテリアルシステム、カリング、LOD
- 物理エンジンは保留（ユーザー判断）

---

## 7. 運用メモ

- 開発は指定ブランチで行い、変更は **`.patch` ファイルで受け渡し**（アシスタント側の環境から push できないため）。適用は
  `git apply --ignore-whitespace --whitespace=nowarn <name>.patch`
  取り消しは `git apply -R <name>.patch`（`git revert` ではない。patch は commit していないため）
- パッチを作る側は、生成後に `git apply --check -R` で当該ツリーとの整合を必ず確認する
- ツリーが分岐すると patch が当たらなくなる。当たらないときは、まず片方に未適用/逆適用の patch がないか確認する
