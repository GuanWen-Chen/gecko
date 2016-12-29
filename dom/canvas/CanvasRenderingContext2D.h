/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef CanvasRenderingContext2D_h
#define CanvasRenderingContext2D_h

#include "mozilla/dom/CanvasRenderingContext2DBinding.h"
#include "RenderingContext2D.h"

namespace mozilla {
namespace dom {

class CanvasRenderingContext2D : public RenderingContext2D
{
public:
  NS_DECL_CYCLE_COLLECTION_SKIPPABLE_SCRIPT_HOLDER_CLASS_INHERITED(CanvasRenderingContext2D,
                                                                   RenderingContext2D)

  explicit CanvasRenderingContext2D(layers::LayersBackend aCompositorBackend)
    : RenderingContext2D(aCompositorBackend){}

  virtual JSObject* WrapObject(JSContext *aCx, JS::Handle<JSObject*> aGivenProto) override;

  // CanvasFilter
  void GetFilter(nsAString& aFilter)
  {
    aFilter = CurrentState().filterString;
  }

  void SetFilter(const nsAString& aFilter, mozilla::ErrorResult& aError);

  /**
    * Returns true if the result of a drawing operation should be
    * drawn with a filter.
    */
  bool NeedToApplyFilter() override
  {
    return EnsureUpdatedFilter().mPrimitives.Length() > 0;
  }

  /**
   * Calls UpdateFilter if the canvas's WriteOnly state has changed between the
   * last call to UpdateFilter and now.
   */
  const gfx::FilterDescription& EnsureUpdatedFilter() {
    bool isWriteOnly = mCanvasElement && mCanvasElement->IsWriteOnly();
    if (CurrentState().filterSourceGraphicTainted != isWriteOnly) {
      UpdateFilter();
    }
    MOZ_ASSERT(CurrentState().filterSourceGraphicTainted == isWriteOnly);
    return CurrentState().filter;
  }

  bool NeedToCalculateBounds() override
  {
    return NeedToDrawShadow() || NeedToApplyFilter();
  }

  mozilla::gfx::CompositionOp UsedOperation() override
  {
    if (NeedToDrawShadow() || NeedToApplyFilter()) {
      // In this case the shadow or filter rendering will use the operator.
      return mozilla::gfx::CompositionOp::OP_OVER;
    }

    return CurrentState().op;
  }


private:
  ~CanvasRenderingContext2D(){}
  NS_DECL_ISUPPORTS_INHERITED

protected:
  friend class CanvasFilterChainObserver;

   // Returns whether a filter was successfully parsed.
  bool ParseFilter(const nsAString& aString,
                   nsTArray<nsStyleFilter>& aFilterChain,
                   ErrorResult& aError);

  /**
   * Update CurrentState().filter with the filter description for
   * CurrentState().filterChain.
   * Flushes the PresShell, so the world can change if you call this function.
   */
  void UpdateFilter();

};

} // namespace dom
} // namespace mozilla

#endif
