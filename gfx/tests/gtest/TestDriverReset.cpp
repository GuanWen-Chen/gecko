/* vim:set ts=2 sw=2 sts=2 et: */
/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/
 */

#include "DeviceManagerD3D9.h"
#include "gfxConfig.h"
#include "gfxPrefs.h"
#include "gfxPlatform.h"
#include "gfxUtils.h"
#include "gtest/gtest.h"
#include "gtest/MozGTestBench.h"
/*#include "TestLayers.h"
#include "mozilla/gfx/2D.h"*/
#include "mozilla/RefPtr.h"
#include "mozilla/layers/BasicCompositor.h"  // for BasicCompositor
#include "mozilla/layers/Compositor.h"  // for Compositor
#include "mozilla/layers/CompositorD3D11.h"  // for Compositor
#include "mozilla/layers/CompositorD3D9.h"  // for Compositor
/*#include "mozilla/layers/CompositorOGL.h"  // for CompositorOGL
#include "mozilla/layers/CompositorTypes.h"*/
#include "mozilla/layers/LayersTypes.h"
#include "mozilla/layers/TextureClient.h"
#include "mozilla/layers/TextureD3D11.h"
#include "mozilla/layers/TextureD3D9.h"
#include "mozilla/layers/TextureDIB.h"
#include "mozilla/layers/TextureHost.h"
/*#include "mozilla/layers/LayerManagerComposite.h"*/
#include "mozilla/widget/InProcessCompositorWidget.h"
#include "nsBaseWidget.h"
#include "GLContext.h"
#include "GLContextProvider.h"
#include <vector>

using mozilla::gl::CreateContextFlags;
using mozilla::gl::GLContext;
using mozilla::gl::GLContextProvider;
using mozilla::layers::LayersBackend;
using mozilla::layers::TextureAllocationFlags;
using mozilla::layers::TextureHost;

namespace mozilla {
namespace layers {

const int gCompWidth = 256;
const int gCompHeight = 256;
class MockWidget : public nsBaseWidget
{
public:
  MockWidget() {}

  NS_DECL_ISUPPORTS_INHERITED

  virtual LayoutDeviceIntRect GetClientBounds() override {
    return LayoutDeviceIntRect(0, 0, gCompWidth, gCompHeight);
  }
  virtual LayoutDeviceIntRect GetBounds() override {
    return GetClientBounds();
  }

  void* GetNativeData(uint32_t aDataType) override {
    if (aDataType == NS_NATIVE_OPENGL_CONTEXT) {
      mozilla::gl::SurfaceCaps caps = mozilla::gl::SurfaceCaps::ForRGB();
      caps.preserve = false;
      caps.bpp16 = false;
      nsCString discardFailureId;
      RefPtr<GLContext> context = GLContextProvider::CreateOffscreen(
        IntSize(gCompWidth, gCompHeight), caps,
        CreateContextFlags::REQUIRE_COMPAT_PROFILE,
        &discardFailureId);
      return context.forget().take();
    }
    return nullptr;
  }

  virtual nsresult        Create(nsIWidget* aParent,
                                 nsNativeWidget aNativeParent,
                                 const LayoutDeviceIntRect& aRect,
                                 nsWidgetInitData* aInitData = nullptr) override { return NS_OK; }
  virtual nsresult        Create(nsIWidget* aParent,
                                 nsNativeWidget aNativeParent,
                                 const DesktopIntRect& aRect,
                                 nsWidgetInitData* aInitData = nullptr) override { return NS_OK; }
  NS_IMETHOD              Show(bool aState) override { return NS_OK; }
  virtual bool            IsVisible() const override { return true; }
  NS_IMETHOD              Move(double aX, double aY) override { return NS_OK; }
  NS_IMETHOD              Resize(double aWidth, double aHeight, bool aRepaint) override { return NS_OK; }
  NS_IMETHOD              Resize(double aX, double aY,
                                 double aWidth, double aHeight, bool aRepaint) override { return NS_OK; }

  NS_IMETHOD              Enable(bool aState) override { return NS_OK; }
  virtual bool            IsEnabled() const override { return true; }
  NS_IMETHOD              SetFocus(bool aRaise) override { return NS_OK; }
  virtual nsresult        ConfigureChildren(const nsTArray<Configuration>& aConfigurations) override { return NS_OK; }
  NS_IMETHOD              Invalidate(const LayoutDeviceIntRect& aRect) override { return NS_OK; }
  NS_IMETHOD              SetTitle(const nsAString& title) override { return NS_OK; }
  virtual LayoutDeviceIntPoint WidgetToScreenOffset() override { return LayoutDeviceIntPoint(0, 0); }
  NS_IMETHOD              DispatchEvent(mozilla::WidgetGUIEvent* aEvent,
                                        nsEventStatus& aStatus) override { return NS_OK; }
  NS_IMETHOD_(void)       SetInputContext(const InputContext& aContext,
                                          const InputContextAction& aAction) override {}
  NS_IMETHOD_(InputContext) GetInputContext() override { abort(); }

private:
  ~MockWidget() {}
};

NS_IMPL_ISUPPORTS_INHERITED0(MockWidget, nsBaseWidget)

static already_AddRefed<TextureClient>
CreateTextureClient(LayersBackend aLayersBackend, bool aYCbCrFlag)
{
  IntSize size = IntSize(400,300);

  if (!gfx::Factory::AllowedSurfaceSize(size)) {
    return nullptr;
  }

  TextureData* data;

#ifdef XP_WIN
  if (aYCbCrFlag) {
    printf_stderr("GUAN YCbCr\n");
    data = new DXGIYCbCrTextureData();
  } else {
    gfx::SurfaceFormat format =
      gfxPlatform::GetPlatform()
        ->Optimal2DFormatForContent(gfxContentType::COLOR_ALPHA);
    gfx::BackendType moz2DBackend =
      gfxPlatform::GetPlatform()->GetContentBackendFor(aLayersBackend);
    printf_stderr("GUAN moz2DBackend : %d \n", moz2DBackend);
    TextureAllocationFlags allocFlags = TextureAllocationFlags::ALLOC_DEFAULT;

    if (aLayersBackend == LayersBackend::LAYERS_D3D11 &&
        (moz2DBackend == gfx::BackendType::DIRECT2D ||
         moz2DBackend == gfx::BackendType::DIRECT2D1_1))
    {
      printf_stderr("GUAN DXGI\n");
      data = DXGITextureData::Create(size, format, allocFlags);
      //data = new DXGIYCbCrTextureData();
    }
    if (aLayersBackend == LayersBackend::LAYERS_D3D9 &&
        moz2DBackend == gfx::BackendType::CAIRO &&
        DeviceManagerD3D9::GetDevice()) {
      printf_stderr("GUAN D3D9\n");
      data = D3D9TextureData::Create(size, format, allocFlags);
      //data = new DXGIYCbCrTextureData();
    }

/*
  if (!data && format == SurfaceFormat::B8G8R8X8 &&
      moz2DBackend == gfx::BackendType::CAIRO) {
    printf_stderr("GUAN DIB\n");
    data = DIBTextureData::Create(size, format, nullptr);
  }*/
  }
#endif

  if (data) {
    return MakeAndAddRef<TextureClient>(data, TextureFlags::NO_FLAGS, nullptr);
  }
  printf_stderr("GUAN no texture\n");
  return nullptr;
}


already_AddRefed<TextureHost>
CreateTestTextureHost(TextureClient* aClient, LayersBackend& aLayersBackend)
{
  // client serialization
  SurfaceDescriptor descriptor;
  aClient->ToSurfaceDescriptor(descriptor);
  RefPtr<TextureHost> textureHost;
  return TextureHost::Create(descriptor, nullptr, aLayersBackend,
                                    TextureFlags::NO_FLAGS);
}

already_AddRefed<TextureHost>
CreateTestTexure(LayersBackend& aLayersBackend, bool aYCbCrFlag)
{
  RefPtr<TextureClient> textureClient = CreateTextureClient(aLayersBackend, aYCbCrFlag);
  if (textureClient) {
    return CreateTestTextureHost(textureClient, aLayersBackend);
  } else {
    return nullptr;
  }
}

/*
static void ApplyTexture(RefPtr<TextureHost> aTextureHost, LayersBackend& aLayersBackend)
{
  RefPtr<MockWidget> widget = new MockWidget();
  RefPtr<widget::CompositorWidget> proxy =
    new widget::InProcessCompositorWidget(widget);
  gfxPrefs::GetSingleton();
  RefPtr<Compositor> compositor;
  switch (aLayersBackend) {
    case LayersBackend::LAYERS_D3D9:
      printf_stderr("GUAN D3D9 compositor\n");
      compositor = new CompositorD3D9(nullptr, proxy);
      break;
    case LayersBackend::LAYERS_D3D11:
      printf_stderr("GUAN D3D11 compositor\n");
      compositor = new CompositorD3D11(nullptr, proxy);
      break;
    default:
      printf_stderr("GUAN BASIC compositor\n");
      compositor = new BasicCompositor(nullptr, proxy);
      break;
  }
  aTextureHost->SetCompositor(compositor);
}
*/
static bool ComputeShouldAccelerate()
{
#ifdef MOZ_WIDGET_GTK
  return false;
#else
  bool enabled = gfx::gfxConfig::IsEnabled(gfx::Feature::HW_COMPOSITING);
  return enabled;
#endif
}

/**
 * This will return the default list of backends that
 * units test should run against.
 */
static void GetPlatformBackends(nsTArray<LayersBackend>& aBackends)
{
  if (gfxPlatform::GetPlatform()) {
    gfxPlatform::GetPlatform()->GetCompositorBackends(ComputeShouldAccelerate(),
                                                      aBackends);
    printf_stderr("GUAN has platform\n");
  } else {
    aBackends.AppendElement(LayersBackend::LAYERS_BASIC);
    printf_stderr("GUAN no platform\n");
  }
}

}
}

TEST(Gfx, DriverResetTest)
{
  nsTArray<LayersBackend> backendHints;
  mozilla::layers::GetPlatformBackends(backendHints);

  for (uint32_t i=0; i < backendHints.Length(); i++) {
    if (backendHints[i] != LayersBackend::LAYERS_D3D11 &&
        backendHints[i] != LayersBackend::LAYERS_D3D9) {
      continue;
    }
    printf_stderr("GUAN backend : %d \n",backendHints[i]);
    RefPtr<TextureHost> texture;
    texture = mozilla::layers::CreateTestTexure(backendHints[i], false);
    if (texture) {
      printf_stderr("GUAN has texture\n");
      //mozilla::layers::ApplyTexture(texture, backendHints[i]);
      if (texture->Lock()) {
        printf_stderr("GUAN should not pass\n");
        MOZ_CRASH();
      }
    } else {
      printf_stderr("GUAN no texture\n");
    }
    texture = mozilla::layers::CreateTestTexure(backendHints[i], true);
    if (texture) {
      printf_stderr("GUAN has texture\n");
      //mozilla::layers::ApplyTexture(texture, backendHints[i]);
      if (texture->Lock()) {
        printf_stderr("GUAN should not pass\n");
        MOZ_CRASH();
      }
      break;
    } else {
      printf_stderr("GUAN no texture\n");
    }
  }
}
