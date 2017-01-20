/* -*- Mode: C++; tab-width: 20; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GFX_IMFYCBCRIMAGE_H
#define GFX_IMFYCBCRIMAGE_H

#include "mozilla/layers/TextureD3D11.h"
#include "mozilla/RefPtr.h"
#include "ImageContainer.h"
#include "mfidl.h"

namespace mozilla {
namespace layers {

struct AutoLockTexture
{
  explicit AutoLockTexture(ID3D11Texture2D* aTexture)
  {
    aTexture->QueryInterface((IDXGIKeyedMutex**)getter_AddRefs(mMutex));
    if (!mMutex) {
      return;
    }
    HRESULT hr = mMutex->AcquireSync(0, 10000);
    if (hr == WAIT_TIMEOUT) {
      MOZ_CRASH("GFX: IMFYCbCrImage timeout");
    }

    if (FAILED(hr)) {
      NS_WARNING("Failed to lock the texture");
    }
  }

  ~AutoLockTexture()
  {
    if (!mMutex) {
      return;
    }
    HRESULT hr = mMutex->ReleaseSync(0);
    if (FAILED(hr)) {
      NS_WARNING("Failed to unlock the texture");
    }
  }

  RefPtr<IDXGIKeyedMutex> mMutex;
};

class IMFYCbCrImage : public RecyclingPlanarYCbCrImage
{
public:
  IMFYCbCrImage(IMFMediaBuffer* aBuffer, IMF2DBuffer* a2DBuffer);

  virtual bool IsValid() { return true; }

  virtual TextureClient* GetTextureClient(KnowsCompositor* aForwarder) override;

  static bool UploadData(IDirect3DDevice9* aDevice,
                         RefPtr<IDirect3DTexture9>& aTexture,
                         HANDLE& aHandle,
                         uint8_t* aSrc,
                         const gfx::IntSize& aSrcSize,
                         int32_t aSrcStride);

protected:

  TextureClient* GetD3D9TextureClient(KnowsCompositor* aForwarder);

  ~IMFYCbCrImage();

  RefPtr<IMFMediaBuffer> mBuffer;
  RefPtr<IMF2DBuffer> m2DBuffer;
  RefPtr<TextureClient> mTextureClient;
};

} // namepace layers
} // namespace mozilla

#endif // GFX_D3DSURFACEIMAGE_H
