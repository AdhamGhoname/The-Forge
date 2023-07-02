#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

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

#define MAX_DIRECTIONAL_LIGHTS 20


struct UniformBlock {
	Matrix4 mModel;
	Matrix4 mView;
	Matrix4 mProjection;

	Vector4 ScreenDimsWorldDims;
};


const uint32_t gImageCount = 3;
Buffer* pUniformBuffer[gImageCount] = { NULL };

Renderer* pRenderer = NULL;

Queue*   pGraphicsQueue = NULL;
CmdPool* pCmdPools[gImageCount] = { NULL };
Cmd*     pCmds[gImageCount] = { NULL };

SwapChain*    pSwapChain = NULL;
RenderTarget* pDepthBuffer = NULL;
RenderTarget* pRenderTargetIntermediate;
Fence*        pRenderCompleteFences[gImageCount] = { NULL };
Semaphore*    pImageAcquiredSemaphore = NULL;
Semaphore*    pRenderCompleteSemaphores[gImageCount] = { NULL };


Shader* pDepthShader = NULL;
Shader* pBlueprintShader = NULL;
Buffer* pModelVertexBuffers[100];
Buffer* pVertexBufferQuad;
Pipeline* pDepthPipeline, *pBlueprintPipeline;

const uint32_t gSamplerCount = 1;
Sampler* pSampler;

RootSignature* pRootSignature = NULL;
DescriptorSet* pDescriptorSetTexture = { NULL };
DescriptorSet* pDescriptorSetUniforms = { NULL };

uint32_t gFrameIndex = 0;
ProfileToken gGpuProfileToken = PROFILE_INVALID_TOKEN;

UniformBlock     gUniformData;

Camera* pCamera;
UIComponent*    pGuiWindow = NULL;

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

struct Mesh {
	eastl::vector<Vertex> vertices;
	eastl::vector<uint32_t> indices;
	eastl::vector<TextureStruct> textures;
};


Texture* halftoneTexture;

Matrix4 pModelMatrix;
eastl::vector<Mesh> pMeshes;

eastl::vector<TextureStruct> loadMaterialTextures(aiMaterial *material, aiTextureType type) {
	eastl::vector<TextureStruct> textures;
	for (uint32_t i = 0; i < material->GetTextureCount(type) > 0; i++) {
		aiString aiFilename;
		material->GetTexture(type, i, &aiFilename);
		eastl::string filename = aiFilename.C_Str();
		TextureStruct texture = {};
		texture.textureType = (TextureType)type;

		eastl::string base_filename = filename.substr(filename.find_last_of("/\\") + 1);
		base_filename = base_filename.substr(0, base_filename.find_last_of('.'));


		TextureLoadDesc textureDesc = {};
		textureDesc.pFileName = base_filename.c_str();
		textureDesc.ppTexture = &texture.texture;
		// Textures representing color should be stored in SRGB or HDR format
		textureDesc.mCreationFlag = TEXTURE_CREATION_FLAG_SRGB;
		addResource(&textureDesc, NULL);
	}
	return textures;
}


Mesh processMesh(aiMesh *mesh, const aiScene *scene) {
	eastl::vector<Vertex> vertices;
	eastl::vector<uint32_t> indices;
	eastl::vector<TextureStruct> textures;


	for (uint32_t i = 0; i < mesh->mNumVertices; i++) {
		Vertex vertex;
		vertex.Position = Vector4(mesh->mVertices[i].x, mesh->mVertices[i].y, mesh->mVertices[i].z, 0.0f);
		vertex.Normal = Vector4(mesh->mNormals[i].x, mesh->mNormals[i].y, mesh->mNormals[i].z, 0.0f);
		vertex.Tangent = Vector4(mesh->mTangents[i].x, mesh->mTangents[i].y, mesh->mTangents[i].z, 0.0f);
		if (mesh->mTextureCoords[0]) {
			vertex.uv = Vector4(mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y, 0.0f, 0.0f);
		}
		else {
			vertex.uv = Vector4(0.0f);
		}
		vertices.push_back(vertex);
	}

	for (uint32_t i = 0; i < mesh->mNumFaces; i++) {
		aiFace face = mesh->mFaces[i];
		for (uint32_t j = 0; j < mesh->mFaces[i].mNumIndices; j++) {
			indices.push_back(mesh->mFaces[i].mIndices[j]);
		}
	}

	//if (mesh->mMaterialIndex >= 0) {
	//	aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];
	//	eastl::vector<TextureStruct> diffuse = loadMaterialTextures(material, aiTextureType_DIFFUSE);
	//	eastl::vector<TextureStruct> specular = loadMaterialTextures(material, aiTextureType_SPECULAR);
	//	eastl::vector<TextureStruct> normal = loadMaterialTextures(material, aiTextureType_NORMALS);
	//	eastl::vector<TextureStruct> ao = loadMaterialTextures(material, aiTextureType_AMBIENT_OCCLUSION);
	//	eastl::vector<TextureStruct> roughness = loadMaterialTextures(material, aiTextureType_DIFFUSE_ROUGHNESS);
	//	textures.insert(textures.end(), diffuse.begin(), diffuse.end());
	//	textures.insert(textures.end(), specular.begin(), specular.end());
	//	textures.insert(textures.end(), normal.begin(), normal.end());
	//	textures.insert(textures.end(), ao.begin(), ao.end());
	//	textures.insert(textures.end(), roughness.begin(), roughness.end());
	//	std::sort(textures.begin(), textures.end(), [](TextureStruct a, TextureStruct b) { return a.textureType < b.textureType; });
	//}

	return { vertices, indices, textures };
}

void processNode(aiNode *currentNode, const aiScene *scene) {
	for (uint32_t i = 0; i < currentNode->mNumMeshes; i++) {
		aiMesh* mesh = scene->mMeshes[currentNode->mMeshes[i]];
		pMeshes.push_back(processMesh(mesh, scene));
	}
	for (uint32_t i = 0; i < currentNode->mNumChildren; i++) {
		processNode(currentNode->mChildren[i], scene);
	}
}

void loadModel(const char* path) {
	Assimp::Importer importer;
	uint32_t postprocess_options = aiProcess_Triangulate |
		aiProcess_FlipUVs |
		aiProcess_CalcTangentSpace;
	const aiScene* scene = importer.ReadFile(path, postprocess_options);
	if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode) {
		return;
	}
	processNode(scene->mRootNode, scene);
	importer.FreeScene();
}

DECLARE_RENDERER_FUNCTION(void, mapBuffer, Renderer* pRenderer, Buffer* pBuffer, ReadRange* pRange)
DECLARE_RENDERER_FUNCTION(void, unmapBuffer, Renderer* pRenderer, Buffer* pBuffer)

class Test : public IApp {
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

		/// TODO: Load model

		char modelPath[256];
		fsAppendPathComponent(fsGetResourceDirectory(RD_MESHES), "Octane.fbx", modelPath);


		loadModel(modelPath);
		///


		for (uint32_t i = 0; i < pMeshes.size(); i++) {
			uint64_t meshDataSize = pMeshes[i].vertices.size() * sizeof(Vertex);
			BufferLoadDesc meshVbDesc = {};
			meshVbDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_VERTEX_BUFFER;
			meshVbDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_GPU_ONLY;
			meshVbDesc.mDesc.mSize = meshDataSize;
			meshVbDesc.pData = &pMeshes[i].vertices[0];
			meshVbDesc.ppBuffer = &pModelVertexBuffers[i];
			addResource(&meshVbDesc, NULL);
		}

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
		*pCamera = Camera(Vector3(0.0f, 0.0f, -5.0f),
			Vector3(0.0f, 0.0f, 1.0f),
			"persp",
			0.1f,
			1000.0f,
			45.0f,
			(float)mSettings.mWidth / mSettings.mHeight,
			0.001f,
			-Vector3(PI / 2.0f, PI / 2.0f, 0.0f),
			Vector3(PI / 2.0f, PI / 2.0f, 0.0f));

		pModelMatrix = Matrix4::rotationY(PI / 2.0f) * Matrix4::rotationX(-PI / 2.0f);

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


		for (uint32_t i = 0; i < pMeshes.size(); i++) {
			removeResource(pModelVertexBuffers[i]);
		}

		for (uint32_t i = 0; i < pMeshes.size(); i++) {
			for (uint32_t j = 0; j < pMeshes[i].textures.size(); i++) {
				removeResource(pMeshes[i].textures[j].texture);
			}
		}

		//removeResource(halftoneTexture);
		removeSampler(pRenderer, pSampler);


		pMeshes.clear();
		pMeshes.shrink_to_fit();

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
			removeRenderTarget(pRenderer, pRenderTargetIntermediate);
		}

		if (pReloadDesc->mType & RELOAD_TYPE_SHADER)
		{
			removeDescriptorSets();
			removeRootSignatures();
			removeShaders();
		}
	}

	void addShaders() {
		ShaderLoadDesc depthPassthrough = {};
		depthPassthrough.mStages[0] = { "depth_passthrough.vert", NULL, 0, NULL, SHADER_STAGE_LOAD_FLAG_NONE };
		depthPassthrough.mStages[1] = { "depth_passthrough.frag", NULL, 0 };

		ShaderLoadDesc blueprint = {};
		blueprint.mStages[0] = { "blueprint.vert", NULL, 0, NULL, SHADER_STAGE_LOAD_FLAG_NONE };
		blueprint.mStages[1] = { "blueprint.frag", NULL, 0 };

		addShader(pRenderer, &depthPassthrough, &pDepthShader);
		addShader(pRenderer, &blueprint, &pBlueprintShader);
	}

	void removeShaders() {
		removeShader(pRenderer, pDepthShader);
		removeShader(pRenderer, pBlueprintShader);
	}

	void addRootSignatures() {
		Shader* shaders[2];
		shaders[0] = pDepthShader;
		shaders[1] = pBlueprintShader;
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
		addDescriptorSet(pRenderer, &desc1, &pDescriptorSetTexture);
		DescriptorSetDesc desc2 = { pRootSignature, DESCRIPTOR_UPDATE_FREQ_PER_FRAME, gImageCount };
		addDescriptorSet(pRenderer, &desc2, &pDescriptorSetUniforms);
	}

	void removeDescriptorSets()
	{
		removeDescriptorSet(pRenderer, pDescriptorSetTexture);
		removeDescriptorSet(pRenderer, pDescriptorSetUniforms);
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
		vertexLayout.mAttribs[3].mLocation = 1;
		vertexLayout.mAttribs[3].mOffset = offsetof(Vertex, uv);


		RasterizerStateDesc modelRasterizerStateDesc = {};
		modelRasterizerStateDesc.mCullMode = CULL_MODE_FRONT;
		modelRasterizerStateDesc.mFillMode = FILL_MODE_WIREFRAME;

		DepthStateDesc depthStateDesc = {};
		depthStateDesc.mDepthTest = true;
		depthStateDesc.mDepthWrite = true;
		depthStateDesc.mDepthFunc = CMP_GEQUAL;

		PipelineDesc desc = {};
		desc.mType = PIPELINE_TYPE_GRAPHICS;
		GraphicsPipelineDesc& pipelineSettings = desc.mGraphicsDesc;
		pipelineSettings.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
		pipelineSettings.mRenderTargetCount = 1;
		pipelineSettings.pDepthState = &depthStateDesc;
		pipelineSettings.pColorFormats = &pRenderTargetIntermediate->mFormat;
		pipelineSettings.mSampleCount = pRenderTargetIntermediate->mSampleCount;
		pipelineSettings.mSampleQuality = pRenderTargetIntermediate->mSampleQuality;
		pipelineSettings.mDepthStencilFormat = pDepthBuffer->mFormat;
		pipelineSettings.pRootSignature = pRootSignature;
		pipelineSettings.pShaderProgram = pDepthShader;
		pipelineSettings.pVertexLayout = &vertexLayout;
		pipelineSettings.pRasterizerState = &modelRasterizerStateDesc;
		pipelineSettings.mVRFoveatedRendering = true;
		addPipeline(pRenderer, &desc, &pDepthPipeline);


		// 2nd pipeline

		modelRasterizerStateDesc.mFillMode = FILL_MODE_SOLID;
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
		pipelineSettings.pShaderProgram = pBlueprintShader;
		addPipeline(pRenderer, &desc, &pBlueprintPipeline);
	}

	void removePipelines()
	{
		removePipeline(pRenderer, pDepthPipeline);
		removePipeline(pRenderer, pBlueprintPipeline);
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

		DescriptorData params[1] = {};
		params[0].pName = "DepthTexture";
		params[0].ppTextures = &pRenderTargetIntermediate->pTexture;
		updateDescriptorSet(pRenderer, 0, pDescriptorSetTexture, 1, params);
	}
	
	bool addIntermediateRenderTarget()
	{
		// Add depth buffer
		RenderTargetDesc rtDesc = {};
		rtDesc.mArraySize = 1;
		rtDesc.mClearValue = { { 0.001f, 0.001f, 0.001f, 0.001f } }; // This is a temporary workaround for AMD cards on macOS. Setting this to (0,0,0,0) will introduce weird behavior.
		rtDesc.mDepth = 1;
		rtDesc.mDescriptors = DESCRIPTOR_TYPE_TEXTURE;
		rtDesc.mFormat = TinyImageFormat_R32G32B32A32_SFLOAT;
		rtDesc.mStartState = RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		rtDesc.mHeight = mSettings.mHeight;
		rtDesc.mSampleCount = SAMPLE_COUNT_1;
		rtDesc.mSampleQuality = 0;
		rtDesc.mWidth = mSettings.mWidth;
		rtDesc.mFlags = TEXTURE_CREATION_FLAG_VR_MULTIVIEW;
		addRenderTarget(pRenderer, &rtDesc, &pRenderTargetIntermediate);

		return pRenderTargetIntermediate != NULL;
	}

	void Update(float deltaTime) {
		updateInputSystem(deltaTime, mSettings.mWidth, mSettings.mHeight);

		pCamera->aspect_ratio = (float)mSettings.mWidth / mSettings.mHeight;

		Matrix4 mView = pCamera->GetViewMatrix();
		Matrix4 mProj = pCamera->GetProjectionMatrix();



		gUniformData.mModel = pModelMatrix;
		gUniformData.mView = mView;
		gUniformData.mProjection = mProj;
		gUniformData.ScreenDimsWorldDims = Vector4(mSettings.mWidth, mSettings.mHeight, 4.0f, 4.0f);
		//gUniformData.mOrthoProjection = mat4::orthographic(-1.0f * pCamera->aspect_ratio, 1.0f * pCamera->aspect_ratio, -1.0f, 1.0f, 0.0f, 1.0f);

		//memset(gUniformData.mLightDirections, 0, sizeof gUniformData.mLightDirections);
		////for (int i = 0; i < 8; i++) {
		////	gUniformData.mLightDirections[i] = Vector4((i & 1 ? -1.0f : 1.0f), 
		////											   (i >> 1 & 1 ? -1.0f : 1.0f), 
		////											   (i >> 2 & 1 ? -1.0f : 1.0f),
		////												1.0f);	
		////}
		//gUniformData.mLightDirections[0] = Vector4(-2.0f, -1.0f, 1.0f, 1.0f);
		//gUniformData.mLightDirections[1] = Vector4(2.0f, 1.0f, 1.0f, 1.0f);
		//gUniformData.mCameraPosition = Vector4(pCamera->position, 0.0f);
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

		RenderTarget* pRenderTarget = pRenderTargetIntermediate;
		RenderTarget* pScreenRenderTarget = pSwapChain->ppRenderTargets[swapchainImageIndex];
		Semaphore*    pRenderCompleteSemaphore = pRenderCompleteSemaphores[gFrameIndex];
		Fence*        pRenderCompleteFence = pRenderCompleteFences[gFrameIndex];

		LoadActionsDesc loadActions = {};
		loadActions.mLoadActionsColor[0] = LOAD_ACTION_CLEAR;
		loadActions.mLoadActionDepth = LOAD_ACTION_CLEAR;
		loadActions.mClearColorValues[0] = pRenderTarget->mClearValue;

		Cmd* cmd = pCmds[gFrameIndex];
		beginCmd(cmd);

		RenderTargetBarrier barriers[] = {
			{ pRenderTarget, RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_RENDER_TARGET },
			{ pScreenRenderTarget, RESOURCE_STATE_PRESENT, RESOURCE_STATE_RENDER_TARGET },
		};

		cmdResourceBarrier(cmd, 0, NULL, 0, NULL, 2, barriers);

		// simply record the screen cleaning command
		loadActions.mClearDepth.depth = 0.0f;
		cmdBindRenderTargets(cmd, 1, &pRenderTarget, pDepthBuffer, &loadActions, NULL, NULL, -1, -1);
		cmdSetViewport(cmd, 0.0f, 0.0f, (float)pRenderTarget->mWidth, (float)pRenderTarget->mHeight, 1.0f, 0.0f);
		cmdSetScissor(cmd, 0, 0, pRenderTarget->mWidth, pRenderTarget->mHeight);

		const uint32_t modelVbStride = sizeof(Vertex);
		cmdBindDescriptorSet(cmd, gFrameIndex, pDescriptorSetUniforms);
		cmdBindPipeline(cmd, pDepthPipeline);
		for (uint32_t i = 0; i < pMeshes.size(); i++) {
			cmdBindVertexBuffer(cmd, 1, &pModelVertexBuffers[i], &modelVbStride, NULL);
			cmdDraw(cmd, pMeshes[i].vertices.size(), 0);
		}

		cmdBindRenderTargets(cmd, 0, NULL, NULL, NULL, NULL, NULL, -1, -1);
		RenderTargetBarrier srvBarrier[] = {
			{ pRenderTarget, RESOURCE_STATE_RENDER_TARGET, RESOURCE_STATE_PIXEL_SHADER_RESOURCE },
		};
		cmdResourceBarrier(cmd, 0, NULL, 0, NULL, 1, srvBarrier);

		loadActions.mLoadActionsColor[0] = LOAD_ACTION_CLEAR;
		loadActions.mLoadActionDepth = LOAD_ACTION_DONTCARE;
		loadActions.mClearColorValues[0] = pScreenRenderTarget->mClearValue;
		cmdBindRenderTargets(cmd, 1, &pScreenRenderTarget, NULL, &loadActions, NULL, NULL, -1, -1);

		const uint32_t quadStride = sizeof(QuadVertex);
		cmdBindPipeline(cmd, pBlueprintPipeline);
		cmdBindDescriptorSet(cmd, gFrameIndex, pDescriptorSetUniforms);
		cmdBindDescriptorSet(cmd, 0, pDescriptorSetTexture);
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

	const char* GetName() { return "40_Blueprint"; }
};

DEFINE_APPLICATION_MAIN(Test)
