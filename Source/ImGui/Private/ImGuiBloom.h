#pragma once

#if WITH_ENGINE && !UE_SERVER

#include <Math/IntRect.h>
#include <Rendering/RenderingCommon.h>
#include <RHIResources.h>
#include <Templates/SharedPointer.h>

struct FImGuiBloomSettings
{
	float Intensity = 0.0f;
	float Threshold = 0.6f;
};

struct FImGuiDrawData;

class FImGuiPresentDrawer : public ICustomSlateElement
{
public:
	FImGuiPresentDrawer(TSharedPtr<const FImGuiDrawData, ESPMode::ThreadSafe> InDrawData, const FIntRect& InOutputRect, const FImGuiBloomSettings& InSettings);

	virtual void Draw_RenderThread(FRDGBuilder& GraphBuilder, const FDrawPassInputs& Inputs) override;

private:
	TSharedPtr<const FImGuiDrawData, ESPMode::ThreadSafe> DrawData;
	FIntRect OutputRect;
	FImGuiBloomSettings Settings;
};

#endif
