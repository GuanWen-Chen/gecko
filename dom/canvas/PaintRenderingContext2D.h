/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef PaintRenderingContext2D_h
#define PaintRenderingContext2D_h

#include "mozilla/dom/BasicRenderingContext2D.h"

namespace mozilla {
namespace dom {

class PaintRenderingContext2D :
  public BasicRenderingContext2D
{
private:
  ~PaintRenderingContext2D() {};
public:
  explicit PaintRenderingContext2D(layers::LayersBackend aCompositorBackend)
    : BasicRenderingContext2D(aCompositorBackend) {}

  nsISupports* GetParentObject() const { return nullptr; }

  virtual JSObject* WrapObject(JSContext *aCx, JS::Handle<JSObject*> aGivenProto) override;

  // nsISupports interface + CC
  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS(PaintRenderingContext2D)

  void SetDimensions(int32_t aWidth, int32_t aHeight);

  already_AddRefed<mozilla::gfx::SourceSurface> GetSnapShot();

protected:
  HTMLCanvasElement* GetCanvasElement() override { return nullptr; }

  bool ParseColor(const nsAString& aString, nscolor* aColor) override;

  bool NeedToApplyFilter() override { return false; }

  void DidImageDrawCall() override { /*Do nothing.*/ }

  void RedrawUser(const gfxRect& aR) override {}

  nsresult Redraw() override { return NS_OK;}

  bool
  TrySkiaGLTarget(RefPtr<gfx::DrawTarget>& aOutDT,
                  RefPtr<layers::PersistentBufferProvider>& aOutProvider) override;

  bool
  TrySharedTarget(RefPtr<gfx::DrawTarget>& aOutDT,
                  RefPtr<layers::PersistentBufferProvider>& aOutProvider) override;

  /**
    * Returns the surface format this canvas should be allocated using. Takes
    * into account mOpaque, platform requirements, etc.
    */
  mozilla::gfx::SurfaceFormat GetSurfaceFormat() const override
  {
    return mozilla::gfx::SurfaceFormat::B8G8R8A8;
  }
};

} // namespace dom
} // namespace mozilla
#endif
