/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef CanvasRenderingContext2D_h
#define CanvasRenderingContext2D_h

#include "mozilla/Attributes.h"
#include <vector>
#include "nsIDOMCanvasRenderingContext2D.h"
#include "nsICanvasRenderingContextInternal.h"
#include "mozilla/RefPtr.h"
#include "nsColor.h"
#include "mozilla/dom/HTMLCanvasElement.h"
#include "mozilla/dom/HTMLVideoElement.h"
#include "gfxTextRun.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/dom/BasicRenderingContext2D.h"
#include "mozilla/dom/CanvasRenderingContext2DBinding.h"
#include "mozilla/dom/CanvasPattern.h"
#include "mozilla/gfx/Rect.h"
#include "mozilla/UniquePtr.h"
#include "gfx2DGlue.h"
#include "imgIEncoder.h"
#include "nsLayoutUtils.h"
#include "mozilla/EnumeratedArray.h"
#include "Layers.h"

class nsGlobalWindow;
class nsXULElement;

namespace mozilla {
namespace gl {
class SourceSurface;
} // namespace gl

namespace dom {
class ImageData;
class StringOrCanvasGradientOrCanvasPattern;
class OwningStringOrCanvasGradientOrCanvasPattern;
class TextMetrics;
class CanvasFilterChainObserver;
class CanvasPath;

template<typename T> class Optional;

struct CanvasBidiProcessor;
class CanvasRenderingContext2DUserData;
class CanvasDrawObserver;
class CanvasShutdownObserver;

/**
 ** CanvasRenderingContext2D
 **/
class CanvasRenderingContext2D final :
  public nsICanvasRenderingContextInternal,
  public BasicRenderingContext2D,
  public nsWrapperCache
{
  virtual ~CanvasRenderingContext2D();

public:
  explicit CanvasRenderingContext2D(layers::LayersBackend aCompositorBackend);

  virtual JSObject* WrapObject(JSContext *aCx, JS::Handle<JSObject*> aGivenProto) override;

  HTMLCanvasElement* GetCanvas() const
  {
    if (!mCanvasElement || mCanvasElement->IsInNativeAnonymousSubtree()) {
      return nullptr;
    }

    // corresponds to changes to the old bindings made in bug 745025
    return mCanvasElement->GetOriginalCanvas();
  }

  void GetFilter(nsAString& aFilter)
  {
    aFilter = CurrentState().filterString;
  }

  void SetFilter(const nsAString& aFilter, mozilla::ErrorResult& aError);
  void ClearRect(double aX, double aY, double aW, double aH) override;
  void FillRect(double aX, double aY, double aW, double aH) override;
  void StrokeRect(double aX, double aY, double aW, double aH) override;
  void BeginPath();
  void Fill(const CanvasWindingRule& aWinding);
  void Fill(const CanvasPath& aPath, const CanvasWindingRule& aWinding);
  void Stroke();
  void Stroke(const CanvasPath& aPath);
  void DrawFocusIfNeeded(mozilla::dom::Element& aElement, ErrorResult& aRv);
  bool DrawCustomFocusRing(mozilla::dom::Element& aElement);
  void Clip(const CanvasWindingRule& aWinding);
  void Clip(const CanvasPath& aPath, const CanvasWindingRule& aWinding);
  bool IsPointInPath(double aX, double aY, const CanvasWindingRule& aWinding);
  bool IsPointInPath(const CanvasPath& aPath, double aX, double aY, const CanvasWindingRule& aWinding);
  bool IsPointInStroke(double aX, double aY);
  bool IsPointInStroke(const CanvasPath& aPath, double aX, double aY);
  void FillText(const nsAString& aText, double aX, double aY,
                const Optional<double>& aMaxWidth,
                mozilla::ErrorResult& aError);
  void StrokeText(const nsAString& aText, double aX, double aY,
                  const Optional<double>& aMaxWidth,
                  mozilla::ErrorResult& aError);
  TextMetrics*
    MeasureText(const nsAString& aRawText, mozilla::ErrorResult& aError);

  void AddHitRegion(const HitRegionOptions& aOptions, mozilla::ErrorResult& aError);
  void RemoveHitRegion(const nsAString& aId);
  void ClearHitRegions();

  void DrawImage(const CanvasImageSource& aImage, double aDx, double aDy,
                 mozilla::ErrorResult& aError) override
  {
    DrawImage(aImage, 0.0, 0.0, 0.0, 0.0, aDx, aDy, 0.0, 0.0, 0, aError);
  }

  void DrawImage(const CanvasImageSource& aImage, double aDx, double aDy,
                 double aDw, double aDh, mozilla::ErrorResult& aError) override
  {
    DrawImage(aImage, 0.0, 0.0, 0.0, 0.0, aDx, aDy, aDw, aDh, 2, aError);
  }

  void DrawImage(const CanvasImageSource& aImage,
                 double aSx, double aSy, double aSw, double aSh,
                 double aDx, double aDy, double aDw, double aDh,
                 mozilla::ErrorResult& aError) override
  {
    DrawImage(aImage, aSx, aSy, aSw, aSh, aDx, aDy, aDw, aDh, 6, aError);
  }

  already_AddRefed<ImageData>
    CreateImageData(JSContext* aCx, double aSw, double aSh,
                    mozilla::ErrorResult& aError);
  already_AddRefed<ImageData>
    CreateImageData(JSContext* aCx, ImageData& aImagedata,
                    mozilla::ErrorResult& aError);
  already_AddRefed<ImageData>
    GetImageData(JSContext* aCx, double aSx, double aSy, double aSw, double aSh,
                 mozilla::ErrorResult& aError);
  void PutImageData(ImageData& aImageData,
                    double aDx, double aDy, mozilla::ErrorResult& aError);
  void PutImageData(ImageData& aImageData,
                    double aDx, double aDy, double aDirtyX, double aDirtyY,
                    double aDirtyWidth, double aDirtyHeight,
                    mozilla::ErrorResult& aError);

  double LineWidth() override
  {
    return CurrentState().lineWidth;
  }

  void SetLineWidth(double aWidth) override
  {
    if (aWidth > 0.0) {
      CurrentState().lineWidth = ToFloat(aWidth);
    }
  }
  void GetLineCap(nsAString& aLinecapStyle) override;
  void SetLineCap(const nsAString& aLinecapStyle) override;
  void GetLineJoin(nsAString& aLinejoinStyle,
                   mozilla::ErrorResult& aError) override;
  void SetLineJoin(const nsAString& aLinejoinStyle) override;

  double MiterLimit() override
  {
    return CurrentState().miterLimit;
  }

  void SetMiterLimit(double aMiter) override
  {
    if (aMiter > 0.0) {
      CurrentState().miterLimit = ToFloat(aMiter);
    }
  }

  void GetFont(nsAString& aFont)
  {
    aFont = GetFont();
  }

  void SetFont(const nsAString& aFont, mozilla::ErrorResult& aError);
  void GetTextAlign(nsAString& aTextAlign);
  void SetTextAlign(const nsAString& aTextAlign);
  void GetTextBaseline(nsAString& aTextBaseline);
  void SetTextBaseline(const nsAString& aTextBaseline);

  void ClosePath() override
  {
    EnsureWritablePath();

    if (mPathBuilder) {
      mPathBuilder->Close();
    } else {
      mDSPathBuilder->Close();
    }
  }

  void MoveTo(double aX, double aY) override
  {
    EnsureWritablePath();

    if (mPathBuilder) {
      mPathBuilder->MoveTo(mozilla::gfx::Point(ToFloat(aX), ToFloat(aY)));
    } else {
      mDSPathBuilder->MoveTo(mTarget->GetTransform().TransformPoint(
                             mozilla::gfx::Point(ToFloat(aX), ToFloat(aY))));
    }
  }

  void LineTo(double aX, double aY) override
  {
    EnsureWritablePath();

    LineTo(mozilla::gfx::Point(ToFloat(aX), ToFloat(aY)));
  }

  void QuadraticCurveTo(double aCpx, double aCpy, double aX, double aY) override
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
                     double aX, double aY) override
  {
    EnsureWritablePath();

    BezierTo(mozilla::gfx::Point(ToFloat(aCp1x), ToFloat(aCp1y)),
             mozilla::gfx::Point(ToFloat(aCp2x), ToFloat(aCp2y)),
             mozilla::gfx::Point(ToFloat(aX), ToFloat(aY)));
  }

  void ArcTo(double aX1, double aY1, double aX2, double aY2,
             double aRadius, mozilla::ErrorResult& aError) override;
  void Rect(double aX, double aY, double aW, double aH) override;
  void Arc(double aX, double aY, double aRadius, double aStartAngle,
           double aEndAngle, bool aAnticlockwise,
           mozilla::ErrorResult& aError) override;
  void Ellipse(double aX, double aY, double aRadiusX, double aRadiusY,
               double aRotation, double aStartAngle, double aEndAngle,
               bool aAnticlockwise, ErrorResult& aError) override;

  void GetMozCurrentTransform(JSContext* aCx,
                              JS::MutableHandle<JSObject*> aResult,
                              mozilla::ErrorResult& aError);
  void SetMozCurrentTransform(JSContext* aCx,
                              JS::Handle<JSObject*> aCurrentTransform,
                              mozilla::ErrorResult& aError);
  void GetMozCurrentTransformInverse(JSContext* aCx,
                                     JS::MutableHandle<JSObject*> aResult,
                                     mozilla::ErrorResult& aError);
  void SetMozCurrentTransformInverse(JSContext* aCx,
                                     JS::Handle<JSObject*> aCurrentTransform,
                                     mozilla::ErrorResult& aError);
  void GetFillRule(nsAString& aFillRule);
  void SetFillRule(const nsAString& aFillRule);

  void SetLineDash(const Sequence<double>& aSegments,
                   mozilla::ErrorResult& aRv) override;
  void GetLineDash(nsTArray<double>& aSegments) const override;

  void SetLineDashOffset(double aOffset) override;
  double LineDashOffset() const override;

  void GetMozTextStyle(nsAString& aMozTextStyle)
  {
    GetFont(aMozTextStyle);
  }

  void SetMozTextStyle(const nsAString& aMozTextStyle,
                       mozilla::ErrorResult& aError)
  {
    SetFont(aMozTextStyle, aError);
  }

  void DrawWindow(nsGlobalWindow& aWindow, double aX, double aY,
		  double aW, double aH,
                  const nsAString& aBgColor, uint32_t aFlags,
                  mozilla::ErrorResult& aError);

  bool SwitchRenderingMode(RenderingMode aRenderingMode);

  // Eventually this should be deprecated. Keeping for now to keep the binding functional.
  void Demote();

  nsresult Redraw();

  virtual int32_t GetWidth() const override;
  virtual int32_t GetHeight() const override;
  gfx::IntSize GetSize() const { return gfx::IntSize(mWidth, mHeight); }

  // nsICanvasRenderingContextInternal
  /**
    * Gets the pres shell from either the canvas element or the doc shell
    */
  virtual nsIPresShell *GetPresShell() override {
    if (mCanvasElement) {
      return mCanvasElement->OwnerDoc()->GetShell();
    }
    if (mDocShell) {
      return mDocShell->GetPresShell();
    }
    return nullptr;
  }
  NS_IMETHOD SetDimensions(int32_t aWidth, int32_t aHeight) override;
  NS_IMETHOD InitializeWithDrawTarget(nsIDocShell* aShell,
                                      NotNull<gfx::DrawTarget*> aTarget) override;

  NS_IMETHOD GetInputStream(const char* aMimeType,
                            const char16_t* aEncoderOptions,
                            nsIInputStream** aStream) override;

  already_AddRefed<mozilla::gfx::SourceSurface> GetSurfaceSnapshot(bool* aPremultAlpha = nullptr) override
  {
    EnsureTarget();
    if (aPremultAlpha) {
      *aPremultAlpha = true;
    }
    return mTarget->Snapshot();
  }

  NS_IMETHOD SetIsOpaque(bool aIsOpaque) override;
  bool GetIsOpaque() override { return mOpaque; }
  NS_IMETHOD Reset() override;
  already_AddRefed<Layer> GetCanvasLayer(nsDisplayListBuilder* aBuilder,
                                         Layer* aOldLayer,
                                         LayerManager* aManager,
                                         bool aMirror = false) override;
  virtual bool ShouldForceInactiveLayer(LayerManager* aManager) override;
  void MarkContextClean() override;
  void MarkContextCleanForFrameCapture() override;
  bool IsContextCleanForFrameCapture() override;
  NS_IMETHOD SetIsIPC(bool aIsIPC) override;
  // this rect is in canvas device space
  void Redraw(const mozilla::gfx::Rect& aR);
  NS_IMETHOD Redraw(const gfxRect& aR) override { Redraw(ToRect(aR)); return NS_OK; }
  NS_IMETHOD SetContextOptions(JSContext* aCx,
                               JS::Handle<JS::Value> aOptions,
                               ErrorResult& aRvForDictionaryInit) override;

  /**
   * An abstract base class to be implemented by callers wanting to be notified
   * that a refresh has occurred. Callers must ensure an observer is removed
   * before it is destroyed.
   */
  virtual void DidRefresh() override;

  // this rect is in mTarget's current user space
  void RedrawUser(const gfxRect& aR);

  // nsISupports interface + CC
  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_SKIPPABLE_SCRIPT_HOLDER_CLASS_AMBIGUOUS(CanvasRenderingContext2D,
                                                                   nsICanvasRenderingContextInternal)

  enum class CanvasMultiGetterType : uint8_t {
    STRING = 0,
    PATTERN = 1,
    GRADIENT = 2
  };

  nsINode* GetParentObject()
  {
    return mCanvasElement;
  }

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

  friend class CanvasRenderingContext2DUserData;

  virtual UniquePtr<uint8_t[]> GetImageBuffer(int32_t* aFormat) override;


  // Given a point, return hit region ID if it exists
  nsString GetHitRegion(const mozilla::gfx::Point& aPoint) override;


  // return true and fills in the bound rect if element has a hit region.
  bool GetHitRegionRect(Element* aElement, nsRect& aRect) override;

  void OnShutdown();

  // Check the global setup, as well as the compositor type:
  bool AllowOpenGLCanvas() const;

protected:
  nsresult GetImageDataArray(JSContext* aCx, int32_t aX, int32_t aY,
                             uint32_t aWidth, uint32_t aHeight,
                             JSObject** aRetval);

  nsresult PutImageData_explicit(int32_t aX, int32_t aY, uint32_t aW, uint32_t aH,
                                 dom::Uint8ClampedArray* aArray,
                                 bool aHasDirtyRect, int32_t aDirtyX, int32_t aDirtyY,
                                 int32_t aDirtyWidth, int32_t aDirtyHeight);

  bool CopyBufferProvider(layers::PersistentBufferProvider& aOld,
                          gfx::DrawTarget& aTarget,
                          gfx::IntRect aCopyRect);

  /**
   * Internal method to complete initialisation, expects mTarget to have been set
   */
  nsresult Initialize(int32_t aWidth, int32_t aHeight);

  nsresult InitializeWithTarget(mozilla::gfx::DrawTarget* aSurface,
                                int32_t aWidth, int32_t aHeight);

  /**
    * The number of living nsCanvasRenderingContexts.  When this goes down to
    * 0, we free the premultiply and unpremultiply tables, if they exist.
    */
  static uint32_t sNumLivingContexts;

  static mozilla::gfx::DrawTarget* sErrorTarget;

  // Some helpers.  Doesn't modify a color on failure.
  void GetStyleAsUnion(OwningStringOrCanvasGradientOrCanvasPattern& aValue,
                       Style aWhichStyle);

  // Returns whether a color was successfully parsed.
  virtual bool ParseColor(const nsAString& aString, nscolor* aColor) override;

   // Returns whether a filter was successfully parsed.
  bool ParseFilter(const nsAString& aString,
                   nsTArray<nsStyleFilter>& aFilterChain,
                   ErrorResult& aError);

  // Returns whether the font was successfully updated.
  bool SetFontInternal(const nsAString& aFont, mozilla::ErrorResult& aError);


  /**
   * Creates the error target, if it doesn't exist
   */
  static void EnsureErrorTarget();

  /* This function ensures there is a writable pathbuilder available, this
   * pathbuilder may be working in user space or in device space or
   * device space.
   * After calling this function mPathTransformWillUpdate will be false
   */
  void EnsureWritablePath();

  // Ensures a path in UserSpace is available.
  void EnsureUserSpacePath(const CanvasWindingRule& aWinding = CanvasWindingRule::Nonzero);

  // Report the fillRule has changed.
  void FillRuleChanged();

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
               RenderingMode aRenderMode = RenderingMode::DefaultBackendMode) override;

  void RestoreClipsAndTransformToTarget();

  bool TrySkiaGLTarget(RefPtr<gfx::DrawTarget>& aOutDT,
                       RefPtr<layers::PersistentBufferProvider>& aOutProvider);

  bool TrySharedTarget(RefPtr<gfx::DrawTarget>& aOutDT,
                       RefPtr<layers::PersistentBufferProvider>& aOutProvider);

  bool TryBasicTarget(RefPtr<gfx::DrawTarget>& aOutDT,
                      RefPtr<layers::PersistentBufferProvider>& aOutProvider);

  void RegisterAllocation();

  void SetInitialState();

  void SetErrorState();

  /**
   * This method is run at the end of the event-loop spin where
   * ScheduleStableStateCallback was called.
   *
   * We use it to unlock resources that need to be locked while drawing.
   */
  void OnStableState();

  /**
   * Cf. OnStableState.
   */
  void ScheduleStableStateCallback();

  /**
   * Disposes an old target and prepares to lazily create a new target.
   */
  void ClearTarget();

  /*
   * Returns the target to the buffer provider. i.e. this will queue a frame for
   * rendering.
   */
  void ReturnTarget(bool aForceReset = false);

  /**
   * Check if the target is valid after calling EnsureTarget.
   */
  bool IsTargetValid() const override{
    return !!mTarget && mTarget != sErrorTarget;
  }

  /**
    * Returns the surface format this canvas should be allocated using. Takes
    * into account mOpaque, platform requirements, etc.
    */
  mozilla::gfx::SurfaceFormat GetSurfaceFormat() const;

  /**
   * Returns true if we know for sure that the pattern for a given style is opaque.
   * Usefull to know if we can discard the content below in certain situations.
   */
  bool PatternIsOpaque(Style aStyle) const;

  /**
   * Update CurrentState().filter with the filter description for
   * CurrentState().filterChain.
   * Flushes the PresShell, so the world can change if you call this function.
   */
  void UpdateFilter();

  nsLayoutUtils::SurfaceFromElementResult
    CachedSurfaceFromElement(Element* aElement);

  void DrawImage(const CanvasImageSource& aImgElt,
                 double aSx, double aSy, double aSw, double aSh,
                 double aDx, double aDy, double aDw, double aDh,
                 uint8_t aOptional_argc, mozilla::ErrorResult& aError);

  void DrawDirectlyToCanvas(const nsLayoutUtils::DirectDrawInfo& aImage,
                            mozilla::gfx::Rect* aBounds,
                            mozilla::gfx::Rect aDest,
                            mozilla::gfx::Rect aSrc,
                            gfx::IntSize aImgSize);

  nsString& GetFont()
  {
    /* will initilize the value if not set, else does nothing */
    GetCurrentFontStyle();

    return CurrentState().font;
  }

  // This function maintains a list of raw pointers to cycle-collected
  // objects. We need to ensure that no entries persist beyond unlink,
  // since the objects are logically destructed at that point.
  static std::vector<CanvasRenderingContext2D*>& DemotableContexts();
  static void DemoteOldestContextIfNecessary();

  static void AddDemotableContext(CanvasRenderingContext2D* aContext);
  static void RemoveDemotableContext(CanvasRenderingContext2D* aContext);

  RenderingMode mRenderingMode;

  layers::LayersBackend mCompositorBackend;

  // Member vars
  int32_t mWidth, mHeight;

  // This is true when the canvas is valid, but of zero size, this requires
  // specific behavior on some operations.
  bool mZero;

  bool mOpaque;

  // This is true when the next time our layer is retrieved we need to
  // recreate it (i.e. our backing surface changed)
  bool mResetLayer;
  // This is needed for drawing in drawAsyncXULElement
  bool mIPC;
  // True if the current DrawTarget is using skia-gl, used so we can avoid
  // requesting the DT from mBufferProvider to check.
  bool mIsSkiaGL;

  bool mHasPendingStableStateCallback;

  nsTArray<CanvasRenderingContext2DUserData*> mUserDatas;

  // If mCanvasElement is not provided, then a docshell is
  nsCOMPtr<nsIDocShell> mDocShell;

  RefPtr<mozilla::layers::PersistentBufferProvider> mBufferProvider;

  uint32_t SkiaGLTex() const;

  // This observes our draw calls at the beginning of the canvas
  // lifetime and switches to software or GPU mode depending on
  // what it thinks is best
  CanvasDrawObserver* mDrawObserver;
  void RemoveDrawObserver();

  RefPtr<CanvasShutdownObserver> mShutdownObserver;
  void RemoveShutdownObserver();
  virtual bool AlreadyShutDown() const override{ return !mShutdownObserver; }

  /**
    * Flag to avoid duplicate calls to InvalidateFrame. Set to true whenever
    * Redraw is called, reset to false when Render is called.
    */
  bool mIsEntireFrameInvalid;
  /**
    * When this is set, the first call to Redraw(gfxRect) should set
    * mIsEntireFrameInvalid since we expect it will be followed by
    * many more Redraw calls.
    */
  bool mPredictManyRedrawCalls;

  /**
   * Flag to avoid unnecessary surface copies to FrameCaptureListeners in the
   * case when the canvas is not currently being drawn into and not rendered
   * but canvas capturing is still ongoing.
   */
  bool mIsCapturedFrameInvalid;

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

  /**
    * Number of times we've invalidated before calling redraw
    */
  uint32_t mInvalidateCount;
  static const uint32_t kCanvasMaxInvalidateCount = 100;

  /**
    * State information for hit regions
    */
  struct RegionInfo
  {
    nsString          mId;
    // fallback element for a11y
    RefPtr<Element> mElement;
    // Path of the hit region in the 2d context coordinate space (not user space)
    RefPtr<gfx::Path> mPath;
  };

  nsTArray<RegionInfo> mHitRegionsOptions;

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
  bool NeedToApplyFilter()
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
      EnsureTarget();
    }
    MOZ_ASSERT(CurrentState().filterSourceGraphicTainted == isWriteOnly);
    return CurrentState().filter;
  }

  bool NeedToCalculateBounds()
  {
    return NeedToDrawShadow() || NeedToApplyFilter();
  }

  mozilla::gfx::CompositionOp UsedOperation()
  {
    if (NeedToDrawShadow() || NeedToApplyFilter()) {
      // In this case the shadow or filter rendering will use the operator.
      return mozilla::gfx::CompositionOp::OP_OVER;
    }

    return CurrentState().op;
  }

  // text

protected:
  enum class TextDrawOperation : uint8_t {
    FILL,
    STROKE,
    MEASURE
  };

protected:
  gfxFontGroup *GetCurrentFontStyle();

  /**
   * Implementation of the fillText, strokeText, and measure functions with
   * the operation abstracted to a flag.
   */
  nsresult DrawOrMeasureText(const nsAString& aText,
                             float aX,
                             float aY,
                             const Optional<double>& aMaxWidth,
                             TextDrawOperation aOp,
                             float* aWidth);

  bool CheckSizeForSkiaGL(mozilla::gfx::IntSize aSize);

  friend class CanvasGeneralPattern;
  friend class CanvasFilterChainObserver;
  friend class AdjustedTarget;
  friend class AdjustedTargetForShadow;
  friend class AdjustedTargetForFilter;

  // other helpers
  void GetAppUnitsValues(int32_t* aPerDevPixel, int32_t* aPerCSSPixel)
  {
    // If we don't have a canvas element, we just return something generic.
    int32_t devPixel = 60;
    int32_t cssPixel = 60;

    nsIPresShell *ps = GetPresShell();
    nsPresContext *pc;

    if (!ps) goto FINISH;
    pc = ps->GetPresContext();
    if (!pc) goto FINISH;
    devPixel = pc->AppUnitsPerDevPixel();
    cssPixel = pc->AppUnitsPerCSSPixel();

  FINISH:
    if (aPerDevPixel)
      *aPerDevPixel = devPixel;
    if (aPerCSSPixel)
      *aPerCSSPixel = cssPixel;
  }

  friend struct CanvasBidiProcessor;
  friend class CanvasDrawObserver;
};

} // namespace dom
} // namespace mozilla

#endif /* CanvasRenderingContext2D_h */
