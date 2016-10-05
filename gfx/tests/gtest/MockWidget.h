/* vim:set ts=2 sw=2 sts=2 et: */
/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/
 */

#include "mozilla/widget/InProcessCompositorWidget.h"
#include "nsBaseWidget.h"
#include "GLContext.h"
#include "GLContextProvider.h"

using mozilla::gl::CreateContextFlags;
using mozilla::gl::GLContext;
using mozilla::gl::GLContextProvider;

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

} // namespace layers
} // namespace mozilla