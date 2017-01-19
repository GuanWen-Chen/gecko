/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gfxConfig.h"
#include "gfxPlatform.h"
#include "gtest/gtest.h"
#include "gtest/MozGTestBench.h"
#include "MockWidget.h"
#include "mozilla/layers/BasicCompositor.h"
#include "mozilla/layers/Compositor.h"
#include "mozilla/layers/LayersTypes.h"
#include "mozilla/layers/TextureClient.h"
#include "mozilla/layers/TextureHost.h"
#include "mozilla/RefPtr.h"
#include "TextureHelper.h"

using mozilla::gfx::Feature;
using mozilla::gfx::gfxConfig;
using mozilla::layers::Compositor;
using mozilla::layers::BasicCompositor;
using mozilla::layers::CompositorOptions;
using mozilla::layers::LayersBackend;
using mozilla::layers::TextureClient;
using mozilla::layers::TextureHost;
using mozilla::widget::CompositorWidget;
using mozilla::widget::InProcessCompositorWidget;

/**
 * This function will create possible TextureHosts according to the given backend.
 */
void
CreateTextureWithBackend(LayersBackend& aLayersBackend,
                         nsTArray<RefPtr<TextureClient>>& aTextureClients,
                         nsTArray<RefPtr<TextureHost>>& aTextures)
{
  aTextureClients.AppendElement(CreateTextureClientWithBackend(aLayersBackend));

  aTextureClients.AppendElement(
    CreateYCbCrTextureClientWithBackend(aLayersBackend));

  for (uint32_t i = 0; i < aTextureClients.Length(); i++) {
    aTextures.AppendElement(
      TestCreateTextureHost(aTextureClients[i], aLayersBackend));
  }
}

/**
 * This will return the default list of backends that
 * units test should run against.
 */
static void
GetPlatformBackends(nsTArray<LayersBackend>& aBackends)
{
  gfxPlatform::GetPlatform()->GetCompositorBackends(
    gfxConfig::IsEnabled(Feature::HW_COMPOSITING), aBackends);

  if (aBackends.IsEmpty()) {
    aBackends.AppendElement(LayersBackend::LAYERS_BASIC);
  }
}

/**
 * This function will return a BasicCompositor to caller.
 */
already_AddRefed<Compositor>
CreateTestCompositor()
{
  RefPtr<Compositor> compositor;
  // Init the platform.
  if (gfxPlatform::GetPlatform()) {
    RefPtr<MockWidget> widget = new MockWidget(256, 256);
	CompositorOptions options;
    RefPtr<CompositorWidget> proxy
      = new InProcessCompositorWidget(options, widget);
    compositor = new BasicCompositor(nullptr, proxy);
  }
  return compositor.forget();
}

/**
 * This function checks if the textures are compatible with given compositor..
 */
void
VerifyTextures(LayersBackend aBackends,
               nsTArray<RefPtr<TextureHost>>& aTextures,
               RefPtr<Compositor> aCompositor)
{
  for (uint32_t i = 0; i < aTextures.Length(); i++) {
    if (!aTextures[i]) {
      continue;
    }
    aTextures[i]->SetCompositor(aCompositor);
    bool lockResult = aTextures[i]->Lock();
    if (aBackends != LayersBackend::LAYERS_BASIC) {
      EXPECT_FALSE(lockResult);
    } else {
      EXPECT_TRUE(lockResult);
    }
    if (lockResult) {
      aTextures[i]->Unlock();
    }
  }
}

TEST(Gfx, TestTextureCompatibility)
{
  RefPtr<Compositor> compositor = CreateTestCompositor();
  nsTArray<LayersBackend> backendHints;
  nsTArray<RefPtr<TextureClient>> textureClients;

  GetPlatformBackends(backendHints);
  for (uint32_t i = 0; i < backendHints.Length(); i++) {
    nsTArray<RefPtr<TextureHost>> textures;

    CreateTextureWithBackend(backendHints[i], textureClients, textures);
    VerifyTextures(backendHints[i], textures, compositor);
  }
}
