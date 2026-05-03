#ifndef OVERLAY_IMAGE_H
#define OVERLAY_IMAGE_H

#include <windows.h>
#include <d3d11.h>

struct OverlayImage
{
    ID3D11ShaderResourceView* srv = nullptr;
    int width = 0;
    int height = 0;
};

bool OverlayImage_LoadFromFile(const char* utf8Path, ID3D11Device* device, OverlayImage& out);
void OverlayImage_Release(OverlayImage& img);

// Build a HICON from a 32bpp RGBA buffer (e.g. stb_image output). Returns NULL on failure.
HICON OverlayImage_CreateHIconFromRGBA(const unsigned char* rgba, int width, int height);

// Decode an image from disk and turn it into an HICON. Returns NULL on failure.
HICON OverlayImage_LoadHIconFromFile(const char* utf8Path);

#endif // OVERLAY_IMAGE_H
