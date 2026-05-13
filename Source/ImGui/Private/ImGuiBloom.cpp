#include "ImGuiBloom.h"

#if WITH_ENGINE && !UE_SERVER

#include "SImGuiOverlay.h"

#include <GlobalShader.h>
#include <HAL/IConsoleManager.h>
#include <Math/UnrealMathUtility.h>
#include <RenderGraphBuilder.h>
#include <RenderGraphUtils.h>
#include <RenderResource.h>
#include <RHICommandList.h>
#include <RHIStaticStates.h>
#include <ScreenPass.h>
#include <ShaderParameterStruct.h>

namespace
{
	constexpr int32 MaxBloomMipCount = 6;
#if !UE_BUILD_SHIPPING
	TAutoConsoleVariable<int32> CVarImGuiBloomValidate(
		TEXT("imgui.BloomValidate"),
		0,
		TEXT("Validate ImGui bloom by checking for unexpected energy on the final bloom texture border. Disabled by default because readback is expensive."));
#endif

	int32 ComputeBloomMipCount(const FIntPoint& Extent)
	{
		const int32 MaxDimension = FMath::Max(Extent.X, Extent.Y);
		if (MaxDimension <= 1)
		{
			return 1;
		}

		return FMath::Clamp(FMath::FloorLog2(MaxDimension), 1, MaxBloomMipCount);
	}

	FIntPoint GetMipExtent(const FIntPoint& BaseExtent, const int32 MipLevel)
	{
		return FIntPoint(
			FMath::Max(BaseExtent.X >> MipLevel, 1),
			FMath::Max(BaseExtent.Y >> MipLevel, 1)
		);
	}

	FScreenPassTextureSlice MakeTextureSlice(FRDGBuilder& GraphBuilder, FRDGTextureRef Texture, const FIntPoint& Extent)
	{
		return FScreenPassTextureSlice(GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(Texture)), FIntRect(FIntPoint::ZeroValue, Extent));
	}

	FScreenPassTextureSlice MakeTextureMipSlice(FRDGBuilder& GraphBuilder, FRDGTextureRef Texture, const int32 MipLevel)
	{
		return FScreenPassTextureSlice(
			GraphBuilder.CreateSRV(FRDGTextureSRVDesc::CreateForMipLevel(Texture, MipLevel)),
			FIntRect(FIntPoint::ZeroValue, GetMipExtent(Texture->Desc.Extent, MipLevel))
		);
	}

	BEGIN_SHADER_PARAMETER_STRUCT(FImGuiRasterPassParameters, )
		RDG_BUFFER_ACCESS(VertexBuffer, ERHIAccess::VertexOrIndexBuffer)
		RDG_BUFFER_ACCESS(IndexBuffer, ERHIAccess::VertexOrIndexBuffer)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	class FImGuiRasterVS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FImGuiRasterVS);
		SHADER_USE_PARAMETER_STRUCT(FImGuiRasterVS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(FVector2f, DisplayPos)
			SHADER_PARAMETER(FVector2f, DisplaySize)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	class FImGuiRasterPS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FImGuiRasterPS);
		SHADER_USE_PARAMETER_STRUCT(FImGuiRasterPS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_TEXTURE(Texture2D, InputTexture)
			SHADER_PARAMETER_SAMPLER(SamplerState, InputSampler)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	class FImGuiVertexDeclaration : public FRenderResource
	{
	public:
		FVertexDeclarationRHIRef VertexDeclarationRHI;

		virtual void InitRHI(FRHICommandListBase& RHICmdList) override
		{
			FVertexDeclarationElementList Elements;
			const uint16 Stride = sizeof(ImDrawVert);
			Elements.Add(FVertexElement(0, STRUCT_OFFSET(ImDrawVert, pos), VET_Float2, 0, Stride));
			Elements.Add(FVertexElement(0, STRUCT_OFFSET(ImDrawVert, uv), VET_Float2, 1, Stride));
			Elements.Add(FVertexElement(0, STRUCT_OFFSET(ImDrawVert, col), VET_Color, 2, Stride));
			Elements.Add(FVertexElement(0, STRUCT_OFFSET(ImDrawVert, hdr), VET_Float1, 3, Stride));
			VertexDeclarationRHI = RHICreateVertexDeclaration(Elements);
		}

		virtual void ReleaseRHI() override
		{
			VertexDeclarationRHI.SafeRelease();
		}
	};

	TGlobalResource<FImGuiVertexDeclaration> GImGuiVertexDeclaration;

	class FImGuiPresentSourcePS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FImGuiPresentSourcePS);
		SHADER_USE_PARAMETER_STRUCT(FImGuiPresentSourcePS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_STRUCT(FScreenPassTextureInput, Source)
			RENDER_TARGET_BINDING_SLOTS()
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	class FImGuiBloomThresholdPS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FImGuiBloomThresholdPS);
		SHADER_USE_PARAMETER_STRUCT(FImGuiBloomThresholdPS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_STRUCT(FScreenPassTextureInput, Source)
			SHADER_PARAMETER(float, Threshold)
			SHADER_PARAMETER(float, Intensity)
			RENDER_TARGET_BINDING_SLOTS()
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	class FImGuiBloomBlurPS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FImGuiBloomBlurPS);
		SHADER_USE_PARAMETER_STRUCT(FImGuiBloomBlurPS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_STRUCT(FScreenPassTextureSliceInput, Source)
			SHADER_PARAMETER(FVector2f, Direction)
			RENDER_TARGET_BINDING_SLOTS()
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	class FImGuiBloomDownsamplePS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FImGuiBloomDownsamplePS);
		SHADER_USE_PARAMETER_STRUCT(FImGuiBloomDownsamplePS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_STRUCT(FScreenPassTextureSliceInput, Source)
			RENDER_TARGET_BINDING_SLOTS()
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	class FImGuiBloomUpsamplePS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FImGuiBloomUpsamplePS);
		SHADER_USE_PARAMETER_STRUCT(FImGuiBloomUpsamplePS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_STRUCT(FScreenPassTextureSliceInput, HighMip)
			SHADER_PARAMETER_STRUCT(FScreenPassTextureSliceInput, LowMip)
			RENDER_TARGET_BINDING_SLOTS()
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	class FImGuiBloomCompositePS : public FGlobalShader
	{
	public:
		DECLARE_GLOBAL_SHADER(FImGuiBloomCompositePS);
		SHADER_USE_PARAMETER_STRUCT(FImGuiBloomCompositePS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_STRUCT(FScreenPassTextureInput, Bloom)
			RENDER_TARGET_BINDING_SLOTS()
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};

	IMPLEMENT_GLOBAL_SHADER(FImGuiRasterVS, "/Plugin/ImGui/Private/ImGuiBloom.usf", "RasterVS", SF_Vertex);
	IMPLEMENT_GLOBAL_SHADER(FImGuiRasterPS, "/Plugin/ImGui/Private/ImGuiBloom.usf", "RasterPS", SF_Pixel);
	IMPLEMENT_GLOBAL_SHADER(FImGuiPresentSourcePS, "/Plugin/ImGui/Private/ImGuiBloom.usf", "SourcePS", SF_Pixel);
	IMPLEMENT_GLOBAL_SHADER(FImGuiBloomThresholdPS, "/Plugin/ImGui/Private/ImGuiBloom.usf", "ThresholdPS", SF_Pixel);
	IMPLEMENT_GLOBAL_SHADER(FImGuiBloomBlurPS, "/Plugin/ImGui/Private/ImGuiBloom.usf", "BlurPS", SF_Pixel);
	IMPLEMENT_GLOBAL_SHADER(FImGuiBloomDownsamplePS, "/Plugin/ImGui/Private/ImGuiBloom.usf", "DownsamplePS", SF_Pixel);
	IMPLEMENT_GLOBAL_SHADER(FImGuiBloomUpsamplePS, "/Plugin/ImGui/Private/ImGuiBloom.usf", "UpsamplePS", SF_Pixel);
	IMPLEMENT_GLOBAL_SHADER(FImGuiBloomCompositePS, "/Plugin/ImGui/Private/ImGuiBloom.usf", "CompositePS", SF_Pixel);

	struct FImGuiRasterBuffers
	{
		FRDGBufferRef VertexBuffer = nullptr;
		FRDGBufferRef IndexBuffer = nullptr;
		uint32 VertexCount = 0;
	};

	FImGuiRasterBuffers CreateRasterBuffers(FRDGBuilder& GraphBuilder, const FImGuiDrawList& DrawList)
	{
		FImGuiRasterBuffers Buffers;
		Buffers.VertexCount = DrawList.VtxBuffer.Size;
		if (Buffers.VertexCount == 0 || DrawList.IdxBuffer.Size <= 0)
		{
			return Buffers;
		}

		FRDGBufferDesc VertexBufferDesc;
		VertexBufferDesc.Usage = EBufferUsageFlags::Static | EBufferUsageFlags::VertexBuffer;
		VertexBufferDesc.BytesPerElement = sizeof(ImDrawVert);
		VertexBufferDesc.NumElements = DrawList.VtxBuffer.Size;
		Buffers.VertexBuffer = CreateVertexBuffer(
			GraphBuilder,
			TEXT("ImGuiVertexBuffer"),
			VertexBufferDesc,
			DrawList.VtxBuffer.Data,
			DrawList.VtxBuffer.Size * sizeof(ImDrawVert));

		FRDGBufferDesc IndexBufferDesc;
		IndexBufferDesc.Usage = EBufferUsageFlags::Static | EBufferUsageFlags::IndexBuffer;
		IndexBufferDesc.BytesPerElement = sizeof(ImDrawIdx);
		IndexBufferDesc.NumElements = DrawList.IdxBuffer.Size;
		Buffers.IndexBuffer = GraphBuilder.CreateBuffer(IndexBufferDesc, TEXT("ImGuiIndexBuffer"));
		GraphBuilder.QueueBufferUpload(Buffers.IndexBuffer, DrawList.IdxBuffer.Data, DrawList.IdxBuffer.Size * sizeof(ImDrawIdx));
		return Buffers;
	}

	FIntRect GetScissorRect(const FVector4f& ClipRect, const FVector2f& DisplayPos, const FIntPoint& SourceExtent)
	{
		FIntRect Rect(
			FIntPoint(
				FMath::Clamp(FMath::FloorToInt(ClipRect.X - DisplayPos.X), 0, SourceExtent.X),
				FMath::Clamp(FMath::FloorToInt(ClipRect.Y - DisplayPos.Y), 0, SourceExtent.Y)),
			FIntPoint(
				FMath::Clamp(FMath::CeilToInt(ClipRect.Z - DisplayPos.X), 0, SourceExtent.X),
				FMath::Clamp(FMath::CeilToInt(ClipRect.W - DisplayPos.Y), 0, SourceExtent.Y)));
		return Rect;
	}

	FRDGTextureRef AddRasterSourcePass(
		FRDGBuilder& GraphBuilder,
		const FScreenPassViewInfo& ViewInfo,
		TSharedPtr<const FImGuiDrawData, ESPMode::ThreadSafe> DrawData)
	{
		const FIntPoint SourceExtent(
			FMath::Max(FMath::CeilToInt(DrawData->DisplaySize.X), 1),
			FMath::Max(FMath::CeilToInt(DrawData->DisplaySize.Y), 1));

		FRDGTextureRef SourceTexture = GraphBuilder.CreateTexture(
			FRDGTextureDesc::Create2D(SourceExtent, PF_FloatRGBA, FClearValueBinding::Transparent, TexCreate_RenderTargetable | TexCreate_ShaderResource),
			TEXT("ImGuiSource")
		);

		AddClearRenderTargetPass(GraphBuilder, SourceTexture, FLinearColor::Transparent);

		for (int32 DrawListIndex = 0; DrawListIndex < DrawData->DrawLists.Num(); ++DrawListIndex)
		{
			const FImGuiDrawList& DrawList = DrawData->DrawLists[DrawListIndex];
			const FImGuiRasterBuffers RasterBuffers = CreateRasterBuffers(GraphBuilder, DrawList);
			if (!RasterBuffers.VertexBuffer || !RasterBuffers.IndexBuffer)
			{
				continue;
			}

			FImGuiRasterPassParameters* PassParameters = GraphBuilder.AllocParameters<FImGuiRasterPassParameters>();
			PassParameters->VertexBuffer = RasterBuffers.VertexBuffer;
			PassParameters->IndexBuffer = RasterBuffers.IndexBuffer;
			PassParameters->RenderTargets[0] = FRenderTargetBinding(SourceTexture, ERenderTargetLoadAction::ELoad);

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("ImGuiRaster"),
				PassParameters,
				ERDGPassFlags::Raster,
				[DrawData, PassParameters, DrawListIndex, SourceExtent, FeatureLevel = ViewInfo.FeatureLevel, VertexCount = RasterBuffers.VertexCount](FRHICommandList& RHICmdList)
				{
					TShaderMapRef<FImGuiRasterVS> VertexShader(GetGlobalShaderMap(FeatureLevel));
					TShaderMapRef<FImGuiRasterPS> PixelShader(GetGlobalShaderMap(FeatureLevel));
					FRHISamplerState* SamplerState = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
					const FImGuiDrawList& DrawListRef = DrawData->DrawLists[DrawListIndex];

					const auto ApplyRasterState = [&]()
					{
						FGraphicsPipelineStateInitializer GraphicsPSOInit;
						RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
						GraphicsPSOInit.BlendState = TStaticBlendState<
							CW_RGBA,
							BO_Add, BF_SourceAlpha, BF_InverseSourceAlpha,
							BO_Add, BF_One, BF_InverseSourceAlpha>::GetRHI();
						GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
						GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
						GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GImGuiVertexDeclaration.VertexDeclarationRHI;
						GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
						GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
						GraphicsPSOInit.PrimitiveType = PT_TriangleList;
						SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

						FImGuiRasterVS::FParameters VSParameters;
						VSParameters.DisplayPos = DrawData->DisplayPos;
						VSParameters.DisplaySize = DrawData->DisplaySize;
						SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), VSParameters);
					};

					RHICmdList.SetViewport(0.0f, 0.0f, 0.0f, (float)SourceExtent.X, (float)SourceExtent.Y, 1.0f);
					ApplyRasterState();
					RHICmdList.SetStreamSource(0, PassParameters->VertexBuffer->GetRHI(), 0);

					for (const FImGuiDrawCmd& DrawCmd : DrawListRef.CmdBuffer)
					{
						if (DrawCmd.bResetRenderState)
						{
							ApplyRasterState();
							continue;
						}

						if (!DrawCmd.Texture.IsValid()
							|| DrawCmd.ElemCount == 0
							|| DrawCmd.VtxOffset >= (uint32)DrawListRef.VtxBuffer.Size
							|| DrawCmd.IdxOffset + DrawCmd.ElemCount > (uint32)DrawListRef.IdxBuffer.Size)
						{
							continue;
						}

						const FIntRect ScissorRect = GetScissorRect(DrawCmd.ClipRect, DrawData->DisplayPos, SourceExtent);
						if (ScissorRect.Area() <= 0)
						{
							continue;
						}

						FImGuiRasterPS::FParameters PSParameters;
						PSParameters.InputTexture = DrawCmd.Texture;
						PSParameters.InputSampler = SamplerState;
						SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), PSParameters);

						RHICmdList.SetScissorRect(true, ScissorRect.Min.X, ScissorRect.Min.Y, ScissorRect.Max.X, ScissorRect.Max.Y);
						RHICmdList.DrawIndexedPrimitive(
							PassParameters->IndexBuffer->GetRHI(),
							DrawCmd.VtxOffset,
							0,
							VertexCount - DrawCmd.VtxOffset,
							DrawCmd.IdxOffset,
							DrawCmd.ElemCount / 3,
							1);
					}

					RHICmdList.SetScissorRect(false, 0, 0, 0, 0);
				});
		}

		return SourceTexture;
	}

	void AddSourcePresentPass(
		FRDGBuilder& GraphBuilder,
		const FScreenPassViewInfo& ViewInfo,
		FRDGTextureRef SourceTexture,
		FRDGTextureRef OutputTexture,
		const FIntRect& InputRect,
		const FIntRect& OutputRect)
	{
		const FScreenPassTexture SourcePassTexture(SourceTexture, InputRect);
		FImGuiPresentSourcePS::FParameters* PassParameters = GraphBuilder.AllocParameters<FImGuiPresentSourcePS::FParameters>();
		PassParameters->Source = GetScreenPassTextureInput(SourcePassTexture, TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI());
		PassParameters->RenderTargets[0] = FRenderTargetBinding(OutputTexture, ERenderTargetLoadAction::ELoad);

		const FScreenPassTextureViewport InputViewport(SourcePassTexture);
		const FScreenPassTextureViewport OutputViewport(OutputTexture, OutputRect);

		TShaderMapRef<FScreenPassVS> VertexShader(GetGlobalShaderMap(ViewInfo.FeatureLevel));
		TShaderMapRef<FImGuiPresentSourcePS> PixelShader(GetGlobalShaderMap(ViewInfo.FeatureLevel));
		FRHIBlendState* AlphaBlend = TStaticBlendState<
			CW_RGBA,
			BO_Add, BF_SourceAlpha, BF_InverseSourceAlpha,
			BO_Add, BF_One, BF_InverseSourceAlpha>::GetRHI();

		AddDrawScreenPass(GraphBuilder, RDG_EVENT_NAME("ImGuiSourcePresent"), ViewInfo, OutputViewport, InputViewport, VertexShader, PixelShader, AlphaBlend, PassParameters);
	}

	void AddThresholdPass(
		FRDGBuilder& GraphBuilder,
		const FScreenPassViewInfo& ViewInfo,
		const FScreenPassTexture& SourceTexture,
		FRDGTextureRef OutputTexture,
		const FImGuiBloomSettings& Settings)
	{
		FImGuiBloomThresholdPS::FParameters* PassParameters = GraphBuilder.AllocParameters<FImGuiBloomThresholdPS::FParameters>();
		PassParameters->Source = GetScreenPassTextureInput(SourceTexture, TStaticSamplerState<SF_Bilinear, AM_Border, AM_Border, AM_Clamp>::GetRHI());
		PassParameters->Threshold = Settings.Threshold;
		PassParameters->Intensity = Settings.Intensity;
		PassParameters->RenderTargets[0] = FRenderTargetBinding(OutputTexture, ERenderTargetLoadAction::EClear, 0);

		const FScreenPassTextureViewport InputViewport(SourceTexture);
		const FScreenPassTextureViewport OutputViewport(OutputTexture, FIntRect(FIntPoint::ZeroValue, OutputTexture->Desc.Extent));

		TShaderMapRef<FImGuiBloomThresholdPS> PixelShader(GetGlobalShaderMap(ViewInfo.FeatureLevel));
		AddDrawScreenPass(GraphBuilder, RDG_EVENT_NAME("ImGuiBloomThreshold"), ViewInfo, OutputViewport, InputViewport, PixelShader, PassParameters);
	}

	FRDGTextureRef AddDownsamplePass(
		FRDGBuilder& GraphBuilder,
		const FScreenPassViewInfo& ViewInfo,
		const FScreenPassTextureSlice& SourceSlice,
		const FIntPoint& OutputExtent,
		const TCHAR* TextureName)
	{
		FRDGTextureRef OutputTexture = GraphBuilder.CreateTexture(
			FRDGTextureDesc::Create2D(OutputExtent, PF_FloatRGBA, FClearValueBinding::Black, TexCreate_RenderTargetable | TexCreate_ShaderResource),
			TextureName
		);

		FImGuiBloomDownsamplePS::FParameters* PassParameters = GraphBuilder.AllocParameters<FImGuiBloomDownsamplePS::FParameters>();
		PassParameters->Source = GetScreenPassTextureInput(SourceSlice, TStaticSamplerState<SF_Bilinear, AM_Border, AM_Border, AM_Clamp>::GetRHI());
		PassParameters->RenderTargets[0] = FRenderTargetBinding(OutputTexture, ERenderTargetLoadAction::EClear);

		const FScreenPassTextureViewport InputViewport(SourceSlice);
		const FScreenPassTextureViewport OutputViewport(OutputTexture, FIntRect(FIntPoint::ZeroValue, OutputExtent));

		TShaderMapRef<FImGuiBloomDownsamplePS> PixelShader(GetGlobalShaderMap(ViewInfo.FeatureLevel));
		AddDrawScreenPass(GraphBuilder, RDG_EVENT_NAME("ImGuiBloomDownsample"), ViewInfo, OutputViewport, InputViewport, PixelShader, PassParameters);

		return OutputTexture;
	}

	FRDGTextureRef AddBlurPass(
		FRDGBuilder& GraphBuilder,
		const FScreenPassViewInfo& ViewInfo,
		const FScreenPassTextureSlice& SourceSlice,
		const FVector2f Direction,
		const TCHAR* TextureName)
	{
		const FIntPoint Extent = SourceSlice.ViewRect.Size();
		FRDGTextureRef OutputTexture = GraphBuilder.CreateTexture(
			FRDGTextureDesc::Create2D(Extent, PF_FloatRGBA, FClearValueBinding::Black, TexCreate_RenderTargetable | TexCreate_ShaderResource),
			TextureName
		);

		FImGuiBloomBlurPS::FParameters* PassParameters = GraphBuilder.AllocParameters<FImGuiBloomBlurPS::FParameters>();
		PassParameters->Source = GetScreenPassTextureInput(SourceSlice, TStaticSamplerState<SF_Bilinear, AM_Border, AM_Border, AM_Clamp>::GetRHI());
		PassParameters->Direction = Direction;
		PassParameters->RenderTargets[0] = FRenderTargetBinding(OutputTexture, ERenderTargetLoadAction::EClear);

		const FScreenPassTextureViewport InputViewport(SourceSlice);
		const FScreenPassTextureViewport OutputViewport(OutputTexture, FIntRect(FIntPoint::ZeroValue, Extent));

		TShaderMapRef<FImGuiBloomBlurPS> PixelShader(GetGlobalShaderMap(ViewInfo.FeatureLevel));
		AddDrawScreenPass(GraphBuilder, RDG_EVENT_NAME("ImGuiBloomBlur"), ViewInfo, OutputViewport, InputViewport, PixelShader, PassParameters);

		return OutputTexture;
	}

	FRDGTextureRef AddUpsamplePass(
		FRDGBuilder& GraphBuilder,
		const FScreenPassViewInfo& ViewInfo,
		FRDGTextureRef HighMipTexture,
		FRDGTextureRef LowMipTexture,
		const TCHAR* TextureName)
	{
		const FIntPoint Extent = HighMipTexture->Desc.Extent;
		FRDGTextureRef OutputTexture = GraphBuilder.CreateTexture(
			FRDGTextureDesc::Create2D(Extent, PF_FloatRGBA, FClearValueBinding::Black, TexCreate_RenderTargetable | TexCreate_ShaderResource),
			TextureName
		);

		FImGuiBloomUpsamplePS::FParameters* PassParameters = GraphBuilder.AllocParameters<FImGuiBloomUpsamplePS::FParameters>();
		PassParameters->HighMip = GetScreenPassTextureInput(MakeTextureSlice(GraphBuilder, HighMipTexture, Extent), TStaticSamplerState<SF_Bilinear, AM_Border, AM_Border, AM_Clamp>::GetRHI());
		PassParameters->LowMip = GetScreenPassTextureInput(MakeTextureSlice(GraphBuilder, LowMipTexture, LowMipTexture->Desc.Extent), TStaticSamplerState<SF_Bilinear, AM_Border, AM_Border, AM_Clamp>::GetRHI());
		PassParameters->RenderTargets[0] = FRenderTargetBinding(OutputTexture, ERenderTargetLoadAction::EClear);

		const FScreenPassTextureViewport OutputViewport(OutputTexture, FIntRect(FIntPoint::ZeroValue, Extent));
		const FScreenPassTextureViewport InputViewport(HighMipTexture, FIntRect(FIntPoint::ZeroValue, Extent));

		TShaderMapRef<FImGuiBloomUpsamplePS> PixelShader(GetGlobalShaderMap(ViewInfo.FeatureLevel));
		AddDrawScreenPass(GraphBuilder, RDG_EVENT_NAME("ImGuiBloomUpsample"), ViewInfo, OutputViewport, InputViewport, PixelShader, PassParameters);

		return OutputTexture;
	}

#if !UE_BUILD_SHIPPING
	void AddBloomValidationPass(
		FRDGBuilder& GraphBuilder,
		FRDGTextureRef BloomTexture,
		const FIntRect& OutputRect,
		const FIntRect& SceneViewRect)
	{
		if (CVarImGuiBloomValidate.GetValueOnRenderThread() <= 0)
		{
			return;
		}

		if (OutputRect.Min.X <= SceneViewRect.Min.X
			|| OutputRect.Min.Y <= SceneViewRect.Min.Y
			|| OutputRect.Max.X >= SceneViewRect.Max.X
			|| OutputRect.Max.Y >= SceneViewRect.Max.Y)
		{
			return;
		}

		AddReadbackTexturePass(GraphBuilder, RDG_EVENT_NAME("ImGuiBloomValidate"), BloomTexture,
			[BloomTexture](FRHICommandListImmediate& RHICmdList)
		{
			TArray<FLinearColor> Pixels;
			RHICmdList.ReadSurfaceData(
				BloomTexture->GetRHI(),
				FIntRect(FIntPoint::ZeroValue, BloomTexture->Desc.Extent),
				Pixels,
				FReadSurfaceDataFlags(RCM_MinMax));

			const int32 Width = BloomTexture->Desc.Extent.X;
			const int32 Height = BloomTexture->Desc.Extent.Y;
			if (Pixels.Num() != Width * Height || Width < 3 || Height < 3)
			{
				return;
			}

			float MaxBorderBrightness = 0.0f;
			float AvgBorderBrightness = 0.0f;
			int32 BorderPixelCount = 0;

			auto AccumulateBrightness = [&Pixels, Width, &MaxBorderBrightness, &AvgBorderBrightness, &BorderPixelCount](const int32 X, const int32 Y)
			{
				const FLinearColor& Pixel = Pixels[Y * Width + X];
				const float Brightness = FMath::Max3(Pixel.R, Pixel.G, Pixel.B);
				MaxBorderBrightness = FMath::Max(MaxBorderBrightness, Brightness);
				AvgBorderBrightness += Brightness;
				++BorderPixelCount;
			};

			for (int32 X = 0; X < Width; ++X)
			{
				AccumulateBrightness(X, 0);
				AccumulateBrightness(X, Height - 1);
			}

			for (int32 Y = 1; Y < Height - 1; ++Y)
			{
				AccumulateBrightness(0, Y);
				AccumulateBrightness(Width - 1, Y);
			}

			if (BorderPixelCount > 0)
			{
				AvgBorderBrightness /= static_cast<float>(BorderPixelCount);
			}

			if (MaxBorderBrightness > 0.02f || AvgBorderBrightness > 0.005f)
			{
				UE_LOG(LogTemp, Warning, TEXT("ImGui bloom validation failed: border brightness max=%.4f avg=%.4f size=%dx%d"),
					MaxBorderBrightness,
					AvgBorderBrightness,
					Width,
					Height);
			}
			else
			{
				UE_LOG(LogTemp, Display, TEXT("ImGui bloom validation passed: border brightness max=%.4f avg=%.4f size=%dx%d"),
					MaxBorderBrightness,
					AvgBorderBrightness,
					Width,
					Height);
			}
		});
	}
#endif
}

FImGuiPresentDrawer::FImGuiPresentDrawer(TSharedPtr<const FImGuiDrawData, ESPMode::ThreadSafe> InDrawData, const FIntRect& InOutputRect, const FImGuiBloomSettings& InSettings, bool bInClipToSceneViewRect)
	: DrawData(MoveTemp(InDrawData))
	, OutputRect(InOutputRect)
	, Settings(InSettings)
	, bClipToSceneViewRect(bInClipToSceneViewRect)
{
}

void FImGuiPresentDrawer::Draw_RenderThread(FRDGBuilder& GraphBuilder, const FDrawPassInputs& Inputs)
{
	if (!DrawData.IsValid() || !DrawData->bValid)
	{
		return;
	}

	FIntRect ClampedOutputRect = OutputRect;
	ClampedOutputRect.Clip(FIntRect(FIntPoint::ZeroValue, Inputs.OutputTexture->Desc.Extent));
	if (bClipToSceneViewRect)
	{
		ClampedOutputRect.Clip(Inputs.SceneViewRect);
	}
	if (ClampedOutputRect.Area() <= 0)
	{
		return;
	}

	const FScreenPassViewInfo ViewInfo(GMaxRHIFeatureLevel);
	FRDGTextureRef SourceTexture = AddRasterSourcePass(GraphBuilder, ViewInfo, DrawData);
	if (!SourceTexture)
	{
		return;
	}

	const FIntPoint SourceExtent = SourceTexture->Desc.Extent;
	const FIntPoint SourceMin = ClampedOutputRect.Min - OutputRect.Min;
	FIntRect SourceRect(SourceMin, SourceMin + ClampedOutputRect.Size());
	SourceRect.Clip(FIntRect(FIntPoint::ZeroValue, SourceExtent));
	if (SourceRect.Area() <= 0)
	{
		return;
	}

	AddSourcePresentPass(GraphBuilder, ViewInfo, SourceTexture, Inputs.OutputTexture, SourceRect, ClampedOutputRect);
	if (Settings.Intensity <= UE_SMALL_NUMBER)
	{
		return;
	}

	const int32 MipCount = ComputeBloomMipCount(SourceExtent);

	TArray<FRDGTextureRef, TInlineAllocator<MaxBloomMipCount>> BloomMipTextures;
	BloomMipTextures.SetNum(MipCount);

	BloomMipTextures[0] = GraphBuilder.CreateTexture(
		FRDGTextureDesc::Create2D(SourceExtent, PF_FloatRGBA, FClearValueBinding::Black, TexCreate_RenderTargetable | TexCreate_ShaderResource),
		TEXT("ImGuiBloomThreshold")
	);
	AddThresholdPass(GraphBuilder, ViewInfo, FScreenPassTexture(SourceTexture), BloomMipTextures[0], Settings);

	for (int32 MipLevel = 1; MipLevel < MipCount; ++MipLevel)
	{
		BloomMipTextures[MipLevel] = AddDownsamplePass(
			GraphBuilder,
			ViewInfo,
			MakeTextureSlice(GraphBuilder, BloomMipTextures[MipLevel - 1], BloomMipTextures[MipLevel - 1]->Desc.Extent),
			GetMipExtent(SourceExtent, MipLevel),
			TEXT("ImGuiBloomDownsample"));
	}

	TArray<FRDGTextureRef, TInlineAllocator<MaxBloomMipCount>> BlurredMipTextures;
	BlurredMipTextures.SetNum(MipCount);

	for (int32 MipLevel = 0; MipLevel < MipCount; ++MipLevel)
	{
		const FScreenPassTextureSlice BloomMip = MakeTextureSlice(GraphBuilder, BloomMipTextures[MipLevel], BloomMipTextures[MipLevel]->Desc.Extent);
		FRDGTextureRef HorizontalBlur = AddBlurPass(GraphBuilder, ViewInfo, BloomMip, FVector2f(1.0f, 0.0f), TEXT("ImGuiBloomBlurH"));
		BlurredMipTextures[MipLevel] = AddBlurPass(GraphBuilder, ViewInfo, MakeTextureSlice(GraphBuilder, HorizontalBlur, HorizontalBlur->Desc.Extent), FVector2f(0.0f, 1.0f), TEXT("ImGuiBloomBlurV"));
	}

	FRDGTextureRef BloomComposite = BlurredMipTextures[MipCount - 1];
	for (int32 MipLevel = MipCount - 2; MipLevel >= 0; --MipLevel)
	{
		BloomComposite = AddUpsamplePass(GraphBuilder, ViewInfo, BlurredMipTextures[MipLevel], BloomComposite, TEXT("ImGuiBloomUpsampleChain"));
	}

	FImGuiBloomCompositePS::FParameters* CompositeParameters = GraphBuilder.AllocParameters<FImGuiBloomCompositePS::FParameters>();
	CompositeParameters->Bloom = GetScreenPassTextureInput(FScreenPassTexture(BloomComposite), TStaticSamplerState<SF_Bilinear, AM_Border, AM_Border, AM_Clamp>::GetRHI());
	CompositeParameters->RenderTargets[0] = FRenderTargetBinding(Inputs.OutputTexture, ERenderTargetLoadAction::ELoad);

	const FIntPoint BloomMin = SourceMin;
	FIntRect BloomRect(BloomMin, BloomMin + ClampedOutputRect.Size());
	BloomRect.Clip(FIntRect(FIntPoint::ZeroValue, BloomComposite->Desc.Extent));
	if (BloomRect.Area() <= 0)
	{
		return;
	}

	const FScreenPassTextureViewport OutputViewport(Inputs.OutputTexture, ClampedOutputRect);
	const FScreenPassTextureViewport InputViewport(BloomComposite, BloomRect);

	TShaderMapRef<FScreenPassVS> VertexShader(GetGlobalShaderMap(ViewInfo.FeatureLevel));
	TShaderMapRef<FImGuiBloomCompositePS> PixelShader(GetGlobalShaderMap(ViewInfo.FeatureLevel));
	FRHIBlendState* AdditiveBlend = TStaticBlendState<CW_RGB, BO_Add, BF_One, BF_One>::GetRHI();

	AddDrawScreenPass(GraphBuilder, RDG_EVENT_NAME("ImGuiBloomComposite"), ViewInfo, OutputViewport, InputViewport, VertexShader, PixelShader, AdditiveBlend, CompositeParameters);

#if !UE_BUILD_SHIPPING
	AddBloomValidationPass(GraphBuilder, BloomComposite, ClampedOutputRect, Inputs.SceneViewRect);
#endif
}

#endif
