#include "Camera/Camera.h"

//Interfaces
#include "../../../../Common_3/Application/Interfaces/ICameraController.h"
#include "../../../../Common_3/Application/Interfaces/IApp.h"
#include "../../../../Common_3/Utilities/ThirdParty/OpenSource/EASTL/vector.h"
#include "../../../../Common_3/Utilities/ThirdParty/OpenSource/EASTL/string.h"
#include "../../../../Common_3/Utilities/Interfaces/ILog.h"
#include "../../../../Common_3/Application/Interfaces/IInput.h"
#include "../../../../Common_3/Utilities/Interfaces/IFileSystem.h"
#include "../../../../Common_3/Utilities/Interfaces/ITime.h"
#include "../../../../Common_3/Application/Interfaces/IProfiler.h"
#include "../../../../Common_3/Application/Interfaces/IScreenshot.h"
#include "../../../../Common_3/Game/Interfaces/IScripting.h"
#include "../../../../Common_3/Application/Interfaces/IFont.h"
#include "../../../../Common_3/Application/Interfaces/IUI.h"

//Renderer
#include "../../../../Common_3/Graphics/Interfaces/IGraphics.h"
#include "../../../../Common_3/Resources/ResourceLoader/Interfaces/IResourceLoader.h"

//Math
#include "../../../../Common_3/Utilities/Math/MathTypes.h"
#include "../../../../Common_3/Utilities/Interfaces/IMemory.h"

#define GBUFFER_RT_COUNT 3
#define POINT_LIGHT_COUNT 4

struct PointLight {
	Vector3 position;
	float attenuation;
	Vector4 baseColor;
};

struct UniformBlock {
	Matrix4 mModel;
	Matrix4 mView;
	Matrix4 mProj;

	Matrix4 mModelInv;
	Matrix4 mViewInv;
	Matrix4 mProjInv;

	Vector4 cameraPosition;
	Vector4 ScreenDims;

	PointLight lights[POINT_LIGHT_COUNT];
};


struct Material {
	Texture* albedo;
	Texture* normal;
	Texture* roughness;
	Texture* metallic;
	Texture* ao;
};

Material gMaterial;


const uint32_t gImageCount = 3;
Buffer* pUniformBuffer[gImageCount] = { NULL };

Renderer* pRenderer = NULL;
Geometry* pModelGeo;

Queue*   pGraphicsQueue = NULL;
CmdPool* pCmdPools[gImageCount] = { NULL };
Cmd*     pCmds[gImageCount] = { NULL };

SwapChain*    pSwapChain = NULL;
RenderTarget* pDepthBuffer = NULL;
RenderTarget* pRenderTargetGBuffer[GBUFFER_RT_COUNT];
Fence*        pRenderCompleteFences[gImageCount] = { NULL };
Semaphore*    pImageAcquiredSemaphore = NULL;
Semaphore*    pRenderCompleteSemaphores[gImageCount] = { NULL };
Matrix4 pModelMatrix;


Shader* pGPassShader = NULL;
Shader* pPBRShader = NULL;
Buffer* pVertexBufferQuad;
Pipeline* pGPassPipeline, *pPBRPipeline;

const uint32_t gSamplerCount = 1;
Sampler* pSampler;

RootSignature* pRootSignature = NULL;
DescriptorSet* pDescriptorSetGBuffer = { NULL };
DescriptorSet* pDescriptorSetMaterial = { NULL };
DescriptorSet* pDescriptorSetUniforms = { NULL };

uint32_t gFrameIndex = 0;
ProfileToken gGpuProfileToken = PROFILE_INVALID_TOKEN;

UniformBlock     gUniformData;

Camera* pCamera;
UIComponent*    pGuiWindow = NULL;

TinyImageFormat gGBufferColorFormats[] = { TinyImageFormat_R32G32B32A32_SFLOAT,
								TinyImageFormat_R32G32B32A32_SFLOAT,
								TinyImageFormat_R32G32B32A32_SFLOAT };

static float gThreshold = -0.2f;

FontDrawDesc gFrameTimeDraw;

uint32_t gFontID = 0;


struct Vertex {
	Vector4 Position;
	Vector4 Normal;
	Vector4 Tangent;
	Vector4 uv;
};

struct QuadVertex
{
	float3 position;
	float2 uv;
};

enum TextureType {
	DIFFUSE = 1,
	SPECULAR = 2,
	NORMALS = 6
};

struct TextureStruct {
	Texture* texture;
	TextureType textureType;
};

DECLARE_RENDERER_FUNCTION(void, mapBuffer, Renderer* pRenderer, Buffer* pBuffer, ReadRange* pRange)
DECLARE_RENDERER_FUNCTION(void, unmapBuffer, Renderer* pRenderer, Buffer* pBuffer)

class DeferredPBR : public IApp {
public:
	bool Init() {
		// FILE PATHS
		fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT, RD_SHADER_SOURCES, "Shaders");
		fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT, RD_SHADER_BINARIES, "CompiledShaders");
		fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT, RD_GPU_CONFIG, "GPUCfg");
		fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT, RD_TEXTURES, "Textures");
		fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT, RD_FONTS, "Fonts");
		fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT, RD_MESHES, "Meshes");

		RendererDesc settings;
		memset(&settings, 0, sizeof(settings));
		settings.mD3D11Supported = true;
		settings.mGLESSupported = false;
		initRenderer(GetName(), &settings, &pRenderer);

		if (!pRenderer)
			return false;

		QueueDesc queueDesc = {};
		queueDesc.mType = QUEUE_TYPE_GRAPHICS;
		queueDesc.mFlag = QUEUE_FLAG_INIT_MICROPROFILE;
		addQueue(pRenderer, &queueDesc, &pGraphicsQueue);


		for (uint32_t i = 0; i < gImageCount; ++i)
		{
			CmdPoolDesc cmdPoolDesc = {};
			cmdPoolDesc.pQueue = pGraphicsQueue;
			addCmdPool(pRenderer, &cmdPoolDesc, &pCmdPools[i]);
			CmdDesc cmdDesc = {};
			cmdDesc.pPool = pCmdPools[i];
			addCmd(pRenderer, &cmdDesc, &pCmds[i]);

			addFence(pRenderer, &pRenderCompleteFences[i]);
			addSemaphore(pRenderer, &pRenderCompleteSemaphores[i]);
		}


		addSemaphore(pRenderer, &pImageAcquiredSemaphore);
		initResourceLoaderInterface(pRenderer);



		SamplerDesc samplerDesc = { FILTER_NEAREST,
									FILTER_NEAREST,
									MIPMAP_MODE_NEAREST,
									ADDRESS_MODE_REPEAT,
									ADDRESS_MODE_REPEAT,
									ADDRESS_MODE_REPEAT };
		addSampler(pRenderer, &samplerDesc, &pSampler);

		// Define the geometry for a triangle.
		QuadVertex quadVertices[] = { { { -1.0f, -1.0f, 0.0f }, { 0.0f, 1.0f } }, { { -1.0f, 1.0f, 0.0f }, { 0.0f, 0.0f } },
									{ { 1.0f, 1.0f, 0.0f }, { 1.0f, 0.0f } },

									{ { -1.0f, -1.0f, 0.0f }, { 0.0f, 1.0f } }, { { 1.0f, 1.0f, 0.0f }, { 1.0f, 0.0f } },
									{ { 1.0f, -1.0f, 0.0f }, { 1.0f, 1.0f } } };

		BufferLoadDesc quadUVDesc = {};
		quadUVDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_VERTEX_BUFFER;
		quadUVDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_GPU_ONLY;
		quadUVDesc.mDesc.mSize = sizeof(quadVertices);
		quadUVDesc.pData = quadVertices;
		quadUVDesc.ppBuffer = &pVertexBufferQuad;
		addResource(&quadUVDesc, NULL);


		// Loading material textures

		TextureLoadDesc textureDesc = {};
		textureDesc.pFileName = "PBR/metal_0002_color_4k";
		textureDesc.ppTexture = &gMaterial.albedo;
		textureDesc.mCreationFlag = TEXTURE_CREATION_FLAG_SRGB;
		textureDesc.mContainer = TEXTURE_CONTAINER_BASIS;
		addResource(&textureDesc, NULL);

		textureDesc.pFileName = "PBR/metal_0002_metallic_4k";
		textureDesc.ppTexture = &gMaterial.metallic;
		textureDesc.mCreationFlag = TEXTURE_CREATION_FLAG_SRGB;
		textureDesc.mContainer = TEXTURE_CONTAINER_BASIS;
		addResource(&textureDesc, NULL);

		textureDesc.pFileName = "PBR/metal_0002_roughness_4k";
		textureDesc.ppTexture = &gMaterial.roughness;
		textureDesc.mCreationFlag = TEXTURE_CREATION_FLAG_SRGB;
		textureDesc.mContainer = TEXTURE_CONTAINER_BASIS;
		addResource(&textureDesc, NULL);

		textureDesc.pFileName = "PBR/metal_0002_ao_4k";
		textureDesc.ppTexture = &gMaterial.ao;
		textureDesc.mCreationFlag = TEXTURE_CREATION_FLAG_SRGB;
		textureDesc.mContainer = TEXTURE_CONTAINER_BASIS;
		addResource(&textureDesc, NULL);

		textureDesc.pFileName = "PBR/metal_0002_normal_directx_4k";
		textureDesc.ppTexture = &gMaterial.normal;
		textureDesc.mCreationFlag = TEXTURE_CREATION_FLAG_SRGB;
		textureDesc.mContainer = TEXTURE_CONTAINER_BASIS;
		addResource(&textureDesc, NULL);


		VertexLayout modelLayout;
		modelLayout.mAttribCount = 4;

		modelLayout.mAttribs[0].mSemantic = SEMANTIC_POSITION;
		modelLayout.mAttribs[0].mFormat = TinyImageFormat_R32G32B32A32_SFLOAT;
		modelLayout.mAttribs[0].mBinding = 0;
		modelLayout.mAttribs[0].mLocation = 0;
		modelLayout.mAttribs[0].mOffset = 0;

		modelLayout.mAttribs[1].mSemantic = SEMANTIC_NORMAL;
		modelLayout.mAttribs[1].mFormat = TinyImageFormat_R32G32B32A32_SFLOAT;
		modelLayout.mAttribs[1].mBinding = 0;
		modelLayout.mAttribs[1].mLocation = 1;
		modelLayout.mAttribs[1].mOffset = offsetof(Vertex, Normal);

		modelLayout.mAttribs[2].mSemantic = SEMANTIC_TANGENT;
		modelLayout.mAttribs[2].mFormat = TinyImageFormat_R32G32B32A32_SFLOAT;
		modelLayout.mAttribs[2].mBinding = 0;
		modelLayout.mAttribs[2].mLocation = 2;
		modelLayout.mAttribs[2].mOffset = offsetof(Vertex, Tangent);

		modelLayout.mAttribs[3].mSemantic = SEMANTIC_TEXCOORD0;
		modelLayout.mAttribs[3].mFormat = TinyImageFormat_R32G32B32A32_SFLOAT;
		modelLayout.mAttribs[3].mBinding = 0;
		modelLayout.mAttribs[3].mLocation = 3;
		modelLayout.mAttribs[3].mOffset = offsetof(Vertex, uv);
		

		GeometryLoadDesc loadDesc = {};
		loadDesc.pFileName = "matBall.gltf";
		loadDesc.ppGeometry = &pModelGeo;
		loadDesc.pVertexLayout = &modelLayout;
		addResource(&loadDesc, NULL);

		BufferLoadDesc ubDesc = {};
		ubDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		ubDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_CPU_TO_GPU;
		ubDesc.mDesc.mSize = sizeof(UniformBlock);
		ubDesc.mDesc.mFlags = BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT;
		ubDesc.pData = NULL;

		for (uint32_t i = 0; i < gImageCount; ++i)
		{
			ubDesc.ppBuffer = &pUniformBuffer[i];
			addResource(&ubDesc, NULL);
		}

		// Load fonts
		FontDesc font = {};
		font.pFontPath = "TitilliumText/TitilliumText-Bold.otf";
		fntDefineFonts(&font, 1, &gFontID);

		FontSystemDesc fontRenderDesc = {};
		fontRenderDesc.pRenderer = pRenderer;
		if (!initFontSystem(&fontRenderDesc))
			return false; // report?

		// Initialize Forge User Interface Rendering
		UserInterfaceDesc uiRenderDesc = {};
		uiRenderDesc.pRenderer = pRenderer;
		initUserInterface(&uiRenderDesc);

		UIComponentDesc guiDesc = {};
		guiDesc.mStartPosition = vec2(mSettings.mWidth * 0.01f, mSettings.mHeight * 0.2f);
		uiCreateComponent(GetName(), &guiDesc, &pGuiWindow);

		//SliderFloatWidget thresholdSlider;
		//thresholdSlider.pData = &gThreshold;
		//thresholdSlider.mMin = -0.5f;
		//thresholdSlider.mMax = 0.5f;
		//luaRegisterWidget(uiCreateComponentWidget(pGuiWindow, "Threshold : ", &thresholdSlider, WIDGET_TYPE_SLIDER_FLOAT));


		waitForAllResourceLoads();


		// Initialize Camera and Model Matrix
		pCamera = tf_placement_new<Camera>(tf_calloc(1, sizeof(Camera)));
		*pCamera = Camera(Vector3(-8.0f, 0.0f, 0.0f),
			Vector3(1.0f, 0.0f, 0.0f),
			"persp",
			0.1f,
			1000.0f,
			45.0f,
			(float)mSettings.mWidth / mSettings.mHeight,
			0.001f,
			-Vector3(PI / 2.0f, PI / 2.0f, 0.0f),
			Vector3(PI / 2.0f, PI / 2.0f, 0.0f));

		pModelMatrix = Matrix4::rotationY(PI / 2.0f) * Matrix4::scale(Vector3(0.5f));

		// Input system
		InputSystemDesc inputDesc = {};
		inputDesc.pRenderer = pRenderer;
		inputDesc.pWindow = pWindow;
		if (!initInputSystem(&inputDesc))
			return false;

		InputActionDesc actionDesc = { DefaultInputActions::DUMP_PROFILE_DATA, [](InputActionContext* ctx) {  dumpProfileData(((Renderer*)ctx->pUserData)->pName); return true; }, pRenderer };
		addInputAction(&actionDesc);
		actionDesc = { DefaultInputActions::TOGGLE_FULLSCREEN, [](InputActionContext* ctx) { toggleFullscreen(((IApp*)ctx->pUserData)->pWindow); return true; }, this };
		addInputAction(&actionDesc);
		actionDesc = { DefaultInputActions::EXIT, [](InputActionContext* ctx) { requestShutdown(); return true; } };
		addInputAction(&actionDesc);
		InputActionCallback onAnyInput = [](InputActionContext* ctx)
		{
			if (ctx->mActionId > UISystemInputActions::UI_ACTION_START_ID_)
			{
				uiOnInput(ctx->mActionId, ctx->mBool, ctx->pPosition, &ctx->mFloat2);
			}

			return true;
		};

		typedef bool(*CameraInputHandler)(InputActionContext* ctx, uint32_t index);
		static CameraInputHandler onCameraInput = [](InputActionContext* ctx, uint32_t index)
		{
			if (*(ctx->pCaptured))
			{
				float2 delta = uiIsFocused() ? float2(0.f, 0.f) : ctx->mFloat2;
				index ? pCamera->OnMouseMove(-delta.getX(), delta.getY()) : pCamera->TranslateRelative(Vector3(delta.getX() * 1.0f / 5.0f, 0.0f, delta.getY() * 1.0f / 5.0f));
			}
			return true;
		};
		actionDesc = { DefaultInputActions::CAPTURE_INPUT, [](InputActionContext* ctx) {setEnableCaptureInput(!uiIsFocused() && INPUT_ACTION_PHASE_CANCELED != ctx->mPhase);	return true;}, NULL };
		addInputAction(&actionDesc);
		actionDesc = { DefaultInputActions::ROTATE_CAMERA, [](InputActionContext* ctx) { return onCameraInput(ctx, 1); }, NULL };
		addInputAction(&actionDesc);
		actionDesc = { DefaultInputActions::TRANSLATE_CAMERA, [](InputActionContext* ctx) { return onCameraInput(ctx, 0); }, NULL };
		addInputAction(&actionDesc);
		GlobalInputActionDesc globalInputActionDesc = { GlobalInputActionDesc::ANY_BUTTON_ACTION, onAnyInput, this };
		setGlobalInputAction(&globalInputActionDesc);


		gFrameIndex = 0;

		return true;
	}

	void Exit()
	{
		exitInputSystem();

		tf_free(pCamera);

		exitUserInterface();

		exitFontSystem();


		for (uint32_t i = 0; i < gImageCount; ++i)
		{
			removeResource(pUniformBuffer[i]);
		}
		removeResource(pVertexBufferQuad);
		removeResource(pModelGeo);

		
		removeResource(gMaterial.albedo);
		removeResource(gMaterial.metallic);
		removeResource(gMaterial.roughness);
		removeResource(gMaterial.ao);
		removeResource(gMaterial.normal);

		removeSampler(pRenderer, pSampler);

		//for (uint32_t i = 0; i < gSamplerCount; i++) {
		//	removeSampler(pRenderer, pSamplers[i]);
		//}


		for (uint32_t i = 0; i < gImageCount; ++i)
		{
			removeFence(pRenderer, pRenderCompleteFences[i]);
			removeSemaphore(pRenderer, pRenderCompleteSemaphores[i]);

			removeCmd(pRenderer, pCmds[i]);
			removeCmdPool(pRenderer, pCmdPools[i]);
		}


		removeSemaphore(pRenderer, pImageAcquiredSemaphore);

		exitResourceLoaderInterface(pRenderer);

		removeQueue(pRenderer, pGraphicsQueue);

		exitRenderer(pRenderer);
		pRenderer = NULL;
	}

	bool Load(ReloadDesc* pReloadDesc)
	{
		if (pReloadDesc->mType & RELOAD_TYPE_SHADER)
		{
			addShaders();
			addRootSignatures();
			addDescriptorSets();
		}

		if (pReloadDesc->mType & (RELOAD_TYPE_RESIZE | RELOAD_TYPE_RENDERTARGET))
		{
			if (!addSwapChain())
				return false;

			if (!addDepthBuffer())
				return false;

			if (!addIntermediateRenderTarget())
				return false;
		}

		if (pReloadDesc->mType & (RELOAD_TYPE_SHADER | RELOAD_TYPE_RENDERTARGET))
		{
			addPipelines();
		}

		prepareDescriptorSets();

		UserInterfaceLoadDesc uiLoad = {};
		uiLoad.mColorFormat = pSwapChain->ppRenderTargets[0]->mFormat;
		uiLoad.mHeight = mSettings.mHeight;
		uiLoad.mWidth = mSettings.mWidth;
		uiLoad.mLoadType = pReloadDesc->mType;
		loadUserInterface(&uiLoad);

		FontSystemLoadDesc fontLoad = {};
		fontLoad.mColorFormat = pSwapChain->ppRenderTargets[0]->mFormat;
		fontLoad.mHeight = mSettings.mHeight;
		fontLoad.mWidth = mSettings.mWidth;
		fontLoad.mLoadType = pReloadDesc->mType;
		loadFontSystem(&fontLoad);

		return true;
	}


	void Unload(ReloadDesc* pReloadDesc)
	{
		waitQueueIdle(pGraphicsQueue);
		unloadFontSystem(pReloadDesc->mType);
		unloadUserInterface(pReloadDesc->mType);

		if (pReloadDesc->mType & (RELOAD_TYPE_SHADER | RELOAD_TYPE_RENDERTARGET))
		{
			removePipelines();
		}

		if (pReloadDesc->mType & (RELOAD_TYPE_RESIZE | RELOAD_TYPE_RENDERTARGET))
		{
			removeSwapChain(pRenderer, pSwapChain);
			removeRenderTarget(pRenderer, pDepthBuffer);
			for (int i = 0; i < GBUFFER_RT_COUNT; i++) {
				removeRenderTarget(pRenderer, pRenderTargetGBuffer[i]);
			}
		}

		if (pReloadDesc->mType & RELOAD_TYPE_SHADER)
		{
			removeDescriptorSets();
			removeRootSignatures();
			removeShaders();
		}
	}

	void addShaders() {
		ShaderLoadDesc gpass = {};
		gpass.mStages[0] = { "gpass.vert", NULL, 0, NULL, SHADER_STAGE_LOAD_FLAG_NONE };
		gpass.mStages[1] = { "gpass.frag", NULL, 0 };

		ShaderLoadDesc pbr = {};
		pbr.mStages[0] = { "pbr.vert", NULL, 0, NULL, SHADER_STAGE_LOAD_FLAG_NONE };
		pbr.mStages[1] = { "pbr.frag", NULL, 0 };

		addShader(pRenderer, &gpass, &pGPassShader);
		addShader(pRenderer, &pbr, &pPBRShader);
	}

	void removeShaders() {
		removeShader(pRenderer, pGPassShader);
		removeShader(pRenderer, pPBRShader);
	}

	void addRootSignatures() {
		Shader* shaders[2];
		shaders[0] = pGPassShader;
		shaders[1] = pPBRShader;
		const char* pStaticSamplers[] = { "uSampler0" };
		RootSignatureDesc rootDesc = {};
		rootDesc.mStaticSamplerCount = 1;
		rootDesc.ppStaticSamplerNames = pStaticSamplers;
		rootDesc.ppStaticSamplers = &pSampler;
		rootDesc.mShaderCount = 2;
		rootDesc.ppShaders = shaders;
		addRootSignature(pRenderer, &rootDesc, &pRootSignature);
	}

	void removeRootSignatures() {
		removeRootSignature(pRenderer, pRootSignature);
	}

	void addDescriptorSets() {
		DescriptorSetDesc desc1 = { pRootSignature, DESCRIPTOR_UPDATE_FREQ_NONE, 1 };
		addDescriptorSet(pRenderer, &desc1, &pDescriptorSetGBuffer);

		DescriptorSetDesc desc2 = { pRootSignature, DESCRIPTOR_UPDATE_FREQ_NONE, 1 };
		addDescriptorSet(pRenderer, &desc2, &pDescriptorSetMaterial);

		DescriptorSetDesc desc3 = { pRootSignature, DESCRIPTOR_UPDATE_FREQ_PER_FRAME, gImageCount };
		addDescriptorSet(pRenderer, &desc3, &pDescriptorSetUniforms);
	}

	void removeDescriptorSets()
	{
		removeDescriptorSet(pRenderer, pDescriptorSetGBuffer);
		removeDescriptorSet(pRenderer, pDescriptorSetUniforms);
		removeDescriptorSet(pRenderer, pDescriptorSetMaterial);
	}

	bool addSwapChain() {
		SwapChainDesc swapChainDesc = {};
		swapChainDesc.mWindowHandle = pWindow->handle;
		swapChainDesc.mPresentQueueCount = 1;
		swapChainDesc.ppPresentQueues = &pGraphicsQueue;
		swapChainDesc.mWidth = mSettings.mWidth;
		swapChainDesc.mHeight = mSettings.mHeight;
		swapChainDesc.mImageCount = gImageCount;
		swapChainDesc.mColorFormat = getRecommendedSwapchainFormat(true, true);
		swapChainDesc.mEnableVsync = mSettings.mVSyncEnabled;
		::addSwapChain(pRenderer, &swapChainDesc, &pSwapChain);

		return pSwapChain != NULL;
	}

	bool addDepthBuffer() {
		// Add depth buffer
		RenderTargetDesc depthRT = {};
		depthRT.mArraySize = 1;
		depthRT.mClearValue.depth = 0.0f;
		depthRT.mClearValue.stencil = 0;
		depthRT.mDepth = 1;
		depthRT.mFormat = TinyImageFormat_D32_SFLOAT;
		depthRT.mStartState = RESOURCE_STATE_DEPTH_WRITE;
		depthRT.mHeight = mSettings.mHeight;
		depthRT.mSampleCount = SAMPLE_COUNT_1;
		depthRT.mSampleQuality = 0;
		depthRT.mWidth = mSettings.mWidth;
		depthRT.mFlags = TEXTURE_CREATION_FLAG_ON_TILE;
		addRenderTarget(pRenderer, &depthRT, &pDepthBuffer);

		return pDepthBuffer != NULL;
	}

	void addPipelines()
	{
		//layout and pipeline
		VertexLayout vertexLayout = {};
		vertexLayout.mAttribCount = 4;
		vertexLayout.mAttribs[0].mSemantic = SEMANTIC_POSITION;
		vertexLayout.mAttribs[0].mFormat = TinyImageFormat_R32G32B32A32_SFLOAT;
		vertexLayout.mAttribs[0].mBinding = 0;
		vertexLayout.mAttribs[0].mLocation = 0;
		vertexLayout.mAttribs[0].mOffset = 0;

		vertexLayout.mAttribs[1].mSemantic = SEMANTIC_NORMAL;
		vertexLayout.mAttribs[1].mFormat = TinyImageFormat_R32G32B32A32_SFLOAT;
		vertexLayout.mAttribs[1].mBinding = 0;
		vertexLayout.mAttribs[1].mLocation = 1;
		vertexLayout.mAttribs[1].mOffset = offsetof(Vertex, Normal);

		vertexLayout.mAttribs[2].mSemantic = SEMANTIC_TANGENT;
		vertexLayout.mAttribs[2].mFormat = TinyImageFormat_R32G32B32A32_SFLOAT;
		vertexLayout.mAttribs[2].mBinding = 0;
		vertexLayout.mAttribs[2].mLocation = 2;
		vertexLayout.mAttribs[2].mOffset = offsetof(Vertex, Tangent);

		vertexLayout.mAttribs[3].mSemantic = SEMANTIC_TEXCOORD0;
		vertexLayout.mAttribs[3].mFormat = TinyImageFormat_R32G32B32A32_SFLOAT;
		vertexLayout.mAttribs[3].mBinding = 0;
		vertexLayout.mAttribs[3].mLocation = 2;
		vertexLayout.mAttribs[3].mOffset = offsetof(Vertex, uv);


		RasterizerStateDesc modelRasterizerStateDesc = {};
		modelRasterizerStateDesc.mCullMode = CULL_MODE_NONE;

		DepthStateDesc depthStateDesc = {};
		depthStateDesc.mDepthTest = true;
		depthStateDesc.mDepthWrite = true;
		depthStateDesc.mDepthFunc = CMP_LEQUAL;

		PipelineDesc desc = {};
		desc.mType = PIPELINE_TYPE_GRAPHICS;


		GraphicsPipelineDesc& pipelineSettings = desc.mGraphicsDesc;
		pipelineSettings.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
		pipelineSettings.mRenderTargetCount = 3;
		pipelineSettings.pDepthState = &depthStateDesc;
		pipelineSettings.pColorFormats = gGBufferColorFormats;
		pipelineSettings.mSampleCount = pRenderTargetGBuffer[0]->mSampleCount;
		pipelineSettings.mSampleQuality = pRenderTargetGBuffer[0]->mSampleQuality;
		pipelineSettings.mDepthStencilFormat = pDepthBuffer->mFormat;
		pipelineSettings.pRootSignature = pRootSignature;
		pipelineSettings.pShaderProgram = pGPassShader;
		pipelineSettings.pVertexLayout = &vertexLayout;
		pipelineSettings.pRasterizerState = &modelRasterizerStateDesc;
		pipelineSettings.mVRFoveatedRendering = true;
		addPipeline(pRenderer, &desc, &pGPassPipeline);


		// 2nd pipeline

		pipelineSettings.mRenderTargetCount = 1;
		pipelineSettings.pColorFormats = &pSwapChain->ppRenderTargets[0]->mFormat;
		pipelineSettings.mSampleCount = pSwapChain->ppRenderTargets[0]->mSampleCount;
		pipelineSettings.mSampleQuality = pSwapChain->ppRenderTargets[0]->mSampleQuality;


		vertexLayout.mAttribCount = 2;
		vertexLayout.mAttribs[0].mSemantic = SEMANTIC_POSITION;
		vertexLayout.mAttribs[0].mFormat = TinyImageFormat_R32G32B32_SFLOAT;
		vertexLayout.mAttribs[0].mBinding = 0;
		vertexLayout.mAttribs[0].mLocation = 0;
		vertexLayout.mAttribs[0].mOffset = 0;

		vertexLayout.mAttribs[1].mSemantic = SEMANTIC_TEXCOORD0;
		vertexLayout.mAttribs[1].mFormat = TinyImageFormat_R32G32_SFLOAT;
		vertexLayout.mAttribs[1].mBinding = 0;
		vertexLayout.mAttribs[1].mLocation = 1;
		vertexLayout.mAttribs[1].mOffset = offsetof(QuadVertex, uv);

		depthStateDesc.mDepthTest = false;
		depthStateDesc.mDepthWrite = false;
		pipelineSettings.pShaderProgram = pPBRShader;
		addPipeline(pRenderer, &desc, &pPBRPipeline);
	}

	void removePipelines()
	{
		removePipeline(pRenderer, pGPassPipeline);
		removePipeline(pRenderer, pPBRPipeline);
	}

	void prepareDescriptorSets()
	{
		// Prepare descriptor sets

		for (uint32_t i = 0; i < gImageCount; ++i)
		{
			DescriptorData params[1] = {};
			params[0].pName = "uniformBlock";
			params[0].ppBuffers = &pUniformBuffer[i];
			updateDescriptorSet(pRenderer, i, pDescriptorSetUniforms, 1, params);
		}

		DescriptorData params2[5] = {};
		params2[0].pName = "AlbedoTexture";
		params2[0].ppTextures = &gMaterial.albedo;

		params2[1].pName = "MetallicTexture";
		params2[1].ppTextures = &gMaterial.metallic;

		params2[2].pName = "RoughnessTexture";
		params2[2].ppTextures = &gMaterial.roughness;

		params2[3].pName = "AOTexture";
		params2[3].ppTextures = &gMaterial.ao;

		params2[4].pName = "NormalTexture";
		params2[4].ppTextures = &gMaterial.normal;
		updateDescriptorSet(pRenderer, 0, pDescriptorSetMaterial, 5, params2);


		DescriptorData params[3] = {};
		params[0].pName = "AlbedoTexture";
		params[0].ppTextures = &pRenderTargetGBuffer[0]->pTexture;

		params[1].pName = "MaterialPropsTexture";
		params[1].ppTextures = &pRenderTargetGBuffer[1]->pTexture;

		params[2].pName = "NormalDepthTexture";
		params[2].ppTextures = &pRenderTargetGBuffer[2]->pTexture;

		updateDescriptorSet(pRenderer, 0, pDescriptorSetGBuffer, 3, params);


	}
	
	bool addIntermediateRenderTarget()
	{
		// Add depth buffer
		RenderTargetDesc rtDesc = {};
		rtDesc.mArraySize = 1;
		rtDesc.mClearValue = { { 0.0f, 0.0f, 0.0f, 0.0f } };
		rtDesc.mDepth = 1;
		rtDesc.mDescriptors = DESCRIPTOR_TYPE_TEXTURE;
		rtDesc.mStartState = RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		rtDesc.mHeight = mSettings.mHeight;
		rtDesc.mWidth = mSettings.mWidth;
		rtDesc.mSampleQuality = 0;
		rtDesc.mSampleCount = SAMPLE_COUNT_1;
		rtDesc.mFlags = TEXTURE_CREATION_FLAG_VR_MULTIVIEW;
		rtDesc.pName = "GBuffer RTs";



		for (int i = 0; i < GBUFFER_RT_COUNT; i++) {
			rtDesc.mFormat = gGBufferColorFormats[i];
			addRenderTarget(pRenderer, &rtDesc, &pRenderTargetGBuffer[i]);
		}


		return pRenderTargetGBuffer != NULL;
	}

	void Update(float deltaTime) {
		updateInputSystem(deltaTime, mSettings.mWidth, mSettings.mHeight);

		pCamera->aspect_ratio = (float)mSettings.mWidth / mSettings.mHeight;

		Matrix4 mView = pCamera->GetViewMatrix();
		Matrix4 mProj = pCamera->GetProjectionMatrix();



		gUniformData.mModel = pModelMatrix;
		gUniformData.mView = mView;
		gUniformData.mProj = mProj;


		Vector4 res = mProj * mView * pModelMatrix * Vector4(-1, 0, 1, 1);

		gUniformData.mModelInv = inverse(gUniformData.mModel);
		gUniformData.mViewInv = inverse(gUniformData.mView);
		gUniformData.mProjInv = inverse(gUniformData.mProj);

		gUniformData.cameraPosition = Vector4(pCamera->position, 1.0f);

		gUniformData.ScreenDims = Vector4(mSettings.mWidth, mSettings.mHeight, 1.0f, 1.0f);

		gUniformData.lights[0].position = Vector3(-1, 0, 0);
		gUniformData.lights[0].attenuation = 3.0f;
		gUniformData.lights[0].baseColor = Vector4(1.0f, 1.0f, 0.6f, 1.0f);

		gUniformData.lights[1].position = Vector3(4, 0, 0);
		gUniformData.lights[1].attenuation = 3.0f;
		gUniformData.lights[1].baseColor = Vector4(1.0f, 1.0f, 0.6f, 1.0f);

		gUniformData.lights[2].position = Vector3(0, 0, -2);
		gUniformData.lights[2].attenuation = 3.0f;
		gUniformData.lights[2].baseColor = Vector4(1.0f, 1.0f, 0.6f, 1.0f);

		gUniformData.lights[3].position = Vector3(0, 0, 3);
		gUniformData.lights[3].attenuation = 3.0f;
		gUniformData.lights[3].baseColor = Vector4(1.0f, 1.0f, 0.6f, 1.0f);
	}

	void Draw() {
		if (pSwapChain->mEnableVsync != mSettings.mVSyncEnabled)
		{
			waitQueueIdle(pGraphicsQueue);
			::toggleVSync(pRenderer, &pSwapChain);
		}

		uint32_t swapchainImageIndex;
		acquireNextImage(pRenderer, pSwapChain, pImageAcquiredSemaphore, NULL, &swapchainImageIndex);

		// Stall if CPU is running "Swap Chain Buffer Count" frames ahead of GPU
		FenceStatus fenceStatus;
		Fence*      pNextFence = pRenderCompleteFences[gFrameIndex];
		getFenceStatus(pRenderer, pNextFence, &fenceStatus);
		if (fenceStatus == FENCE_STATUS_INCOMPLETE)
			waitForFences(pRenderer, 1, &pNextFence);

		resetCmdPool(pRenderer, pCmdPools[gFrameIndex]);

		// Update uniform buffers
		BufferUpdateDesc viewProjCbv = { pUniformBuffer[gFrameIndex] };
		beginUpdateResource(&viewProjCbv);
		*(UniformBlock*)viewProjCbv.pMappedData = gUniformData;
		endUpdateResource(&viewProjCbv, NULL);

		RenderTarget** pRenderTargets = pRenderTargetGBuffer;
		RenderTarget* pScreenRenderTarget = pSwapChain->ppRenderTargets[swapchainImageIndex];
		Semaphore*    pRenderCompleteSemaphore = pRenderCompleteSemaphores[gFrameIndex];
		Fence*        pRenderCompleteFence = pRenderCompleteFences[gFrameIndex];

		LoadActionsDesc loadActions = {};
		for (int i = 0; i < GBUFFER_RT_COUNT; i++) {
			loadActions.mLoadActionsColor[i] = LOAD_ACTION_CLEAR;
			loadActions.mLoadActionDepth = LOAD_ACTION_CLEAR;
			loadActions.mClearColorValues[i] = pRenderTargets[i]->mClearValue;
		}

		Cmd* cmd = pCmds[gFrameIndex];
		beginCmd(cmd);

		RenderTargetBarrier barriers[] = {
			{ pRenderTargets[0], RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_RENDER_TARGET },
			{ pRenderTargets[1], RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_RENDER_TARGET },
			{ pRenderTargets[2], RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_RENDER_TARGET },
			{ pScreenRenderTarget, RESOURCE_STATE_PRESENT, RESOURCE_STATE_RENDER_TARGET },
		};

		cmdResourceBarrier(cmd, 0, NULL, 0, NULL, 4, barriers);

		// simply record the screen cleaning command
		loadActions.mClearDepth.depth = 1.0f;
		cmdBindRenderTargets(cmd, GBUFFER_RT_COUNT, pRenderTargets, pDepthBuffer, &loadActions, NULL, NULL, -1, -1);
		cmdSetViewport(cmd, 0.0f, 0.0f, (float)pRenderTargets[0]->mWidth, (float)pRenderTargets[0]->mHeight, 0.0f, 1.0f);
		cmdSetScissor(cmd, 0, 0, pRenderTargets[0]->mWidth, pRenderTargets[0]->mHeight);

		const uint32_t modelVbStride = sizeof(Vertex);
		cmdBindDescriptorSet(cmd, gFrameIndex, pDescriptorSetUniforms);
		cmdBindDescriptorSet(cmd, 0, pDescriptorSetMaterial);
		cmdBindPipeline(cmd, pGPassPipeline);

		cmdBindVertexBuffer(cmd, 1, &pModelGeo->pVertexBuffers[0], pModelGeo->mVertexStrides, NULL);
		cmdBindIndexBuffer(cmd, pModelGeo->pIndexBuffer, pModelGeo->mIndexType, 0);

		cmdDrawIndexed(cmd, pModelGeo->mIndexCount, 0, 0);

		cmdBindRenderTargets(cmd, 0, NULL, NULL, NULL, NULL, NULL, -1, -1);
		RenderTargetBarrier srvBarrier[] = {
			{ pRenderTargets[0], RESOURCE_STATE_RENDER_TARGET, RESOURCE_STATE_PIXEL_SHADER_RESOURCE },
			{ pRenderTargets[1], RESOURCE_STATE_RENDER_TARGET, RESOURCE_STATE_PIXEL_SHADER_RESOURCE },
			{ pRenderTargets[2], RESOURCE_STATE_RENDER_TARGET, RESOURCE_STATE_PIXEL_SHADER_RESOURCE },
		};
		cmdResourceBarrier(cmd, 0, NULL, 0, NULL, 3, srvBarrier);

		loadActions.mLoadActionsColor[0] = LOAD_ACTION_CLEAR;
		loadActions.mLoadActionDepth = LOAD_ACTION_DONTCARE;
		loadActions.mClearColorValues[0] = pScreenRenderTarget->mClearValue;
		cmdBindRenderTargets(cmd, 1, &pScreenRenderTarget, NULL, &loadActions, NULL, NULL, -1, -1);

		const uint32_t quadStride = sizeof(QuadVertex);
		cmdBindPipeline(cmd, pPBRPipeline);
		cmdBindDescriptorSet(cmd, gFrameIndex, pDescriptorSetUniforms);
		cmdBindDescriptorSet(cmd, 0, pDescriptorSetGBuffer);
		cmdBindVertexBuffer(cmd, 1, &pVertexBufferQuad, &quadStride, NULL);
		cmdDrawInstanced(cmd, 6, 0, 2, 0);


		loadActions = {};
		loadActions.mLoadActionsColor[0] = LOAD_ACTION_LOAD;
		cmdBindRenderTargets(cmd, 1, &pScreenRenderTarget, nullptr, &loadActions, NULL, NULL, -1, -1);
		cmdBeginGpuTimestampQuery(cmd, gGpuProfileToken, "Draw UI");

		gFrameTimeDraw.mFontColor = 0xff00ffff;
		gFrameTimeDraw.mFontSize = 18.0f;
		gFrameTimeDraw.mFontID = gFontID;
		float2 txtSizePx = cmdDrawCpuProfile(cmd, float2(8.f, 15.f), &gFrameTimeDraw);
		cmdDrawGpuProfile(cmd, float2(8.f, txtSizePx.y + 75.f), gGpuProfileToken, &gFrameTimeDraw);

		cmdDrawUserInterface(cmd);

		cmdBindRenderTargets(cmd, 0, NULL, NULL, NULL, NULL, NULL, -1, -1);
		cmdEndGpuTimestampQuery(cmd, gGpuProfileToken);

		RenderTargetBarrier presentBarrier = { pScreenRenderTarget, RESOURCE_STATE_RENDER_TARGET, RESOURCE_STATE_PRESENT };
		cmdResourceBarrier(cmd, 0, NULL, 0, NULL, 1, &presentBarrier);
		endCmd(cmd);

		QueueSubmitDesc submitDesc = {};
		submitDesc.mCmdCount = 1;
		submitDesc.mSignalSemaphoreCount = 1;
		submitDesc.mWaitSemaphoreCount = 1;
		submitDesc.ppCmds = &cmd;
		submitDesc.ppSignalSemaphores = &pRenderCompleteSemaphore;
		submitDesc.ppWaitSemaphores = &pImageAcquiredSemaphore;
		submitDesc.pSignalFence = pRenderCompleteFence;
		queueSubmit(pGraphicsQueue, &submitDesc);
		QueuePresentDesc presentDesc = {};
		presentDesc.mIndex = swapchainImageIndex;
		presentDesc.mWaitSemaphoreCount = 1;
		presentDesc.pSwapChain = pSwapChain;
		presentDesc.ppWaitSemaphores = &pRenderCompleteSemaphore;
		presentDesc.mSubmitDone = true;
		queuePresent(pGraphicsQueue, &presentDesc);
		gFrameIndex = (gFrameIndex + 1) % gImageCount;
	}

	const char* GetName() { return "41_DeferredPBR"; }
};

DEFINE_APPLICATION_MAIN(DeferredPBR)
