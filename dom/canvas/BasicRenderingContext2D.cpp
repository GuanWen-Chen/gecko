/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "AdjustedTarget.h"
#include "CanvasImageCache.h"
#include "CanvasUtils.h"
#include "DrawResult.h"
#include "gfxPrefs.h"
#include "gfxUtils.h"
#include "GLContext.h"
#include "ImageRegion.h"
#include "mozilla/dom/BasicRenderingContext2D.h"
#include "mozilla/dom/HTMLCanvasElement.h"
#include "mozilla/dom/HTMLImageElement.h"
#include "mozilla/dom/HTMLVideoElement.h"
#include "mozilla/dom/ImageBitmap.h"
#include "mozilla/gfx/PathHelpers.h"
#include "mozilla/layers/PersistentBufferProvider.h"
#include "nsContentUtils.h"
#include "nsICanvasRenderingContextInternal.h"
#include "nsIMemoryReporter.h"
#include "nsPrintfCString.h"
#include "nsStyleUtil.h"

#include "SkiaGLGlue.h"
#ifdef USE_SKIA
#include "GLBlitHelper.h"
#include "SurfaceTypes.h"
#endif

using mozilla::CanvasUtils::FloatValidate;
using mozilla::gfx::AntialiasMode;
using mozilla::gfx::ArcToBezier;
using mozilla::gfx::CapStyle;
using mozilla::gfx::Color;
using mozilla::gfx::CriticalLog;
using mozilla::gfx::DrawOptions;
using mozilla::gfx::DrawSurfaceOptions;
using mozilla::gfx::ExtendMode;
using mozilla::gfx::Factory;
using mozilla::gfx::FillRule;
using mozilla::gfx::IntPoint;
using mozilla::gfx::IntRect;
using mozilla::gfx::JoinStyle;
using mozilla::gfx::LogReason;
using mozilla::gfx::IntSize;
using mozilla::gfx::NativeSurface;
using mozilla::gfx::NativeSurfaceType;
using mozilla::gfx::Path;
using mozilla::gfx::SamplingBounds;
using mozilla::gfx::SamplingFilter;
using mozilla::gfx::Size;
using mozilla::gfx::SourceSurface;
using mozilla::gfx::StrokeOptions;
using mozilla::gfx::ToDeviceColor;
using mozilla::image::DrawResult;
using mozilla::image::ImageRegion;
using mozilla::layers::AutoLockImage;
using mozilla::layers::PersistentBufferProvider;
using mozilla::layers::PersistentBufferProviderBasic;

namespace mozilla {
namespace dom{

typedef BasicRenderingContext2D::ContextState ContextState;

NS_IMPL_ISUPPORTS(ContextState, nsISupports)

// This is KIND_OTHER because it's not always clear where in memory the pixels
// of a canvas are stored.  Furthermore, this memory will be tracked by the
// underlying surface implementations.  See bug 655638 for details.
class Canvas2dPixelsReporter final : public nsIMemoryReporter
{
  ~Canvas2dPixelsReporter() {}
public:
  NS_DECL_ISUPPORTS

  NS_IMETHOD CollectReports(nsIHandleReportCallback* aHandleReport,
                            nsISupports* aData, bool aAnonymize) override
  {
    MOZ_COLLECT_REPORT(
      "canvas-2d-pixels", KIND_OTHER, UNITS_BYTES,
      BasicRenderingContext2D::sCanvasAzureMemoryUsed,
      "Memory used by 2D canvases. Each canvas requires "
      "(width * height * 4) bytes.");

    return NS_OK;
  }
};

NS_IMPL_ISUPPORTS(Canvas2dPixelsReporter, nsIMemoryReporter)

NS_IMPL_CYCLE_COLLECTION_ROOT_NATIVE(CanvasGradient, AddRef)
NS_IMPL_CYCLE_COLLECTION_UNROOT_NATIVE(CanvasGradient, Release)

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(CanvasGradient, mContext)

NS_IMPL_CYCLE_COLLECTION_ROOT_NATIVE(CanvasPattern, AddRef)
NS_IMPL_CYCLE_COLLECTION_UNROOT_NATIVE(CanvasPattern, Release)

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(CanvasPattern, mContext)

NS_IMPL_ISUPPORTS(CanvasShutdownObserver, nsIObserver)

NS_IMETHODIMP
CanvasShutdownObserver::Observe(nsISupports *aSubject,
                                const char *aTopic,
                                const char16_t *aData)
{
  if (mCanvas && strcmp(aTopic, NS_XPCOM_SHUTDOWN_OBSERVER_ID) == 0) {
    mCanvas->OnShutdown();
    nsContentUtils::UnregisterShutdownObserver(this);
  }

  return NS_OK;
}

// Cap sigma to avoid overly large temp surfaces.
const mozilla::gfx::Float SIGMA_MAX = 100;

const size_t MAX_STYLE_STACK_SIZE = 1024;

/**
 ** BasicRenderingContext2D impl
 **/

// Initialize our static variables.
uint32_t BasicRenderingContext2D::sNumLivingContexts = 0;
DrawTarget* BasicRenderingContext2D::sErrorTarget = nullptr;
int64_t BasicRenderingContext2D::sCanvasAzureMemoryUsed = 0;

BasicRenderingContext2D::BasicRenderingContext2D(layers::LayersBackend aCompositorBackend)
  : mRenderingMode(RenderingMode::OpenGLBackendMode)
  , mCompositorBackend(aCompositorBackend)
    // these are the default values from the Canvas spec
  , mWidth(0), mHeight(0)
  , mPathTransformWillUpdate(false)
  , mIsSkiaGL(false)
  , mHasPendingStableStateCallback(false)
{
  sNumLivingContexts++;

  mShutdownObserver = new CanvasShutdownObserver(this);
  nsContentUtils::RegisterShutdownObserver(mShutdownObserver);
}

BasicRenderingContext2D::~BasicRenderingContext2D()
{
  RemoveShutdownObserver();
  Reset();

  sNumLivingContexts--;
  if (!sNumLivingContexts) {
    NS_IF_RELEASE(sErrorTarget);
  }
}

nsresult
BasicRenderingContext2D::Reset()
{
  // only do this for non-docshell created contexts,
  // since those are the ones that we created a surface for
  if (mTarget && IsTargetValid()) {
    sCanvasAzureMemoryUsed -= mWidth * mHeight * 4;
  }

  bool forceReset = true;
  ReturnTarget(forceReset);
  mTarget = nullptr;
  mBufferProvider = nullptr;

  return NS_OK;
}

void
BasicRenderingContext2D::RemoveShutdownObserver()
{
  if (mShutdownObserver) {
    nsContentUtils::UnregisterShutdownObserver(mShutdownObserver);
    mShutdownObserver = nullptr;
  }
}

void
BasicRenderingContext2D::OnShutdown()
{
  mShutdownObserver = nullptr;

  RefPtr<PersistentBufferProvider> provider = mBufferProvider;

  Reset();

  if (provider) {
    provider->OnShutdown();
  }
}

BasicRenderingContext2D::RenderingMode
BasicRenderingContext2D::EnsureTarget(const gfx::Rect* aCoveredRect,
                                      RenderingMode aRenderingMode)
{
  if (AlreadyShutDown()) {
    gfxCriticalError() << "Attempt to render into a Canvas2d after shutdown.";
    SetErrorState();
    return aRenderingMode;
  }

  // This would make no sense, so make sure we don't get ourselves in a mess
  MOZ_ASSERT(mRenderingMode != RenderingMode::DefaultBackendMode);

  RenderingMode mode = (aRenderingMode == RenderingMode::DefaultBackendMode) ? mRenderingMode : aRenderingMode;

  if (mTarget && mode == mRenderingMode) {
    return mRenderingMode;
  }

  // Check that the dimensions are sane
  if (mWidth > gfxPrefs::MaxCanvasSize() ||
      mHeight > gfxPrefs::MaxCanvasSize() ||
      mWidth < 0 || mHeight < 0) {

    SetErrorState();
    return aRenderingMode;
  }

  // If the next drawing command covers the entire canvas, we can skip copying
  // from the previous frame and/or clearing the canvas.
  gfx::Rect canvasRect(0, 0, mWidth, mHeight);
  bool canDiscardContent = aCoveredRect &&
    CurrentState()->transform.TransformBounds(*aCoveredRect).Contains(canvasRect);

  // If a clip is active we don't know for sure that the next drawing command
  // will really cover the entire canvas.
  for (const auto& style : mStyleStack) {
    if (!canDiscardContent) {
      break;
    }
    for (const auto& clipOrTransform : style->clipsAndTransforms) {
      if (clipOrTransform.IsClip()) {
        canDiscardContent = false;
        break;
      }
    }
  }

  ScheduleStableStateCallback();

  IntRect persistedRect = canDiscardContent ? IntRect()
                                            : IntRect(0, 0, mWidth, mHeight);

  if (mBufferProvider && mode == mRenderingMode) {
    mTarget = mBufferProvider->BorrowDrawTarget(persistedRect);

    if (mTarget && !mBufferProvider->PreservesDrawingState()) {
      RestoreClipsAndTransformToTarget();
    }

    if (mTarget) {
      return mode;
    }
  }

  RefPtr<DrawTarget> newTarget;
  RefPtr<PersistentBufferProvider> newProvider;

  if (mode == RenderingMode::OpenGLBackendMode &&
      !TrySkiaGLTarget(newTarget, newProvider)) {
    // Fall back to software.
    mode = RenderingMode::SoftwareBackendMode;
  }

  if (mode == RenderingMode::SoftwareBackendMode &&
      !TrySharedTarget(newTarget, newProvider) &&
      !TryBasicTarget(newTarget, newProvider)) {

    gfxCriticalError(
      CriticalLog::DefaultOptions(Factory::ReasonableSurfaceSize(GetSize()))
    ) << "Failed borrow shared and basic targets.";

    SetErrorState();
    return mode;
  }


  MOZ_ASSERT(newTarget);
  MOZ_ASSERT(newProvider);

  bool needsClear = !canDiscardContent;
  if (newTarget->GetBackendType() == gfx::BackendType::SKIA) {
    // Skia expects the unused X channel to contains 0xFF even for opaque operations
    // so we can't skip clearing in that case, even if we are going to cover the
    // entire canvas in the next drawing operation.
    newTarget->ClearRect(canvasRect);
    needsClear = false;
  }

  // Try to copy data from the previous buffer provider if there is one.
  if (!canDiscardContent && mBufferProvider && CopyBufferProvider(*mBufferProvider, *newTarget, persistedRect)) {
    needsClear = false;
  }

  if (needsClear) {
    newTarget->ClearRect(canvasRect);
  }

  mTarget = newTarget.forget();
  mBufferProvider = newProvider.forget();

  RegisterAllocation();

  RestoreClipsAndTransformToTarget();

  // Force a full layer transaction since we didn't have a layer before
  // and now we might need one.
  if (GetCanvasElement()) {
    GetCanvasElement()->InvalidateCanvas();
  }
  // Calling Redraw() tells our invalidation machinery that the entire
  // canvas is already invalid, which can speed up future drawing.
  Redraw();

  return mode;
}

void
BasicRenderingContext2D::RegisterAllocation()
{
  // XXX - It would make more sense to track the allocation in
  // PeristentBufferProvider, rather than here.
  static bool registered = false;
  // FIXME: Disable the reporter for now, see bug 1241865
  if (!registered && false) {
    registered = true;
    RegisterStrongMemoryReporter(new Canvas2dPixelsReporter());
  }

  sCanvasAzureMemoryUsed += mWidth * mHeight * 4;
  JSContext* context = nsContentUtils::GetCurrentJSContext();
  if (context) {
    JS_updateMallocCounter(context, mWidth * mHeight * 4);
  }

  JSObject* wrapper = GetWrapperPreserveColor();
  if (wrapper) {
    CycleCollectedJSContext::Get()->
      AddZoneWaitingForGC(JS::GetObjectZone(wrapper));
  }
}

bool
BasicRenderingContext2D::TryBasicTarget(RefPtr<gfx::DrawTarget>& aOutDT,
                                        RefPtr<layers::PersistentBufferProvider>& aOutProvider)
{
  aOutDT = gfxPlatform::GetPlatform()->CreateOffscreenCanvasDrawTarget(GetSize(),
                                                                       GetSurfaceFormat());
  if (!aOutDT) {
    return false;
  }

  aOutProvider = new PersistentBufferProviderBasic(aOutDT);
  return true;
}

bool
BasicRenderingContext2D::CopyBufferProvider(PersistentBufferProvider& aOld,
                                            DrawTarget& aTarget,
                                            IntRect aCopyRect)
{
  // Borrowing the snapshot must be done after ReturnTarget.
  RefPtr<SourceSurface> snapshot = aOld.BorrowSnapshot();

  if (!snapshot) {
    return false;
  }

  aTarget.CopySurface(snapshot, aCopyRect, IntPoint());
  aOld.ReturnSnapshot(snapshot.forget());
  return true;
}

void
BasicRenderingContext2D::RestoreClipsAndTransformToTarget()
{
  // Restore clips and transform.
  mTarget->SetTransform(Matrix());

  if (mTarget->GetBackendType() == gfx::BackendType::CAIRO) {
    // Cairo doesn't play well with huge clips. When given a very big clip it
    // will try to allocate big mask surface without taking the target
    // size into account which can cause OOM. See bug 1034593.
    // This limits the clip extents to the size of the canvas.
    // A fix in Cairo would probably be preferable, but requires somewhat
    // invasive changes.
    mTarget->PushClipRect(gfx::Rect(0, 0, mWidth, mHeight));
  }

  for (const auto& style : mStyleStack) {
    for (const auto& clipOrTransform : style->clipsAndTransforms) {
      if (clipOrTransform.IsClip()) {
        mTarget->PushClip(clipOrTransform.clip);
      } else {
        mTarget->SetTransform(clipOrTransform.transform);
      }
    }
  }
}

void
BasicRenderingContext2D::ReturnTarget(bool aForceReset)
{
  if (mTarget && mBufferProvider && mTarget != sErrorTarget) {
    CurrentState()->transform = mTarget->GetTransform();
    if (aForceReset || !mBufferProvider->PreservesDrawingState()) {
      for (const auto& style : mStyleStack) {
        for (const auto& clipOrTransform : style->clipsAndTransforms) {
          if (clipOrTransform.IsClip()) {
            mTarget->PopClip();
          }
        }
      }

      if (mTarget->GetBackendType() == gfx::BackendType::CAIRO) {
        // With the cairo backend we pushed an extra clip rect which we have to
        // balance out here. See the comment in RestoreClipsAndTransformToTarget.
        mTarget->PopClip();
      }

      mTarget->SetTransform(Matrix());
    }

    mBufferProvider->ReturnDrawTarget(mTarget.forget());
  }
}

void
BasicRenderingContext2D::ScheduleStableStateCallback()
{
  if (mHasPendingStableStateCallback) {
    return;
  }
  mHasPendingStableStateCallback = true;

  nsContentUtils::RunInStableState(
    NewRunnableMethod(this, &BasicRenderingContext2D::OnStableState)
  );
}

void
BasicRenderingContext2D::OnStableState()
{
  if (!mHasPendingStableStateCallback) {
    return;
  }

  ReturnTarget();

  mHasPendingStableStateCallback = false;
}

void
BasicRenderingContext2D::SetErrorState()
{
  EnsureErrorTarget();

  if (mTarget && mTarget != sErrorTarget) {
    sCanvasAzureMemoryUsed -= mWidth * mHeight * 4;
  }

  mTarget = sErrorTarget;
  mBufferProvider = nullptr;

  // clear transforms, clips, etc.
  SetInitialState();
}

void
BasicRenderingContext2D::SetInitialState()
{
  // Set up the initial canvas defaults
  mPathBuilder = nullptr;
  mPath = nullptr;
  mDSPathBuilder = nullptr;

  mStyleStack.Clear();

  RefPtr<ContextState> state = CreateContextState();
  state->globalAlpha = 1.0;
  state->colorStyles[Style::FILL] = NS_RGB(0,0,0);
  state->colorStyles[Style::STROKE] = NS_RGB(0,0,0);
  state->shadowColor = NS_RGBA(0,0,0,0);

  mStyleStack.AppendElement(state);

}

void
BasicRenderingContext2D::EnsureErrorTarget()
{
  if (sErrorTarget) {
    return;
  }

  RefPtr<DrawTarget> errorTarget = gfxPlatform::GetPlatform()->CreateOffscreenCanvasDrawTarget(IntSize(1, 1), SurfaceFormat::B8G8R8A8);
  MOZ_ASSERT(errorTarget, "Failed to allocate the error target!");

  sErrorTarget = errorTarget;
  NS_ADDREF(sErrorTarget);
}

bool
BasicRenderingContext2D::PatternIsOpaque(BasicRenderingContext2D::Style aStyle) const
{
  const ContextState* state = CurrentState();
  if (state->globalAlpha < 1.0) {
    return false;
  }

  if (state->patternStyles[aStyle] && state->patternStyles[aStyle]->mSurface) {
    return IsOpaqueFormat(state->patternStyles[aStyle]->mSurface->GetFormat());
  }

  // TODO: for gradient patterns we could check that all stops are opaque
  // colors.

  if (!state->gradientStyles[aStyle]) {
    // it's a color pattern.
    return Color::FromABGR(state->colorStyles[aStyle]).a >= 1.0;
  }

  return false;
}

void
BasicRenderingContext2D::GetStyleAsUnion(OwningStringOrCanvasGradientOrCanvasPattern& aValue,
                                         Style aWhichStyle)
{
  const ContextState* state = CurrentState();
  if (state->patternStyles[aWhichStyle]) {
    aValue.SetAsCanvasPattern() = state->patternStyles[aWhichStyle];
  } else if (state->gradientStyles[aWhichStyle]) {
    aValue.SetAsCanvasGradient() = state->gradientStyles[aWhichStyle];
  } else {
    StyleColorToString(state->colorStyles[aWhichStyle], aValue.SetAsString());
  }
}

void
BasicRenderingContext2D::SetStyleFromString(const nsAString& aStr,
                                            Style aWhichStyle)
{
  MOZ_ASSERT(!aStr.IsVoid());

  nscolor color;
  if (!ParseColor(aStr, &color)) {
    return;
  }

  CurrentState()->SetColorStyle(aWhichStyle, color);
}

// static
void
BasicRenderingContext2D::StyleColorToString(const nscolor& aColor, nsAString& aStr)
{
  // We can't reuse the normal CSS color stringification code,
  // because the spec calls for a different algorithm for canvas.
  if (NS_GET_A(aColor) == 255) {
    CopyUTF8toUTF16(nsPrintfCString("#%02x%02x%02x",
                                    NS_GET_R(aColor),
                                    NS_GET_G(aColor),
                                    NS_GET_B(aColor)),
                                    aStr);
  } else {
    CopyUTF8toUTF16(nsPrintfCString("rgba(%d, %d, %d, ",
                                    NS_GET_R(aColor),
                                    NS_GET_G(aColor),
                                    NS_GET_B(aColor)),
                                    aStr);
    aStr.AppendFloat(nsStyleUtil::ColorComponentToFloat(NS_GET_A(aColor)));
    aStr.Append(')');
  }
}

//
// state
//

void
BasicRenderingContext2D::Save()
{
  EnsureTarget();
  mStyleStack[mStyleStack.Length() - 1]->transform = mTarget->GetTransform();
  RefPtr<ContextState> state = CreateContextState(CurrentState());
  mStyleStack.AppendElement(state);

  if (mStyleStack.Length() > MAX_STYLE_STACK_SIZE) {
    // This is not fast, but is better than OOMing and shouldn't be hit by
    // reasonable code.
    mStyleStack.RemoveElementAt(0);
  }
}

void
BasicRenderingContext2D::Restore()
{
  if (mStyleStack.Length() - 1 == 0)
    return;

  TransformWillUpdate();

  for (const auto& clipOrTransform : CurrentState()->clipsAndTransforms) {
    if (clipOrTransform.IsClip()) {
      mTarget->PopClip();
    }
  }

  mStyleStack.RemoveElementAt(mStyleStack.Length() - 1);

  mTarget->SetTransform(CurrentState()->transform);
}

//
// transformations
//

void
BasicRenderingContext2D::Scale(double aX, double aY, ErrorResult& aError)
{
  TransformWillUpdate();
  if (!IsTargetValid()) {
    aError.Throw(NS_ERROR_FAILURE);
    return;
  }

  Matrix newMatrix = mTarget->GetTransform();
  newMatrix.PreScale(aX, aY);

  SetTransformInternal(newMatrix);
}

void
BasicRenderingContext2D::Rotate(double aAngle, ErrorResult& aError)
{
  TransformWillUpdate();
  if (!IsTargetValid()) {
    aError.Throw(NS_ERROR_FAILURE);
    return;
  }

  Matrix newMatrix = Matrix::Rotation(aAngle) * mTarget->GetTransform();

  SetTransformInternal(newMatrix);
}

void
BasicRenderingContext2D::Translate(double aX, double aY, ErrorResult& aError)
{
  TransformWillUpdate();
  if (!IsTargetValid()) {
    aError.Throw(NS_ERROR_FAILURE);
    return;
  }

  Matrix newMatrix = mTarget->GetTransform();
  newMatrix.PreTranslate(aX, aY);

  SetTransformInternal(newMatrix);
}

void
BasicRenderingContext2D::Transform(double aM11, double aM12, double aM21,
                                   double aM22, double aDx, double aDy,
                                   ErrorResult& aError)
{
  TransformWillUpdate();
  if (!IsTargetValid()) {
    aError.Throw(NS_ERROR_FAILURE);
    return;
  }

  Matrix newMatrix(aM11, aM12, aM21, aM22, aDx, aDy);
  newMatrix *= mTarget->GetTransform();

  SetTransformInternal(newMatrix);
}

void
BasicRenderingContext2D::SetTransform(double aM11, double aM12,
                                      double aM21, double aM22,
                                      double aDx, double aDy,
                                      ErrorResult& aError)
{
  TransformWillUpdate();
  if (!IsTargetValid()) {
    aError.Throw(NS_ERROR_FAILURE);
    return;
  }

  SetTransformInternal(Matrix(aM11, aM12, aM21, aM22, aDx, aDy));
}

void
BasicRenderingContext2D::SetTransformInternal(const Matrix& aTransform)
{
  if (!aTransform.IsFinite()) {
    return;
  }

  // Save the transform in the clip stack to be able to replay clips properly.
  auto& clipsAndTransforms = CurrentState()->clipsAndTransforms;
  if (clipsAndTransforms.IsEmpty() || clipsAndTransforms.LastElement().IsClip()) {
    clipsAndTransforms.AppendElement(ClipState(aTransform));
  } else {
    // If the last item is a transform we can replace it instead of appending
    // a new item.
    clipsAndTransforms.LastElement().transform = aTransform;
  }
  mTarget->SetTransform(aTransform);
}

void
BasicRenderingContext2D::ResetTransform(ErrorResult& aError)
{
  SetTransform(1.0, 0.0, 0.0, 1.0, 0.0, 0.0, aError);
}

//
// compositing
//

void
BasicRenderingContext2D::SetGlobalCompositeOperation(const nsAString& aOp,
                                                     ErrorResult& aError)
{
  CompositionOp comp_op;

#define CANVAS_OP_TO_GFX_OP(cvsop, op2d) \
  if (aOp.EqualsLiteral(cvsop))   \
    comp_op = CompositionOp::OP_##op2d;

  CANVAS_OP_TO_GFX_OP("copy", SOURCE)
  else CANVAS_OP_TO_GFX_OP("source-atop", ATOP)
  else CANVAS_OP_TO_GFX_OP("source-in", IN)
  else CANVAS_OP_TO_GFX_OP("source-out", OUT)
  else CANVAS_OP_TO_GFX_OP("source-over", OVER)
  else CANVAS_OP_TO_GFX_OP("destination-in", DEST_IN)
  else CANVAS_OP_TO_GFX_OP("destination-out", DEST_OUT)
  else CANVAS_OP_TO_GFX_OP("destination-over", DEST_OVER)
  else CANVAS_OP_TO_GFX_OP("destination-atop", DEST_ATOP)
  else CANVAS_OP_TO_GFX_OP("lighter", ADD)
  else CANVAS_OP_TO_GFX_OP("xor", XOR)
  else CANVAS_OP_TO_GFX_OP("multiply", MULTIPLY)
  else CANVAS_OP_TO_GFX_OP("screen", SCREEN)
  else CANVAS_OP_TO_GFX_OP("overlay", OVERLAY)
  else CANVAS_OP_TO_GFX_OP("darken", DARKEN)
  else CANVAS_OP_TO_GFX_OP("lighten", LIGHTEN)
  else CANVAS_OP_TO_GFX_OP("color-dodge", COLOR_DODGE)
  else CANVAS_OP_TO_GFX_OP("color-burn", COLOR_BURN)
  else CANVAS_OP_TO_GFX_OP("hard-light", HARD_LIGHT)
  else CANVAS_OP_TO_GFX_OP("soft-light", SOFT_LIGHT)
  else CANVAS_OP_TO_GFX_OP("difference", DIFFERENCE)
  else CANVAS_OP_TO_GFX_OP("exclusion", EXCLUSION)
  else CANVAS_OP_TO_GFX_OP("hue", HUE)
  else CANVAS_OP_TO_GFX_OP("saturation", SATURATION)
  else CANVAS_OP_TO_GFX_OP("color", COLOR)
  else CANVAS_OP_TO_GFX_OP("luminosity", LUMINOSITY)
  // XXX ERRMSG we need to report an error to developers here! (bug 329026)
  else return;

#undef CANVAS_OP_TO_GFX_OP
  CurrentState()->op = comp_op;
}

void
BasicRenderingContext2D::GetGlobalCompositeOperation(nsAString& aOp,
                                                     ErrorResult& aError)
{
  CompositionOp comp_op = CurrentState()->op;

#define CANVAS_OP_TO_GFX_OP(cvsop, op2d) \
  if (comp_op == CompositionOp::OP_##op2d) \
    aOp.AssignLiteral(cvsop);

  CANVAS_OP_TO_GFX_OP("copy", SOURCE)
  else CANVAS_OP_TO_GFX_OP("destination-atop", DEST_ATOP)
  else CANVAS_OP_TO_GFX_OP("destination-in", DEST_IN)
  else CANVAS_OP_TO_GFX_OP("destination-out", DEST_OUT)
  else CANVAS_OP_TO_GFX_OP("destination-over", DEST_OVER)
  else CANVAS_OP_TO_GFX_OP("lighter", ADD)
  else CANVAS_OP_TO_GFX_OP("source-atop", ATOP)
  else CANVAS_OP_TO_GFX_OP("source-in", IN)
  else CANVAS_OP_TO_GFX_OP("source-out", OUT)
  else CANVAS_OP_TO_GFX_OP("source-over", OVER)
  else CANVAS_OP_TO_GFX_OP("xor", XOR)
  else CANVAS_OP_TO_GFX_OP("multiply", MULTIPLY)
  else CANVAS_OP_TO_GFX_OP("screen", SCREEN)
  else CANVAS_OP_TO_GFX_OP("overlay", OVERLAY)
  else CANVAS_OP_TO_GFX_OP("darken", DARKEN)
  else CANVAS_OP_TO_GFX_OP("lighten", LIGHTEN)
  else CANVAS_OP_TO_GFX_OP("color-dodge", COLOR_DODGE)
  else CANVAS_OP_TO_GFX_OP("color-burn", COLOR_BURN)
  else CANVAS_OP_TO_GFX_OP("hard-light", HARD_LIGHT)
  else CANVAS_OP_TO_GFX_OP("soft-light", SOFT_LIGHT)
  else CANVAS_OP_TO_GFX_OP("difference", DIFFERENCE)
  else CANVAS_OP_TO_GFX_OP("exclusion", EXCLUSION)
  else CANVAS_OP_TO_GFX_OP("hue", HUE)
  else CANVAS_OP_TO_GFX_OP("saturation", SATURATION)
  else CANVAS_OP_TO_GFX_OP("color", COLOR)
  else CANVAS_OP_TO_GFX_OP("luminosity", LUMINOSITY)
  else {
    aError.Throw(NS_ERROR_FAILURE);
  }

#undef CANVAS_OP_TO_GFX_OP
}

//
// gradients and patterns
//

already_AddRefed<CanvasGradient>
BasicRenderingContext2D::CreateLinearGradient(double aX0, double aY0, double aX1, double aY1)
{
  RefPtr<CanvasGradient> grad =
    new CanvasLinearGradient(this, Point(aX0, aY0), Point(aX1, aY1));

  return grad.forget();
}

already_AddRefed<CanvasGradient>
BasicRenderingContext2D::CreateRadialGradient(double aX0, double aY0, double aR0,
                                              double aX1, double aY1, double aR1,
                                              ErrorResult& aError)
{
  if (aR0 < 0.0 || aR1 < 0.0) {
    aError.Throw(NS_ERROR_DOM_INDEX_SIZE_ERR);
    return nullptr;
  }

  RefPtr<CanvasGradient> grad =
    new CanvasRadialGradient(this, Point(aX0, aY0), aR0, Point(aX1, aY1), aR1);

  return grad.forget();
}

already_AddRefed<CanvasPattern>
BasicRenderingContext2D::CreatePattern(const CanvasImageSource& aSource,
                                       const nsAString& aRepeat,
                                       ErrorResult& aError)
{
  CanvasPattern::RepeatMode repeatMode =
    CanvasPattern::RepeatMode::NOREPEAT;

  if (aRepeat.IsEmpty() || aRepeat.EqualsLiteral("repeat")) {
    repeatMode = CanvasPattern::RepeatMode::REPEAT;
  } else if (aRepeat.EqualsLiteral("repeat-x")) {
    repeatMode = CanvasPattern::RepeatMode::REPEATX;
  } else if (aRepeat.EqualsLiteral("repeat-y")) {
    repeatMode = CanvasPattern::RepeatMode::REPEATY;
  } else if (aRepeat.EqualsLiteral("no-repeat")) {
    repeatMode = CanvasPattern::RepeatMode::NOREPEAT;
  } else {
    aError.Throw(NS_ERROR_DOM_SYNTAX_ERR);
    return nullptr;
  }

  Element* htmlElement;
  if (aSource.IsHTMLCanvasElement()) {
    HTMLCanvasElement* canvas = &aSource.GetAsHTMLCanvasElement();
    htmlElement = canvas;

    nsIntSize size = canvas->GetSize();
    if (size.width == 0 || size.height == 0) {
      aError.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
      return nullptr;
    }

    // Special case for Canvas, which could be an Azure canvas!
    nsICanvasRenderingContextInternal *srcCanvas = canvas->GetContextAtIndex(0);
    if (srcCanvas) {
      // This might not be an Azure canvas!
      RefPtr<SourceSurface> srcSurf = srcCanvas->GetSurfaceSnapshot();
      if (!srcSurf) {
        JSContext* context = nsContentUtils::GetCurrentJSContext();
        if (context) {
          JS_ReportWarningASCII(context,
                                "CanvasRenderingContext2D.createPattern()"
                                " failed to snapshot source canvas.");
        }
        aError.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
        return nullptr;
      }

      RefPtr<CanvasPattern> pat =
        new CanvasPattern(this, srcSurf, repeatMode,
                          htmlElement->NodePrincipal(),
                          canvas->IsWriteOnly(), false);

      return pat.forget();
    }
  } else if (aSource.IsHTMLImageElement()) {
    HTMLImageElement* img = &aSource.GetAsHTMLImageElement();
    if (img->IntrinsicState().HasState(NS_EVENT_STATE_BROKEN)) {
      aError.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
      return nullptr;
    }

    htmlElement = img;
  } else if (aSource.IsHTMLVideoElement()) {
    auto& video = aSource.GetAsHTMLVideoElement();
    video.MarkAsContentSource(mozilla::dom::HTMLVideoElement::CallerAPI::CREATE_PATTERN);
    htmlElement = &video;
  } else {
    // Special case for ImageBitmap
    ImageBitmap& imgBitmap = aSource.GetAsImageBitmap();
    EnsureTarget();
    RefPtr<SourceSurface> srcSurf = imgBitmap.PrepareForDrawTarget(mTarget);
    if (!srcSurf) {
      JSContext* context = nsContentUtils::GetCurrentJSContext();
      if (context) {
        JS_ReportWarningASCII(context,
                              "CanvasRenderingContext2D.createPattern()"
                              " failed to prepare source ImageBitmap.");
      }
      aError.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
      return nullptr;
    }

    // An ImageBitmap never taints others so we set principalForSecurityCheck to
    // nullptr and set CORSUsed to true for passing the security check in
    // CanvasUtils::DoDrawImageSecurityCheck().
    RefPtr<CanvasPattern> pat =
      new CanvasPattern(this, srcSurf, repeatMode, nullptr, false, true);

    return pat.forget();
  }

  EnsureTarget();

  // The canvas spec says that createPattern should use the first frame
  // of animated images
  nsLayoutUtils::SurfaceFromElementResult res =
    nsLayoutUtils::SurfaceFromElement(htmlElement,
      nsLayoutUtils::SFE_WANT_FIRST_FRAME, mTarget);

  if (!res.GetSourceSurface()) {
    return nullptr;
  }

  RefPtr<CanvasPattern> pat = new CanvasPattern(this, res.GetSourceSurface(), repeatMode,
                                                res.mPrincipal, res.mIsWriteOnly,
                                                res.mCORSUsed);
  return pat.forget();
}

//
// colors
//

void
BasicRenderingContext2D::SetStyleFromUnion(const StringOrCanvasGradientOrCanvasPattern& aValue,
                                           Style aWhichStyle)
{
  if (aValue.IsString()) {
    SetStyleFromString(aValue.GetAsString(), aWhichStyle);
    return;
  }

  if (aValue.IsCanvasGradient()) {
    SetStyleFromGradient(aValue.GetAsCanvasGradient(), aWhichStyle);
    return;
  }

  if (aValue.IsCanvasPattern()) {
    SetStyleFromPattern(aValue.GetAsCanvasPattern(), aWhichStyle);
    return;
  }

  MOZ_ASSERT_UNREACHABLE("Invalid union value");
}

//
// shadows
//

void
BasicRenderingContext2D::SetShadowColor(const nsAString& aShadowColor)
{
  nscolor color;
  if (!ParseColor(aShadowColor, &color)) {
    return;
  }

  CurrentState()->shadowColor = color;
}

//
// rects
//

static bool
ValidateRect(double& aX, double& aY, double& aWidth, double& aHeight, bool aIsZeroSizeValid)
{
  if (!aIsZeroSizeValid && (aWidth == 0.0 || aHeight == 0.0)) {
    return false;
  }

  // bug 1018527
  // The values of canvas API input are in double precision, but Moz2D APIs are
  // using float precision. Bypass canvas API calls when the input is out of
  // float precision to avoid precision problem
  if (!std::isfinite((float)aX) | !std::isfinite((float)aY) |
      !std::isfinite((float)aWidth) | !std::isfinite((float)aHeight)) {
    return false;
  }

  // bug 1074733
  // The canvas spec does not forbid rects with negative w or h, so given
  // corners (x, y), (x+w, y), (x+w, y+h), and (x, y+h) we must generate
  // the appropriate rect by flipping negative dimensions. This prevents
  // draw targets from receiving "empty" rects later on.
  if (aWidth < 0) {
    aWidth = -aWidth;
    aX -= aWidth;
  }
  if (aHeight < 0) {
    aHeight = -aHeight;
    aY -= aHeight;
  }
  return true;
}

void
BasicRenderingContext2D::ClearRect(double aX, double aY, double aW,
                                   double aH)
{
  // Do not allow zeros - it's a no-op at that point per spec.
  if (!ValidateRect(aX, aY, aW, aH, false)) {
    return;
  }

  gfx::Rect clearRect(aX, aY, aW, aH);

  EnsureTarget(&clearRect);

  mTarget->ClearRect(clearRect);

  RedrawUser(gfxRect(aX, aY, aW, aH));
}

void
BasicRenderingContext2D::FillRect(double aX, double aY, double aW,
                                  double aH)
{
  const ContextState* state = CurrentState();

  if (!ValidateRect(aX, aY, aW, aH, true)) {
    return;
  }

  if (state->patternStyles[Style::FILL]) {
    CanvasPattern::RepeatMode repeat =
      state->patternStyles[Style::FILL]->mRepeat;
    // In the FillRect case repeat modes are easy to deal with.
    bool limitx = repeat == CanvasPattern::RepeatMode::NOREPEAT || repeat == CanvasPattern::RepeatMode::REPEATY;
    bool limity = repeat == CanvasPattern::RepeatMode::NOREPEAT || repeat == CanvasPattern::RepeatMode::REPEATX;

    IntSize patternSize =
      state->patternStyles[Style::FILL]->mSurface->GetSize();

    // We always need to execute painting for non-over operators, even if
    // we end up with w/h = 0.
    if (limitx) {
      if (aX < 0) {
        aW += aX;
        if (aW < 0) {
          aW = 0;
        }

        aX = 0;
      }
      if (aX + aW > patternSize.width) {
        aW = patternSize.width - aX;
        if (aW < 0) {
          aW = 0;
        }
      }
    }
    if (limity) {
      if (aY < 0) {
        aH += aY;
        if (aH < 0) {
          aH = 0;
        }

        aY = 0;
      }
      if (aY + aH > patternSize.height) {
        aH = patternSize.height - aY;
        if (aH < 0) {
          aH = 0;
        }
      }
    }
  }

  CompositionOp op = UsedOperation();
  bool discardContent = PatternIsOpaque(Style::FILL)
    && (op == CompositionOp::OP_OVER || op == CompositionOp::OP_SOURCE);

  const gfx::Rect fillRect(aX, aY, aW, aH);
  EnsureTarget(discardContent ? &fillRect : nullptr);

  gfx::Rect bounds;
  if (NeedToCalculateBounds()) {
    bounds = mTarget->GetTransform().TransformBounds(fillRect);
  }

  AntialiasMode antialiasMode = CurrentState()->imageSmoothingEnabled ?
                                AntialiasMode::DEFAULT : AntialiasMode::NONE;

  AdjustedTarget(this, bounds.IsEmpty() ? nullptr : &bounds)->
    FillRect(gfx::Rect(aX, aY, aW, aH),
             CanvasGeneralPattern().ForStyle(this, Style::FILL, mTarget),
             DrawOptions(state->globalAlpha, op, antialiasMode));

  RedrawUser(gfxRect(aX, aY, aW, aH));
}

void
BasicRenderingContext2D::StrokeRect(double aX, double aY, double aW,
                                    double aH)
{
  const ContextState* state = CurrentState();

  gfx::Rect bounds;

  if (!aW && !aH) {
    return;
  }

  if (!ValidateRect(aX, aY, aW, aH, true)) {
    return;
  }

  EnsureTarget();
  if (!IsTargetValid()) {
    return;
  }

  if (NeedToCalculateBounds()) {
    bounds = gfx::Rect(aX - state->lineWidth / 2.0f, aY - state->lineWidth / 2.0f,
                       aW + state->lineWidth, aH + state->lineWidth);
    bounds = mTarget->GetTransform().TransformBounds(bounds);
  }

  if (!aH) {
    CapStyle cap = CapStyle::BUTT;
    if (state->lineJoin == JoinStyle::ROUND) {
      cap = CapStyle::ROUND;
    }
    AdjustedTarget(this, bounds.IsEmpty() ? nullptr : &bounds)->
      StrokeLine(Point(aX, aY), Point(aX + aW, aY),
                  CanvasGeneralPattern().ForStyle(this, Style::STROKE, mTarget),
                  StrokeOptions(state->lineWidth, state->lineJoin,
                                cap, state->miterLimit,
                                state->dash.Length(),
                                state->dash.Elements(),
                                state->dashOffset),
                  DrawOptions(state->globalAlpha, UsedOperation()));
    return;
  }

  if (!aW) {
    CapStyle cap = CapStyle::BUTT;
    if (state->lineJoin == JoinStyle::ROUND) {
      cap = CapStyle::ROUND;
    }
    AdjustedTarget(this, bounds.IsEmpty() ? nullptr : &bounds)->
      StrokeLine(Point(aX, aY), Point(aX, aY + aH),
                  CanvasGeneralPattern().ForStyle(this, Style::STROKE, mTarget),
                  StrokeOptions(state->lineWidth, state->lineJoin,
                                cap, state->miterLimit,
                                state->dash.Length(),
                                state->dash.Elements(),
                                state->dashOffset),
                  DrawOptions(state->globalAlpha, UsedOperation()));
    return;
  }

  AdjustedTarget(this, bounds.IsEmpty() ? nullptr : &bounds)->
    StrokeRect(gfx::Rect(aX, aY, aW, aH),
               CanvasGeneralPattern().ForStyle(this, Style::STROKE, mTarget),
               StrokeOptions(state->lineWidth, state->lineJoin,
                             state->lineCap, state->miterLimit,
                             state->dash.Length(),
                             state->dash.Elements(),
                             state->dashOffset),
               DrawOptions(state->globalAlpha, UsedOperation()));

  Redraw();
}

//
// path bits
//

void
BasicRenderingContext2D::BeginPath()
{
  mPath = nullptr;
  mPathBuilder = nullptr;
  mDSPathBuilder = nullptr;
  mPathTransformWillUpdate = false;
}

void
BasicRenderingContext2D::Fill(const CanvasWindingRule& aWinding)
{
  EnsureUserSpacePath(aWinding);

  if (!mPath) {
    return;
  }

  gfx::Rect bounds;

  if (NeedToCalculateBounds()) {
    bounds = mPath->GetBounds(mTarget->GetTransform());
  }

  AdjustedTarget(this, bounds.IsEmpty() ? nullptr : &bounds)->
    Fill(mPath, CanvasGeneralPattern().ForStyle(this, Style::FILL, mTarget),
         DrawOptions(CurrentState()->globalAlpha, UsedOperation()));

  Redraw();
}

void BasicRenderingContext2D::Fill(const CanvasPath& aPath, const CanvasWindingRule& aWinding)
{
  EnsureTarget();

  RefPtr<gfx::Path> gfxpath = aPath.GetPath(aWinding, mTarget);

  if (!gfxpath) {
    return;
  }

  gfx::Rect bounds;

  if (NeedToCalculateBounds()) {
    bounds = gfxpath->GetBounds(mTarget->GetTransform());
  }

  AdjustedTarget(this, bounds.IsEmpty() ? nullptr : &bounds)->
    Fill(gfxpath, CanvasGeneralPattern().ForStyle(this, Style::FILL, mTarget),
         DrawOptions(CurrentState()->globalAlpha, UsedOperation()));

  Redraw();
}

void
BasicRenderingContext2D::Stroke()
{
  EnsureUserSpacePath();

  if (!mPath) {
    return;
  }

  const ContextState* state = CurrentState();

  StrokeOptions strokeOptions(state->lineWidth, state->lineJoin,
                              state->lineCap, state->miterLimit,
                              state->dash.Length(), state->dash.Elements(),
                              state->dashOffset);

  gfx::Rect bounds;
  if (NeedToCalculateBounds()) {
    bounds =
      mPath->GetStrokedBounds(strokeOptions, mTarget->GetTransform());
  }

  AdjustedTarget(this, bounds.IsEmpty() ? nullptr : &bounds)->
    Stroke(mPath, CanvasGeneralPattern().ForStyle(this, Style::STROKE, mTarget),
           strokeOptions, DrawOptions(state->globalAlpha, UsedOperation()));

  Redraw();
}

void
BasicRenderingContext2D::Stroke(const CanvasPath& aPath)
{
  EnsureTarget();

  RefPtr<gfx::Path> gfxpath = aPath.GetPath(CanvasWindingRule::Nonzero, mTarget);

  if (!gfxpath) {
    return;
  }

  const ContextState* state = CurrentState();

  StrokeOptions strokeOptions(state->lineWidth, state->lineJoin,
                              state->lineCap, state->miterLimit,
                              state->dash.Length(), state->dash.Elements(),
                              state->dashOffset);

  gfx::Rect bounds;
  if (NeedToCalculateBounds()) {
    bounds =
      gfxpath->GetStrokedBounds(strokeOptions, mTarget->GetTransform());
  }

  AdjustedTarget(this, bounds.IsEmpty() ? nullptr : &bounds)->
    Stroke(gfxpath, CanvasGeneralPattern().ForStyle(this, Style::STROKE, mTarget),
           strokeOptions, DrawOptions(state->globalAlpha, UsedOperation()));

  Redraw();
}

void
BasicRenderingContext2D::Clip(const CanvasWindingRule& aWinding)
{
  EnsureUserSpacePath(aWinding);

  if (!mPath) {
    return;
  }

  mTarget->PushClip(mPath);
  CurrentState()->clipsAndTransforms.AppendElement(ClipState(mPath));
}

void
BasicRenderingContext2D::Clip(const CanvasPath& aPath, const CanvasWindingRule& aWinding)
{
  EnsureTarget();

  RefPtr<gfx::Path> gfxpath = aPath.GetPath(aWinding, mTarget);

  if (!gfxpath) {
    return;
  }

  mTarget->PushClip(gfxpath);
  CurrentState()->clipsAndTransforms.AppendElement(ClipState(gfxpath));
}

bool
BasicRenderingContext2D::IsPointInPath(double aX, double aY, const CanvasWindingRule& aWinding)
{
  if (!FloatValidate(aX, aY)) {
    return false;
  }

  EnsureUserSpacePath(aWinding);
  if (!mPath) {
    return false;
  }

  if (mPathTransformWillUpdate) {
    return mPath->ContainsPoint(Point(aX, aY), mPathToDS);
  }

  return mPath->ContainsPoint(Point(aX, aY), mTarget->GetTransform());
}

bool BasicRenderingContext2D::IsPointInPath(const CanvasPath& aPath, double aX, double aY, const CanvasWindingRule& aWinding)
{
  if (!FloatValidate(aX, aY)) {
    return false;
  }

  EnsureTarget();
  RefPtr<gfx::Path> tempPath = aPath.GetPath(aWinding, mTarget);

  return tempPath->ContainsPoint(Point(aX, aY), mTarget->GetTransform());
}

bool
BasicRenderingContext2D::IsPointInStroke(double aX, double aY)
{
  if (!FloatValidate(aX, aY)) {
    return false;
  }

  EnsureUserSpacePath();
  if (!mPath) {
    return false;
  }

  const ContextState* state = CurrentState();

  StrokeOptions strokeOptions(state->lineWidth,
                              state->lineJoin,
                              state->lineCap,
                              state->miterLimit,
                              state->dash.Length(),
                              state->dash.Elements(),
                              state->dashOffset);

  if (mPathTransformWillUpdate) {
    return mPath->StrokeContainsPoint(strokeOptions, Point(aX, aY), mPathToDS);
  }
  return mPath->StrokeContainsPoint(strokeOptions, Point(aX, aY), mTarget->GetTransform());
}

bool BasicRenderingContext2D::IsPointInStroke(const CanvasPath& aPath, double aX, double aY)
{
  if (!FloatValidate(aX, aY)) {
    return false;
  }

  EnsureTarget();
  RefPtr<gfx::Path> tempPath = aPath.GetPath(CanvasWindingRule::Nonzero, mTarget);

  const ContextState* state = CurrentState();

  StrokeOptions strokeOptions(state->lineWidth,
                              state->lineJoin,
                              state->lineCap,
                              state->miterLimit,
                              state->dash.Length(),
                              state->dash.Elements(),
                              state->dashOffset);

  return tempPath->StrokeContainsPoint(strokeOptions, Point(aX, aY), mTarget->GetTransform());
}

void
BasicRenderingContext2D::ArcTo(double aX1, double aY1, double aX2,
                               double aY2, double aRadius,
                               ErrorResult& aError)
{
  if (aRadius < 0) {
    aError.Throw(NS_ERROR_DOM_INDEX_SIZE_ERR);
    return;
  }

  EnsureWritablePath();

  // Current point in user space!
  Point p0;
  if (mPathBuilder) {
    p0 = mPathBuilder->CurrentPoint();
  } else {
    Matrix invTransform = mTarget->GetTransform();
    if (!invTransform.Invert()) {
      return;
    }

    p0 = invTransform.TransformPoint(mDSPathBuilder->CurrentPoint());
  }

  Point p1(aX1, aY1);
  Point p2(aX2, aY2);

  // Execute these calculations in double precision to avoid cumulative
  // rounding errors.
  double dir, a2, b2, c2, cosx, sinx, d, anx, any,
         bnx, bny, x3, y3, x4, y4, cx, cy, angle0, angle1;
  bool anticlockwise;

  if (p0 == p1 || p1 == p2 || aRadius == 0) {
    LineTo(p1.x, p1.y);
    return;
  }

  // Check for colinearity
  dir = (p2.x - p1.x) * (p0.y - p1.y) + (p2.y - p1.y) * (p1.x - p0.x);
  if (dir == 0) {
    LineTo(p1.x, p1.y);
    return;
  }


  // XXX - Math for this code was already available from the non-azure code
  // and would be well tested. Perhaps converting to bezier directly might
  // be more efficient longer run.
  a2 = (p0.x-aX1)*(p0.x-aX1) + (p0.y-aY1)*(p0.y-aY1);
  b2 = (aX1-aX2)*(aX1-aX2) + (aY1-aY2)*(aY1-aY2);
  c2 = (p0.x-aX2)*(p0.x-aX2) + (p0.y-aY2)*(p0.y-aY2);
  cosx = (a2+b2-c2)/(2*sqrt(a2*b2));

  sinx = sqrt(1 - cosx*cosx);
  d = aRadius / ((1 - cosx) / sinx);

  anx = (aX1-p0.x) / sqrt(a2);
  any = (aY1-p0.y) / sqrt(a2);
  bnx = (aX1-aX2) / sqrt(b2);
  bny = (aY1-aY2) / sqrt(b2);
  x3 = aX1 - anx*d;
  y3 = aY1 - any*d;
  x4 = aX1 - bnx*d;
  y4 = aY1 - bny*d;
  anticlockwise = (dir < 0);
  cx = x3 + any*aRadius*(anticlockwise ? 1 : -1);
  cy = y3 - anx*aRadius*(anticlockwise ? 1 : -1);
  angle0 = atan2((y3-cy), (x3-cx));
  angle1 = atan2((y4-cy), (x4-cx));


  LineTo(x3, y3);

  Arc(cx, cy, aRadius, angle0, angle1, anticlockwise, aError);
}

void
BasicRenderingContext2D::Arc(double aX, double aY, double aR,
                             double aStartAngle, double aEndAngle,
                             bool aAnticlockwise, ErrorResult& aError)
{
  if (aR < 0.0) {
    aError.Throw(NS_ERROR_DOM_INDEX_SIZE_ERR);
    return;
  }

  EnsureWritablePath();

  ArcToBezier(this, Point(aX, aY), Size(aR, aR), aStartAngle, aEndAngle, aAnticlockwise);
}

void
BasicRenderingContext2D::Rect(double aX, double aY, double aW, double aH)
{
  EnsureWritablePath();

  if (mPathBuilder) {
    mPathBuilder->MoveTo(Point(aX, aY));
    mPathBuilder->LineTo(Point(aX + aW, aY));
    mPathBuilder->LineTo(Point(aX + aW, aY + aH));
    mPathBuilder->LineTo(Point(aX, aY + aH));
    mPathBuilder->Close();
  } else {
    mDSPathBuilder->MoveTo(mTarget->GetTransform().TransformPoint(Point(aX, aY)));
    mDSPathBuilder->LineTo(mTarget->GetTransform().TransformPoint(Point(aX + aW, aY)));
    mDSPathBuilder->LineTo(mTarget->GetTransform().TransformPoint(Point(aX + aW, aY + aH)));
    mDSPathBuilder->LineTo(mTarget->GetTransform().TransformPoint(Point(aX, aY + aH)));
    mDSPathBuilder->Close();
  }
}

void
BasicRenderingContext2D::Ellipse(double aX, double aY, double aRadiusX, double aRadiusY,
                                 double aRotation, double aStartAngle, double aEndAngle,
                                 bool aAnticlockwise, ErrorResult& aError)
{
  if (aRadiusX < 0.0 || aRadiusY < 0.0) {
    aError.Throw(NS_ERROR_DOM_INDEX_SIZE_ERR);
    return;
  }

  EnsureWritablePath();

  ArcToBezier(this, Point(aX, aY), Size(aRadiusX, aRadiusY), aStartAngle, aEndAngle,
              aAnticlockwise, aRotation);
}

void
BasicRenderingContext2D::TransformWillUpdate()
{
  EnsureTarget();

  // Store the matrix that would transform the current path to device
  // space.
  if (mPath || mPathBuilder) {
    if (!mPathTransformWillUpdate) {
      // If the transform has already been updated, but a device space builder
      // has not been created yet mPathToDS contains the right transform to
      // transform the current mPath into device space.
      // We should leave it alone.
      mPathToDS = mTarget->GetTransform();
    }
    mPathTransformWillUpdate = true;
  }
}

void
BasicRenderingContext2D::EnsureUserSpacePath(const CanvasWindingRule& aWinding)
{
  FillRule fillRule = CurrentState()->fillRule;
  if (aWinding == CanvasWindingRule::Evenodd)
    fillRule = FillRule::FILL_EVEN_ODD;

  EnsureTarget();

  if (!mPath && !mPathBuilder && !mDSPathBuilder) {
    mPathBuilder = mTarget->CreatePathBuilder(fillRule);
  }

  if (mPathBuilder) {
    mPath = mPathBuilder->Finish();
    mPathBuilder = nullptr;
  }

  if (mPath &&
      mPathTransformWillUpdate) {
    mDSPathBuilder =
      mPath->TransformedCopyToBuilder(mPathToDS, fillRule);
    mPath = nullptr;
    mPathTransformWillUpdate = false;
  }

  if (mDSPathBuilder) {
    RefPtr<Path> dsPath;
    dsPath = mDSPathBuilder->Finish();
    mDSPathBuilder = nullptr;

    Matrix inverse = mTarget->GetTransform();
    if (!inverse.Invert()) {
      NS_WARNING("Could not invert transform");
      return;
    }

    mPathBuilder =
      dsPath->TransformedCopyToBuilder(inverse, fillRule);
    mPath = mPathBuilder->Finish();
    mPathBuilder = nullptr;
  }

  if (mPath && mPath->GetFillRule() != fillRule) {
    mPathBuilder = mPath->CopyToBuilder(fillRule);
    mPath = mPathBuilder->Finish();
    mPathBuilder = nullptr;
  }

  NS_ASSERTION(mPath, "mPath should exist");
}

void
BasicRenderingContext2D::EnsureWritablePath()
{
  EnsureTarget();

  if (mDSPathBuilder) {
    return;
  }

  FillRule fillRule = CurrentState()->fillRule;

  if (mPathBuilder) {
    if (mPathTransformWillUpdate) {
      mPath = mPathBuilder->Finish();
      mDSPathBuilder =
        mPath->TransformedCopyToBuilder(mPathToDS, fillRule);
      mPath = nullptr;
      mPathBuilder = nullptr;
      mPathTransformWillUpdate = false;
    }
    return;
  }

  if (!mPath) {
    NS_ASSERTION(!mPathTransformWillUpdate, "mPathTransformWillUpdate should be false, if all paths are null");
    mPathBuilder = mTarget->CreatePathBuilder(fillRule);
  } else if (!mPathTransformWillUpdate) {
    mPathBuilder = mPath->CopyToBuilder(fillRule);
  } else {
    mDSPathBuilder =
      mPath->TransformedCopyToBuilder(mPathToDS, fillRule);
    mPathTransformWillUpdate = false;
    mPath = nullptr;
  }
}

//
// line caps/joins
//

void
BasicRenderingContext2D::SetLineCap(const nsAString& aLinecapStyle)
{
  CapStyle cap;

  if (aLinecapStyle.EqualsLiteral("butt")) {
    cap = CapStyle::BUTT;
  } else if (aLinecapStyle.EqualsLiteral("round")) {
    cap = CapStyle::ROUND;
  } else if (aLinecapStyle.EqualsLiteral("square")) {
    cap = CapStyle::SQUARE;
  } else {
    // XXX ERRMSG we need to report an error to developers here! (bug 329026)
    return;
  }
  CurrentState()->lineCap = cap;
}

void
BasicRenderingContext2D::GetLineCap(nsAString& aLinecapStyle)
{
  switch (CurrentState()->lineCap) {
  case CapStyle::BUTT:
    aLinecapStyle.AssignLiteral("butt");
    break;
  case CapStyle::ROUND:
    aLinecapStyle.AssignLiteral("round");
    break;
  case CapStyle::SQUARE:
    aLinecapStyle.AssignLiteral("square");
    break;
  }
}

void
BasicRenderingContext2D::SetLineJoin(const nsAString& aLinejoinStyle)
{
  JoinStyle j;

  if (aLinejoinStyle.EqualsLiteral("round")) {
    j = JoinStyle::ROUND;
  } else if (aLinejoinStyle.EqualsLiteral("bevel")) {
    j = JoinStyle::BEVEL;
  } else if (aLinejoinStyle.EqualsLiteral("miter")) {
    j = JoinStyle::MITER_OR_BEVEL;
  } else {
    // XXX ERRMSG we need to report an error to developers here! (bug 329026)
    return;
  }
  CurrentState()->lineJoin = j;
}

void
BasicRenderingContext2D::GetLineJoin(nsAString& aLinejoinStyle, ErrorResult& aError)
{
  switch (CurrentState()->lineJoin) {
  case JoinStyle::ROUND:
    aLinejoinStyle.AssignLiteral("round");
    break;
  case JoinStyle::BEVEL:
    aLinejoinStyle.AssignLiteral("bevel");
    break;
  case JoinStyle::MITER_OR_BEVEL:
    aLinejoinStyle.AssignLiteral("miter");
    break;
  default:
    aError.Throw(NS_ERROR_FAILURE);
  }
}

void
BasicRenderingContext2D::SetLineDash(const Sequence<double>& aSegments,
                                     ErrorResult& aRv)
{
  nsTArray<mozilla::gfx::Float> dash;

  for (uint32_t x = 0; x < aSegments.Length(); x++) {
    if (aSegments[x] < 0.0) {
      // Pattern elements must be finite "numbers" >= 0, with "finite"
      // taken care of by WebIDL
      return;
    }

    if (!dash.AppendElement(aSegments[x], fallible)) {
      aRv.Throw(NS_ERROR_OUT_OF_MEMORY);
      return;
    }
  }
  if (aSegments.Length() % 2) { // If the number of elements is odd, concatenate again
    for (uint32_t x = 0; x < aSegments.Length(); x++) {
      if (!dash.AppendElement(aSegments[x], fallible)) {
        aRv.Throw(NS_ERROR_OUT_OF_MEMORY);
        return;
      }
    }
  }

  CurrentState()->dash = Move(dash);
}

void
BasicRenderingContext2D::GetLineDash(nsTArray<double>& aSegments) const {
  const nsTArray<mozilla::gfx::Float>& dash = CurrentState()->dash;
  aSegments.Clear();

  for (uint32_t x = 0; x < dash.Length(); x++) {
    aSegments.AppendElement(dash[x]);
  }
}

void
BasicRenderingContext2D::SetLineDashOffset(double aOffset) {
  CurrentState()->dashOffset = aOffset;
}

double
BasicRenderingContext2D::LineDashOffset() const {
  return CurrentState()->dashOffset;
}

// Returns a surface that contains only the part needed to draw aSourceRect.
// On entry, aSourceRect is relative to aSurface, and on return aSourceRect is
// relative to the returned surface.
static already_AddRefed<SourceSurface>
ExtractSubrect(SourceSurface* aSurface, gfx::Rect* aSourceRect, DrawTarget* aTargetDT)
{
  gfx::Rect roundedOutSourceRect = *aSourceRect;
  roundedOutSourceRect.RoundOut();
  gfx::IntRect roundedOutSourceRectInt;
  if (!roundedOutSourceRect.ToIntRect(&roundedOutSourceRectInt)) {
    RefPtr<SourceSurface> surface(aSurface);
    return surface.forget();
  }

  RefPtr<DrawTarget> subrectDT =
    aTargetDT->CreateSimilarDrawTarget(roundedOutSourceRectInt.Size(), SurfaceFormat::B8G8R8A8);

  if (!subrectDT) {
    RefPtr<SourceSurface> surface(aSurface);
    return surface.forget();
  }

  *aSourceRect -= roundedOutSourceRect.TopLeft();

  subrectDT->CopySurface(aSurface, roundedOutSourceRectInt, IntPoint());
  return subrectDT->Snapshot();
}

//
// image
//

void
BasicRenderingContext2D::DrawDirectlyToCanvas(
                         const nsLayoutUtils::DirectDrawInfo& aImage,
                         gfx::Rect* aBounds,
                         gfx::Rect aDest,
                         gfx::Rect aSrc,
                         gfx::IntSize aImgSize)
{
  MOZ_ASSERT(aSrc.width > 0 && aSrc.height > 0,
             "Need positive source width and height");

  gfxMatrix contextMatrix;
  AdjustedTarget tempTarget(this, aBounds->IsEmpty() ? nullptr: aBounds);

  // Get any existing transforms on the context, including transformations used
  // for context shadow.
  if (tempTarget) {
    Matrix matrix = tempTarget->GetTransform();
    contextMatrix = gfxMatrix(matrix._11, matrix._12, matrix._21,
                              matrix._22, matrix._31, matrix._32);
  }
  gfxSize contextScale(contextMatrix.ScaleFactors(true));

  // Scale the dest rect to include the context scale.
  aDest.Scale(contextScale.width, contextScale.height);

  // Scale the image size to the dest rect, and adjust the source rect to match.
  gfxSize scale(aDest.width / aSrc.width, aDest.height / aSrc.height);
  IntSize scaledImageSize = IntSize::Ceil(aImgSize.width * scale.width,
                                          aImgSize.height * scale.height);
  aSrc.Scale(scale.width, scale.height);

  // We're wrapping tempTarget's (our) DrawTarget here, so we need to restore
  // the matrix even though this is a temp gfxContext.
  AutoRestoreTransform autoRestoreTransform(mTarget);

  RefPtr<gfxContext> context = gfxContext::CreateOrNull(tempTarget);
  if (!context) {
    gfxDevCrash(LogReason::InvalidContext) << "Canvas context problem";
    return;
  }
  context->SetMatrix(contextMatrix.
                       Scale(1.0 / contextScale.width,
                             1.0 / contextScale.height).
                       Translate(aDest.x - aSrc.x, aDest.y - aSrc.y));

  // FLAG_CLAMP is added for increased performance, since we never tile here.
  uint32_t modifiedFlags = aImage.mDrawingFlags | imgIContainer::FLAG_CLAMP;

  CSSIntSize sz(scaledImageSize.width, scaledImageSize.height); // XXX hmm is scaledImageSize really in CSS pixels?
  SVGImageContext svgContext(sz, Nothing(), CurrentState()->globalAlpha);

  auto result = aImage.mImgContainer->
    Draw(context, scaledImageSize,
         ImageRegion::Create(gfxRect(aSrc.x, aSrc.y, aSrc.width, aSrc.height)),
         aImage.mWhichFrame, SamplingFilter::GOOD, Some(svgContext), modifiedFlags, 1.0);

  if (result != DrawResult::SUCCESS) {
    NS_WARNING("imgIContainer::Draw failed");
  }
}


static void
ClipImageDimension(double& aSourceCoord, double& aSourceSize, int32_t aImageSize,
                   double& aDestCoord, double& aDestSize)
{
  double scale = aDestSize / aSourceSize;
  if (aSourceCoord < 0.0) {
    double destEnd = aDestCoord + aDestSize;
    aDestCoord -= aSourceCoord * scale;
    aDestSize = destEnd - aDestCoord;
    aSourceSize += aSourceCoord;
    aSourceCoord = 0.0;
  }
  double delta = aImageSize - (aSourceCoord + aSourceSize);
  if (delta < 0.0) {
    aDestSize += delta * scale;
    aSourceSize = aImageSize - aSourceCoord;
  }
}

bool
BasicRenderingContext2D::AllowOpenGLCanvas() const
{
  // If we somehow didn't have the correct compositor in the constructor,
  // we could do something like this to get it:
  //
  // HTMLCanvasElement* el = GetCanvas();
  // if (el) {
  //   mCompositorBackend = el->GetCompositorBackendType();
  // }
  //
  // We could have LAYERS_NONE if there was no widget at the time of
  // canvas creation, but in that case the
  // HTMLCanvasElement::GetCompositorBackendType would return LAYERS_NONE
  // as well, so it wouldn't help much.

  return (mCompositorBackend == layers::LayersBackend::LAYERS_OPENGL) &&
    gfxPlatform::GetPlatform()->AllowOpenGLCanvas();
}

// Acts like nsLayoutUtils::SurfaceFromElement, but it'll attempt
// to pull a SourceSurface from our cache. This allows us to avoid
// reoptimizing surfaces if content and canvas backends are different.
nsLayoutUtils::SurfaceFromElementResult
BasicRenderingContext2D::CachedSurfaceFromElement(Element* aElement)
{
  nsLayoutUtils::SurfaceFromElementResult res;
  nsCOMPtr<nsIImageLoadingContent> imageLoader = do_QueryInterface(aElement);
  if (!imageLoader) {
    return res;
  }

  nsCOMPtr<imgIRequest> imgRequest;
  imageLoader->GetRequest(nsIImageLoadingContent::CURRENT_REQUEST,
                          getter_AddRefs(imgRequest));
  if (!imgRequest) {
    return res;
  }

  uint32_t status = 0;
  if (NS_FAILED(imgRequest->GetImageStatus(&status)) ||
      !(status & imgIRequest::STATUS_LOAD_COMPLETE)) {
    return res;
  }

  nsCOMPtr<nsIPrincipal> principal;
  if (NS_FAILED(imgRequest->GetImagePrincipal(getter_AddRefs(principal))) ||
      !principal) {
    return res;
  }

  res.mSourceSurface =
    CanvasImageCache::LookupAllCanvas(aElement, mIsSkiaGL);
  if (!res.mSourceSurface) {
    return res;
  }

  int32_t corsmode = imgIRequest::CORS_NONE;
  if (NS_SUCCEEDED(imgRequest->GetCORSMode(&corsmode))) {
    res.mCORSUsed = corsmode != imgIRequest::CORS_NONE;
  }

  res.mSize = res.mSourceSurface->GetSize();
  res.mPrincipal = principal.forget();
  res.mIsWriteOnly = false;
  res.mImageRequest = imgRequest.forget();

  return res;
}

// drawImage(in HTMLImageElement image, in float dx, in float dy);
//   -- render image from 0,0 at dx,dy top-left coords
// drawImage(in HTMLImageElement image, in float dx, in float dy, in float dw, in float dh);
//   -- render image from 0,0 at dx,dy top-left coords clipping it to dw,dh
// drawImage(in HTMLImageElement image, in float sx, in float sy, in float sw, in float sh, in float dx, in float dy, in float dw, in float dh);
//   -- render the region defined by (sx,sy,sw,wh) in image-local space into the region (dx,dy,dw,dh) on the canvas

// If only dx and dy are passed in then optional_argc should be 0. If only
// dx, dy, dw and dh are passed in then optional_argc should be 2. The only
// other valid value for optional_argc is 6 if sx, sy, sw, sh, dx, dy, dw and dh
// are all passed in.

void
BasicRenderingContext2D::DrawImage(const CanvasImageSource& aImage,
                                   double aSx, double aSy, double aSw,
                                   double aSh, double aDx, double aDy,
                                   double aDw, double aDh,
                                   uint8_t aOptional_argc,
                                   ErrorResult& aError)
{
  DidImageDrawCall();

  MOZ_ASSERT(aOptional_argc == 0 || aOptional_argc == 2 || aOptional_argc == 6);

  if (!ValidateRect(aDx, aDy, aDw, aDh, true)) {
    return;
  }
  if (aOptional_argc == 6) {
    if (!ValidateRect(aSx, aSy, aSw, aSh, true)) {
      return;
    }
  }

  RefPtr<SourceSurface> srcSurf;
  gfx::IntSize imgSize;

  Element* element = nullptr;

  EnsureTarget();
  if (aImage.IsHTMLCanvasElement()) {
    HTMLCanvasElement* canvas = &aImage.GetAsHTMLCanvasElement();
    element = canvas;
    nsIntSize size = canvas->GetSize();
    if (size.width == 0 || size.height == 0) {
      aError.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
      return;
    }
  } else if (aImage.IsImageBitmap()) {
    ImageBitmap& imageBitmap = aImage.GetAsImageBitmap();
    srcSurf = imageBitmap.PrepareForDrawTarget(mTarget);

    if (!srcSurf) {
      return;
    }

    imgSize = gfx::IntSize(imageBitmap.Width(), imageBitmap.Height());
  }
  else {
    if (aImage.IsHTMLImageElement()) {
      HTMLImageElement* img = &aImage.GetAsHTMLImageElement();
      element = img;
    } else {
      HTMLVideoElement* video = &aImage.GetAsHTMLVideoElement();
      video->MarkAsContentSource(mozilla::dom::HTMLVideoElement::CallerAPI::DRAW_IMAGE);
      element = video;
    }

    srcSurf =
     CanvasImageCache::LookupCanvas(element, GetCanvasElement(), &imgSize, mIsSkiaGL);
  }

  nsLayoutUtils::DirectDrawInfo drawInfo;

#ifdef USE_SKIA_GPU
  if (mRenderingMode == RenderingMode::OpenGLBackendMode &&
      mIsSkiaGL &&
      !srcSurf &&
      aImage.IsHTMLVideoElement() &&
      AllowOpenGLCanvas()) {
    mozilla::gl::GLContext* gl = gfxPlatform::GetPlatform()->GetSkiaGLGlue()->GetGLContext();
    MOZ_ASSERT(gl);

    HTMLVideoElement* video = &aImage.GetAsHTMLVideoElement();
    if (!video) {
      return;
    }

    if (video->ContainsRestrictedContent()) {
      aError.Throw(NS_ERROR_NOT_AVAILABLE);
      return;
    }

    uint16_t readyState;
    if (NS_SUCCEEDED(video->GetReadyState(&readyState)) &&
        readyState < nsIDOMHTMLMediaElement::HAVE_CURRENT_DATA) {
      // still loading, just return
      return;
    }

    // If it doesn't have a principal, just bail
    nsCOMPtr<nsIPrincipal> principal = video->GetCurrentVideoPrincipal();
    if (!principal) {
      aError.Throw(NS_ERROR_NOT_AVAILABLE);
      return;
    }

    mozilla::layers::ImageContainer* container = video->GetImageContainer();
    if (!container) {
      aError.Throw(NS_ERROR_NOT_AVAILABLE);
      return;
    }

    AutoLockImage lockImage(container);
    layers::Image* srcImage = lockImage.GetImage();
    if (!srcImage) {
      aError.Throw(NS_ERROR_NOT_AVAILABLE);
      return;
    }

    gl->MakeCurrent();
    GLuint videoTexture = 0;
    gl->fGenTextures(1, &videoTexture);
    // skiaGL expect upload on drawing, and uses texture 0 for texturing,
    // so we must active texture 0 and bind the texture for it.
    gl->fActiveTexture(LOCAL_GL_TEXTURE0);
    gl->fBindTexture(LOCAL_GL_TEXTURE_2D, videoTexture);

    gl->fTexImage2D(LOCAL_GL_TEXTURE_2D, 0, LOCAL_GL_RGB, srcImage->GetSize().width, srcImage->GetSize().height, 0, LOCAL_GL_RGB, LOCAL_GL_UNSIGNED_SHORT_5_6_5, nullptr);
    gl->fTexParameteri(LOCAL_GL_TEXTURE_2D, LOCAL_GL_TEXTURE_WRAP_S, LOCAL_GL_CLAMP_TO_EDGE);
    gl->fTexParameteri(LOCAL_GL_TEXTURE_2D, LOCAL_GL_TEXTURE_WRAP_T, LOCAL_GL_CLAMP_TO_EDGE);
    gl->fTexParameteri(LOCAL_GL_TEXTURE_2D, LOCAL_GL_TEXTURE_MAG_FILTER, LOCAL_GL_LINEAR);
    gl->fTexParameteri(LOCAL_GL_TEXTURE_2D, LOCAL_GL_TEXTURE_MIN_FILTER, LOCAL_GL_LINEAR);

    const gl::OriginPos destOrigin = gl::OriginPos::TopLeft;
    bool ok = gl->BlitHelper()->BlitImageToTexture(srcImage, srcImage->GetSize(),
                                                   videoTexture, LOCAL_GL_TEXTURE_2D,
                                                   destOrigin);
    if (ok) {
      NativeSurface texSurf;
      texSurf.mType = NativeSurfaceType::OPENGL_TEXTURE;
      texSurf.mFormat = SurfaceFormat::R5G6B5_UINT16;
      texSurf.mSize.width = srcImage->GetSize().width;
      texSurf.mSize.height = srcImage->GetSize().height;
      texSurf.mSurface = (void*)((uintptr_t)videoTexture);

      srcSurf = mTarget->CreateSourceSurfaceFromNativeSurface(texSurf);
      if (!srcSurf) {
        gl->fDeleteTextures(1, &videoTexture);
      }
      imgSize.width = srcImage->GetSize().width;
      imgSize.height = srcImage->GetSize().height;

      int32_t displayWidth = video->VideoWidth();
      int32_t displayHeight = video->VideoHeight();
      aSw *= (double)imgSize.width / (double)displayWidth;
      aSh *= (double)imgSize.height / (double)displayHeight;
    } else {
      gl->fDeleteTextures(1, &videoTexture);
    }
    srcImage = nullptr;

    if (GetCanvasElement()) {
      CanvasUtils::DoDrawImageSecurityCheck(GetCanvasElement(),
                                            principal, false,
                                            video->GetCORSMode() != CORS_NONE);
    }

  }
#endif
  if (!srcSurf) {
    // The canvas spec says that drawImage should draw the first frame
    // of animated images. We also don't want to rasterize vector images.
    uint32_t sfeFlags = nsLayoutUtils::SFE_WANT_FIRST_FRAME |
                        nsLayoutUtils::SFE_NO_RASTERIZING_VECTORS;

    nsLayoutUtils::SurfaceFromElementResult res =
      BasicRenderingContext2D::CachedSurfaceFromElement(element);

    if (!res.mSourceSurface) {
      res = nsLayoutUtils::SurfaceFromElement(element, sfeFlags, mTarget);
    }

    if (!res.mSourceSurface && !res.mDrawInfo.mImgContainer) {
      // The spec says to silently do nothing in the following cases:
      //   - The element is still loading.
      //   - The image is bad, but it's not in the broken state (i.e., we could
      //     decode the headers and get the size).
      if (!res.mIsStillLoading && !res.mHasSize) {
        aError.Throw(NS_ERROR_NOT_AVAILABLE);
      }
      return;
    }

    imgSize = res.mSize;

    // Scale sw/sh based on aspect ratio
    if (aImage.IsHTMLVideoElement()) {
      HTMLVideoElement* video = &aImage.GetAsHTMLVideoElement();
      int32_t displayWidth = video->VideoWidth();
      int32_t displayHeight = video->VideoHeight();
      aSw *= (double)imgSize.width / (double)displayWidth;
      aSh *= (double)imgSize.height / (double)displayHeight;
    }

    if (GetCanvasElement()) {
      CanvasUtils::DoDrawImageSecurityCheck(GetCanvasElement(),
                                            res.mPrincipal, res.mIsWriteOnly,
                                            res.mCORSUsed);
    }

    if (res.mSourceSurface) {
      if (res.mImageRequest) {
        CanvasImageCache::NotifyDrawImage(element, GetCanvasElement(), res.mSourceSurface, imgSize, mIsSkiaGL);
      }
      srcSurf = res.mSourceSurface;
    } else {
      drawInfo = res.mDrawInfo;
    }
  }

  if (aOptional_argc == 0) {
    aSx = aSy = 0.0;
    aDw = aSw = (double) imgSize.width;
    aDh = aSh = (double) imgSize.height;
  } else if (aOptional_argc == 2) {
    aSx = aSy = 0.0;
    aSw = (double) imgSize.width;
    aSh = (double) imgSize.height;
  }

  if (aSw == 0.0 || aSh == 0.0) {
    aError.Throw(NS_ERROR_DOM_INDEX_SIZE_ERR);
    return;
  }

  ClipImageDimension(aSx, aSw, imgSize.width, aDx, aDw);
  ClipImageDimension(aSy, aSh, imgSize.height, aDy, aDh);

  if (aSw <= 0.0 || aSh <= 0.0 ||
      aDw <= 0.0 || aDh <= 0.0) {
    // source and/or destination are fully clipped, so nothing is painted
    return;
  }

  SamplingFilter samplingFilter;
  AntialiasMode antialiasMode;

  if (CurrentState()->imageSmoothingEnabled) {
    samplingFilter = gfx::SamplingFilter::LINEAR;
    antialiasMode = AntialiasMode::DEFAULT;
  } else {
    samplingFilter = gfx::SamplingFilter::POINT;
    antialiasMode = AntialiasMode::NONE;
  }

  gfx::Rect bounds;

  if (NeedToCalculateBounds()) {
    bounds = gfx::Rect(aDx, aDy, aDw, aDh);
    bounds = mTarget->GetTransform().TransformBounds(bounds);
  }

  if (!IsTargetValid()) {
    aError.Throw(NS_ERROR_FAILURE);
    return;
  }

  if (srcSurf) {
    gfx::Rect sourceRect(aSx, aSy, aSw, aSh);
    if (element == GetCanvasElement()) {
      // srcSurf is a snapshot of mTarget. If we draw to mTarget now, we'll
      // trigger a COW copy of the whole canvas into srcSurf. That's a huge
      // waste if sourceRect doesn't cover the whole canvas.
      // We avoid copying the whole canvas by manually copying just the part
      // that we need.
      srcSurf = ExtractSubrect(srcSurf, &sourceRect, mTarget);
    }

    AdjustedTarget tempTarget(this, bounds.IsEmpty() ? nullptr : &bounds);
    if (!tempTarget) {
      gfxDevCrash(LogReason::InvalidDrawTarget) << "Invalid adjusted target in Canvas2D " << gfx::hexa((DrawTarget*)mTarget) << ", " << NeedToDrawShadow() << NeedToApplyFilter();
      return;
    }
    tempTarget->DrawSurface(srcSurf,
                  gfx::Rect(aDx, aDy, aDw, aDh),
                  sourceRect,
                  DrawSurfaceOptions(samplingFilter, SamplingBounds::UNBOUNDED),
                  DrawOptions(CurrentState()->globalAlpha, UsedOperation(), antialiasMode));
  } else {
    DrawDirectlyToCanvas(drawInfo, &bounds,
                         gfx::Rect(aDx, aDy, aDw, aDh),
                         gfx::Rect(aSx, aSy, aSw, aSh),
                         imgSize);
  }

  RedrawUser(gfxRect(aDx, aDy, aDw, aDh));
}

Pattern&
CanvasGeneralPattern::ForStyle(BasicRenderingContext2D* aCtx,
                               BasicRenderingContext2D::Style aStyle,
                               DrawTarget* aRT)
{
  // This should only be called once or the mPattern destructor will
  // not be executed.
  NS_ASSERTION(!mPattern.GetPattern(), "ForStyle() should only be called once on CanvasGeneralPattern!");

  const BasicRenderingContext2D::ContextState* state = aCtx->CurrentState();

  if (state->StyleIsColor(aStyle)) {
    mPattern.InitColorPattern(ToDeviceColor(state->colorStyles[aStyle]));
  } else if (state->gradientStyles[aStyle] &&
             state->gradientStyles[aStyle]->GetType() == CanvasGradient::Type::LINEAR) {
    CanvasLinearGradient *gradient =
      static_cast<CanvasLinearGradient*>(state->gradientStyles[aStyle].get());

    mPattern.InitLinearGradientPattern(gradient->mBegin, gradient->mEnd,
                                       gradient->GetGradientStopsForTarget(aRT));
  } else if (state->gradientStyles[aStyle] &&
             state->gradientStyles[aStyle]->GetType() == CanvasGradient::Type::RADIAL) {
    CanvasRadialGradient *gradient =
      static_cast<CanvasRadialGradient*>(state->gradientStyles[aStyle].get());

    mPattern.InitRadialGradientPattern(gradient->mCenter1, gradient->mCenter2,
                                       gradient->mRadius1, gradient->mRadius2,
                                       gradient->GetGradientStopsForTarget(aRT));
  } else if (state->patternStyles[aStyle]) {
    if (aCtx->GetCanvasElement()) {
      CanvasUtils::DoDrawImageSecurityCheck(aCtx->GetCanvasElement(),
                                            state->patternStyles[aStyle]->mPrincipal,
                                            state->patternStyles[aStyle]->mForceWriteOnly,
                                            state->patternStyles[aStyle]->mCORSUsed);
    }
    ExtendMode mode;
    if (state->patternStyles[aStyle]->mRepeat == CanvasPattern::RepeatMode::NOREPEAT) {
      mode = ExtendMode::CLAMP;
    } else {
      mode = ExtendMode::REPEAT;
    }

    SamplingFilter samplingFilter;
    if (state->imageSmoothingEnabled) {
      samplingFilter = SamplingFilter::GOOD;
    } else {
      samplingFilter = SamplingFilter::POINT;
    }

    mPattern.InitSurfacePattern(state->patternStyles[aStyle]->mSurface, mode,
                                state->patternStyles[aStyle]->mTransform,
                                samplingFilter);
  }

  return *mPattern.GetPattern();
}

} // namespace dom
} // namespace mozilla
