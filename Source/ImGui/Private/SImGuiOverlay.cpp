#include "SImGuiOverlay.h"

#include <Framework/Application/SlateApplication.h>

#if WITH_ENGINE
#include <Engine/TextureRenderTarget2D.h>
#include <Slate/WidgetRenderer.h>
#endif

#include "ImGuiBloom.h"
#include "ImGuiContext.h"

FImGuiDrawList::FImGuiDrawList(ImDrawList* Source)
{
	VtxBuffer.swap(Source->VtxBuffer);
	IdxBuffer.swap(Source->IdxBuffer);
	CmdBuffer.swap(Source->CmdBuffer);
	Flags = Source->Flags;
}

FImGuiDrawData::FImGuiDrawData(const ImDrawData* Source)
{
	bValid = Source->Valid;

	TotalIdxCount = Source->TotalIdxCount;
	TotalVtxCount = Source->TotalVtxCount;

	ImGui::CopyArray(Source->CmdLists, DrawLists);

	DisplayPos = Source->DisplayPos;
	DisplaySize = Source->DisplaySize;
	FrameBufferScale = Source->FramebufferScale;
}

namespace
{
	void UpdateTextureBrush(const ImDrawCmd& DrawCmd, FSlateBrush& TextureBrush)
	{
#if WITH_ENGINE
		UTexture* Texture = DrawCmd.GetTexID();
		if (TextureBrush.GetResourceObject() != Texture)
		{
			TextureBrush.SetResourceObject(Texture);
			if (IsValid(Texture))
			{
				TextureBrush.ImageSize.X = Texture->GetSurfaceWidth();
				TextureBrush.ImageSize.Y = Texture->GetSurfaceHeight();
				TextureBrush.ImageType = ESlateBrushImageType::FullColor;
				TextureBrush.DrawAs = ESlateBrushDrawType::Image;
			}
			else
			{
				TextureBrush.ImageSize = FVector2D::ZeroVector;
				TextureBrush.ImageType = ESlateBrushImageType::NoImage;
				TextureBrush.DrawAs = ESlateBrushDrawType::NoDrawType;
			}
		}
#else
		FSlateBrush* Texture = DrawCmd.GetTexID();
		if (Texture)
		{
			TextureBrush = *Texture;
		}
		else
		{
			TextureBrush.ImageSize = FVector2D::ZeroVector;
			TextureBrush.ImageType = ESlateBrushImageType::NoImage;
			TextureBrush.DrawAs = ESlateBrushDrawType::NoDrawType;
		}
#endif
	}

	void DrawCustomVerts(
		FSlateWindowElementList& OutDrawElements,
		const int32 LayerId,
		FSlateBrush& TextureBrush,
		const TArray<FSlateVertex>& Vertices,
		const TArray<SlateIndex>& Indices,
		const ImDrawCmd& DrawCmd,
		const FSlateRect& ClipRect)
	{
		const int32 VertexOffset = static_cast<int32>(DrawCmd.VtxOffset);
		const int32 IndexOffset = static_cast<int32>(DrawCmd.IdxOffset);
		const int32 ElementCount = static_cast<int32>(DrawCmd.ElemCount);

		if (ElementCount <= 0
			|| VertexOffset < 0
			|| VertexOffset >= Vertices.Num()
			|| IndexOffset < 0
			|| IndexOffset + ElementCount > Indices.Num())
		{
			return;
		}

		TArray<SlateIndex> RebasedIndices;
		RebasedIndices.Reserve(ElementCount);

		int32 MaxLocalVertexIndex = INDEX_NONE;

		for (int32 ElementIndex = 0; ElementIndex < ElementCount; ++ElementIndex)
		{
			const int32 LocalVertexIndex = static_cast<int32>(Indices[IndexOffset + ElementIndex]);
			if (LocalVertexIndex < 0)
			{
				return;
			}

			RebasedIndices.Add(static_cast<SlateIndex>(LocalVertexIndex));
			MaxLocalVertexIndex = FMath::Max(MaxLocalVertexIndex, LocalVertexIndex);
		}

		const int32 VertexCount = MaxLocalVertexIndex + 1;
		if (VertexCount <= 0 || VertexOffset + VertexCount > Vertices.Num())
		{
			return;
		}

		TArray<FSlateVertex> DrawVertices;
		DrawVertices.Append(Vertices.GetData() + VertexOffset, VertexCount);

		OutDrawElements.PushClip(FSlateClippingZone(ClipRect));

		FSlateDrawElement::MakeCustomVerts(
			OutDrawElements,
			LayerId,
			TextureBrush.GetRenderingResource(),
			MoveTemp(DrawVertices),
			MoveTemp(RebasedIndices),
			nullptr,
			0,
			0
		);

		OutDrawElements.PopClip();
	}

	int32 DrawImGuiDrawData(
		const FImGuiDrawData& DrawData,
		const FGeometry& AllottedGeometry,
		FSlateWindowElementList& OutDrawElements,
		const int32 LayerId)
	{
		if (!DrawData.bValid)
		{
			return LayerId;
		}

		const FSlateRenderTransform Transform(AllottedGeometry.GetAccumulatedRenderTransform().GetTranslation() - FVector2d(DrawData.DisplayPos));

		TArray<FSlateVertex> Vertices;
		TArray<SlateIndex> Indices;
		FSlateBrush TextureBrush;

		for (const FImGuiDrawList& DrawList : DrawData.DrawLists)
		{
			Vertices.SetNumUninitialized(DrawList.VtxBuffer.Size);

			ImDrawVert* SrcVertex = DrawList.VtxBuffer.Data;
			FSlateVertex* DstVertex = Vertices.GetData();

			for (int32 BufferIdx = 0; BufferIdx < Vertices.Num(); ++BufferIdx, ++SrcVertex, ++DstVertex)
			{
				DstVertex->TexCoords[0] = SrcVertex->uv.x;
				DstVertex->TexCoords[1] = SrcVertex->uv.y;
				DstVertex->TexCoords[2] = 1;
				DstVertex->TexCoords[3] = 1;
				DstVertex->Position = TransformPoint(Transform, FVector2f(SrcVertex->pos));
				DstVertex->Color.Bits = SrcVertex->col;
			}

			ImGui::CopyArray(DrawList.IdxBuffer, Indices);

			for (const ImDrawCmd& DrawCmd : DrawList.CmdBuffer)
			{
				UpdateTextureBrush(DrawCmd, TextureBrush);

				FSlateRect ClipRect(DrawCmd.ClipRect.x, DrawCmd.ClipRect.y, DrawCmd.ClipRect.z, DrawCmd.ClipRect.w);
				ClipRect = TransformRect(Transform, ClipRect);

				DrawCustomVerts(OutDrawElements, LayerId, TextureBrush, Vertices, Indices, DrawCmd, ClipRect);
			}
		}

		return LayerId;
	}
}

#if WITH_ENGINE
class SImGuiSourceWidget : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SImGuiSourceWidget)
	{
	}
	SLATE_END_ARGS()

	void Construct(const FArguments& Args)
	{
		SetVisibility(EVisibility::SelfHitTestInvisible);
	}

	void SetDrawData(const FImGuiDrawData& InDrawData)
	{
		DrawData = InDrawData;
	}

	virtual int32 OnPaint(
		const FPaintArgs& Args,
		const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements,
		int32 LayerId,
		const FWidgetStyle& InWidgetStyle,
		bool bParentEnabled) const override
	{
		return DrawImGuiDrawData(
			DrawData,
			AllottedGeometry,
			OutDrawElements,
			LayerId);
	}

	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override
	{
		return FVector2D(
			FMath::Max(FMath::CeilToInt(DrawData.DisplaySize.X), 1),
			FMath::Max(FMath::CeilToInt(DrawData.DisplaySize.Y), 1));
	}

private:
	FImGuiDrawData DrawData;
};
#endif

class FImGuiInputProcessor : public IInputProcessor
{
public:
	explicit FImGuiInputProcessor(SImGuiOverlay* InOwner)
	{
		Owner = InOwner;

		FSlateApplication::Get().OnApplicationActivationStateChanged().AddRaw(this, &FImGuiInputProcessor::OnApplicationActivationChanged);
		FSlateApplication::Get().OnFocusChanging().AddRaw(this, &FImGuiInputProcessor::OnFocusChanging);

		LastFocusedWindow = FSlateApplication::Get().GetActiveTopLevelRegularWindow();
	}

	virtual ~FImGuiInputProcessor() override
	{
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().OnApplicationActivationStateChanged().RemoveAll(this);
			FSlateApplication::Get().OnFocusChanging().RemoveAll(this);
		}
	}

	void OnApplicationActivationChanged(bool bIsActive) const
	{
		ImGui::FScopedContext ScopedContext(Owner->GetContext());

		ImGuiIO& IO = ImGui::GetIO();

		IO.AddFocusEvent(bIsActive);
	}

	void OnFocusChanging(const FFocusEvent& Event, const FWeakWidgetPath& OldWidgetPath, const TSharedPtr<SWidget>& OldWidget, const FWidgetPath& NewWidgetPath, const TSharedPtr<SWidget>& NewWidget)
	{
		if (NewWidgetPath.IsValid())
		{
			LastFocusedWindow = NewWidgetPath.GetDeepestWindow();
		}
		else
		{
			LastFocusedWindow.Reset();
		}
	}

	virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> SlateCursor) override
	{
		ImGui::FScopedContext ScopedContext(Owner->GetContext());

		ImGuiIO& IO = ImGui::GetIO();

		const bool bHasGamepad = (IO.BackendFlags & ImGuiBackendFlags_HasGamepad);
		if (bHasGamepad != SlateApp.IsGamepadAttached())
		{
			IO.BackendFlags ^= ImGuiBackendFlags_HasGamepad;
		}

		if (IO.WantSetMousePos)
		{
			FVector2f Position = IO.MousePos;
			if (!(IO.ConfigFlags & ImGuiConfigFlags_ViewportsEnable))
			{
				Position += Owner->GetTickSpaceGeometry().AbsolutePosition;
			}

			SlateCursor->SetPosition(Position.X, Position.Y);
		}

		if (IO.WantTextInput && !Owner->HasKeyboardFocus())
		{
			SlateApp.SetKeyboardFocus(Owner->AsShared());
		}
	}

	virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& Event) override
	{
		ImGui::FScopedContext ScopedContext(Owner->GetContext());

		if (!ShouldHandleEvent(SlateApp, Event))
		{
			return false;
		}

		ImGuiIO& IO = ImGui::GetIO();

		IO.AddKeyEvent(ImGui::ConvertKey(Event.GetKey()), true);

		const FModifierKeysState& ModifierKeys = Event.GetModifierKeys();
		IO.AddKeyEvent(ImGuiMod_Ctrl, ModifierKeys.IsControlDown());
		IO.AddKeyEvent(ImGuiMod_Shift, ModifierKeys.IsShiftDown());
		IO.AddKeyEvent(ImGuiMod_Alt, ModifierKeys.IsAltDown());
		IO.AddKeyEvent(ImGuiMod_Super, ModifierKeys.IsCommandDown());

		return IO.WantCaptureKeyboard;
	}

	virtual bool HandleKeyUpEvent(FSlateApplication& SlateApp, const FKeyEvent& Event) override
	{
		ImGui::FScopedContext ScopedContext(Owner->GetContext());

		if (!ShouldHandleEvent(SlateApp, Event))
		{
			return false;
		}

		ImGuiIO& IO = ImGui::GetIO();

		IO.AddKeyEvent(ImGui::ConvertKey(Event.GetKey()), false);

		const FModifierKeysState& ModifierKeys = Event.GetModifierKeys();
		IO.AddKeyEvent(ImGuiMod_Ctrl, ModifierKeys.IsControlDown());
		IO.AddKeyEvent(ImGuiMod_Shift, ModifierKeys.IsShiftDown());
		IO.AddKeyEvent(ImGuiMod_Alt, ModifierKeys.IsAltDown());
		IO.AddKeyEvent(ImGuiMod_Super, ModifierKeys.IsCommandDown());

		return IO.WantCaptureKeyboard;
	}

	virtual bool HandleAnalogInputEvent(FSlateApplication& SlateApp, const FAnalogInputEvent& Event) override
	{
		ImGui::FScopedContext ScopedContext(Owner->GetContext());

		if (!ShouldHandleEvent(SlateApp, Event))
		{
			return false;
		}

		ImGuiIO& IO = ImGui::GetIO();

		const float Value = Event.GetAnalogValue();
		IO.AddKeyAnalogEvent(ImGui::ConvertKey(Event.GetKey()), FMath::Abs(Value) > 0.1f, Value);

		return IO.WantCaptureKeyboard;
	}

	virtual bool HandleMouseMoveEvent(FSlateApplication& SlateApp, const FPointerEvent& Event) override
	{
		ImGui::FScopedContext ScopedContext(Owner->GetContext());

		if (!ShouldHandleEvent(SlateApp, Event))
		{
			return false;
		}

		ImGuiIO& IO = ImGui::GetIO();

		const TSharedPtr<FSlateUser> SlateUser = SlateApp.GetUser(Event.GetUserIndex());
		if (SlateUser.IsValid())
		{
			const FImGuiViewportData* TargetViewport = nullptr;

			if (!SlateUser->HasCapture(Event.GetPointerIndex()))
			{
				const FWeakWidgetPath LastWidgetsUnderPointer = SlateUser->GetLastWidgetsUnderPointer(Event.GetPointerIndex());
				TargetViewport = FindViewportForWindow(LastWidgetsUnderPointer.Window.Pin());
			}

			if (!TargetViewport && !ImGui::IsMouseDragging(0))
			{
				IO.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
				return false;
			}
		}

		FVector2f Position = Event.GetScreenSpacePosition();
		if (!(IO.ConfigFlags & ImGuiConfigFlags_ViewportsEnable))
		{
			Position -= Owner->GetTickSpaceGeometry().AbsolutePosition;
		}

		IO.AddMousePosEvent(Position.X, Position.Y);

		return IO.WantCaptureMouse;
	}

	virtual bool HandleMouseButtonDownEvent(FSlateApplication& SlateApp, const FPointerEvent& Event) override
	{
		ImGui::FScopedContext ScopedContext(Owner->GetContext());

		if (!ShouldHandleEvent(SlateApp, Event))
		{
			return false;
		}

		ImGuiIO& IO = ImGui::GetIO();

		const FKey Button = Event.GetEffectingButton();
		if (Button == EKeys::LeftMouseButton)
		{
			IO.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
		}
		else if (Button == EKeys::RightMouseButton)
		{
			IO.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
		}
		else if (Button == EKeys::MiddleMouseButton)
		{
			IO.AddMouseButtonEvent(ImGuiMouseButton_Middle, true);
		}

		return IO.WantCaptureMouse;
	}

	virtual bool HandleMouseButtonUpEvent(FSlateApplication& SlateApp, const FPointerEvent& Event) override
	{
		ImGui::FScopedContext ScopedContext(Owner->GetContext());

		if (!ShouldHandleEvent(SlateApp, Event))
		{
			return false;
		}

		ImGuiIO& IO = ImGui::GetIO();

		const FKey Button = Event.GetEffectingButton();
		if (Button == EKeys::LeftMouseButton)
		{
			IO.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
		}
		else if (Button == EKeys::RightMouseButton)
		{
			IO.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
		}
		else if (Button == EKeys::MiddleMouseButton)
		{
			IO.AddMouseButtonEvent(ImGuiMouseButton_Middle, false);
		}

		return false;
	}

	virtual bool HandleMouseButtonDoubleClickEvent(FSlateApplication& SlateApp, const FPointerEvent& Event) override
	{
		return HandleMouseButtonDownEvent(SlateApp, Event);
	}

	virtual bool HandleMouseWheelOrGestureEvent(FSlateApplication& SlateApp, const FPointerEvent& Event, const FPointerEvent* GestureEvent) override
	{
		ImGui::FScopedContext ScopedContext(Owner->GetContext());

		if (!ShouldHandleEvent(SlateApp, Event))
		{
			return false;
		}

		ImGuiIO& IO = ImGui::GetIO();

		IO.AddMouseWheelEvent(0.0f, Event.GetWheelDelta());

		return IO.WantCaptureMouse;
	}

	bool ShouldHandleEvent(FSlateApplication& SlateApp, const FInputEvent& Event) const
	{
#if WITH_EDITORONLY_DATA
		if (GIntraFrameDebuggingGameThread)
		{
			return false;
		}
#endif

		if (Event.IsKeyEvent())
		{
			const FImGuiViewportData* FocusedViewport = FindViewportForWindow(LastFocusedWindow.Pin());
			return FocusedViewport != nullptr;
		}

		return true;
	}

	static FImGuiViewportData* FindViewportForWindow(const TSharedPtr<SWindow>& Window)
	{
		if (!Window.IsValid())
		{
			return nullptr;
		}

		for (ImGuiViewport* Viewport : ImGui::GetPlatformIO().Viewports)
		{
			FImGuiViewportData* ViewportData = FImGuiViewportData::GetOrCreate(Viewport);
			if (ViewportData->Window == Window)
			{
				return ViewportData;
			}
		}

		return nullptr;
	}

private:
	SImGuiOverlay* Owner = nullptr;
	TWeakPtr<SWindow> LastFocusedWindow;
};

void SImGuiOverlay::Construct(const FArguments& Args)
{
	SetVisibility(EVisibility::HitTestInvisible);
	ForceVolatile(true);

	Context = Args._Context.IsValid() ? Args._Context : FImGuiContext::Create();
	if (Args._HandleInput)
	{
		InputProcessor = MakeShared<FImGuiInputProcessor>(this);
		FSlateApplication::Get().RegisterInputPreProcessor(InputProcessor.ToSharedRef(), 0);
	}

#if WITH_ENGINE && !UE_SERVER
	SourceWidget = SNew(SImGuiSourceWidget);
	SourceWidgetRenderer = MakeUnique<FWidgetRenderer>(false, true);
	SourceWidgetRenderer->SetApplyColorDeficiencyCorrection(false);
	SourceWidgetRenderer->SetIsPrepassNeeded(false);
	SourceWidgetRenderer->SetClearHitTestGrid(false);
#endif
}

SImGuiOverlay::~SImGuiOverlay()
{
	if (FSlateApplication::IsInitialized() && InputProcessor.IsValid())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(InputProcessor);
	}
}

int32 SImGuiOverlay::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	if (!DrawData.bValid)
	{
		PresentDrawer.Reset();
		return LayerId;
	}

#if WITH_ENGINE && !UE_SERVER
	if (SourceRenderTarget.IsValid() && SourceRenderTargetSize.X > 0 && SourceRenderTargetSize.Y > 0)
	{
		const FVector2d AbsolutePosition = AllottedGeometry.GetAccumulatedRenderTransform().GetTranslation();
		const FIntPoint OutputMin(FMath::FloorToInt(AbsolutePosition.X), FMath::FloorToInt(AbsolutePosition.Y));
		const FIntRect OutputRect(OutputMin, OutputMin + SourceRenderTargetSize);
		const FImGuiBloomSettings BloomSettings = Context.IsValid()
			? FImGuiBloomSettings{ Context->GetBloomIntensity(), Context->GetBloomThreshold() }
			: FImGuiBloomSettings{};

		PresentDrawer =
			MakeShared<FImGuiPresentDrawer, ESPMode::ThreadSafe>(SourceRenderTarget->TextureReference.TextureReferenceRHI, OutputRect, BloomSettings);
		FSlateDrawElement::MakeCustom(OutDrawElements, LayerId, PresentDrawer);
	}
	else
	{
		PresentDrawer.Reset();
	}
#endif

	return LayerId;
}

FVector2D SImGuiOverlay::ComputeDesiredSize(float LayoutScaleMultiplier) const
{
	return FVector2D::ZeroVector;
}

bool SImGuiOverlay::SupportsKeyboardFocus() const
{
	return true;
}

FReply SImGuiOverlay::OnKeyChar(const FGeometry& MyGeometry, const FCharacterEvent& Event)
{
	ImGui::FScopedContext ScopedContext(Context);

	ImGuiIO& IO = ImGui::GetIO();

	IO.AddInputCharacter(CharCast<ANSICHAR>(Event.GetCharacter()));

	return IO.WantTextInput ? FReply::Handled() : FReply::Unhandled();
}

TSharedPtr<FImGuiContext> SImGuiOverlay::GetContext() const
{
	return Context;
}

void SImGuiOverlay::SetDrawData(const ImDrawData* InDrawData)
{
	DrawData = FImGuiDrawData(InDrawData);
	UpdateSourceCapture();
}

void SImGuiOverlay::UpdateSourceCapture()
{
#if WITH_ENGINE && !UE_SERVER
	if (!Context.IsValid() || !SourceWidget.IsValid() || !SourceWidgetRenderer.IsValid())
	{
		return;
	}

	if (!DrawData.bValid)
	{
		SourceRenderTargetSize = FIntPoint::ZeroValue;
		return;
	}

	const FIntPoint TargetSize(
		FMath::Max(FMath::CeilToInt(DrawData.DisplaySize.X), 1),
		FMath::Max(FMath::CeilToInt(DrawData.DisplaySize.Y), 1));

	if (!SourceRenderTarget.IsValid() || SourceRenderTargetSize != TargetSize)
	{
		SourceRenderTarget.Reset(FWidgetRenderer::CreateTargetFor(FVector2D(TargetSize), TF_Bilinear, false));

		UTextureRenderTarget2D* RenderTarget = SourceRenderTarget.Get();
		if (IsValid(RenderTarget))
		{
			RenderTarget->ClearColor = FLinearColor::Transparent;
			RenderTarget->TargetGamma = 1.0f;
			RenderTarget->Filter = TF_Bilinear;
			RenderTarget->AddressX = TA_Clamp;
			RenderTarget->AddressY = TA_Clamp;
			RenderTarget->bAutoGenerateMips = false;
			RenderTarget->UpdateResourceImmediate(true);
		}
	}

	SourceRenderTargetSize = TargetSize;
	if (IsValid(SourceRenderTarget.Get()))
	{
		SourceWidget->SetDrawData(DrawData);
		SourceWidgetRenderer->DrawWidget(SourceRenderTarget.Get(), SourceWidget.ToSharedRef(), FVector2D(TargetSize), 0.0f, true);
	}
#endif
}
