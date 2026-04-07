# ImGui Bloom Implementation Notes

This fork now uses a single offscreen ImGui render path.

The old version rendered ImGui twice when bloom was enabled:

1. draw ImGui directly to the viewport through Slate
2. draw the same ImGui data again into an offscreen render target for bloom

That was easy to bolt on, but it duplicated the expensive part: converting and rasterizing the same ImGui draw data twice.

The current design is:

1. render ImGui once into a full-size offscreen source render target
2. present that source render target back to Slate with normal UI alpha blending
3. build bloom from the same source render target
4. additively composite bloom over the presented source

## Important constraint

Stock ImGui vertex colors are packed into `ImDrawVert::col`, which is 8-bit per channel.

That means regular ImGui widgets are still fundamentally LDR at the vertex-data level. The bloom system therefore works from the captured UI texture plus:

- a global bloom intensity
- a global bloom threshold

It does not carry true HDR widget values through the stock ImGui vertex format.

## Main files

- `Source/ImGui/Private/SImGuiOverlay.cpp`
- `Source/ImGui/Private/SImGuiOverlay.h`
- `Source/ImGui/Private/ImGuiBloom.cpp`
- `Source/ImGui/Private/ImGuiBloom.h`
- `Shaders/Private/ImGuiBloom.usf`

Shader bootstrapping is still handled by:

- `ImGui.uplugin`
- `Source/ImGui/ImGui.Build.cs`
- `Source/ImGui/Private/ImGuiModule.cpp`

## Frame flow

### 1. ImGui finishes recording draw lists

At the end of the frame the plugin calls `ImGui::Render()`.

`ImGuiViewport->DrawData` is then handed to `SImGuiOverlay::SetDrawData()`.

The overlay stores that frame's ImGui data in `FImGuiDrawData` / `FImGuiDrawList`.

### 2. ImGui is rendered once into the source render target

`SImGuiOverlay::UpdateSourceCapture()` allocates a full-size `UTextureRenderTarget2D` using `DrawData.DisplaySize`.

It uses `FWidgetRenderer` to render `SImGuiSourceWidget` into that texture.

`SImGuiSourceWidget::OnPaint()` calls `DrawImGuiDrawData()`, which:

- converts `ImDrawVert` to `FSlateVertex`
- copies command-local indices
- resolves each command texture
- submits `FSlateDrawElement::MakeCustomVerts()`

That means the actual GPU rasterization is still done by Slate, but it now targets the offscreen source render target instead of the viewport.

There is no crop rect path anymore. Capture is always full `DisplaySize`.

### 3. Slate enqueues one custom present element

`SImGuiOverlay::OnPaint()` no longer draws ImGui directly with `MakeCustomVerts()`.

Instead it enqueues a single custom Slate element, `FImGuiPresentDrawer`, with:

- the source render target texture reference
- the screen output rect for this overlay
- bloom intensity / threshold settings

### 4. The custom Slate element runs render-thread work

`FImGuiPresentDrawer::Draw_RenderThread()` imports the offscreen source texture into RDG.

RDG means Render Dependency Graph, UE5's render-graph API for declaring transient textures and fullscreen passes.

The render-thread flow is:

1. present source RT to the Slate output with normal alpha blending
2. if bloom intensity is zero, stop here
3. threshold the source RT into a bloom texture
4. build an explicit downsample chain
5. blur each level with separable blur
6. upsample and accumulate back to full resolution
7. additively composite the final bloom texture over the already-presented source

This preserves glow outside the original UI alpha silhouette.

If source and bloom were folded into one final texture and then alpha-blended once, bloom could get clipped by the original source alpha.

## Shader passes

`Shaders/Private/ImGuiBloom.usf` contains:

- `SourcePS`
  - presents the captured UI texture back to the viewport
- `ThresholdPS`
  - extracts bright parts from the source texture
- `DownsamplePS`
  - builds the bloom pyramid explicitly
- `BlurPS`
  - separable blur
- `UpsamplePS`
  - combines low-resolution bloom back upward
- `CompositePS`
  - outputs the final bloom color for additive blending

The source-present pass uses normal UI alpha blending.

The bloom composite uses additive RGB blending.

## Why full-size capture is used

The earlier crop-rect logic made the code harder to reason about and added instability risk.

The current implementation always captures the full ImGui display size because:

- it keeps coordinate mapping simple
- it keeps the source RT and output rect 1:1
- it removes crop jitter and padding heuristics
- it makes the render-thread composite easier to debug

## Why the source is not overwritten in-place

The bloom chain is built in separate transient textures.

That is intentional:

- the source RT keeps the original UI color and alpha intact
- the source pass can use normal UI alpha blending
- bloom remains a separate additive layer

Overwriting source mip 0 with pre-composited bloom would make final alpha behavior harder to control.

## Public controls

Bloom is still controlled per ImGui context through:

- `ImGui::SetBloomIntensity(float)`
- `ImGui::GetBloomIntensity()`
- `ImGui::SetBloomThreshold(float)`
- `ImGui::GetBloomThreshold()`

If intensity is `0`, the system still renders ImGui through the offscreen source RT, but it skips the bloom-generation passes.

## Practical mental model

Think of the current UE5 path like this:

- ImGui produces CPU draw lists
- Slate turns those draw lists into pixels in an offscreen source RT
- RDG post-processes that source RT
- Slate's custom render-thread hook composites source + bloom to the final output

That is the whole system.
