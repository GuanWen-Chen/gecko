/* vim:set ts=2 sw=2 sts=2 et: */
/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/
 */

#include "TextureHelper.h"
#include "MockWidget.h"
#include "gfxConfig.h"
#include "gfxPlatform.h"
#include "gtest/gtest.h"
#include "gtest/MozGTestBench.h"
#include "mozilla/RefPtr.h"
#include "mozilla/layers/BasicCompositor.h"  // for BasicCompositor
#include "mozilla/layers/Compositor.h"  // for Compositor
#include "mozilla/layers/LayersTypes.h"
#include "mozilla/layers/TextureClient.h"
#include "mozilla/layers/TextureHost.h"
#include <vector>

namespace mozilla {
namespace layers {
/**
 * This function will create possible TextureHosts according to the given backend.
 */
void
CreateTextureWithBackend(LayersBackend& aLayersBackend, nsTArray<RefPtr<TextureHost>>& aTextures)
{
  nsTArray<RefPtr<TextureClient>> textureClients;
  RefPtr<TextureClient> textureClient;

  textureClient = CreateTextureClientWithBackend(aLayersBackend);
  if (textureClient) {
    printf_stderr("Has texture\n");
    textureClients.AppendElement(textureClient);  
  } else {
    printf_stderr("No texture\n");
  }

  textureClient = CreateYCbCrTextureClientWithBackend(aLayersBackend);
  if (textureClient) {
    printf_stderr("Has texture\n");
    textureClients.AppendElement(textureClient);  
  } else {
    printf_stderr("No texture\n");
  }

  for (uint32_t i = 0; i < textureClients.Length(); i++) {
    aTextures.AppendElement(CreateTestTextureHost(textureClients[i], aLayersBackend));
  }
}

/**
 * This will return the default list of backends that
 * units test should run against.
 */
static void GetPlatformBackends(nsTArray<LayersBackend>& aBackends)
{
  gfxPlatform::GetPlatform()->GetCompositorBackends(gfx::gfxConfig::IsEnabled(gfx::Feature::HW_COMPOSITING),
                                                    aBackends);
  if (aBackends.IsEmpty()) {
    aBackends.AppendElement(LayersBackend::LAYERS_BASIC);
    printf_stderr("GUAN no platform\n");
  }
}

/**
 * This function will return a BasicCompositor to caller.
 */
Compositor* CreateTestCompositor(){
  // Init the platform.
  if (gfxPlatform::GetPlatform()) {
    RefPtr<MockWidget> widget = new MockWidget();
    RefPtr<widget::CompositorWidget> proxy =
      new widget::InProcessCompositorWidget(widget);
    return new BasicCompositor(nullptr, proxy);
  }
  return nullptr;
}

void
VerifyTextures(LayersBackend aBackends, nsTArray<RefPtr<TextureHost>>& aTextures, RefPtr<Compositor> aCompositor)
{
  for (uint32_t i = 0; i < aTextures.Length(); i++) {
    aTextures[i]->SetCompositor(aCompositor);
    bool LockSuccess = aTextures[i]->Lock();
    if (LockSuccess && aBackends != LayersBackend::LAYERS_BASIC) {
      printf_stderr("GUAN crash 1\n");
      MOZ_CRASH();
    }
    if (!LockSuccess && aBackends == LayersBackend::LAYERS_BASIC) {
      printf_stderr("GUAN crash 2\n");
      MOZ_CRASH();
    }
    if (LockSuccess) {
      aTextures[i]->Unlock();
    }
  }
}

} // namespace layers
} // namespace mozilla

TEST(Gfx, DriverResetTest)
{
  RefPtr<Compositor> compositor = CreateTestCompositor();
  nsTArray<LayersBackend> backendHints;

  GetPlatformBackends(backendHints);
  for (uint32_t i=0; i < backendHints.Length(); i++) {
    nsTArray<RefPtr<TextureHost>> textures;

    printf_stderr("GUAN backend : %d \n",backendHints[i]);
    CreateTextureWithBackend(backendHints[i], textures);
    VerifyTextures(backendHints[i], textures, compositor);
  }  
}