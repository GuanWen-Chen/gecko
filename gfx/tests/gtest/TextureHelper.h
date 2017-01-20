/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <vector>

#include "gfxImageSurface.h"
#include "gfxPlatform.h"
#include "mozilla/layers/BufferTexture.h"
#include "mozilla/layers/LayersTypes.h"
#include "mozilla/layers/TextureClient.h"
#include "mozilla/layers/TextureHost.h"
#include "mozilla/RefPtr.h"
#ifdef XP_WIN
#include "DeviceManagerD3D9.h"
#include "IMFYCbCrImage.h"
#include "mozilla/gfx/DeviceManagerDx.h"
#include "mozilla/layers/TextureD3D11.h"
#include "mozilla/layers/TextureD3D9.h"
#include "mozilla/layers/TextureDIB.h"
#endif

using mozilla::gfx::SurfaceFormat;

namespace mozilla {
namespace layers {
#ifdef XP_WIN
/**
 * This function creates a D3D9TextureData with given data.
 */
void
GetD3D9TextureData(PlanarYCbCrData& aClientData, TextureData*& aData)
{
  DeviceManagerD3D9::Get();
  RefPtr<IDirect3DDevice9> device = DeviceManagerD3D9::GetDevice();
  if (!device) {
    return;
  }

  RefPtr<IDirect3DTexture9> textureY;
  HANDLE shareHandleY = 0;
  if (!IMFYCbCrImage::UploadData(device, textureY, shareHandleY, aClientData.mYChannel,
                  aClientData.mYSize, aClientData.mYStride)) {
    return;
  }

  RefPtr<IDirect3DTexture9> textureCb;
  HANDLE shareHandleCb = 0;
  if (!IMFYCbCrImage::UploadData(device, textureCb, shareHandleCb, aClientData.mCbChannel,
                  aClientData.mCbCrSize, aClientData.mCbCrStride)) {
    return;
  }

  RefPtr<IDirect3DTexture9> textureCr;
  HANDLE shareHandleCr = 0;
  if (!IMFYCbCrImage::UploadData(device, textureCr, shareHandleCr, aClientData.mCrChannel,
                  aClientData.mCbCrSize, aClientData.mCbCrStride)) {
    return;
  }

  RefPtr<IDirect3DQuery9> query;
  HRESULT hr = device->CreateQuery(D3DQUERYTYPE_EVENT, getter_AddRefs(query));
  hr = query->Issue(D3DISSUE_END);

  int iterations = 0;
  bool valid = false;
  while (iterations < 10) {
    HRESULT hr = query->GetData(nullptr, 0, D3DGETDATA_FLUSH);
    if (hr == S_FALSE) {
      Sleep(1);
      iterations++;
      continue;
    }
    if (hr == S_OK) {
      valid = true;
    }
    break;
  }

  if (!valid) {
    return;
  }

  aData = DXGIYCbCrTextureData::Create(
    TextureFlags::DEALLOCATE_CLIENT, textureY, textureCb, textureCr,
    shareHandleY, shareHandleCb, shareHandleCr, IntSize(200, 150),
    aClientData.mYSize, aClientData.mCbCrSize);
}

TextureData*
CreateDXGID3D9TextureData(IntSize aSize, SurfaceFormat aFormat,
                          TextureFlags aTextureFlag)
{

  RefPtr<IDirect3D9Ex> d3d9Ex;
  HMODULE d3d9lib = LoadLibraryW(L"d3d9.dll");
  decltype(Direct3DCreate9Ex)* d3d9Create =
    (decltype(Direct3DCreate9Ex)*)GetProcAddress(d3d9lib, "Direct3DCreate9Ex");
  HRESULT hr = d3d9Create(D3D_SDK_VERSION, getter_AddRefs(d3d9Ex));

  if (!d3d9Ex) {
    return nullptr;
  }

  D3DPRESENT_PARAMETERS params = { 0 };
  params.BackBufferWidth = 1;
  params.BackBufferHeight = 1;
  params.BackBufferFormat = D3DFMT_A8R8G8B8;
  params.BackBufferCount = 1;
  params.SwapEffect = D3DSWAPEFFECT_DISCARD;
  params.hDeviceWindow = nullptr;
  params.Windowed = TRUE;
  params.Flags = D3DPRESENTFLAG_VIDEO;

  RefPtr<IDirect3DDevice9Ex> device;
  hr = d3d9Ex->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, nullptr,
                              D3DCREATE_FPU_PRESERVE | D3DCREATE_MULTITHREADED |
                                D3DCREATE_MIXED_VERTEXPROCESSING,
                              &params, nullptr, getter_AddRefs(device));

  return DXGID3D9TextureData::Create(aSize, aFormat, aTextureFlag, device);
}
#endif

/**
 * Create a YCbCrTextureClient according to the given backend.
 */
static already_AddRefed<TextureClient>
CreateYCbCrTextureClientWithBackend(LayersBackend aLayersBackend)
{

  TextureData* data = nullptr;

  RefPtr<gfxImageSurface> ySurface =
    new gfxImageSurface(IntSize(400, 300), SurfaceFormat::A8);
  RefPtr<gfxImageSurface> cbSurface =
    new gfxImageSurface(IntSize(200, 150), SurfaceFormat::A8);
  RefPtr<gfxImageSurface> crSurface =
    new gfxImageSurface(IntSize(200, 150), SurfaceFormat::A8);

  PlanarYCbCrData clientData;
  clientData.mYChannel = ySurface->Data();
  clientData.mCbChannel = cbSurface->Data();
  clientData.mCrChannel = crSurface->Data();
  clientData.mYSize = ySurface->GetSize();
  clientData.mPicSize = ySurface->GetSize();
  clientData.mCbCrSize = cbSurface->GetSize();
  clientData.mYStride = ySurface->Stride();
  clientData.mCbCrStride = cbSurface->Stride();
  clientData.mStereoMode = StereoMode::MONO;
  clientData.mYSkip = 0;
  clientData.mCbSkip = 0;
  clientData.mCrSkip = 0;
  clientData.mCrSkip = 0;
  clientData.mPicX = 0;
  clientData.mPicX = 0;

  // Create YCbCrTexture for basice backend.
  if (aLayersBackend == LayersBackend::LAYERS_BASIC) {
    return TextureClient::CreateForYCbCr(nullptr, clientData.mYSize,
                                         clientData.mCbCrSize, StereoMode::MONO,
                                         YUVColorSpace::BT601,
                                         TextureFlags::DEALLOCATE_CLIENT);
  }

#ifdef XP_WIN
  RefPtr<ID3D11Device> device = DeviceManagerDx::Get()->GetContentDevice();

  // Create GetD3D9TextureData.
  if (!device || aLayersBackend != LayersBackend::LAYERS_D3D11) {
    if (aLayersBackend == LayersBackend::LAYERS_D3D11 ||
        aLayersBackend == LayersBackend::LAYERS_D3D9) {
      GetD3D9TextureData(clientData, data);
    }
    if (data) {
      return MakeAndAddRef<TextureClient>(data, TextureFlags::DEALLOCATE_CLIENT,
                                          nullptr);
    } else {
      return nullptr;
    }
  }

  // Create YCbCrD3D11TextureData
  CD3D11_TEXTURE2D_DESC newDesc(DXGI_FORMAT_R8_UNORM, clientData.mYSize.width,
                                clientData.mYSize.height, 1, 1);
  newDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
  RefPtr<ID3D11Texture2D> textureY;

  D3D11_SUBRESOURCE_DATA yData = { clientData.mYChannel,
                                   (UINT)clientData.mYStride, 0 };

  HRESULT hr =
    device->CreateTexture2D(&newDesc, &yData, getter_AddRefs(textureY));
  NS_ENSURE_TRUE(SUCCEEDED(hr), nullptr);

  newDesc.Width = clientData.mCbCrSize.width;
  newDesc.Height = clientData.mCbCrSize.height;

  RefPtr<ID3D11Texture2D> textureCb;
  D3D11_SUBRESOURCE_DATA cbData = { clientData.mCbChannel,
                                    (UINT)clientData.mCbCrStride, 0 };
  hr = device->CreateTexture2D(&newDesc, &cbData, getter_AddRefs(textureCb));

  NS_ENSURE_TRUE(SUCCEEDED(hr), nullptr);

  RefPtr<ID3D11Texture2D> textureCr;
  D3D11_SUBRESOURCE_DATA crData = { clientData.mCrChannel,
                                    (UINT)clientData.mCbCrStride, 0 };
  hr = device->CreateTexture2D(&newDesc, &crData, getter_AddRefs(textureCr));
  NS_ENSURE_TRUE(SUCCEEDED(hr), nullptr);

  // Even though the textures we created are meant to be protected by a keyed
  // mutex, it appears that D3D doesn't include the initial memory upload within
  // this synchronization. Add an empty lock/unlock pair since that appears to
  // be sufficient to make sure we synchronize.
  {
    AutoLockTexture lockCr(textureCr);
  }

  data = DXGIYCbCrTextureData::Create(TextureFlags::DEALLOCATE_CLIENT, textureY,
                                      textureCb, textureCr, IntSize(200, 150),
                                      clientData.mYSize, clientData.mCbCrSize);
#endif

  if (data) {
    return MakeAndAddRef<TextureClient>(data, TextureFlags::DEALLOCATE_CLIENT,
                                        nullptr);
  }

  return nullptr;
}

/**
 * Create a TextureClient according to the given backend.
 */
static already_AddRefed<TextureClient>
CreateTextureClientWithBackend(LayersBackend aLayersBackend)
{
  TextureData* data = nullptr;
  SurfaceFormat format = gfxPlatform::GetPlatform()->Optimal2DFormatForContent(
    gfxContentType::COLOR_ALPHA);
  BackendType moz2DBackend =
    gfxPlatform::GetPlatform()->GetContentBackendFor(aLayersBackend);
  TextureAllocationFlags allocFlags = TextureAllocationFlags::ALLOC_DEFAULT;
  IntSize size = IntSize(400, 300);
  TextureFlags textureFlags = TextureFlags::DEALLOCATE_CLIENT;

  if (!gfx::Factory::AllowedSurfaceSize(size)) {
    return nullptr;
  }

#ifdef XP_WIN

  // Create DXGITextureData.
  if (aLayersBackend == LayersBackend::LAYERS_D3D11 &&
      (moz2DBackend == BackendType::DIRECT2D ||
       moz2DBackend == BackendType::DIRECT2D1_1)) {
    data = DXGITextureData::Create(size, format, allocFlags);
  }

  // Create DXGID3D9TextureData or D3D9TextureData.
  if (aLayersBackend == LayersBackend::LAYERS_D3D9 &&
      moz2DBackend == BackendType::CAIRO) {
    data = CreateDXGID3D9TextureData(size, format, textureFlags);

    if (!data && DeviceManagerD3D9::GetDevice()) {
      data = D3D9TextureData::Create(size, format, allocFlags);
    }
  }

  // Create DIBTextureData.
  if (!data && format == SurfaceFormat::B8G8R8X8 &&
      moz2DBackend == BackendType::CAIRO) {
    data = DIBTextureData::Create(size, format, nullptr);
  }
#endif

  // Create BufferTextureData.
  if (!data && aLayersBackend == LayersBackend::LAYERS_BASIC) {
    data = BufferTextureData::Create(size, format, moz2DBackend, aLayersBackend,
                                     textureFlags, allocFlags, nullptr);
  }

  if (data) {
    return MakeAndAddRef<TextureClient>(data, textureFlags, nullptr);
  }

  return nullptr;
}

/**
 * This function create TextureHost according to given TextureClients.
 */
already_AddRefed<TextureHost>
TestCreateTextureHost(TextureClient* aClient, LayersBackend& aLayersBackend)
{
  if (!aClient) {
    return nullptr;
  }

  // client serialization
  SurfaceDescriptor descriptor;
  RefPtr<TextureHost> textureHost;

  aClient->ToSurfaceDescriptor(descriptor);

  if (aLayersBackend == LayersBackend::LAYERS_BASIC) {
    return CreateBackendIndependentTextureHost(descriptor, nullptr,
                                               aClient->GetFlags());
  }

  return TextureHost::Create(descriptor, nullptr, aLayersBackend,
                             aClient->GetFlags());
}

} // namespace layers
} // namespace mozilla
