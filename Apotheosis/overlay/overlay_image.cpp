#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "overlay_image.h"

// Each TU that uses stb_image must own a private static copy because the
// other consumer (scr/other_tools.cpp) builds it with STB_IMAGE_STATIC.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <vector>
#include <cstdio>

namespace
{
    bool CreateTextureFromRGBA(ID3D11Device* device, const unsigned char* rgba, int w, int h, ID3D11ShaderResourceView** outSrv)
    {
        if (!device || !rgba || w <= 0 || h <= 0)
            return false;

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(w);
        desc.Height = static_cast<UINT>(h);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem = rgba;
        init.SysMemPitch = static_cast<UINT>(w * 4);

        ID3D11Texture2D* tex = nullptr;
        HRESULT hr = device->CreateTexture2D(&desc, &init, &tex);
        if (FAILED(hr) || !tex)
            return false;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = desc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;

        hr = device->CreateShaderResourceView(tex, &srvDesc, outSrv);
        tex->Release();
        return SUCCEEDED(hr);
    }
}

bool OverlayImage_LoadFromFile(const char* utf8Path, ID3D11Device* device, OverlayImage& out)
{
    out = {};
    if (!utf8Path || !device)
        return false;

    int w = 0, h = 0, comp = 0;
    unsigned char* pixels = stbi_load(utf8Path, &w, &h, &comp, 4);
    if (!pixels)
        return false;

    ID3D11ShaderResourceView* srv = nullptr;
    const bool ok = CreateTextureFromRGBA(device, pixels, w, h, &srv);
    stbi_image_free(pixels);

    if (!ok)
        return false;

    out.srv = srv;
    out.width = w;
    out.height = h;
    return true;
}

void OverlayImage_Release(OverlayImage& img)
{
    if (img.srv)
    {
        img.srv->Release();
        img.srv = nullptr;
    }
    img.width = 0;
    img.height = 0;
}

HICON OverlayImage_LoadHIconFromFile(const char* utf8Path)
{
    if (!utf8Path) return NULL;
    int w = 0, h = 0, comp = 0;
    unsigned char* px = stbi_load(utf8Path, &w, &h, &comp, 4);
    if (!px) return NULL;
    HICON hIcon = OverlayImage_CreateHIconFromRGBA(px, w, h);
    stbi_image_free(px);
    return hIcon;
}

HICON OverlayImage_CreateHIconFromRGBA(const unsigned char* rgba, int width, int height)
{
    if (!rgba || width <= 0 || height <= 0)
        return NULL;

    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(BITMAPV5HEADER);
    bi.bV5Width = width;
    bi.bV5Height = -height; // top-down
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    HDC hdc = ::GetDC(NULL);
    void* bits = nullptr;
    HBITMAP hColor = ::CreateDIBSection(hdc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, NULL, 0);
    ::ReleaseDC(NULL, hdc);

    if (!hColor || !bits)
    {
        if (hColor) ::DeleteObject(hColor);
        return NULL;
    }

    // stb gives RGBA, DIB wants BGRA (with our mask layout above).
    unsigned char* dst = static_cast<unsigned char*>(bits);
    const int n = width * height;
    for (int i = 0; i < n; ++i)
    {
        const unsigned char r = rgba[i * 4 + 0];
        const unsigned char g = rgba[i * 4 + 1];
        const unsigned char b = rgba[i * 4 + 2];
        const unsigned char a = rgba[i * 4 + 3];
        dst[i * 4 + 0] = b;
        dst[i * 4 + 1] = g;
        dst[i * 4 + 2] = r;
        dst[i * 4 + 3] = a;
    }

    HBITMAP hMask = ::CreateBitmap(width, height, 1, 1, NULL);
    if (!hMask)
    {
        ::DeleteObject(hColor);
        return NULL;
    }

    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmMask = hMask;
    ii.hbmColor = hColor;
    HICON hIcon = ::CreateIconIndirect(&ii);

    ::DeleteObject(hColor);
    ::DeleteObject(hMask);
    return hIcon;
}
