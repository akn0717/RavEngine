//
//  RenderEngine.cpp
//  RavEngine_Static
//
//  Copyright © 2020 Ravbug.
//	Some code adapted from: https://github.com/pezcode/Cluster/blob/master/src/Renderer/DeferredRenderer.cpp
//

#include "RenderEngine.hpp"
#include <string>
#include "CameraComponent.hpp"
#include "World.hpp"
#include <memory>
#include "Material.hpp"
#include <bgfx/platform.h>
#include <bx/bx.h>
#include "StaticMesh.hpp"
#include "App.hpp"
#include "GUI.hpp"
#include <RmlUi/Debugger.h>
#include "Utilities.hpp"
#include "InputManager.hpp"

	#if defined __linux__ && !defined(__ANDROID__)
	#define SDL_VIDEO_DRIVER_X11 1		//Without this X11 support doesn't work
	#define SDL_VIDEO_DRIVER_WAYLAND 1
#endif
#include <SDL_syswm.h>
#include <SDL.h>
#include <SDL_video.h>
#include <im3d.h>
#include <BuiltinMaterials.hpp>
#include "Common3D.hpp"
#include "DebugDrawer.hpp"
#include <glm/gtc/type_ptr.hpp>
#include <fmt/core.h>
#include <iostream>
#include "Debug.hpp"
#include <chrono>
#include <cstdio>

#ifdef __APPLE__
	#include "AppleUtilities.h"
#endif

#if BX_PLATFORM_WINDOWS
#define _WIN32_WINNT _WIN32_WINNT_WIN10
#include <ShellScalingApi.h>
#pragma comment(lib, "Shcore.lib")
#elif BX_PLATFORM_WINRT
#include <winrt/Windows.Graphics.Display.h>
using namespace winrt;
#endif

using namespace std;
using namespace RavEngine;

SDL_Window* RenderEngine::window = nullptr;
RenderEngine::vs RenderEngine::VideoSettings;
//debug drawing vertex layout
static bgfx::VertexLayout debuglayout;
bgfx::VertexLayout RenderEngine::RmlLayout;

decltype(RenderEngine::skinningOutputLayout) RenderEngine::skinningOutputLayout, RenderEngine::skinningInputLayout;
decltype(RenderEngine::opaquemtxhandle) RenderEngine::opaquemtxhandle = BGFX_INVALID_HANDLE;
STATIC(RenderEngine::allVerticesHandle) = BGFX_INVALID_HANDLE;
STATIC(RenderEngine::allIndicesHandle) = BGFX_INVALID_HANDLE;
STATIC(RenderEngine::guiMaterial);

static bgfx::ProgramHandle skinningShaderHandle, copyIndicesShaderHandle, shadowMapShaderHandle, shadowVolumeHandleLT;
static bgfx::VertexBufferHandle screenSpaceQuadVert, shadowTriangleVertexBuffer;
static bgfx::DynamicVertexBufferHandle lightDataHandle = BGFX_INVALID_HANDLE;
static bgfx::IndexBufferHandle screenSpaceQuadInd, shadowTriangleIndexBuffer;

// this one's externable
bgfx::ProgramHandle rve_debugShaderHandle;

#ifdef _DEBUG
static std::optional<GUIComponent> debuggerContext;
STATIC(RenderEngine::debuggerInput);
UnorderedMap<uint16_t, RenderEngine::DebugMsg> RenderEngine::debugprints;
SpinLock RenderEngine::dbgmtx;
#endif

static Ref<DebugMaterialInstance> mat;

static Ref<RavEngine::DeferredBlitShader> blitShader;

static bgfx::BackbufferRatio::Enum TexRatio = bgfx::BackbufferRatio::Equal;

static constexpr uint16_t shadowMapSize = 2048;


#ifdef _DEBUG
static DebugDrawer dbgdraw;	//for rendering debug primitives
#endif


struct bgfx_msghandler : public bgfx::CallbackI{
    struct cacheItem{
        char* data = nullptr;
        uint32_t size = 0;
    };
    UnorderedMap<uint64_t,cacheItem> cache;
    
	static bool diagnostic_logging; // see below
    
	void fatal(const char *_filePath, uint16_t _line, bgfx::Fatal::Enum _code, const char *_str) final{
		Debug::Fatal("BGFX error {} in {} line {}: {}",_code, _filePath, _line, _str);
	}
	void traceVargs(const char *_filePath, uint16_t _line, const char *_format, va_list _argList) final{
#ifdef _DEBUG
		if(diagnostic_logging){
            char buffer[256]{0};
            std::vsnprintf(buffer,sizeof(buffer)/sizeof(buffer[0]),_format,_argList);
			Debug::LogTemp("BGFX diagnostic: {}", buffer);
		}
#endif
	}
	void profilerBegin(const char *_name, uint32_t _abgr, const char *_filePath, uint16_t _line) final{
		Debug::Fatal("profiler not implemented");
	}
	void profilerBeginLiteral(const char *_name, uint32_t _abgr, const char *_filePath, uint16_t _line) final{
		Debug::Fatal("profilerliteral not implemented");
	}
	void profilerEnd() override{
		Debug::Fatal("profiler not implemented");
	}
	uint32_t cacheReadSize(uint64_t _id) final{
        if (cache.contains(_id)){
            return cache.at(_id).size;
        }
		return 0;
	}
	bool cacheRead(uint64_t _id, void *_data, uint32_t _size) final{
        if (cache.contains(_id)){
            auto ptr = cache.at(_id).data;
            std::memcpy(_data, ptr, _size);
            return true;
        }
		return false;
	}
	void cacheWrite(uint64_t _id, const void *_data, uint32_t _size) final{
        auto& item = cache[_id];
        if (item.data != nullptr){
            delete[] item.data;
        }
        item.data = new char[_size];
        item.size = _size;
        std::memcpy(item.data,_data,_size);
	}
	void screenShot(const char *_filePath, uint32_t _width, uint32_t _height, uint32_t _pitch, const void *_data, uint32_t _size, bool _yflip) final{
		Debug::Fatal("screenshot not implemented");
	}
	void captureBegin(uint32_t _width, uint32_t _height, uint32_t _pitch, bgfx::TextureFormat::Enum format, bool _yflip) final {
		Debug::Fatal("video capture not implemented");
	}
	void captureEnd() final{
		Debug::Fatal("video capture not implemented");
	}
	void captureFrame(const void *_data, uint32_t _size) final{
		Debug::Fatal("frame capture not implemented");
	}
    
    ~bgfx_msghandler(){
        // deallocate memory in the cache
        for(const auto& item : cache){
            delete[] item.second.data;
        }
    }
};
bool bgfx_msghandler::diagnostic_logging = false;    // set to true to enable bgfx TRACE logging
static bgfx_msghandler global_msghandler;

/**
 Create an SDL window for different platforms, and reference it to bgfx
 @param _window the SDL window pointer (modified)
 @note supported platforms: Linux, BSD, OSX, Windows, SteamLink
 */
inline bgfx::PlatformData sdlSetWindow(SDL_Window* _window)
{
#ifndef __EMSCRIPTEN__
	SDL_SysWMinfo wmi;
	SDL_VERSION(&wmi.version);
	if (!SDL_GetWindowWMInfo(_window, &wmi)) {
		Debug::Fatal("Cannot get native window information");
	}
#endif
    bgfx::PlatformData pd;
    std::memset(&pd, 0, sizeof(pd));
	
#if BX_PLATFORM_LINUX || BX_PLATFORM_BSD
    // load the correct one of WayLand or X11
    switch(wmi.subsystem){
        case SDL_SYSWM_X11:
            pd.ndt = wmi.info.x11.display;
            pd.nwh = (void*)(uintptr_t)wmi.info.x11.window;
            Debug::LogTemp("Initialized X11");
            break;
        case SDL_SYSWM_WAYLAND:
            pd.ndt = wmi.info.wl.display;
            pd.nwh = wmi.info.wl.surface;
            Debug::LogTemp("Initialized Wayland");
            break;
        default:
            Debug::Fatal("Running Linux or Unix, but window manager ({}) is neither X ({}) nor Wayland ({})",wmi.subsystem, SDL_SYSWM_X11, SDL_SYSWM_WAYLAND);
    };
#elif BX_PLATFORM_OSX
	pd.ndt = NULL;
	pd.nwh = cbSetupMetalLayer(wmi.info.cocoa.window);
#elif BX_PLATFORM_IOS
	pd.ndt = NULL;
	pd.nwh = cbSetupMetalLayer(wmi.info.uikit.window);
#elif BX_PLATFORM_WINDOWS
	pd.ndt = NULL;
	pd.nwh = wmi.info.win.window;
#elif BX_PLATFORM_STEAMLINK
	pd.ndt = wmi.info.vivante.display;
	pd.nwh = wmi.info.vivante.window;
#elif BX_PLATFORM_WINRT || BX_PLATFORM_XBOXONE
	pd.ndt = NULL;
	pd.nwh = wmi.info.winrt.window;
#elif __EMSCRIPTEN__
    static const char* canvas = "#canvas";
    pd.ndt = NULL;
    pd.nwh = (void*)canvas;
#elif __ANDROID__
    Debug::Fatal("Not implemented");
#else
	#error This system / display manager is not supported
#endif // BX_PLATFORM_
	pd.context = NULL;
	pd.backBuffer = NULL;
	pd.backBufferDS = NULL;
	
	return pd;
}

void DebugRender(const Im3d::DrawList& drawList){
#ifdef _DEBUG
	switch(drawList.m_primType){
		case Im3d::DrawPrimitive_Triangles:
			//Set BGFX state to triangles
			bgfx::setState(BGFX_STATE_DEFAULT);
			break;
		case Im3d::DrawPrimitive_Lines:
			bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_CULL_CW | BGFX_STATE_MSAA | BGFX_STATE_PT_LINES);
			//set BGFX state to lines
			break;
		case Im3d::DrawPrimitive_Points:
			//set BGFX state to points
			bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_CULL_CW | BGFX_STATE_MSAA | BGFX_STATE_PT_POINTS);
			break;
		default:
			Debug::Fatal("Invalid Im3d state");
			break;
	}
	//perform drawing here
	const Im3d::VertexData* vertexdata = drawList.m_vertexData;
	const auto verts = drawList.m_vertexCount;
	
	bgfx::VertexBufferHandle vbuf;
	{
		maybestackarray(converted, VertexColor, verts)
		for (uint32_t x = 0; x < verts; x++) {
			Im3d::VertexData d = vertexdata[x];
			converted[x] = { d.m_positionSize.x,d.m_positionSize.y,d.m_positionSize.z,d.m_color };
		}
		vbuf = bgfx::createVertexBuffer(bgfx::copy(&converted[0], verts * sizeof(converted[0])), debuglayout);
	}
	
	bgfx::IndexBufferHandle ibuf;
	{
		maybestackarray(indices, uint16_t, verts)
		for (uint32_t i = 0; i < verts; i++) {
			indices[i] = i;
		}
		ibuf = bgfx::createIndexBuffer(bgfx::copy(&indices[0], verts * sizeof(indices[0])));
	}
	
	mat->Draw(vbuf,ibuf,matrix4(1),RenderEngine::Views::FinalBlit);
	bgfx::destroy(vbuf);
	bgfx::destroy(ibuf);
#endif
}

/**
Initialize static singletons. Invoked automatically if needed.
*/
void RenderEngine::Init(const AppConfig& config)
{
	//setup bgfx if it is not already setup
	if (window != nullptr)
	{
		return;
	}
	
	window = SDL_CreateWindow("RavEngine", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, VideoSettings.width, VideoSettings.height, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
	
	if (window == NULL){
		Debug::Fatal("Unable to create main window: {}",SDL_GetError());
	}

	//start the render thread here
	{
		auto pd = sdlSetWindow(RenderEngine::GetWindow());
#if BX_PLATFORM_IOS
		metalLayer = pd.nwh;
#endif
		UpdateBufferDims();

		bgfx::Init settings;

		auto SelectRenderer = [&](bgfx::RendererType::Enum desired) {
			constexpr auto maxRenderers = 6;
			bgfx::RendererType::Enum supportedRenderers[maxRenderers];
			auto count = bgfx::getSupportedRenderers(maxRenderers, supportedRenderers);

			if (std::find(std::begin(supportedRenderers), supportedRenderers + count, desired) != std::end(supportedRenderers)) {
				settings.type = desired;
			}
			else {
				Debug::Fatal("{} API not found", BackendStringName(desired));
			}
		};

		if (config.preferredBackend == AppConfig::RenderBackend::AutoSelect) {
#ifdef __linux__
			SelectRenderer(bgfx::RendererType::Vulkan);
#elif defined _WIN32
			SelectRenderer(bgfx::RendererType::Direct3D12);
#elif defined __APPLE__
			SelectRenderer(bgfx::RendererType::Metal);
#elif defined __EMSCRIPTEN__
            SelectRenderer(bgfx::RendererType::OpenGLES);
#endif
		}
		else {
			switch (config.preferredBackend) {
#if BX_PLATFORM_IOS || BX_PLATFORM_OSX
			case AppConfig::RenderBackend::Metal:
				SelectRenderer(bgfx::RendererType::Metal);
				break;
#elif BX_PLATFORM_WINDOWS || BX_PLATFORM_WINRT
			case AppConfig::RenderBackend::DirectX12:
				SelectRenderer(bgfx::RendererType::Direct3D12);
				break;
#if BX_PLATFORM_WINDOWS
			case AppConfig::RenderBackend::Vulkan:
				SelectRenderer(bgfx::RendererType::Vulkan);
				break;
#endif
#elif BX_PLATFORM_LINUX
			case AppConfig::RenderBackend::Vulkan:
				SelectRenderer(bgfx::RendererType::Vulkan);
				break;
#endif
			default:
				Debug::Fatal("Invalid preferred backend");
	}
}


		settings.callback = &global_msghandler;

#if XR_AVAILABLE
		if (GetApp()->wantsXR) {
			bgfx::renderFrame();	// switch to single-threaded rendering and make this the render thread
		}
#endif

		// we want to make the transient buffer larger
		settings.limits.transientIbSize = 3.2e+7;  //32 mb
		settings.limits.transientVbSize = 3.2e+7;  //32 mb

		//must be in this order
		settings.platformData = pd;

		settings.resolution.width = bufferdims.width;
		settings.resolution.height = bufferdims.height;
		settings.resolution.reset = RenderEngine::GetResetFlags();
		settings.resolution.maxFrameLatency = 1;	// 0 = default = 3
		if (!bgfx::init(settings)) {
			Debug::Fatal("bgfx::init Failed");
		}

		// must happen after bgfx init because it uses the device
#if XR_AVAILABLE
		if (GetApp()->wantsXR) {
			InitXR();
		}
#endif

		// Enable debug text.
		bgfx::setDebug(BGFX_DEBUG_TEXT /*| BGFX_DEBUG_STATS*/);

		bgfx::setViewRect(0, 0, 0, uint16_t(bufferdims.width), uint16_t(bufferdims.height));

		bgfx::setState(BGFX_STATE_DEFAULT);
	}
    
    // check min spec
#if __APPLE__
    if (!AppleGPUMeetsMinSpec()){
        char buf[30]{0};
        AppleGPUName(buf, 30);
        Debug::Fatal("Cannot proceed: device \"{}\" is under the minimum spec!", buf);
    }
#endif

	//check capabilities
	const auto caps = bgfx::getCaps();
	Debug::Assert(caps->supported & BGFX_CAPS_COMPUTE, "Cannot proceed: this platform does not support compute shaders.");
	Debug::Assert(caps->supported & BGFX_CAPS_INDEX32, "Cannot proceed: this platform does not support 32 bit index buffers");
	Debug::Assert(caps->supported & BGFX_CAPS_INSTANCING, "Cannot proceed: this platform does not support instancing");

	//create screenspace quad
	constexpr uint16_t indices[] = { 0,2,1, 2,3,1 };
	constexpr Vertex vertices[] = { {-1,-1,0}, {-1,1,0}, {1,-1,0}, {1,1,0} };
	bgfx::VertexLayout vl;
	vl.begin()
		.add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
		.end();

	screenSpaceQuadVert = bgfx::createVertexBuffer(bgfx::copy(vertices, sizeof(vertices)), vl);
	screenSpaceQuadInd = bgfx::createIndexBuffer(bgfx::copy(indices, sizeof(indices)));
	blitShader = Material::Manager::Get<DeferredBlitShader>();
	guiMaterial = make_shared<GUIMaterialInstance>(Material::Manager::Get<GUIMaterial>());

	//load compute shader for skinning
	skinningShaderHandle = Material::loadComputeProgram("skincompute/compute.bin");
	copyIndicesShaderHandle = Material::loadComputeProgram("indexcopycompute/compute.bin");
    rve_debugShaderHandle = Material::loadShaderProgram("meshOnly");
    shadowMapShaderHandle = Material::loadShaderProgram("shadowvolume");
    shadowVolumeHandleLT = Material::loadShaderProgram("shadowvolumeLT");
	

	//create compute shader buffers
	skinningOutputLayout.begin()
		.add(bgfx::Attrib::Position, 4, bgfx::AttribType::Float)
		.add(bgfx::Attrib::Position, 4, bgfx::AttribType::Float)
		.add(bgfx::Attrib::Position, 4, bgfx::AttribType::Float)
		.add(bgfx::Attrib::Position, 4, bgfx::AttribType::Float)
		.end();
	
	skinningInputLayout.begin()
		.add(bgfx::Attrib::Position, 4, bgfx::AttribType::Float)
		.add(bgfx::Attrib::Position, 4, bgfx::AttribType::Float)
		.add(bgfx::Attrib::Position, 4, bgfx::AttribType::Float)
		.add(bgfx::Attrib::Position, 4, bgfx::AttribType::Float)
		.end();
	
	float identity[16] = {
		1,0,0,0,
		0,1,0,0,
		0,0,1,0,
		0,0,0,1
	};
	
	opaquemtxhandle = bgfx::createVertexBuffer(bgfx::copy(identity,sizeof(identity)), skinningOutputLayout);
    
    bgfx::VertexLayout allGeoLayout;
    allGeoLayout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)    // 3 verts to make a triangle
        .end();
    allVerticesHandle = bgfx::createDynamicVertexBuffer((1e+7)*3, allGeoLayout, BGFX_BUFFER_COMPUTE_WRITE | BGFX_BUFFER_ALLOW_RESIZE | BGFX_BUFFER_COMPUTE_FORMAT_32X1);
	allIndicesHandle = bgfx::createDynamicIndexBuffer(1e+7, BGFX_BUFFER_COMPUTE_READ_WRITE | BGFX_BUFFER_ALLOW_RESIZE | BGFX_BUFFER_INDEX32);
    
    bgfx::VertexLayout lightBlockingLayout;
    lightBlockingLayout.begin()
        .add(bgfx::Attrib::Position, 4, bgfx::AttribType::Uint8)
        .end();
        
    bgfx::VertexLayout lightDataLayout;
    lightDataLayout.begin()
    .add(bgfx::Attrib::Position, 1, bgfx::AttribType::Uint8)
    .end();
    
	lightDataHandle = bgfx::createDynamicVertexBuffer(65535, lightDataLayout, BGFX_BUFFER_COMPUTE_READ | BGFX_BUFFER_ALLOW_RESIZE | BGFX_BUFFER_COMPUTE_FORMAT_32X1);

	//init lights
	LightManager::Init();

	//vertex format for debug drawing
	debuglayout.begin()
		.add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
		.add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
		.end();
    

	//vertex format for ui
	RmlLayout.begin()
		.add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float)
		.add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
		.add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
		.end();

	{
		constexpr Vertex shadowTriangleVerts[] = {
			// bottom face
			{0,0,0},
			{1,0,0},
			{0,0,1},
			// top face
			{0,1,0},
			{1,1,0},
			{0,1,1},
		};

		constexpr uint16_t shadowTriangleIndices[] = {
			0,1,2,	// top face
			5,4,3,	// bottom face
			
			0,3,4,	// side 1-1
			4,1,0,	// side 1-2

			5,3,0,	// side 2-1
			2,5,0,	// side 2-2

			2,1,4,	// side 3-1	
			2,4,5	// side 3-2
		};

		auto calcNormal = [](auto u, auto v) {

			return glm::normalize(vector3(
				u.position[1] * v.position[2] - u.position[2] * v.position[1],
				u.position[2] * v.position[0] - u.position[0] * v.position[2],
				u.position[0] * v.position[1] - u.position[1] * v.position[0]
			));
		};

		auto triNormal = [&](auto idx) {
			auto a = shadowTriangleVerts[shadowTriangleIndices[idx*3]];
			auto b = shadowTriangleVerts[shadowTriangleIndices[idx*3+1]];
			auto c = shadowTriangleVerts[shadowTriangleIndices[idx*3+2]];
			return calcNormal(a-b,a-c);
		};
		auto p = shadowTriangleVerts;
		for (int i = 0; i < sizeof(shadowTriangleIndices) / sizeof(shadowTriangleIndices[0]) / 3; i++) {
			//cout << i << ": " << triNormal(i) << endl;
		}


		bgfx::VertexLayout layout;
		layout.begin()
			.add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
			.end();

		shadowTriangleVertexBuffer = bgfx::createVertexBuffer(bgfx::copy(shadowTriangleVerts, sizeof(shadowTriangleVerts)), layout);
		shadowTriangleIndexBuffer = bgfx::createIndexBuffer(bgfx::copy(shadowTriangleIndices, sizeof(shadowTriangleIndices)));
	}

	mat = make_shared<DebugMaterialInstance>(Material::Manager::Get<DebugMaterial>());
	auto& data = Im3d::GetAppData();
	data.drawCallback = &DebugRender;

	numRowsUniform = Vector4Uniform("NumObjects");
	computeOffsetsUniform = Vector4Uniform("ComputeOffsets");
	timeUniform.emplace("u_time");
	
	bgfx::setViewName(Views::FinalBlit, "Final Blit");
	bgfx::setViewName(Views::DeferredGeo, "Deferred Geometry");
	bgfx::setViewName(Views::LightingNoShadows, "Lighting Volumes No Shadows");

	bgfx::setViewClear(Views::FinalBlit, BGFX_CLEAR_COLOR);
	bgfx::setViewClear(Views::DeferredGeo, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x000000FF, 1.0f);
	bgfx::setViewClear(Views::LightingNoShadows, BGFX_CLEAR_COLOR | BGFX_CLEAR_STENCIL, 0x000000FF, 1.0f);

	debugNavMeshLayout.begin()
		.add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
		.add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8)
		.add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
		.end();

	auto vertfunc = Material::loadShaderHandle("debugNav/vertex.bin");
	auto fragfunc = Material::loadShaderHandle("debugNav/fragment.bin");
	debugNavProgram = bgfx::createProgram(vertfunc, fragfunc);
}


/**
Construct a render engine instance
@param w the owning world for this engine instance
*/
RenderEngine::RenderEngine(const AppConfig& config) {
	Init(config);

	SDL_GetWindowSize(window, &windowdims.width, &windowdims.height);

	static constexpr uint64_t gBufferSamplerFlags = BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT |
		BGFX_SAMPLER_MIP_POINT | BGFX_SAMPLER_U_CLAMP |
		BGFX_SAMPLER_V_CLAMP;

	//create framebuffers
	const auto gen_framebuffer = [](bgfx::TextureFormat::Enum format, const char* name) -> bgfx::TextureHandle {
		auto th = bgfx::createTexture2D(TexRatio, false, 1, format, BGFX_TEXTURE_RT | gBufferSamplerFlags);
		bgfx::setName(th,name);
		return th;
	};

	const auto gen_framebufferSquare = [](bgfx::TextureFormat::Enum format, uint16_t width, uint16_t height, auto samplerflags, const char* name) -> bgfx::TextureHandle {
		auto th = bgfx::createTexture2D(width, height, false, 1, format, BGFX_TEXTURE_RT | samplerflags);
		bgfx::setName(th, name);
		return th;
	};
	constexpr bgfx::TextureFormat::Enum formats[] = { bgfx::TextureFormat::RGBA32F, bgfx::TextureFormat::RGBA16F, bgfx::TextureFormat::RGBA16F, bgfx::TextureFormat::D24S8 };
	constexpr char* names[] = { "TX_GB_Albedo", "TX_GB_Normal", "TX_GB_Position", "TX_GB_DepthStencil"};
	for (int i = 0; i < BX_COUNTOF(formats); i++) {
		attachments[i] = gen_framebuffer(formats[i], names[i]);
	}

	//lighting textures - light color, and share depth
	lightingAttachments[0] = gen_framebuffer(bgfx::TextureFormat::RGBA16F, "TX_Light");
	lightingAttachments[1] = attachments[3];
	
	for(int i = 0; i < gbufferSize; i++){
		if (!bgfx::isValid(attachments[i])){
			Debug::Fatal("Failed to create gbuffer attachment");
		}
	}

	//create samplers
	constexpr char const* buffersamplers[] = { "s_albedo","s_normal","s_pos","s_depth"};
	for (int i = 0; i < BX_COUNTOF(buffersamplers); i++) {
		gBufferSamplers[i] = bgfx::createUniform(buffersamplers[i], bgfx::UniformType::Sampler);
	}
	
	lightingSamplers[0] = bgfx::createUniform("s_light", bgfx::UniformType::Sampler);
	lightingSamplers[1] = gBufferSamplers[3];
	
	//create gbuffer and bind all the textures together
	gBuffer = bgfx::createFrameBuffer(gbufferSize, attachments, true);
	bgfx::setName(gBuffer, "FB_GBuffers");
	
	lightingBuffer = bgfx::createFrameBuffer(lightingAttachmentsSize, lightingAttachments, true);
	bgfx::setName(lightingBuffer, "FB_LightingBuffer");

	const bgfx::TextureFormat::Enum shadowTextureFormats[] = { bgfx::TextureFormat::R32F, bgfx::TextureFormat::D32F };
	bgfx::TextureHandle shadowTextures[BX_COUNTOF(shadowTextureFormats)];
	for (int i = 0; i < BX_COUNTOF(shadowTextureFormats); i++) {
		shadowTextures[i] = gen_framebufferSquare(shadowTextureFormats[i], shadowMapSize, shadowMapSize, BGFX_SAMPLER_BORDER_COLOR(0xFFFFFF) | BGFX_SAMPLER_UVW_BORDER, "TX_ShadowMap");
	}

	depthMapFB = bgfx::createFrameBuffer(BX_COUNTOF(shadowTextureFormats), shadowTextures, true);
	bgfx::setName(depthMapFB, "FB_DepthmapBuffer");

	shadowSamplers[0] = bgfx::createUniform("s_depthdata", bgfx::UniformType::Sampler);
	shadowSamplers[1] = gBufferSamplers[3];
	
	if(!bgfx::isValid(gBuffer)){
		Debug::Fatal("Failed to create gbuffer");
	}
	if (!bgfx::isValid(depthMapFB)) {
		Debug::Fatal("Failed to create depth map FB");
	}

	skinningComputeBuffer = decltype(skinningComputeBuffer)(1024 * 1024);
	poseStorageBuffer = decltype(poseStorageBuffer)(1024 * 1024);
}

RavEngine::RenderEngine::~RenderEngine()
{
	bgfx::destroy(gBuffer);	//automatically destroys attached textures
	bgfx::destroy(lightingBuffer);	
	skinningComputeBuffer.DestroyBuffer();
	poseStorageBuffer.DestroyBuffer();
}

/**
 Render one frame using the current state of every object in the world
 */
void RenderEngine::Draw(Ref<World> worldOwning){

#ifdef _DEBUG
	//display debug print messages
	bgfx::dbgTextClear();
	RenderEngine::dbgmtx.lock();
	for (const auto msg : RenderEngine::debugprints) {
		bgfx::dbgTextPrintf(0, msg.first, msg.second.color, msg.second.message.c_str());
	}
	RenderEngine::dbgmtx.unlock();
#endif

	bgfx::ViewId allViews[] = {Views::FinalBlit, Views::DeferredGeo, Views::LightingNoShadows};
	
	
	bgfx::setViewFrameBuffer(Views::DeferredGeo, gBuffer);
	bgfx::setViewFrameBuffer(Views::LightingNoShadows, lightingBuffer);
    bgfx::setViewMode(Views::LightingNoShadows,bgfx::ViewMode::Sequential);

	auto dims = bufferdims;
#if XR_AVAILABLE
	// testing: set the final destination as one of the VR eyes
	if (GetApp()->wantsXR) {
		auto& vrfb = GetVRFrameBuffers();
		//bgfx::setViewMode(Views::FinalBlit, bgfx::ViewMode::Sequential);
		bgfx::setViewFrameBuffer(Views::FinalBlit,vrfb.l_eye.handle);
		dims = vrfb.l_eye.dims;
	}
#endif
	for (const auto view : allViews) {
		bgfx::setViewRect(view, 0, 0, dims.width, dims.height);
	}
	
	bgfx::touch(Views::DeferredGeo);
	bgfx::touch(Views::LightingNoShadows);
	
	//copy world framedata into local copy
	//GetApp()->SwapRenderFramedata();
	auto fd = GetApp()->GetCurrentFramedata();
	worldOwning->newFrame = false;	//we are processing this frame now
	
	//setup matrices
	float viewmat[16], projmat[16];
	
	copyMat4(glm::value_ptr(fd->viewmatrix),viewmat);
	copyMat4(glm::value_ptr(fd->projmatrix),projmat);
	

	//set the view transform - all entities drawn will use this matrix
	for(const auto view : allViews){
		bgfx::setViewTransform(view, viewmat, projmat);
	}
	
    uint32_t allVerticesOffset = 0;
	uint32_t allIndicesOffset = 0;
	uint32_t allIndicesIncrement = 0;
	auto execdraw = [&](const auto& row, const auto& skinningfunc, const auto& bindfunc) {
		//call Draw with the staticmesh
		if (std::get<1>(row.first)) {
            if (row.second.items.size() == 0){
                return;
            }
			skinningfunc(row);

			//fill the buffer using the material to write the material data for each instance
				//get the stride for the material (only needs the matrix, all others are uniforms?
			constexpr auto stride = closest_multiple_of(16 * sizeof(float), 16);
			bgfx::InstanceDataBuffer idb;
			assert(row.second.items.size() < numeric_limits<uint32_t>::max());	// too many items!
			Debug::Assert(bgfx::getAvailInstanceDataBuffer(static_cast<uint32_t>(row.second.items.size()), stride) == row.second.items.size(), "Instance data buffer does not have enough space!");
			bgfx::allocInstanceDataBuffer(&idb, static_cast<uint32_t>(row.second.items.size()), stride);
			size_t offset = 0;
			for (const auto& mesh : row.second.items) {
				//write the data into the idb
				auto matrix = glm::value_ptr(mesh);
				float* ptr = (float*)(idb.data + offset);

				copyMat4(matrix, ptr);

				offset += stride;
			}
			bgfx::setInstanceDataBuffer(&idb);
			//set BGFX state
			bgfx::setState((BGFX_STATE_DEFAULT & ~BGFX_STATE_CULL_MASK) | (std::get<1>(row.first)->doubleSided ? BGFX_STATE_NONE : BGFX_STATE_CULL_CW));

			bindfunc();
			
            // both skinend and static need to write to this buffer
            bgfx::setBuffer(12, allVerticesHandle, bgfx::Access::Write);
            
			//bind gbuffer textures
			for (int i = 0; i < BX_COUNTOF(attachments); i++) {
				bgfx::setTexture(i, gBufferSamplers[i], attachments[i]);
			}
            
            // update time and other data
            auto numIndiciesInThisDispatch = std::get<0>(row.first)->GetNumVerts();
            float timeVals[] = {static_cast<float>(fd->Time),static_cast<float>(allVerticesOffset),static_cast<float>(numIndiciesInThisDispatch),0};
            allVerticesOffset += numIndiciesInThisDispatch * row.second.items.size();   // need to account for the number of indices
            timeUniform.value().SetValues(&timeVals, 1);

			std::get<1>(row.first)->Draw(std::get<0>(row.first)->getVertexBuffer(), std::get<0>(row.first)->getIndexBuffer(), matrix4(), Views::DeferredGeo);

			// dispatch the indices copy compute shader
			bgfx::discard();
			bgfx::setBuffer(0, std::get<0>(row.first)->getIndexBuffer(), bgfx::Access::Read);
			bgfx::setBuffer(1, allIndicesHandle, bgfx::Access::Write);
			timeVals[0] = allIndicesOffset;
			timeVals[1] = std::get<0>(row.first)->GetNumIndices();
			timeVals[2] = allIndicesIncrement;
			timeVals[3] = std::get<0>(row.first)->GetNumVerts();
			numRowsUniform.SetValues(timeVals, 1);
			bgfx::dispatch(Views::DeferredGeo, copyIndicesShaderHandle, Debug::AssertSize<uint32_t>(ceil(std::get<0>(row.first)->GetNumIndices() / 64.0)), Debug::AssertSize<uint32_t>(row.second.items.size()), 1);
			allIndicesOffset += std::get<0>(row.first)->GetNumIndices() * row.second.items.size();	// account for the number of instances
			allIndicesIncrement += std::get<0>(row.first)->GetNumVerts() * row.second.items.size();	// begin counting from here

		}
		else {
			Debug::Fatal("Cannot draw a mesh with no material assigned.");
		}
	};
		
	for(const auto& row : fd->opaques){
		execdraw(row, [this](const auto& row) {
			// do nothing
		}, [this]() {
			float values[4] = { 0,0,0,0 };	// pretend there is only one object being 'skinned', the shader will wrap around to only read this matrix
			numRowsUniform.SetValues(&values, 1);
			bgfx::setBuffer(11, opaquemtxhandle, bgfx::Access::Read);
		});
	}
	
	for (const auto& row : fd->skinnedOpaques) {
		size_t computeOffsetIndex;
		float values[4];
		execdraw(row, [&computeOffsetIndex, &values, this](const auto& row) {
			// seed compute shader for skinning
			// input buffer A: skeleton bind pose
			Ref<SkeletonAsset> skeleton = std::get<2>(row.first);
			// input buffer B: vertex weights by bone ID
			auto mesh = std::get<0>(row.first);
			// input buffer C: unposed vertices in mesh
			
			// output buffer A: posed output transformations for vertices
			auto numverts = mesh->GetNumVerts();
			auto numobjects = row.second.items.size();
			
			auto emptySpace = numverts * numobjects;
			assert(emptySpace < numeric_limits<uint32_t>::max());

			computeOffsetIndex = skinningComputeBuffer.AddEmptySpace(static_cast<uint32_t>(emptySpace), skinningOutputLayout);
			bgfx::setBuffer(0, skinningComputeBuffer.GetHandle(), bgfx::Access::Write);
			bgfx::setBuffer(2, mesh->GetWeightsHandle(), bgfx::Access::Read);
		
			//pose SOA values
			if(row.second.skinningdata.size() > 0){
				//convert to float from double
				size_t totalsize = 0;
				for(const auto& array : row.second.skinningdata){
					totalsize += array.size();
				}
				typedef Array<float,16> arrtype;
				stackarray(pose_float, arrtype, totalsize);
				size_t index = 0;
				for(const auto& array : row.second.skinningdata){
					//in case of double mode, need to convert to float
					for(int i = 0; i < array.size(); i++){
						//populate stack array values
						auto ptr = glm::value_ptr(array[i]);
						for(int offset = 0; offset < 16; offset++){
							pose_float[index][offset] = static_cast<float>(ptr[offset]);
						}
						index++;
					}
				}
				assert(totalsize < numeric_limits<uint32_t>::max());	// pose buffer is too big!
				auto poseStart = poseStorageBuffer.AddData(reinterpret_cast<uint8_t*>(pose_float),static_cast<uint32_t>(totalsize), skinningInputLayout);
				
				// set skinning uniform
				values[0] = static_cast<float>(numobjects);
				values[1] = static_cast<float>(numverts);
				values[2] = static_cast<float>(skeleton->GetBindposes().size());
				values[3] = static_cast<float>(poseStart);
				numRowsUniform.SetValues(&values, 1);
				
				float offsets[4] = {static_cast<float>(computeOffsetIndex),0,0,0};
				computeOffsetsUniform.SetValues(&offsets, 1);
				
				bgfx::setBuffer(1, poseStorageBuffer.GetHandle(), bgfx::Access::Read);
				bgfx::dispatch(Views::DeferredGeo, skinningShaderHandle, std::ceil(numobjects / 8.0), std::ceil(numverts / 32.0), 1);	//objects x number of vertices to pose
			}
		}, [&computeOffsetIndex, &values, this]() {
			values[3] = static_cast<float>(computeOffsetIndex);
			numRowsUniform.SetValues(&values, 1);
			bgfx::setBuffer(11, skinningComputeBuffer.GetHandle(), bgfx::Access::Read);
		});
	}

	// debug: draw the unified mesh
	/*bgfx::discard();
	bgfx::setVertexBuffer(0,allVerticesHandle);
	bgfx::setIndexBuffer(allIndicesHandle,allIndicesOffset);
	bgfx::submit(Views::FinalBlit, rve_debugShaderHandle);
	bgfx::discard();*/

	// Lighting pass
   
    uint32_t shadowOffset = 0;
	bgfx::ViewId currentShadowView = Views::LightingShadowsFirstView;
	constexpr auto size = 20;
	auto dlProjMtx = glm::ortho<float>(-size, size, -size, size, 0.5, 100);	// TODO: don't hardcode far clip
	float shadowprojviewmtx[32]{0};

	/**
	Execute instanced draw calls for a given light type
	@param components the componetstore of the world to get the lights from
	@return true light draw calls were executed, false otherwise
	*/
    const auto DrawLightsOfType = [&](const auto& lights, float lighttype) -> void{
		struct DrawLightsResult {
			uint32_t numDrawn = 0;
			uint32_t shadowDataBegin;
			bgfx::InstanceDataBuffer lightdata;
		};
        using LightType = typename std::remove_reference<decltype(lights)>::type::value_type::light_t;
        DrawLightsResult dr;
        //must set before changing shaders
        if (lights.size() == 0){
            return;
        }
        
        constexpr auto stride = LightType::InstancingStride();
        auto numLights = lights.size();
        
        //create buffer for GPU instancing
        auto mem = bgfx::alloc(Debug::AssertSize<uint32_t>(numLights * stride));

        //fill the buffer
        int i = 0;
        for(const auto& l : lights){
			if (l.CastsShadows()) {
				float* ptr = (float*)(mem->data + i);
				l.AddInstanceData(ptr);
				i += stride;

				float lightViewMtx[16];
				const char* Lname = nullptr;
				if constexpr (std::is_same_v<LightType, DirectionalLight>) {
					Lname = "DL";
					auto dirlightViewMat = glm::lookAt(vector3(l.rotation.x, l.rotation.y, l.rotation.z) * -25.f, vector3(0, 0, 0), vector3(0, 1, 0));
					//dirlightViewMat = glm::translate(dirlightViewMat, vector3(fd->viewmatrix[3][2], 0, fd->viewmatrix[3][0]));		// center the projection at the camera
					copyMat4(glm::value_ptr(dirlightViewMat), lightViewMtx);
					copyMat4(glm::value_ptr(dlProjMtx), shadowprojviewmtx);
				}
				else if constexpr (std::is_same_v<LightType, SpotLight>) {
					Lname = "SL";
				}
				else if constexpr (std::is_same_v<LightType, PointLight>) {
					Lname = "PL";
				}

				bgfx::setViewFrameBuffer(currentShadowView, depthMapFB);
				bgfx::setViewName(currentShadowView, fmt::format("Depth Shadow {}", Lname).c_str());
				bgfx::setViewClear(currentShadowView, BGFX_CLEAR_DEPTH | BGFX_CLEAR_COLOR, 1);;
				bgfx::setViewRect(currentShadowView, 0, 0, shadowMapSize, shadowMapSize);

				bgfx::setViewTransform(currentShadowView, lightViewMtx, shadowprojviewmtx);
				// submit whole scene mesh	
				bgfx::setVertexBuffer(0, allVerticesHandle);
				bgfx::setIndexBuffer(allIndicesHandle,0,allIndicesOffset);	// specify count
				bgfx::setState(BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_CULL_CW | BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_Z);
				bgfx::submit(currentShadowView, shadowMapShaderHandle);

				currentShadowView++;
				Debug::Assert(currentShadowView != Views::FinalBlit, "Maximum number of shadow-casting lights exceeded.");

				bgfx::setViewName(currentShadowView, fmt::format("Depth Light {}", Lname).c_str());
				bgfx::setViewFrameBuffer(currentShadowView, lightingBuffer);
				bgfx::setViewTransform(currentShadowView,viewmat, projmat);
				bgfx::setViewRect(currentShadowView, 0,0, bufferdims.width, bufferdims.height);
				bgfx::setViewClear(currentShadowView, BGFX_CLEAR_NONE);	// contribute to existing data


				//set the required state for this light type
				LightType::SetState();
				bgfx::setInstanceCount(Debug::AssertSize<uint32_t>(numLights));

				// bind buffer for writing shadow data
				bgfx::setBuffer(11, lightDataHandle, bgfx::Access::Read);

				// bind shadow data map and depth map
				bgfx::setTexture(0, gBufferSamplers[0], attachments[0]);					// albedo
				bgfx::setTexture(1, gBufferSamplers[1], attachments[1]);					// normal
				bgfx::setTexture(4, shadowSamplers[0], bgfx::getTexture(depthMapFB, 0));	// shadow data buffer
				bgfx::setTexture(3, shadowSamplers[1], bgfx::getTexture(depthMapFB, 1));	// shadow depth buffer
				bgfx::setTexture(2, gBufferSamplers[2], attachments[2]);					// geometry position buffer (for transforming points to lightmap)
				copyMat4(lightViewMtx, &shadowprojviewmtx[16]);
				bgfx::setTransform(shadowprojviewmtx, 2);				// these become u_model[0] and u_model[1]

				bgfx::setBuffer(12, allVerticesHandle, bgfx::Access::Read);
				bgfx::setBuffer(13, allIndicesHandle, bgfx::Access::Read);

				//execute instance draw call
				float uniformData[] = { static_cast<float>(shadowOffset / sizeof(float)), true,0,0 };        // start points for reading shadow data ( beginOffset is in bytes but we want floats, second value is if shadows are enabled
				numRowsUniform.SetValues(uniformData, 1);

				LightType::Draw(currentShadowView);

				currentShadowView++;
				Debug::Assert(currentShadowView != Views::FinalBlit, "Maximum number of shadow-casting lights exceeded.");
			}
        }
		numLights = i / stride;	// get the actual number of lights submitted
			
        bgfx::update(lightDataHandle, shadowOffset, mem);
        dr.numDrawn = Debug::AssertSize<uint32_t>(numLights);
        
        
        // need to update this here because the shadow pass uses that information
        shadowOffset += numLights * stride;
    };
 
    
    DrawLightsOfType(fd->directionals,0);
    //DrawLightsOfType(fd->points,1);
	//DrawLightsOfType(fd->spots, 2);

	const auto DrawLightsOfTypeNoShadow = [&](const auto& lights) -> void {
		
		struct DrawLightsResult {
			uint32_t numDrawn = 0;
			uint32_t shadowDataBegin;
			bgfx::InstanceDataBuffer lightdata;
		};
		using LightType = typename std::remove_reference<decltype(lights)>::type::value_type::light_t;
		DrawLightsResult dr;
		//must set before changing shaders
		if (lights.size() == 0) {
			return;
		}

		constexpr auto stride = LightType::InstancingStride();
		auto numLights = lights.size();

		//create buffer for GPU instancing
		auto mem = bgfx::alloc(Debug::AssertSize<uint32_t>(numLights * stride));

		//fill the buffer
		int i = 0;
		for (const auto& l : lights) {
			if (!l.CastsShadows()) {
				float* ptr = (float*)(mem->data + i);
				l.AddInstanceData(ptr);
				i += stride;
			}
		}
		numLights = i;	// get the actual number of lights
        if (numLights == 0){
            return; // don't submit 0 lights
        }

		bgfx::update(lightDataHandle, shadowOffset, mem);
		dr.numDrawn = Debug::AssertSize<uint32_t>(numLights);

		//set the required state for this light type
		LightType::SetState();
		bgfx::setInstanceCount(Debug::AssertSize<uint32_t>(numLights));

		// bind buffer for writing shadow data
		bgfx::setBuffer(11, lightDataHandle, bgfx::Access::Read);

		//execute instance draw call
		for (int i = 0; i < RenderEngine::gbufferSize; i++) {
			bgfx::setTexture(i, gBufferSamplers[i], attachments[i]);
		}
		float uniformData[] = { static_cast<float>(shadowOffset / sizeof(float)), false,0,0 };        // start points for reading shadow data ( beginOffset is in bytes but we want floats, second value is if shadows are enabled
		numRowsUniform.SetValues(uniformData, 1);

		LightType::Draw(RenderEngine::Views::LightingNoShadows);    //view 2 is the lighting pass
		shadowOffset += numLights * stride;
	};

	DrawLightsOfTypeNoShadow(fd->directionals);
	DrawLightsOfTypeNoShadow(fd->points);
	DrawLightsOfTypeNoShadow(fd->spots);
	DrawLightsOfTypeNoShadow(fd->ambients);


	// lighting is complete, so next we draw the skybox
	bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_CULL_CW | BGFX_STATE_DEPTH_TEST_EQUAL);
	auto& sb = worldOwning->skybox;
    if (sb && sb->enabled){
        auto transform = glm::translate(matrix4(1),fd->cameraWorldpos);
        sb->Draw(transform,Views::FinalBlit);
    }
		
	//blit to view 0 using the fullscreen quad
	bgfx::setTexture(0, lightingSamplers[0], lightingAttachments[0]);
	bgfx::setTexture(1, lightingSamplers[1], lightingAttachments[1]);

	bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_ALWAYS | BGFX_STATE_LINEAA);	//don't clear depth, debug wireframes are drawn forward-style afterwards
	blitShader->Draw(screenSpaceQuadVert, screenSpaceQuadInd, Views::FinalBlit);
	
	// render gui
	auto fng = [](float, auto& gui) {
		gui.Render();	//bgfx state is set in renderer before actual draw calls
	};
	// update GUIs
	worldOwning->Filter<GUIComponent>(fng);
	
#ifdef _DEBUG
	// process debug shapes
	auto fn = [](auto scale, auto dbg, const auto transform){
		for(int i = 0; i < dbg.size(); i++){
			auto& ptr = dbg[i];
			if (ptr.debugEnabled){
				ptr.DebugDraw(dbgdraw,transform[0]);
			}
		}
	};
	worldOwning->FilterPolymorphic<IDebugRenderable, Transform>(fn);
	Im3d::GetContext().draw();
	if (debuggerContext){
		auto& dbg = *debuggerContext;
		dbg.SetDimensions(bufferdims.width, bufferdims.height);
		dbg.SetDPIScale(GetDPIScale());
		dbg.Update();
		dbg.Render();
	}
#endif
	bgfx::frame();
	skinningComputeBuffer.Reset();
	poseStorageBuffer.Reset();

#ifdef _DEBUG
	Im3d::NewFrame();
#endif
	bgfx::dbgTextClear();

	auto stats = bgfx::getStats();

	auto delta = std::chrono::duration<float, std::milli>(
#if defined _WIN32
		std::chrono::duration<int64_t, std::ratio<1, 10000000000>>(stats->cpuTimeFrame)
#else
		std::chrono::high_resolution_clock::time_point::duration(stats->cpuTimeFrame)
#endif
		);

	currentFrameTime = delta.count();
#if __APPLE__
    // don't need to update this, it is available on-demand via call
#else
    currentVRAM = stats->gpuMemoryUsed / 1024 / 1024;
    totalVRAM = stats->gpuMemoryMax / 1024 / 1024;
#endif
}

void RenderEngine::resize(){
	UpdateBufferDims();
#if BX_PLATFORM_IOS
	//view must be manually sized on iOS
	//also this API takes screen points not pixels
	resizeMetalLayer(metalLayer,windowdims.width, windowdims.height);
#endif
	auto& dims = bufferdims;
#if XR_AVAILABLE
	if (GetApp()->wantsXR) {
		dims = GetVRFrameBuffers().l_eye.dims;
	}
#endif
	bgfx::reset(dims.width, dims.height, GetResetFlags());
	bgfx::setViewRect(Views::FinalBlit, 0, 0, uint16_t(dims.width), uint16_t(dims.height));
}

void RenderEngine::SyncVideoSettings(){
	SDL_SetWindowSize(window, VideoSettings.width, VideoSettings.height);
}

/**
@return the name of the current rendering API
*/
const string_view RenderEngine::GetCurrentBackendName(){
	
	return BackendStringName(bgfx::getRendererType());
}

const string_view RenderEngine::BackendStringName(bgfx::RendererType::Enum backend) {
	switch (backend) {
		case bgfx::RendererType::Noop:			return "Disabled";
		case bgfx::RendererType::Direct3D9:		return "DirectX9";
		case bgfx::RendererType::Direct3D11:	return "DirectX11";
		case bgfx::RendererType::Direct3D12:	return "DirectX12";
		case bgfx::RendererType::Gnm:			return "GNM";
		case bgfx::RendererType::Metal:			return "Metal";
		case bgfx::RendererType::OpenGL:		return "OpenGL";
		case bgfx::RendererType::OpenGLES:		return "OpenGL ES";
		case bgfx::RendererType::Vulkan:		return "Vulkan";
		case bgfx::RendererType::Nvn:			return "NVN";
		case bgfx::RendererType::WebGPU:		return "WebGPU";
		default: 		                        return "Error";
	}
}

float RavEngine::RenderEngine::GetCurrentFPS()
{
	return 1.0f / currentFrameTime;
}

float RavEngine::RenderEngine::GetLastFrameTime()
{
	return currentFrameTime * 1000;
}

uint32_t RenderEngine::GetResetFlags(){
	return (VideoSettings.vsync ? BGFX_RESET_VSYNC : BGFX_RESET_NONE);
}

bgfx::FrameBufferHandle RenderEngine::createFrameBuffer(bool hdr, bool depth)
{
	bgfx::TextureHandle textures[2];
	uint8_t attachments = 0;
	
	const uint64_t samplerFlags = BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_MIP_POINT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
	
	bgfx::TextureFormat::Enum format =
	hdr ? bgfx::TextureFormat::RGBA16F : bgfx::TextureFormat::BGRA8; // BGRA is often faster (internal GPU format)
	assert(bgfx::isTextureValid(0, false, 1, format, BGFX_TEXTURE_RT | samplerFlags));
	textures[attachments++] = bgfx::createTexture2D(TexRatio, false, 1, format, BGFX_TEXTURE_RT | samplerFlags);
	
	if(depth)
	{
		//bgfx::TextureFormat::Enum depthFormat = findDepthFormat(BGFX_TEXTURE_RT_WRITE_ONLY | samplerFlags);
		bgfx::TextureFormat::Enum depthFormat = bgfx::TextureFormat::D32F;
		assert(depthFormat != bgfx::TextureFormat::Enum::Count);
		textures[attachments++] = bgfx::createTexture2D(TexRatio, false, 1, depthFormat, BGFX_TEXTURE_RT_WRITE_ONLY | samplerFlags);
	}
	
	bgfx::FrameBufferHandle fb = bgfx::createFrameBuffer(attachments, textures, true);
	
	if(!bgfx::isValid(fb))
		Debug::Fatal("Failed to create framebuffer");
	
	return fb;
}

void RenderEngine::UpdateBufferDims(){
	// on non-apple platforms this is in pixels, on apple platforms it is in "screen points"
	// which will be dealt with later
	SDL_GetWindowSize(window, &windowdims.width, &windowdims.height);

	// update bufferdims
	bufferdims.width = windowdims.width;
	bufferdims.height = windowdims.height;

	// get the canvas size in pixels
# if BX_PLATFORM_WINDOWS
	
	SDL_SysWMinfo wmi;
	SDL_VERSION(&wmi.version);
	if (!SDL_GetWindowWMInfo(window, &wmi)) {
		Debug::Fatal("Cannot get native window information");
	}
	auto monitor = MonitorFromWindow(wmi.info.win.window, MONITOR_DEFAULTTONEAREST);
	DEVICE_SCALE_FACTOR fac;
	if (GetScaleFactorForMonitor(monitor,&fac) == S_OK) {
		win_scalefactor = (static_cast<int>(fac) / 100.0);
	}
	else {
		Debug::Fatal("GetScaleFactorForMonitor failed");
	}
#elif BX_PLATFORM_WINRT
	auto dinf = Windows::Graphics::Display::DisplayInformation::GetForCurrentView();
	win_scalefactor = static_cast<int32_t>(dinf.ResolutionScale()) / 100.0;
#elif BX_PLATFORM_IOS || BX_PLATFORM_OSX
	// since iOS and macOS do not use OpenGL we cannot use the GL call here
	// instead we derive it by querying display data
	float scale = GetWindowScaleFactor(window);
	bufferdims.width = windowdims.width * scale;
	bufferdims.height = windowdims.height * scale;
#endif
}

#ifdef _DEBUG
void RenderEngine::InitDebugger() const{
	if (!debuggerContext){
		Im3d::AppData& data = Im3d::GetAppData();
		data.drawCallback = &DebugRender;
		
		debuggerContext.emplace(10,10);
		auto ctxd = (*debuggerContext).GetData();
		bool status = Rml::Debugger::Initialise(ctxd->context);
		
		debuggerInput = make_unique<InputManager>();
		
		debuggerInput->BindAnyAction(ctxd);
		debuggerInput->AddAxisMap("MouseX", Special::MOUSEMOVE_X);
		debuggerInput->AddAxisMap("MouseY", Special::MOUSEMOVE_Y);
		
		debuggerInput->AddAxisMap("ScrollY", Special::MOUSEWHEEL_Y);

		auto dbg = PointerInputBinder<GUIComponent>(&*debuggerContext);
		debuggerInput->BindAxis("MouseX", dbg, &GUIComponent::MouseX, CID::ANY, 0);	//no deadzone
		debuggerInput->BindAxis("MouseY", dbg, &GUIComponent::MouseY, CID::ANY, 0);
		debuggerInput->BindAxis("ScrollY", dbg, &GUIComponent::ScrollY, CID::ANY, 0);
	}
}

void RenderEngine::DeactivateDebugger() const{
	debuggerContext.reset();
	debuggerInput = nullptr;
}
#endif

void RenderEngine::SetWindowMode(WindowMode mode){
    int flag;
    switch(mode){
        case WindowMode::Windowed:
            flag = 0;
            break;
        case WindowMode::BorderlessFullscreen:
            flag = SDL_WINDOW_FULLSCREEN_DESKTOP;
            break;
        case WindowMode::Fullscreen:
            flag = SDL_WINDOW_FULLSCREEN;
            break;
    }
    SDL_SetWindowFullscreen(window, flag);
}
