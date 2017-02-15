/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "AdjustedTarget.h"
#include "CanvasUtils.h"
#include "gfxUtils.h"
#include "mozilla/dom/BasicRenderingContext2D.h"
#include "mozilla/dom/HTMLCanvasElement.h"
#include "mozilla/dom/HTMLImageElement.h"
#include "mozilla/dom/HTMLVideoElement.h"
#include "mozilla/dom/ImageBitmap.h"
#include "nsContentUtils.h"
#include "nsICanvasRenderingContextInternal.h"
#include "nsPrintfCString.h"
#include "nsStyleUtil.h"

using mozilla::gfx::AntialiasMode;
using mozilla::gfx::CapStyle;
using mozilla::gfx::Color;
using mozilla::gfx::DrawOptions;
using mozilla::gfx::ExtendMode;
using mozilla::gfx::JoinStyle;
using mozilla::gfx::IntSize;
using mozilla::gfx::SamplingFilter;
using mozilla::gfx::SourceSurface;
using mozilla::gfx::StrokeOptions;
using mozilla::gfx::ToDeviceColor;

namespace mozilla {
namespace dom{

NS_IMPL_CYCLE_COLLECTION_ROOT_NATIVE(CanvasGradient, AddRef)
NS_IMPL_CYCLE_COLLECTION_UNROOT_NATIVE(CanvasGradient, Release)

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(CanvasGradient, mContext)

NS_IMPL_CYCLE_COLLECTION_ROOT_NATIVE(CanvasPattern, AddRef)
NS_IMPL_CYCLE_COLLECTION_UNROOT_NATIVE(CanvasPattern, Release)

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(CanvasPattern, mContext)


// Cap sigma to avoid overly large temp surfaces.
const mozilla::gfx::Float SIGMA_MAX = 100;

const size_t MAX_STYLE_STACK_SIZE = 1024;

/**
 ** BasicRenderingContext2D impl
 **/
bool
BasicRenderingContext2D::PatternIsOpaque(BasicRenderingContext2D::Style aStyle) const
{
  const ContextState& state = CurrentState();
  if (state.globalAlpha < 1.0) {
    return false;
  }

  if (state.patternStyles[aStyle] && state.patternStyles[aStyle]->mSurface) {
    return IsOpaqueFormat(state.patternStyles[aStyle]->mSurface->GetFormat());
  }

  // TODO: for gradient patterns we could check that all stops are opaque
  // colors.

  if (!state.gradientStyles[aStyle]) {
    // it's a color pattern.
    return Color::FromABGR(state.colorStyles[aStyle]).a >= 1.0;
  }

  return false;
}


void
BasicRenderingContext2D::GetStyleAsUnion(OwningStringOrCanvasGradientOrCanvasPattern& aValue,
                                         Style aWhichStyle)
{
  const ContextState &state = CurrentState();
  if (state.patternStyles[aWhichStyle]) {
    aValue.SetAsCanvasPattern() = state.patternStyles[aWhichStyle];
  } else if (state.gradientStyles[aWhichStyle]) {
    aValue.SetAsCanvasGradient() = state.gradientStyles[aWhichStyle];
  } else {
    StyleColorToString(state.colorStyles[aWhichStyle], aValue.SetAsString());
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

  CurrentState().SetColorStyle(aWhichStyle, color);
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
  mStyleStack[mStyleStack.Length() - 1].transform = mTarget->GetTransform();
  mStyleStack.SetCapacity(mStyleStack.Length() + 1);
  mStyleStack.AppendElement(CurrentState());

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

  for (const auto& clipOrTransform : CurrentState().clipsAndTransforms) {
    if (clipOrTransform.IsClip()) {
      mTarget->PopClip();
    }
  }

  mStyleStack.RemoveElementAt(mStyleStack.Length() - 1);

  mTarget->SetTransform(CurrentState().transform);
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
  auto& clipsAndTransforms = CurrentState().clipsAndTransforms;
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
  CurrentState().op = comp_op;
}

void
BasicRenderingContext2D::GetGlobalCompositeOperation(nsAString& aOp,
                                                     ErrorResult& aError)
{
  CompositionOp comp_op = CurrentState().op;

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

  CurrentState().shadowColor = color;
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
  const ContextState &state = CurrentState();

  if (!ValidateRect(aX, aY, aW, aH, true)) {
    return;
  }

  if (state.patternStyles[Style::FILL]) {
    CanvasPattern::RepeatMode repeat =
      state.patternStyles[Style::FILL]->mRepeat;
    // In the FillRect case repeat modes are easy to deal with.
    bool limitx = repeat == CanvasPattern::RepeatMode::NOREPEAT || repeat == CanvasPattern::RepeatMode::REPEATY;
    bool limity = repeat == CanvasPattern::RepeatMode::NOREPEAT || repeat == CanvasPattern::RepeatMode::REPEATX;

    IntSize patternSize =
      state.patternStyles[Style::FILL]->mSurface->GetSize();

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

  AntialiasMode antialiasMode = CurrentState().imageSmoothingEnabled ?
                                AntialiasMode::DEFAULT : AntialiasMode::NONE;

  AdjustedTarget(this, bounds.IsEmpty() ? nullptr : &bounds)->
    FillRect(gfx::Rect(aX, aY, aW, aH),
             CanvasGeneralPattern().ForStyle(this, Style::FILL, mTarget),
             DrawOptions(state.globalAlpha, op, antialiasMode));

  RedrawUser(gfxRect(aX, aY, aW, aH));
}

void
BasicRenderingContext2D::StrokeRect(double aX, double aY, double aW,
                                    double aH)
{
  const ContextState &state = CurrentState();

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
    bounds = gfx::Rect(aX - state.lineWidth / 2.0f, aY - state.lineWidth / 2.0f,
                       aW + state.lineWidth, aH + state.lineWidth);
    bounds = mTarget->GetTransform().TransformBounds(bounds);
  }

  if (!aH) {
    CapStyle cap = CapStyle::BUTT;
    if (state.lineJoin == JoinStyle::ROUND) {
      cap = CapStyle::ROUND;
    }
    AdjustedTarget(this, bounds.IsEmpty() ? nullptr : &bounds)->
      StrokeLine(Point(aX, aY), Point(aX + aW, aY),
                  CanvasGeneralPattern().ForStyle(this, Style::STROKE, mTarget),
                  StrokeOptions(state.lineWidth, state.lineJoin,
                                cap, state.miterLimit,
                                state.dash.Length(),
                                state.dash.Elements(),
                                state.dashOffset),
                  DrawOptions(state.globalAlpha, UsedOperation()));
    return;
  }

  if (!aW) {
    CapStyle cap = CapStyle::BUTT;
    if (state.lineJoin == JoinStyle::ROUND) {
      cap = CapStyle::ROUND;
    }
    AdjustedTarget(this, bounds.IsEmpty() ? nullptr : &bounds)->
      StrokeLine(Point(aX, aY), Point(aX, aY + aH),
                  CanvasGeneralPattern().ForStyle(this, Style::STROKE, mTarget),
                  StrokeOptions(state.lineWidth, state.lineJoin,
                                cap, state.miterLimit,
                                state.dash.Length(),
                                state.dash.Elements(),
                                state.dashOffset),
                  DrawOptions(state.globalAlpha, UsedOperation()));
    return;
  }

  AdjustedTarget(this, bounds.IsEmpty() ? nullptr : &bounds)->
    StrokeRect(gfx::Rect(aX, aY, aW, aH),
               CanvasGeneralPattern().ForStyle(this, Style::STROKE, mTarget),
               StrokeOptions(state.lineWidth, state.lineJoin,
                             state.lineCap, state.miterLimit,
                             state.dash.Length(),
                             state.dash.Elements(),
                             state.dashOffset),
               DrawOptions(state.globalAlpha, UsedOperation()));

  Redraw();
}

//
// path bits
//

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

Pattern&
CanvasGeneralPattern::ForStyle(BasicRenderingContext2D* aCtx,
                               BasicRenderingContext2D::Style aStyle,
                               DrawTarget* aRT)
{
  // This should only be called once or the mPattern destructor will
  // not be executed.
  NS_ASSERTION(!mPattern.GetPattern(), "ForStyle() should only be called once on CanvasGeneralPattern!");

  const BasicRenderingContext2D::ContextState &state = aCtx->CurrentState();

  if (state.StyleIsColor(aStyle)) {
    mPattern.InitColorPattern(ToDeviceColor(state.colorStyles[aStyle]));
  } else if (state.gradientStyles[aStyle] &&
             state.gradientStyles[aStyle]->GetType() == CanvasGradient::Type::LINEAR) {
    CanvasLinearGradient *gradient =
      static_cast<CanvasLinearGradient*>(state.gradientStyles[aStyle].get());

    mPattern.InitLinearGradientPattern(gradient->mBegin, gradient->mEnd,
                                       gradient->GetGradientStopsForTarget(aRT));
  } else if (state.gradientStyles[aStyle] &&
             state.gradientStyles[aStyle]->GetType() == CanvasGradient::Type::RADIAL) {
    CanvasRadialGradient *gradient =
      static_cast<CanvasRadialGradient*>(state.gradientStyles[aStyle].get());

    mPattern.InitRadialGradientPattern(gradient->mCenter1, gradient->mCenter2,
                                       gradient->mRadius1, gradient->mRadius2,
                                       gradient->GetGradientStopsForTarget(aRT));
  } else if (state.patternStyles[aStyle]) {
    if (aCtx->GetCanvasElement()) {
      CanvasUtils::DoDrawImageSecurityCheck(aCtx->GetCanvasElement(),
                                            state.patternStyles[aStyle]->mPrincipal,
                                            state.patternStyles[aStyle]->mForceWriteOnly,
                                            state.patternStyles[aStyle]->mCORSUsed);
    }

    ExtendMode mode;
    if (state.patternStyles[aStyle]->mRepeat == CanvasPattern::RepeatMode::NOREPEAT) {
      mode = ExtendMode::CLAMP;
    } else {
      mode = ExtendMode::REPEAT;
    }

    SamplingFilter samplingFilter;
    if (state.imageSmoothingEnabled) {
      samplingFilter = SamplingFilter::GOOD;
    } else {
      samplingFilter = SamplingFilter::POINT;
    }

    mPattern.InitSurfacePattern(state.patternStyles[aStyle]->mSurface, mode,
                                state.patternStyles[aStyle]->mTransform,
                                samplingFilter);
  }

  return *mPattern.GetPattern();
}

} // namespace dom
} // namespace mozilla
