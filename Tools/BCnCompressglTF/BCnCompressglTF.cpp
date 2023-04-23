#include <App/Filesystem.h>
#include <App/Common.h>
#include <Support/MemoryArena.h>
#include "TexConv/texconv.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>

#define JSON_NOEXCEPTION
#define JSON_NO_IO
#include <json/json.hpp>
using json = nlohmann::json;

#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

using namespace ZetaRay;
using namespace ZetaRay::App;
using namespace ZetaRay::Util;
using namespace ZetaRay::Support;

namespace
{
    static constexpr int MAX_TEX_RES = 2048;
    static constexpr const char* COMPRESSED_DIR_NAME = "compressed";

    namespace TEX_CONV_ARGV_NO_OVERWRITE_SRGB
    {
        static const char* CMD = " -w %d -h %d -m 0 -ft dds -f %s -srgb -nologo -o %s %s";
        constexpr int NUM_ARGS = 16;
    };

    namespace TEX_CONV_ARGV_OVERWRITE_SRGB
    {
        static const char* CMD = " -w %d -h %d -m 0 -ft dds -f %s -srgb -nologo -y -o %s %s";
        constexpr int NUM_ARGS = 17;
    }

    namespace TEX_CONV_ARGV_NO_OVERWRITE
    {
        static const char* CMD = " -w %d -h %d -m 0 -ft dds -f %s -nologo -o %s %s";
        constexpr int NUM_ARGS = 15;
    }

    namespace TEX_CONV_ARGV_OVERWRITE
    {
        static const char* CMD = " -w %d -h %d -m 0 -ft dds -f %s -nologo -y -o %s %s";
        constexpr int NUM_ARGS = 16;
    }

    static constexpr int MAX_NUM_ARGS = Math::Max(
        Math::Max(TEX_CONV_ARGV_NO_OVERWRITE_SRGB::NUM_ARGS, TEX_CONV_ARGV_OVERWRITE_SRGB::NUM_ARGS),
        Math::Max(TEX_CONV_ARGV_NO_OVERWRITE::NUM_ARGS, TEX_CONV_ARGV_OVERWRITE::NUM_ARGS));

    enum TEXTURE_TYPE
    {
        BASE_COLOR,
        NORMAL_MAP,
        METALNESS_ROUGHNESS,
        EMISSIVE
    };

    const char* GetTexFormat(TEXTURE_TYPE t) noexcept
    {
        switch (t)
        {
        case BASE_COLOR:
            return "BC7_UNORM_SRGB";
        case NORMAL_MAP:
            return "BC5_UNORM";
        case METALNESS_ROUGHNESS:
            return "BC5_UNORM";
        case EMISSIVE:
            return "BC7_UNORM_SRGB";
        default:
            Check(false, "unreachable case.");
            return "";
        }
    }

    void CreateDevice(ID3D11Device** pDevice) noexcept
    {
        Assert(pDevice, "invalid arg.");
        *pDevice = nullptr;

        const D3D_FEATURE_LEVEL featureLevels[] =
        {
            D3D_FEATURE_LEVEL_11_0,
        };

        ComPtr<IDXGIFactory1> dxgiFactory;
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(dxgiFactory.GetAddressOf()));
        Check(SUCCEEDED(hr), "CreateDXGIFactory1() failed with code: %d", hr);

        ComPtr<IDXGIAdapter> pAdapter;
        if (FAILED(dxgiFactory->EnumAdapters(0, pAdapter.GetAddressOf())))
            Check(false, "ERROR: Invalid GPU adapter index 0!\n");

        hr = D3D11CreateDevice(pAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN,
            nullptr, 0, featureLevels, 1,
            D3D11_SDK_VERSION, pDevice, nullptr, nullptr);

        Check(SUCCEEDED(hr), "D3D11CreateDevice() failed with code: %d", hr);

        ComPtr<IDXGIDevice> dxgiDevice;
        hr = (*pDevice)->QueryInterface(IID_PPV_ARGS(dxgiDevice.GetAddressOf()));
        if (SUCCEEDED(hr))
        {
            hr = dxgiDevice->GetAdapter(pAdapter.ReleaseAndGetAddressOf());
            if (SUCCEEDED(hr))
            {
                DXGI_ADAPTER_DESC desc;
                hr = pAdapter->GetDesc(&desc);
                if (SUCCEEDED(hr))
                {
                    wprintf(L"\n[Using DirectCompute on \"%ls\"]\n", desc.Description);
                }
            }
        }
    }

    bool CompressedExists(const Filesystem::Path& imagePath, const Filesystem::Path& outDir) noexcept
    {
        char filename[MAX_PATH];
        size_t fnLen;
        imagePath.Stem(filename, &fnLen);

        // change the extension to dds
        Check(fnLen + 5 < ZetaArrayLen(filename), "buffer is too small.");
        filename[fnLen] = '.';
        filename[fnLen + 1] = 'd';
        filename[fnLen + 2] = 'd';
        filename[fnLen + 3] = 's';
        filename[fnLen + 4] = '\0';

        Filesystem::Path compressedPath(outDir.Get());
        compressedPath.Append(filename);

        if (Filesystem::Exists(compressedPath.Get()))
        {
            printf("Compressed texture already exists in path %s. Skipping...\n", compressedPath.Get());
            return true;
        }

        return false;
    }

    void ConvertTextures(TEXTURE_TYPE texType, const Filesystem::Path& pathToglTF, const Filesystem::Path& outDir,
        Span<int> textureMaps, Span<Filesystem::Path> imagePaths, ID3D11Device* device, bool srgb, bool forceOverwrite)
    {
        for (auto tex : textureMaps)
        {
            // URI paths are relative to gltf file
            Filesystem::Path imgPath(pathToglTF.Get());
            imgPath.Directory();

            auto& imgUri = imagePaths[tex];
            imgPath.Append(imgUri.Get());

            if (!forceOverwrite && CompressedExists(imgPath, outDir))
                continue;

            int x;
            int y;
            int comp;
            Check(stbi_info(imgPath.Get(), &x, &y, &comp), "stbi_info() for path %s failed: %s", imgPath.Get(), stbi_failure_reason());

            int w = std::min(x, MAX_TEX_RES);
            int h = std::min(y, MAX_TEX_RES);

            // Direct3D requires BC image to be multiple of 4 in width & height
            w = (int)Math::AlignUp(w, 4);
            h = (int)Math::AlignUp(h, 4);

            char buff[512];
            const char* texFormat = GetTexFormat(texType);
            const char* formatStr = srgb ?
                (forceOverwrite ? TEX_CONV_ARGV_OVERWRITE_SRGB::CMD : TEX_CONV_ARGV_NO_OVERWRITE_SRGB::CMD) :
                (forceOverwrite ? TEX_CONV_ARGV_OVERWRITE::CMD : TEX_CONV_ARGV_NO_OVERWRITE::CMD);
            const int len = stbsp_snprintf(buff, sizeof(buff), formatStr, w, h, texFormat, outDir.Get(), imgPath.Get());
            Check(len < sizeof(buff), "Provided buffer is too small.");

            wchar_t wideBuff[1024];
            int n = Common::CharToWideStr(buff, wideBuff);

            wchar_t* ptr = wideBuff;

            const int numArgs = srgb ? 
                (forceOverwrite ? TEX_CONV_ARGV_OVERWRITE_SRGB::NUM_ARGS : TEX_CONV_ARGV_NO_OVERWRITE_SRGB::NUM_ARGS) : 
                (forceOverwrite ? TEX_CONV_ARGV_OVERWRITE::NUM_ARGS : TEX_CONV_ARGV_NO_OVERWRITE::NUM_ARGS);
            wchar_t* args[MAX_NUM_ARGS];
            int currArg = 0;

            while (ptr != wideBuff + n)
            {
                args[currArg++] = ptr;

                while (*ptr != ' ' && *ptr != '\0')
                    ptr++;

                *ptr++ = '\0';
            }

            if (TexConv(numArgs, args, device) != 0)
                return;
        }
    }

    void ModifyImageURIs(json& data, const char* compressedDirName, const Filesystem::Path& gltfPath) noexcept
    {
        std::string s;
        char filename[MAX_PATH];

        size_t fnLen;

        for (auto& img : data["images"])
        {
            s = img["uri"];

            // extract the image file name
            Filesystem::Path p((StrView(s.data(), s.size())));
            p.Stem(filename, &fnLen);

            // change the extension to dds
            Check(fnLen + 5 < ZetaArrayLen(filename), "buffer is too small.");
            filename[fnLen] = '.';
            filename[fnLen + 1] = 'd';
            filename[fnLen + 2] = 'd';
            filename[fnLen + 3] = 's';
            filename[fnLen + 4] = '\0';

            // URI paths are relative to gltf file
            Filesystem::Path newPath(compressedDirName);
            newPath.Append(filename);

            img["uri"] = newPath.Get();
        }
        
        gltfPath.Stem(filename, &fnLen);

        Check(fnLen + 10 < ZetaArrayLen(filename), "buffer is too small.");
        filename[fnLen] = '_';
        filename[fnLen + 1] = 'z';
        filename[fnLen + 2] = 'e';
        filename[fnLen + 3] = 't';
        filename[fnLen + 4] = 'a';
        filename[fnLen + 5] = '.';
        filename[fnLen + 6] = 'g';
        filename[fnLen + 7] = 'l';
        filename[fnLen + 8] = 't';
        filename[fnLen + 9] = 'f';
        filename[fnLen + 10] = '\0';

        Filesystem::Path convertedPath(gltfPath.Get());
        convertedPath.Directory().Append(filename);

        s = data.dump(4);
        uint8_t* str = reinterpret_cast<uint8_t*>(s.data());
        Filesystem::WriteToFile(convertedPath.Get(), str, (uint32_t)s.size());
    }
}

int main(int argc, char* argv[])
{
    if (argc < 2 || argc > 3)
    {
        printf("Usage: BCnCompressglTF <path-to-glTF> -y\n");
        return 0;
    }

    Filesystem::Path gltfPath(argv[1]);
    if (!Filesystem::Exists(gltfPath.Get()))
    {
        printf("Provided path %s was not found. Exiting...\n", gltfPath.Get());
        return 0;
    }

    bool forceOverwrite = false;

    for (int i = 2; i < argc; i++)
    {
        if (strcmp(argv[i], "-y") == 0)
            forceOverwrite = true;
    }

    printf("Compressing textures for %s...\n", argv[1]);

    MemoryArena arena(64 * 1024);

	SmallVector<uint8_t, Support::ArenaAllocator> file(arena);
	Filesystem::LoadFromFile(gltfPath.Get(), file);

	json data = json::parse(file.data(), file.data() + file.size(), nullptr, false);

    SmallVector<Filesystem::Path, Support::ArenaAllocator> imagePaths(arena);
    imagePaths.resize(data["images"].size());

    {
        std::string s;
        s.resize(MAX_PATH);
        int i = 0;

        for (auto& img : data["images"])
        {
            s = img["uri"];
            imagePaths[i++].Reset(StrView(s.data(), s.size()));
        }
    }

    const size_t numMats = data["materials"].size();

    SmallVector<int, Support::ArenaAllocator> normalMaps(arena);
    normalMaps.reserve(numMats);

    // extract normal map texture indices
    for (auto& mat : data["materials"])
    {
        if (mat.contains("normalTexture"))
        {
            const int texIdx = mat["normalTexture"]["index"];
            const int imgIdx = data["textures"][texIdx]["source"];
            normalMaps.push_back(imgIdx);
        }
    }

    SmallVector<int, Support::ArenaAllocator> baseColorMaps(arena);
    baseColorMaps.reserve(numMats);

    SmallVector<int, Support::ArenaAllocator> metalnessRoughnessMaps(arena);
    metalnessRoughnessMaps.reserve(numMats);

    SmallVector<int, Support::ArenaAllocator> emissiveMaps(arena);
    emissiveMaps.reserve(numMats);

    // extract pbr texture indices
    for (auto& mat : data["materials"])
    {
        if (mat.contains("pbrMetallicRoughness"))
        {
            auto& pbr = mat["pbrMetallicRoughness"];
            
            if (pbr.contains("baseColorTexture"))
            {
                const int texIdx = pbr["baseColorTexture"]["index"];
                const int imgIdx = data["textures"][texIdx]["source"];
                baseColorMaps.push_back(imgIdx);
            }

            if (pbr.contains("metallicRoughnessTexture"))
            {
                const int texIdx = pbr["metallicRoughnessTexture"]["index"];
                const int imgIdx = data["textures"][texIdx]["source"];
                metalnessRoughnessMaps.push_back(imgIdx);
            }
        }
    }

    // extract emissive map texture indices
    for (auto& mat : data["materials"])
    {
        if (mat.contains("emissiveTexture"))
        {
            const int texIdx = mat["emissiveTexture"]["index"];
            const int imgIdx = data["textures"][texIdx]["source"];
            emissiveMaps.push_back(imgIdx);
        }
    }

    printf("Stats:\n\
        #images: %llu \n\
        #textures: %llu\n\
        #base-color textures: %llu\n\
        #normal-map textures: %llu\n\
        #metalness-roughness textures: %llu\n\
        #emissive textures: %llu\n", imagePaths.size(), data["textures"].size(), baseColorMaps.size(), normalMaps.size(),
        metalnessRoughnessMaps.size(), emissiveMaps.size());
   
    ComPtr<ID3D11Device> device;
    CreateDevice(device.GetAddressOf());

    Filesystem::Path outDir(gltfPath.Get());
    outDir.Directory().Append(COMPRESSED_DIR_NAME);
    Filesystem::CreateDirectoryIfNotExists(outDir.Get());

    ConvertTextures(TEXTURE_TYPE::BASE_COLOR, gltfPath, outDir, baseColorMaps, imagePaths, device.Get(), true, forceOverwrite);
    ConvertTextures(TEXTURE_TYPE::NORMAL_MAP, gltfPath, outDir, normalMaps, imagePaths, device.Get(), false, forceOverwrite);
    ConvertTextures(TEXTURE_TYPE::METALNESS_ROUGHNESS, gltfPath, outDir, metalnessRoughnessMaps, imagePaths, device.Get(), false, forceOverwrite);
    ConvertTextures(TEXTURE_TYPE::EMISSIVE, gltfPath, outDir, emissiveMaps, imagePaths, device.Get(), true, forceOverwrite);

    ModifyImageURIs(data, COMPRESSED_DIR_NAME, gltfPath);

	return 0;
}