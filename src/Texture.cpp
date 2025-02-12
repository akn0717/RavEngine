#include "Texture.hpp"
#include <bimg/bimg.h>
#include <bx/bx.h>
#include <bx/readerwriter.h>
#include "App.hpp"
//#define STB_IMAGE_IMPLEMENTATION // don't define here, because rlottie & bimg define them
#include <stb_image.h>
#include "Debug.hpp"
#include <lunasvg.h>
#include "Filesystem.hpp"

using namespace std;
using namespace RavEngine;

Ref<RuntimeTexture> TextureManager::defaultTexture;

inline static bool IsRasterImage(const std::string& filepath){
    // assume that anything not in the whitelist is a raster image
    if (Filesystem::Path(filepath).extension() == ".svg"){
        return false;
    }
    return true;
}

Texture::Texture(const std::string& name, uint16_t width, uint16_t height){
    Debug::Assert(!IsRasterImage(name), "This texture constructor only allows vector image formats");
    
    RavEngine::Vector<uint8_t> data;
	GetApp()->GetResources().FileContentsAt(StrFormat("/textures/{}", name).c_str(),data);
    
    // load the SVG
    auto document = lunasvg::Document::loadFromData(reinterpret_cast<char*>(data.data()), data.size());
    auto bitmap = document->renderToBitmap(width,height);
    
    // give to BGFX
    CreateTexture(width, height, false, 1, bitmap.data());
}

RavEngine::Texture::Texture(const Filesystem::Path& pathOnDisk)
{
	int width, height, channels;
	unsigned char* bytes = stbi_load(pathOnDisk.string().c_str(), &width, &height, &channels, 4);

	CreateTexture(width,height,false,1,bytes);
	stbi_image_free(bytes);
}

Texture::Texture(const std::string& name){
    Debug::Assert(IsRasterImage(name), "This texture constructor only allows raster image formats");
    
	//read from resource
	
    RavEngine::Vector<uint8_t> data;
	GetApp()->GetResources().FileContentsAt(("/textures/" + name).c_str(),data);
	
	int width, height,channels;
	auto compressed_size = sizeof(stbi_uc) * data.size();
	
    unsigned char* bytes = stbi_load_from_memory(&data[0], Debug::AssertSize<int>(compressed_size), &width, &height, &channels, 4);
	if (bytes == nullptr){
		Debug::Fatal("Cannot load texture: {}",stbi_failure_reason());
	}
	
	bool hasMipMaps = false;
	uint16_t numlayers = 1;
	
	CreateTexture(width, height, hasMipMaps, numlayers, bytes);
	stbi_image_free(bytes);
	
}

void Texture::Bind(int id, const SamplerUniform &uniform){
	bgfx::setTexture(id, uniform, texture);
}

void Texture::CreateTexture(int width, int height, bool hasMipMaps, int numlayers, const uint8_t *data, int flags){
	uint16_t numChannels = 4;	//TODO: allow n-channel textures
	bgfx::TextureFormat::Enum format;
	switch (numChannels) {
	case 1:
		format = bgfx::TextureFormat::R8;
		break;
	case 2:
		format = bgfx::TextureFormat::RG8;
		break;
	case 3:
		format = bgfx::TextureFormat::RGB8;
		break;
	case 4: 
		format = bgfx::TextureFormat::RGBA8;
		break;
	default:
		Debug::Fatal("Number of channels must be [1,4], got {}",numChannels);
	}
	
	auto uncompressed_size = width * height * numChannels * numlayers;
	const bgfx::Memory* textureData = (data == nullptr) ? nullptr : bgfx::copy(data, uncompressed_size);
	texture = bgfx::createTexture2D(width,height,hasMipMaps,numlayers,format,flags,textureData);
	
	if(!bgfx::isValid(texture)){
		Debug::Fatal("Cannot create texture");
	}
}
