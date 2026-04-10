#include "SImGuiOverlay.h"

#include <Framework/Application/SlateApplication.h>

#if WITH_ENGINE
#include <Engine/Texture.h>
#endif

#include "ImGuiBloom.h"
#include "ImGuiContext.h"

FImGuiDrawList::FImGuiDrawList(ImDrawList* Source)
{
	if (!Source)
	{
		return;
	}

	VtxBuffer.swap(Source->VtxBuffer);
	IdxBuffer.swap(Source->IdxBuffer);
#if WITH_ENGINE
	CmdBuffer.Reserve(Source->CmdBuffer.Size);
	for (const ImDrawCmd& SourceCmd : Source->CmdBuffer)
	{
		if (SourceCmd.UserCallback && SourceCmd.UserCallback != ImDrawCallback_ResetRenderState)
		{
			continue;
		}

		FImGuiDrawCmd& Cmd = CmdBuffer.Emplace_GetRef();
		Cmd.ClipRect = FVector4f(SourceCmd.ClipRect.x, SourceCmd.ClipRect.y, SourceCmd.ClipRect.z, SourceCmd.ClipRect.w);
		Cmd.ElemCount = SourceCmd.ElemCount;
		Cmd.IdxOffset = SourceCmd.IdxOffset;
		Cmd.VtxOffset = SourceCmd.VtxOffset;
		Cmd.bResetRenderState = SourceCmd.UserCallback == ImDrawCallback_ResetRenderState;

		if (!Cmd.bResetRenderState)
		{
			if (UTexture* Texture = SourceCmd.GetTexID())
			{
				if (const FTextureResource* TextureResource = Texture->GetResource())
				{
					Cmd.Texture = TextureResource->TextureRHI;
				}
			}
		}
	}
	Source->CmdBuffer.resize(0);
#else
	CmdBuffer.swap(Source->CmdBuffer);
#endif
	Flags = Source->Flags;
}

FImGuiDrawData::FImGuiDrawData(const ImDrawData* Source)
{
	if (!Source)
	{
		return;
	}

	bValid = Source->Valid;

	TotalIdxCount = Source->TotalIdxCount;
	TotalVtxCount = Source->TotalVtxCount;

	ImGui::CopyArray(Source->CmdLists, DrawLists);

	DisplayPos = Source->DisplayPos;
	DisplaySize = Source->DisplaySize;
	FrameBufferScale = Source->FramebufferScale;
}

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
	if (!DrawData.IsValid() || !DrawData->bValid)
	{
		PresentDrawer.Reset();
		return LayerId;
	}

#if WITH_ENGINE && !UE_SERVER
	const FIntPoint SourceExtent(
		FMath::Max(FMath::CeilToInt(DrawData->DisplaySize.X), 1),
		FMath::Max(FMath::CeilToInt(DrawData->DisplaySize.Y), 1));
	if (SourceExtent.X > 0 && SourceExtent.Y > 0)
	{
		const FVector2d AbsolutePosition = AllottedGeometry.GetAccumulatedRenderTransform().GetTranslation();
		const FIntPoint OutputMin(FMath::FloorToInt(AbsolutePosition.X), FMath::FloorToInt(AbsolutePosition.Y));
		const FIntRect OutputRect(OutputMin, OutputMin + SourceExtent);
		const FImGuiBloomSettings BloomSettings = Context.IsValid()
			? FImGuiBloomSettings{ Context->GetBloomIntensity(), Context->GetBloomThreshold() }
			: FImGuiBloomSettings{};

		PresentDrawer =
			MakeShared<FImGuiPresentDrawer, ESPMode::ThreadSafe>(DrawData, OutputRect, BloomSettings);
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
	DrawData = MakeShared<FImGuiDrawData, ESPMode::ThreadSafe>(InDrawData);
}
