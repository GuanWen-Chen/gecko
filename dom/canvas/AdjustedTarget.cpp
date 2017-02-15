/* This is an RAII based class that can be used as a drawtarget for
 * operations that need to have a filter applied to their results.
 * All coordinates passed to the constructor are in device space.
 */
#include "AdjustedTarget.h"
#include "mozilla/dom/HTMLCanvasElement.h"

using mozilla::gfx::DrawTarget;

namespace mozilla {
namespace dom {

AdjustedTargetForFilter::AdjustedTargetForFilter(BasicRenderingContext2D* aCtx,
                        DrawTarget* aFinalTarget,
                        const gfx::IntPoint& aFilterSpaceToTargetOffset,
                        const gfx::IntRect& aPreFilterBounds,
                        const gfx::IntRect& aPostFilterBounds,
                        gfx::CompositionOp aCompositionOp)
  : mCtx(nullptr)
  , mCompositionOp(aCompositionOp)
{
  mCtx = aCtx;
  mFinalTarget = aFinalTarget;
  mPostFilterBounds = aPostFilterBounds;
  mOffset = aFilterSpaceToTargetOffset;

  nsIntRegion sourceGraphicNeededRegion;
  nsIntRegion fillPaintNeededRegion;
  nsIntRegion strokePaintNeededRegion;

  FilterSupport::ComputeSourceNeededRegions(
    aCtx->CurrentState().filter, mPostFilterBounds,
    sourceGraphicNeededRegion, fillPaintNeededRegion,
    strokePaintNeededRegion);

  mSourceGraphicRect = sourceGraphicNeededRegion.GetBounds();
  mFillPaintRect = fillPaintNeededRegion.GetBounds();
  mStrokePaintRect = strokePaintNeededRegion.GetBounds();

  mSourceGraphicRect = mSourceGraphicRect.Intersect(aPreFilterBounds);

  if (mSourceGraphicRect.IsEmpty()) {
    // The filter might not make any use of the source graphic. We need to
    // create a DrawTarget that we can return from DT() anyway, so we'll
    // just use a 1x1-sized one.
    mSourceGraphicRect.SizeTo(1, 1);
  }

  mTarget =
    mFinalTarget->CreateSimilarDrawTarget(mSourceGraphicRect.Size(), SurfaceFormat::B8G8R8A8);

  if (!mTarget) {
    // XXX - Deal with the situation where our temp size is too big to
    // fit in a texture (bug 1066622).
    mTarget = mFinalTarget;
    mCtx = nullptr;
    mFinalTarget = nullptr;
    return;
  }

  mTarget->SetTransform(
    mFinalTarget->GetTransform().PostTranslate(-mSourceGraphicRect.TopLeft() + mOffset));
}

// Return a SourceSurface that contains the FillPaint or StrokePaint source.
already_AddRefed<SourceSurface>
AdjustedTargetForFilter::DoSourcePaint(gfx::IntRect& aRect, BasicRenderingContext2D::Style aStyle)
{
  if (aRect.IsEmpty()) {
    return nullptr;
  }

  RefPtr<DrawTarget> dt =
    mFinalTarget->CreateSimilarDrawTarget(aRect.Size(), SurfaceFormat::B8G8R8A8);
  if (!dt) {
    aRect.SetEmpty();
    return nullptr;
  }

  Matrix transform =
    mFinalTarget->GetTransform().PostTranslate(-aRect.TopLeft() + mOffset);

  dt->SetTransform(transform);

  if (transform.Invert()) {
    gfx::Rect dtBounds(0, 0, aRect.width, aRect.height);
    gfx::Rect fillRect = transform.TransformBounds(dtBounds);
    dt->FillRect(fillRect, CanvasGeneralPattern().ForStyle(mCtx, aStyle, dt));
  }
  return dt->Snapshot();
}

AdjustedTargetForFilter::~AdjustedTargetForFilter()
{
  if (!mCtx) {
    return;
  }

  RefPtr<SourceSurface> snapshot = mTarget->Snapshot();

  RefPtr<SourceSurface> fillPaint =
    DoSourcePaint(mFillPaintRect, BasicRenderingContext2D::Style::FILL);
  RefPtr<SourceSurface> strokePaint =
    DoSourcePaint(mStrokePaintRect, BasicRenderingContext2D::Style::STROKE);

  AutoRestoreTransform autoRestoreTransform(mFinalTarget);
  mFinalTarget->SetTransform(Matrix());

  MOZ_RELEASE_ASSERT(!mCtx->CurrentState().filter.mPrimitives.IsEmpty());
  gfx::FilterSupport::RenderFilterDescription(
    mFinalTarget, mCtx->CurrentState().filter,
    gfx::Rect(mPostFilterBounds),
    snapshot, mSourceGraphicRect,
    fillPaint, mFillPaintRect,
    strokePaint, mStrokePaintRect,
    mCtx->CurrentState().filterAdditionalImages,
    mPostFilterBounds.TopLeft() - mOffset,
    DrawOptions(1.0f, mCompositionOp));

  const gfx::FilterDescription& filter = mCtx->CurrentState().filter;
  MOZ_RELEASE_ASSERT(!filter.mPrimitives.IsEmpty());
  if (filter.mPrimitives.LastElement().IsTainted() &&
      mCtx->GetCanvasElement()) {
    mCtx->GetCanvasElement()->SetWriteOnly();
  }
}

DrawTarget* AdjustedTargetForFilter::DT()
{
  return mTarget;
}

AdjustedTargetForShadow::AdjustedTargetForShadow(BasicRenderingContext2D* aCtx,
                        DrawTarget* aFinalTarget,
                        const gfx::Rect& aBounds,
                        gfx::CompositionOp aCompositionOp)
  : mCtx(nullptr)
  , mCompositionOp(aCompositionOp)
{
  mCtx = aCtx;
  mFinalTarget = aFinalTarget;

  const ContextState &state = mCtx->CurrentState();

  mSigma = state.ShadowBlurSigma();

  gfx::Rect bounds = aBounds;

  int32_t blurRadius = state.ShadowBlurRadius();

  // We actually include the bounds of the shadow blur, this makes it
  // easier to execute the actual blur on hardware, and shouldn't affect
  // the amount of pixels that need to be touched.
  bounds.Inflate(blurRadius);

  bounds.RoundOut();
  bounds.ToIntRect(&mTempRect);

  mTarget =
    mFinalTarget->CreateShadowDrawTarget(mTempRect.Size(),
                                         SurfaceFormat::B8G8R8A8, mSigma);

  if (!mTarget) {
    // XXX - Deal with the situation where our temp size is too big to
    // fit in a texture (bug 1066622).
    mTarget = mFinalTarget;
    mCtx = nullptr;
    mFinalTarget = nullptr;
  } else {
    mTarget->SetTransform(
      mFinalTarget->GetTransform().PostTranslate(-mTempRect.TopLeft()));
  }
}

AdjustedTargetForShadow::~AdjustedTargetForShadow()
{
  if (!mCtx) {
    return;
  }

  RefPtr<SourceSurface> snapshot = mTarget->Snapshot();

  mFinalTarget->DrawSurfaceWithShadow(snapshot, mTempRect.TopLeft(),
                                      Color::FromABGR(mCtx->CurrentState().shadowColor),
                                      mCtx->CurrentState().shadowOffset, mSigma,
                                      mCompositionOp);
}

DrawTarget* AdjustedTargetForShadow::DT()
{
  return mTarget;
}

gfx::IntPoint AdjustedTargetForShadow::OffsetToFinalDT()
{
  return mTempRect.TopLeft();
}

AdjustedTarget::AdjustedTarget(BasicRenderingContext2D* aCtx, const gfx::Rect *aBounds)
{
  // There are operations that can invalidate aCtx->mTarget along the way,
  // so don't cache the pointer to it too soon.
  mTarget = nullptr;

  // All rects in this function are in the device space of ctx->mTarget.

  // In order to keep our temporary surfaces as small as possible, we first
  // calculate what their maximum required bounds would need to be if we
  // were to fill the whole canvas. Everything outside those bounds we don't
  // need to render.
  gfx::Rect r(0, 0, aCtx->mWidth, aCtx->mHeight);
  gfx::Rect maxSourceNeededBoundsForShadow =
    MaxSourceNeededBoundsForShadow(r, aCtx);
  gfx::Rect maxSourceNeededBoundsForFilter =
    MaxSourceNeededBoundsForFilter(maxSourceNeededBoundsForShadow, aCtx);

  gfx::Rect bounds = maxSourceNeededBoundsForFilter;
  if (aBounds) {
    bounds = bounds.Intersect(*aBounds);
  }
  gfx::Rect boundsAfterFilter = BoundsAfterFilter(bounds, aCtx);

  mozilla::gfx::CompositionOp op = aCtx->CurrentState().op;

  gfx::IntPoint offsetToFinalDT;

  // First set up the shadow draw target, because the shadow goes outside.
  // It applies to the post-filter results, if both a filter and a shadow
  // are used.
  if (aCtx->NeedToDrawShadow()) {
    mShadowTarget = MakeUnique<AdjustedTargetForShadow>(
      aCtx, aCtx->mTarget, boundsAfterFilter, op);
    mTarget = mShadowTarget->DT();
    offsetToFinalDT = mShadowTarget->OffsetToFinalDT();

    // If we also have a filter, the filter needs to be drawn with OP_OVER
    // because shadow drawing already applies op on the result.
    op = gfx::CompositionOp::OP_OVER;
  }

  // Now set up the filter draw target.
  if (aCtx->NeedToApplyFilter()) {
    bounds.RoundOut();

    if (!mTarget) {
      mTarget = aCtx->mTarget;
    }
    gfx::IntRect intBounds;
    if (!bounds.ToIntRect(&intBounds)) {
      return;
    }
    mFilterTarget = MakeUnique<AdjustedTargetForFilter>(
      aCtx, mTarget, offsetToFinalDT, intBounds,
      gfx::RoundedToInt(boundsAfterFilter), op);
    mTarget = mFilterTarget->DT();
  }
  if (!mTarget) {
    mTarget = aCtx->mTarget;
  }
}

AdjustedTarget::~AdjustedTarget()
{
  // The order in which the targets are finalized is important.
  // Filters are inside, any shadow applies to the post-filter results.
  mFilterTarget.reset();
  mShadowTarget.reset();
}

DrawTarget* AdjustedTarget::operator->() MOZ_NO_ADDREF_RELEASE_ON_RETURN
{
  return mTarget;
}

gfx::Rect
AdjustedTarget::MaxSourceNeededBoundsForFilter(const gfx::Rect& aDestBounds, BasicRenderingContext2D* aCtx)
{
  if (!aCtx->NeedToApplyFilter()) {
    return aDestBounds;
  }

  nsIntRegion sourceGraphicNeededRegion;
  nsIntRegion fillPaintNeededRegion;
  nsIntRegion strokePaintNeededRegion;

  FilterSupport::ComputeSourceNeededRegions(
    aCtx->CurrentState().filter, gfx::RoundedToInt(aDestBounds),
    sourceGraphicNeededRegion, fillPaintNeededRegion, strokePaintNeededRegion);

  return gfx::Rect(sourceGraphicNeededRegion.GetBounds());
}

gfx::Rect
AdjustedTarget::MaxSourceNeededBoundsForShadow(const gfx::Rect& aDestBounds, BasicRenderingContext2D* aCtx)
{
  if (!aCtx->NeedToDrawShadow()) {
    return aDestBounds;
  }

  const ContextState &state = aCtx->CurrentState();
  gfx::Rect sourceBounds = aDestBounds - state.shadowOffset;
  sourceBounds.Inflate(state.ShadowBlurRadius());

  // Union the shadow source with the original rect because we're going to
  // draw both.
  return sourceBounds.Union(aDestBounds);
}

gfx::Rect
AdjustedTarget::BoundsAfterFilter(const gfx::Rect& aBounds, BasicRenderingContext2D* aCtx)
{
  if (!aCtx->NeedToApplyFilter()) {
    return aBounds;
  }

  gfx::Rect bounds(aBounds);
  bounds.RoundOut();

  gfx::IntRect intBounds;
  if (!bounds.ToIntRect(&intBounds)) {
    return gfx::Rect();
  }

  nsIntRegion extents =
    gfx::FilterSupport::ComputePostFilterExtents(aCtx->CurrentState().filter,
                                                 intBounds);
  return gfx::Rect(extents.GetBounds());
}

} // dom
} // mozilla