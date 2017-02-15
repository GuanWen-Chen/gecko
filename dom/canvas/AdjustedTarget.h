/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef AdjustedTarget_h
#define AdjustedTarget_h

#include "FilterSupport.h"
#include "mozilla/dom/BasicRenderingContext2D.h"
#include "mozilla/gfx/Helpers.h"

using mozilla::gfx::AutoRestoreTransform;
using mozilla::gfx::Color;
using mozilla::gfx::DrawOptions;
using mozilla::gfx::FilterSupport;
using mozilla::gfx::SurfaceFormat;
using mozilla::gfx::SourceSurface;

namespace mozilla {
namespace dom {

/* This is an RAII based class that can be used as a drawtarget for
 * operations that need to have a filter applied to their results.
 * All coordinates passed to the constructor are in device space.
 */
class AdjustedTargetForFilter
{
  //typedef BasicRenderingContext2D::RenderingClass RenderingClass;

public:
  typedef BasicRenderingContext2D::ContextState ContextState;

  AdjustedTargetForFilter(BasicRenderingContext2D* aCtx,
                          DrawTarget* aFinalTarget,
                          const gfx::IntPoint& aFilterSpaceToTargetOffset,
                          const gfx::IntRect& aPreFilterBounds,
                          const gfx::IntRect& aPostFilterBounds,
                          gfx::CompositionOp aCompositionOp);

  // Return a SourceSurface that contains the FillPaint or StrokePaint source.
  already_AddRefed<SourceSurface>
  DoSourcePaint(gfx::IntRect& aRect, BasicRenderingContext2D::Style aStyle);

  ~AdjustedTargetForFilter();

  DrawTarget* DT();

private:
  RefPtr<DrawTarget> mTarget;
  RefPtr<DrawTarget> mFinalTarget;
  BasicRenderingContext2D *mCtx;
  gfx::IntRect mSourceGraphicRect;
  gfx::IntRect mFillPaintRect;
  gfx::IntRect mStrokePaintRect;
  gfx::IntRect mPostFilterBounds;
  gfx::IntPoint mOffset;
  gfx::CompositionOp mCompositionOp;
};

/* This is an RAII based class that can be used as a drawtarget for
 * operations that need to have a shadow applied to their results.
 * All coordinates passed to the constructor are in device space.
 */
class AdjustedTargetForShadow
{
public:
  typedef BasicRenderingContext2D::ContextState ContextState;

  AdjustedTargetForShadow(BasicRenderingContext2D* aCtx,
                          DrawTarget* aFinalTarget,
                          const gfx::Rect& aBounds,
                          gfx::CompositionOp aCompositionOp);

  ~AdjustedTargetForShadow();

  DrawTarget* DT();

  gfx::IntPoint OffsetToFinalDT();

private:
  RefPtr<DrawTarget> mTarget;
  RefPtr<DrawTarget> mFinalTarget;
  BasicRenderingContext2D *mCtx;
  Float mSigma;
  gfx::IntRect mTempRect;
  gfx::CompositionOp mCompositionOp;
};

/* This is an RAII based class that can be used as a drawtarget for
 * operations that need a shadow or a filter drawn. It will automatically
 * provide a temporary target when needed, and if so blend it back with a
 * shadow, filter, or both.
 * If both a shadow and a filter are needed, the filter is applied first,
 * and the shadow is applied to the filtered results.
 *
 * aBounds specifies the bounds of the drawing operation that will be
 * drawn to the target, it is given in device space! If this is nullptr the
 * drawing operation will be assumed to cover the whole canvas.
 */
class AdjustedTarget
{
public:
  typedef BasicRenderingContext2D::ContextState ContextState;

  explicit AdjustedTarget(BasicRenderingContext2D* aCtx,
                          const gfx::Rect *aBounds = nullptr);

  ~AdjustedTarget();

  operator DrawTarget*()
  {
    return mTarget;
  }

  DrawTarget* operator->() MOZ_NO_ADDREF_RELEASE_ON_RETURN;

private:

  gfx::Rect
  MaxSourceNeededBoundsForFilter(const gfx::Rect& aDestBounds, BasicRenderingContext2D* aCtx);

  gfx::Rect
  MaxSourceNeededBoundsForShadow(const gfx::Rect& aDestBounds, BasicRenderingContext2D* aCtx);

  gfx::Rect
  BoundsAfterFilter(const gfx::Rect& aBounds, BasicRenderingContext2D* aCtx);

  RefPtr<DrawTarget> mTarget;
  UniquePtr<AdjustedTargetForShadow> mShadowTarget;
  UniquePtr<AdjustedTargetForFilter> mFilterTarget;
};

} // namespace dom
} // namespace mozilla

#endif // AdjustedTarget_h