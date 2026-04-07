#pragma once

#if WITH_ENGINE && !UE_SERVER

#include <Math/IntRect.h>
#include <Rendering/RenderingCommon.h>
#include <RHIResources.h>

struct FImGuiBloomSettings
{
	float Intensity = 0.0f;
	float Threshold = 0.6f;
};

class FImGuiPresentDrawer : public ICustomSlateElement
{
public:
	FImGuiPresentDrawer(const FTextureReferenceRHIRef& InSourceTexture, const FIntRect& InOutputRect, const FImGuiBloomSettings& InSettings);

	virtual void Draw_RenderThread(FRDGBuilder& GraphBuilder, const FDrawPassInputs& Inputs) override;

private:
	FTextureReferenceRHIRef SourceTextureReference;
	FIntRect OutputRect;
	FImGuiBloomSettings Settings;
};

#endif
