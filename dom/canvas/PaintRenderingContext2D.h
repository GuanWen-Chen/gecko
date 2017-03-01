/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef PaintRenderingContext2D_h
#define PaintRenderingContext2D_h

#include "mozilla/dom/BasicRenderingContext2D.h"

namespace mozilla {
namespace dom {

class PaintRenderingContext2D :
  public BasicRenderingContext2D,
  public nsWrapperCache
{
private:
  ~PaintRenderingContext2D(){};
public:
  explicit PaintRenderingContext2D(layers::LayersBackend aCompositorBackend,
                                   int32_t aWidth, int32_t aHeight)
    : BasicRenderingContext2D(aCompositorBackend, aWidth, aHeight) {}

  nsISupports* GetParentObject() const { return nullptr; }

  // nsISupports interface + CC
  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_CLASS(PaintRenderingContext2D)

protected:
  HTMLCanvasElement* GetCanvasElement() override { return nullptr; }

  bool AlreadyShutDown() override { return false; }

  virtual RenderingMode
  EnsureTarget(const gfx::Rect* aCoveredRect = nullptr,
               RenderingMode aRenderMode = RenderingMode::DefaultBackendMode) override {}

  bool IsTargetValid() const override { return false; }

  bool ParseColor(const nsAString& aString, nscolor* aColor) override { return false; }

  bool NeedToApplyFilter() override { return false; }

  void DidImageDrawCall() override {}

  void RedrawUser(const gfxRect& aR) override {}

  nsresult Redraw() override { return NS_OK;}

} // namespace dom
} // namespace mozilla
#endif
