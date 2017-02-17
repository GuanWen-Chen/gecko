/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef BasicRenderingContext2D_h
#define BasicRenderingContext2D_h

#include "FilterSupport.h"
#include "gfxTextRun.h"
#include "mozilla/dom/CanvasGradient.h"
#include "mozilla/dom/CanvasPattern.h"
#include "mozilla/dom/CanvasRenderingContext2DBinding.h"
#include "mozilla/gfx/2D.h"
#include "mozilla/gfx/PatternHelpers.h"
#include "nsStyleStruct.h"
#include "nsSVGEffects.h"

using mozilla::gfx::CompositionOp;
using mozilla::gfx::DrawTarget;
using mozilla::gfx::FilterDescription;
using mozilla::gfx::Matrix;
using mozilla::gfx::Pattern;
using mozilla::gfx::GeneralPattern;

namespace mozilla {
namespace dom {

 #define NS_BASICRENDERINGCONTEXT2D_IID \
 { 0xbd7f3f74, 0x5ed8, 0x4451, \
  { 0x89, 0x95, 0x1c, 0x6a, 0x82, 0xeb, 0xef, 0x9f} }

class HTMLImageElementOrHTMLCanvasElementOrHTMLVideoElementOrImageBitmap;
typedef HTMLImageElementOrHTMLCanvasElementOrHTMLVideoElementOrImageBitmap
  CanvasImageSource;

extern const mozilla::gfx::Float SIGMA_MAX;

/*
 * BasicRenderingContext2D
 */
class BasicRenderingContext2D : public nsISupports
{
public:
  enum RenderingMode {
    SoftwareBackendMode,
    OpenGLBackendMode,
    DefaultBackendMode
  };

  // This is created lazily so it is necessary to call EnsureTarget before
  // accessing it. In the event of an error it will be equal to
  // sErrorTarget.
  RefPtr<DrawTarget> mTarget;

  // this rect is in mTarget's current user space
  virtual void RedrawUser(const gfxRect& aR) = 0;

  virtual nsresult Redraw() = 0;

  void LineTo(const mozilla::gfx::Point& aPoint)
  {
    if (mPathBuilder) {
      mPathBuilder->LineTo(aPoint);
    } else {
      mDSPathBuilder->LineTo(mTarget->GetTransform().TransformPoint(aPoint));
    }
  }

  void BezierTo(const mozilla::gfx::Point& aCP1,
                const mozilla::gfx::Point& aCP2,
                const mozilla::gfx::Point& aCP3)
  {
    if (mPathBuilder) {
      mPathBuilder->BezierTo(aCP1, aCP2, aCP3);
    } else {
      mozilla::gfx::Matrix transform = mTarget->GetTransform();
      mDSPathBuilder->BezierTo(transform.TransformPoint(aCP1),
                               transform.TransformPoint(aCP2),
                               transform.TransformPoint(aCP3));
    }
  }

  NS_DECLARE_STATIC_IID_ACCESSOR(NS_BASICRENDERINGCONTEXT2D_IID)
protected:
  virtual ~BasicRenderingContext2D() {}
public:
  explicit BasicRenderingContext2D(layers::LayersBackend aCompositorBackend)
    // these are the default values from the Canvas spec
    : mWidth(0), mHeight(0)
    , mPathTransformWillUpdate(false) {}

  //
  // CanvasState
  //
  void Save();
  void Restore();

  //
  // CanvasTransform
  //
  void Scale(double aX, double aY, mozilla::ErrorResult& aError);
  void Rotate(double aAngle, mozilla::ErrorResult& aError);
  void Translate(double aX, double aY, mozilla::ErrorResult& aError);
  void Transform(double aM11, double aM12, double aM21, double aM22,
                 double aDx, double aDy, mozilla::ErrorResult& aError);
  void SetTransform(double aM11, double aM12, double aM21, double aM22,
                    double aDx, double aDy, mozilla::ErrorResult& aError);
  void ResetTransform(mozilla::ErrorResult& aError);

  //
  // CanvasCompositing
  //
  double GlobalAlpha()
  {
    return CurrentState().globalAlpha;
  }

  // Useful for silencing cast warnings
  static mozilla::gfx::Float ToFloat(double aValue) { return mozilla::gfx::Float(aValue); }

  void SetGlobalAlpha(double aGlobalAlpha)
  {
    if (aGlobalAlpha >= 0.0 && aGlobalAlpha <= 1.0) {
      CurrentState().globalAlpha = ToFloat(aGlobalAlpha);
    }
  }
  void GetGlobalCompositeOperation(nsAString& aOp,
                                   mozilla::ErrorResult& aError);
  void SetGlobalCompositeOperation(const nsAString& aOp,
                                   mozilla::ErrorResult& aError);

  //
  // CanvasImageSmoothing
  //
  bool ImageSmoothingEnabled()
  {
    return CurrentState().imageSmoothingEnabled;
  }

  void SetImageSmoothingEnabled(bool aImageSmoothingEnabled)
  {
    if (aImageSmoothingEnabled != CurrentState().imageSmoothingEnabled) {
      CurrentState().imageSmoothingEnabled = aImageSmoothingEnabled;
    }
  }


  //
  // CanvasFillStrokeStyles
  //
  void
  GetStrokeStyle(OwningStringOrCanvasGradientOrCanvasPattern& aValue)
  {
    GetStyleAsUnion(aValue, Style::STROKE);
  }

  void
  SetStrokeStyle(const StringOrCanvasGradientOrCanvasPattern& aValue)
  {
    SetStyleFromUnion(aValue, Style::STROKE);
  }

  void
  GetFillStyle(OwningStringOrCanvasGradientOrCanvasPattern& aValue)
  {
    GetStyleAsUnion(aValue, Style::FILL);
  }

  void
  SetFillStyle(const StringOrCanvasGradientOrCanvasPattern& aValue)
  {
    SetStyleFromUnion(aValue, Style::FILL);
  }

  virtual already_AddRefed<CanvasGradient> CreateLinearGradient(double aX0,
                                                                double aY0,
                                                                double aX1,
                                                                double aY1);
  virtual already_AddRefed<CanvasGradient> CreateRadialGradient(
    double aX0,
    double aY0,
    double aR0,
    double aX1,
    double aY1,
    double aR1,
    ErrorResult& aError);
  virtual already_AddRefed<CanvasPattern> CreatePattern(
    const CanvasImageSource& aElement,
    const nsAString& aRepeat,
    ErrorResult& aError);

  //
  // CanvasShadowStyles
  //
  double ShadowOffsetX()
  {
    return CurrentState().shadowOffset.x;
  }

  void SetShadowOffsetX(double aShadowOffsetX)
  {
    CurrentState().shadowOffset.x = ToFloat(aShadowOffsetX);
  }

  double ShadowOffsetY()
  {
    return CurrentState().shadowOffset.y;
  }

  void SetShadowOffsetY(double aShadowOffsetY)
  {
    CurrentState().shadowOffset.y = ToFloat(aShadowOffsetY);
  }

  double ShadowBlur()
  {
    return CurrentState().shadowBlur;
  }

  void SetShadowBlur(double aShadowBlur)
  {
    if (aShadowBlur >= 0.0) {
      CurrentState().shadowBlur = ToFloat(aShadowBlur);
    }
  }

  void GetShadowColor(nsAString& aShadowColor)
  {
    StyleColorToString(CurrentState().shadowColor, aShadowColor);
  }

  void SetShadowColor(const nsAString& aShadowColor);

  //
  // CanvasRect
  //
  void ClearRect(double aX, double aY, double aW, double aH);
  void FillRect(double aX, double aY, double aW, double aH);
  void StrokeRect(double aX, double aY, double aW, double aH);

  //
  // CanvasDrawPath
  //
  void BeginPath();
  void Fill(const CanvasWindingRule& aWinding);
  void Fill(const CanvasPath& aPath, const CanvasWindingRule& aWinding);
  void Stroke();
  void Stroke(const CanvasPath& aPath);
  void Clip(const CanvasWindingRule& aWinding);
  void Clip(const CanvasPath& aPath, const CanvasWindingRule& aWinding);
  bool IsPointInPath(double aX, double aY, const CanvasWindingRule& aWinding);
  bool IsPointInPath(const CanvasPath& aPath, double aX, double aY, const CanvasWindingRule& aWinding);
  bool IsPointInStroke(double aX, double aY);
  bool IsPointInStroke(const CanvasPath& aPath, double aX, double aY);

  //
  // CanvasDrawImage
  //
  virtual void DrawImage(const CanvasImageSource& aImage,
                         double aDx,
                         double aDy,
                         mozilla::ErrorResult& aError) = 0;
  virtual void DrawImage(const CanvasImageSource& aImage,
                         double aDx,
                         double aDy,
                         double aDw,
                         double aDh,
                         mozilla::ErrorResult& aError) = 0;
  virtual void DrawImage(const CanvasImageSource& aImage,
                         double aSx,
                         double aSy,
                         double aSw,
                         double aSh,
                         double aDx,
                         double aDy,
                         double aDw,
                         double aDh,
                         mozilla::ErrorResult& aError) = 0;

  //
  // CanvasPathDrawingStyles
  //
  double LineWidth()
  {
    return CurrentState().lineWidth;
  }

  void SetLineWidth(double aWidth)
  {
    if (aWidth > 0.0) {
      CurrentState().lineWidth = ToFloat(aWidth);
    }
  }
  void GetLineCap(nsAString& aLinecapStyle);
  void SetLineCap(const nsAString& aLinecapStyle);
  void GetLineJoin(nsAString& aLinejoinStyle,
                   mozilla::ErrorResult& aError);
  void SetLineJoin(const nsAString& aLinejoinStyle);

  double MiterLimit()
  {
    return CurrentState().miterLimit;
  }

  void SetMiterLimit(double aMiter)
  {
    if (aMiter > 0.0) {
      CurrentState().miterLimit = ToFloat(aMiter);
    }
  }
  void SetLineDash(const Sequence<double>& aSegments,
                   mozilla::ErrorResult& aRv);
  void GetLineDash(nsTArray<double>& aSegments) const;

  void SetLineDashOffset(double aOffset);
  double LineDashOffset() const;

  //
  // CanvasPath
  //
  void ClosePath()
  {
    EnsureWritablePath();

    if (mPathBuilder) {
      mPathBuilder->Close();
    } else {
      mDSPathBuilder->Close();
    }
  }

  void MoveTo(double aX, double aY)
  {
    EnsureWritablePath();

    if (mPathBuilder) {
      mPathBuilder->MoveTo(mozilla::gfx::Point(ToFloat(aX), ToFloat(aY)));
    } else {
      mDSPathBuilder->MoveTo(mTarget->GetTransform().TransformPoint(
                             mozilla::gfx::Point(ToFloat(aX), ToFloat(aY))));
    }
  }

  void LineTo(double aX, double aY)
  {
    EnsureWritablePath();

    LineTo(mozilla::gfx::Point(ToFloat(aX), ToFloat(aY)));
  }

  void QuadraticCurveTo(double aCpx, double aCpy, double aX, double aY)
  {
    EnsureWritablePath();

    if (mPathBuilder) {
      mPathBuilder->QuadraticBezierTo(mozilla::gfx::Point(ToFloat(aCpx), ToFloat(aCpy)),
                                      mozilla::gfx::Point(ToFloat(aX), ToFloat(aY)));
    } else {
      mozilla::gfx::Matrix transform = mTarget->GetTransform();
      mDSPathBuilder->QuadraticBezierTo(transform.TransformPoint(
                                          mozilla::gfx::Point(ToFloat(aCpx), ToFloat(aCpy))),
                                        transform.TransformPoint(
                                          mozilla::gfx::Point(ToFloat(aX), ToFloat(aY))));
    }
  }

  void BezierCurveTo(double aCp1x, double aCp1y, double aCp2x, double aCp2y,
                     double aX, double aY)
  {
    EnsureWritablePath();

    BezierTo(mozilla::gfx::Point(ToFloat(aCp1x), ToFloat(aCp1y)),
             mozilla::gfx::Point(ToFloat(aCp2x), ToFloat(aCp2y)),
             mozilla::gfx::Point(ToFloat(aX), ToFloat(aY)));
  }

  void ArcTo(double aX1, double aY1, double aX2, double aY2,
             double aRadius, mozilla::ErrorResult& aError);
  void Rect(double aX, double aY, double aW, double aH);
  void Arc(double aX, double aY, double aRadius, double aStartAngle,
           double aEndAngle, bool aAnticlockwise,
           mozilla::ErrorResult& aError);
  void Ellipse(double aX, double aY, double aRadiusX, double aRadiusY,
               double aRotation, double aStartAngle, double aEndAngle,
               bool aAnticlockwise, ErrorResult& aError);

protected:
  friend class CanvasGeneralPattern;
  friend class AdjustedTarget;
  friend class AdjustedTargetForShadow;
  friend class AdjustedTargetForFilter;

  enum class Style : uint8_t {
    STROKE = 0,
    FILL,
    MAX
  };

  enum class TextAlign : uint8_t {
    START,
    END,
    LEFT,
    RIGHT,
    CENTER
  };

  enum class TextBaseline : uint8_t {
    TOP,
    HANGING,
    MIDDLE,
    ALPHABETIC,
    IDEOGRAPHIC,
    BOTTOM
  };

  // A clip or a transform, recorded and restored in order.
  struct ClipState {
    explicit ClipState(mozilla::gfx::Path* aClip)
      : clip(aClip)
    {}

    explicit ClipState(const Matrix& aTransform)
      : transform(aTransform)
    {}

    bool IsClip() const { return !!clip; }

    RefPtr<mozilla::gfx::Path> clip;
    Matrix transform;
  };

  // state stack handling
  class ContextState {
  public:
  ContextState() : textAlign(TextAlign::START),
                   textBaseline(TextBaseline::ALPHABETIC),
                   shadowColor(0),
                   lineWidth(1.0f),
                   miterLimit(10.0f),
                   globalAlpha(1.0f),
                   shadowBlur(0.0),
                   dashOffset(0.0f),
                   op(CompositionOp::OP_OVER),
                   fillRule(mozilla::gfx::FillRule::FILL_WINDING),
                   lineCap(mozilla::gfx::CapStyle::BUTT),
                   lineJoin(mozilla::gfx::JoinStyle::MITER_OR_BEVEL),
                   filterString(u"none"),
                   filterSourceGraphicTainted(false),
                   imageSmoothingEnabled(true),
                   fontExplicitLanguage(false)
  { }

  ContextState(const ContextState& aOther)
      : fontGroup(aOther.fontGroup),
        fontLanguage(aOther.fontLanguage),
        fontFont(aOther.fontFont),
        gradientStyles(aOther.gradientStyles),
        patternStyles(aOther.patternStyles),
        colorStyles(aOther.colorStyles),
        font(aOther.font),
        textAlign(aOther.textAlign),
        textBaseline(aOther.textBaseline),
        shadowColor(aOther.shadowColor),
        transform(aOther.transform),
        shadowOffset(aOther.shadowOffset),
        lineWidth(aOther.lineWidth),
        miterLimit(aOther.miterLimit),
        globalAlpha(aOther.globalAlpha),
        shadowBlur(aOther.shadowBlur),
        dash(aOther.dash),
        dashOffset(aOther.dashOffset),
        op(aOther.op),
        fillRule(aOther.fillRule),
        lineCap(aOther.lineCap),
        lineJoin(aOther.lineJoin),
        filterString(aOther.filterString),
        filterChain(aOther.filterChain),
        filterChainObserver(aOther.filterChainObserver),
        filter(aOther.filter),
        filterAdditionalImages(aOther.filterAdditionalImages),
        filterSourceGraphicTainted(aOther.filterSourceGraphicTainted),
        imageSmoothingEnabled(aOther.imageSmoothingEnabled),
        fontExplicitLanguage(aOther.fontExplicitLanguage)
  { }

  void SetColorStyle(Style aWhichStyle, nscolor aColor)
  {
    colorStyles[aWhichStyle] = aColor;
    gradientStyles[aWhichStyle] = nullptr;
    patternStyles[aWhichStyle] = nullptr;
  }

  void SetPatternStyle(Style aWhichStyle, CanvasPattern* aPat)
  {
    gradientStyles[aWhichStyle] = nullptr;
    patternStyles[aWhichStyle] = aPat;
  }

  void SetGradientStyle(Style aWhichStyle, CanvasGradient* aGrad)
  {
    gradientStyles[aWhichStyle] = aGrad;
    patternStyles[aWhichStyle] = nullptr;
  }

  /**
    * returns true iff the given style is a solid color.
    */
  bool StyleIsColor(Style aWhichStyle) const
  {
    return !(patternStyles[aWhichStyle] || gradientStyles[aWhichStyle]);
  }

  int32_t ShadowBlurRadius() const
  {
    static const gfxFloat GAUSSIAN_SCALE_FACTOR = (3 * sqrt(2 * M_PI) / 4) * 1.5;
    return (int32_t)floor(ShadowBlurSigma() * GAUSSIAN_SCALE_FACTOR + 0.5);
  }

  mozilla::gfx::Float ShadowBlurSigma() const
  {
    return std::min(SIGMA_MAX, shadowBlur / 2.0f);
  }

  nsTArray<ClipState> clipsAndTransforms;

  RefPtr<gfxFontGroup> fontGroup;
  nsCOMPtr<nsIAtom> fontLanguage;
  nsFont fontFont;

  EnumeratedArray<Style, Style::MAX, RefPtr<CanvasGradient>> gradientStyles;
  EnumeratedArray<Style, Style::MAX, RefPtr<CanvasPattern>> patternStyles;
  EnumeratedArray<Style, Style::MAX, nscolor> colorStyles;

  nsString font;
  TextAlign textAlign;
  TextBaseline textBaseline;

  nscolor shadowColor;

  Matrix transform;
  mozilla::gfx::Point shadowOffset;
  mozilla::gfx::Float lineWidth;
  mozilla::gfx::Float miterLimit;
  mozilla::gfx::Float globalAlpha;
  mozilla::gfx::Float shadowBlur;
  nsTArray<mozilla::gfx::Float> dash;
  mozilla::gfx::Float dashOffset;

  CompositionOp op;
  mozilla::gfx::FillRule fillRule;
  mozilla::gfx::CapStyle lineCap;
  mozilla::gfx::JoinStyle lineJoin;

  nsString filterString;
  nsTArray<nsStyleFilter> filterChain;
  RefPtr<nsSVGFilterChainObserver> filterChainObserver;
  mozilla::gfx::FilterDescription filter;
  nsTArray<RefPtr<mozilla::gfx::SourceSurface>> filterAdditionalImages;

  // This keeps track of whether the canvas was "tainted" or not when
  // we last used a filter. This is a security measure, whereby the
  // canvas is flipped to write-only if a cross-origin image is drawn to it.
  // This is to stop bad actors from reading back data they shouldn't have
  // access to.
  //
  // This also limits what filters we can apply to the context; in particular
  // feDisplacementMap is restricted.
  //
  // We keep track of this to ensure that if this gets out of sync with the
  // tainted state of the canvas itself, we update our filters accordingly.
  bool filterSourceGraphicTainted;

  bool imageSmoothingEnabled;
  bool fontExplicitLanguage;
  };

  // Member vars
  AutoTArray<ContextState, 3> mStyleStack;

  int32_t mWidth, mHeight;

  /**
    * We also have a device space pathbuilder. The reason for this is as
    * follows, when a path is being built, but the transform changes, we
    * can no longer keep a single path in userspace, considering there's
    * several 'user spaces' now. We therefore transform the current path
    * into device space, and add all operations to this path in device
    * space.
    *
    * When then finally executing a render, the Azure drawing API expects
    * the path to be in userspace. We could then set an identity transform
    * on the DrawTarget and do all drawing in device space. This is
    * undesirable because it requires transforming patterns, gradients,
    * clips, etc. into device space and it would not work for stroking.
    * What we do instead is convert the path back to user space when it is
    * drawn, and draw it with the current transform. This makes all drawing
    * occur correctly.
    *
    * There's never both a device space path builder and a user space path
    * builder present at the same time. There is also never a path and a
    * path builder present at the same time. When writing proceeds on an
    * existing path the Path is cleared and a new builder is created.
    *
    * mPath is always in user-space.
    */
  RefPtr<mozilla::gfx::Path> mPath;
  RefPtr<mozilla::gfx::PathBuilder> mPathBuilder;
  bool mPathTransformWillUpdate;
  Matrix mPathToDS;

  /**
    * We also have a device space pathbuilder. The reason for this is as
    * follows, when a path is being built, but the transform changes, we
    * can no longer keep a single path in userspace, considering there's
    * several 'user spaces' now. We therefore transform the current path
    * into device space, and add all operations to this path in device
    * space.
    *
    * When then finally executing a render, the Azure drawing API expects
    * the path to be in userspace. We could then set an identity transform
    * on the DrawTarget and do all drawing in device space. This is
    * undesirable because it requires transforming patterns, gradients,
    * clips, etc. into device space and it would not work for stroking.
    * What we do instead is convert the path back to user space when it is
    * drawn, and draw it with the current transform. This makes all drawing
    * occur correctly.
    *
    * There's never both a device space path builder and a user space path
    * builder present at the same time. There is also never a path and a
    * path builder present at the same time. When writing proceeds on an
    * existing path the Path is cleared and a new builder is created.
    *
    * mPath is always in user-space.
    */
  RefPtr<mozilla::gfx::PathBuilder> mDSPathBuilder;

protected:
  virtual HTMLCanvasElement* GetCanvasElement() = 0;

  virtual bool AlreadyShutDown() const = 0;

  /**
   * Create the backing surfacing, if it doesn't exist. If there is an error
   * in creating the target then it will put sErrorTarget in place. If there
   * is in turn an error in creating the sErrorTarget then they would both
   * be null so IsTargetValid() would still return null.
   *
   * Returns the actual rendering mode being used by the created target.
   */
  virtual RenderingMode
  EnsureTarget(const gfx::Rect* aCoveredRect = nullptr,
               RenderingMode aRenderMode = RenderingMode::DefaultBackendMode) = 0;

  /**
   * Check if the target is valid after calling EnsureTarget.
   */
  virtual bool IsTargetValid() const = 0;

  inline ContextState& CurrentState() {
    return mStyleStack[mStyleStack.Length() - 1];
  }

  inline const ContextState& CurrentState() const {
    return mStyleStack[mStyleStack.Length() - 1];
  }

  /**
   * Needs to be called before updating the transform. This makes a call to
   * EnsureTarget() so you don't have to.
   */
  void TransformWillUpdate();

  void SetTransformInternal(const Matrix& aTransform);

  // Some helpers.  Doesn't modify a color on failure.
  void SetStyleFromUnion(const StringOrCanvasGradientOrCanvasPattern& aValue,
                         Style aWhichStyle);
  void SetStyleFromString(const nsAString& aStr, Style aWhichStyle);

  void SetStyleFromGradient(CanvasGradient& aGradient, Style aWhichStyle)
  {
    CurrentState().SetGradientStyle(aWhichStyle, &aGradient);
  }

  void SetStyleFromPattern(CanvasPattern& aPattern, Style aWhichStyle)
  {
    CurrentState().SetPatternStyle(aWhichStyle, &aPattern);
  }

  // Returns whether a color was successfully parsed.
  virtual bool ParseColor(const nsAString& aString, nscolor* aColor) = 0;

  static void StyleColorToString(const nscolor& aColor, nsAString& aStr);

  void GetStyleAsUnion(OwningStringOrCanvasGradientOrCanvasPattern& aValue,
                       Style aWhichStyle);


  /**
   * Returns true if we know for sure that the pattern for a given style is opaque.
   * Usefull to know if we can discard the content below in certain situations.
   */
  bool PatternIsOpaque(Style aStyle) const;

  mozilla::gfx::CompositionOp UsedOperation()
  {
    if (NeedToDrawShadow() || NeedToApplyFilter()) {
      // In this case the shadow or filter rendering will use the operator.
      return mozilla::gfx::CompositionOp::OP_OVER;
    }

    return CurrentState().op;
  }

  /**
    * Returns true if a shadow should be drawn along with a
    * drawing operation.
    */
  bool NeedToDrawShadow()
  {
    const ContextState& state = CurrentState();

    // The spec says we should not draw shadows if the operator is OVER.
    // If it's over and the alpha value is zero, nothing needs to be drawn.
    return NS_GET_A(state.shadowColor) != 0 &&
      (state.shadowBlur != 0.f || state.shadowOffset.x != 0.f || state.shadowOffset.y != 0.f);
  }

  /**
    * Returns true if the result of a drawing operation should be
    * drawn with a filter.
    */
  virtual bool NeedToApplyFilter() = 0;

  bool NeedToCalculateBounds()
  {
    return NeedToDrawShadow() || NeedToApplyFilter();
  }

  // Ensures a path in UserSpace is available.
  void EnsureUserSpacePath(const CanvasWindingRule& aWinding = CanvasWindingRule::Nonzero);

  /* This function ensures there is a writable pathbuilder available, this
   * pathbuilder may be working in user space or in device space or
   * device space.
   * After calling this function mPathTransformWillUpdate will be false
   */
  void EnsureWritablePath();
};

 NS_DEFINE_STATIC_IID_ACCESSOR(BasicRenderingContext2D,
                               NS_BASICRENDERINGCONTEXT2D_IID)

// This class is named 'GeneralCanvasPattern' instead of just
// 'GeneralPattern' to keep Windows PGO builds from confusing the
// GeneralPattern class in gfxContext.cpp with this one.
class CanvasGeneralPattern
{
public:
  Pattern& ForStyle(BasicRenderingContext2D* aCtx,
                    BasicRenderingContext2D::Style aStyle, DrawTarget* aRT);

  GeneralPattern mPattern;
};

} // namespace dom
} // namespace mozilla

#endif /* BasicRenderingContext2D_h */
