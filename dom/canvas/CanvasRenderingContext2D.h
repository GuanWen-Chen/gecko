/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef CanvasRenderingContext2D_h
#define CanvasRenderingContext2D_h

#include "FilterSupport.h"
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

using mozilla::gfx::FilterDescription;

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

/**
 ** CanvasRenderingContext2D
 **/
class CanvasRenderingContext2D final :
  public nsICanvasRenderingContextInternal,
  public BasicRenderingContext2D
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
    aFilter = CurrentStateForFilter()->filterString;
  }

  void SetFilter(const nsAString& aFilter, mozilla::ErrorResult& aError);
  void DrawFocusIfNeeded(mozilla::dom::Element& aElement, ErrorResult& aRv);
  bool DrawCustomFocusRing(mozilla::dom::Element& aElement);
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



  void GetFont(nsAString& aFont)
  {
    aFont = GetFont();
  }

  void SetFont(const nsAString& aFont, mozilla::ErrorResult& aError);
  void GetTextAlign(nsAString& aTextAlign);
  void SetTextAlign(const nsAString& aTextAlign);
  void GetTextBaseline(nsAString& aTextBaseline);
  void SetTextBaseline(const nsAString& aTextBaseline);

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

  virtual nsresult Redraw() override;

  virtual int32_t GetWidth() const override;
  virtual int32_t GetHeight() const override;

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
  virtual void RedrawUser(const gfxRect& aR) override;

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

  friend class CanvasRenderingContext2DUserData;

  virtual UniquePtr<uint8_t[]> GetImageBuffer(int32_t* aFormat) override;


  // Given a point, return hit region ID if it exists
  nsString GetHitRegion(const mozilla::gfx::Point& aPoint) override;


  // return true and fills in the bound rect if element has a hit region.
  bool GetHitRegionRect(Element* aElement, nsRect& aRect) override;

  public:
  // state stack handling
  class ContextStateForFilter : public ContextState {
  public:
  ContextStateForFilter() : filterString(u"none"),
                            filterSourceGraphicTainted(false)
  { }

  ContextStateForFilter(const ContextStateForFilter* aOther)
      : ContextState(aOther),
        filterString(aOther->filterString),
        filterChain(aOther->filterChain),
        filterChainObserver(aOther->filterChainObserver),
        filter(aOther->filter),
        filterAdditionalImages(aOther->filterAdditionalImages),
        filterSourceGraphicTainted(aOther->filterSourceGraphicTainted)
  { }
  protected:
  virtual ~ContextStateForFilter() {}
  public:
  NS_DECL_ISUPPORTS_INHERITED

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
  };

protected:
  ContextState* CreateContextState(const ContextState* aOther = nullptr) override {
    return CreateContextState((ContextStateForFilter*) aOther);
  }
  ContextState* CreateContextState(const ContextStateForFilter* aOther = nullptr) {
    return aOther ? new ContextStateForFilter(aOther) : new ContextStateForFilter();
  }

  HTMLCanvasElement* GetCanvasElement() override { return mCanvasElement; }
  nsresult GetImageDataArray(JSContext* aCx, int32_t aX, int32_t aY,
                             uint32_t aWidth, uint32_t aHeight,
                             JSObject** aRetval);

  nsresult PutImageData_explicit(int32_t aX, int32_t aY, uint32_t aW, uint32_t aH,
                                 dom::Uint8ClampedArray* aArray,
                                 bool aHasDirtyRect, int32_t aDirtyX, int32_t aDirtyY,
                                 int32_t aDirtyWidth, int32_t aDirtyHeight);

  /**
   * Internal method to complete initialisation, expects mTarget to have been set
   */
  nsresult Initialize(int32_t aWidth, int32_t aHeight);

  nsresult InitializeWithTarget(mozilla::gfx::DrawTarget* aSurface,
                                int32_t aWidth, int32_t aHeight);

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

  // Report the fillRule has changed.
  void FillRuleChanged();

  bool TrySkiaGLTarget(RefPtr<gfx::DrawTarget>& aOutDT,
                       RefPtr<layers::PersistentBufferProvider>& aOutProvider) override;

  bool TrySharedTarget(RefPtr<gfx::DrawTarget>& aOutDT,
                       RefPtr<layers::PersistentBufferProvider>& aOutProvider) override;

  /**
   * Disposes an old target and prepares to lazily create a new target.
   */
  void ClearTarget();

  /**
    * Returns the surface format this canvas should be allocated using. Takes
    * into account mOpaque, platform requirements, etc.
    */
  mozilla::gfx::SurfaceFormat GetSurfaceFormat() const override;

  /**
   * Update CurrentState()->filter with the filter description for
   * CurrentState()->filterChain.
   * Flushes the PresShell, so the world can change if you call this function.
   */
  void UpdateFilter();

  nsString& GetFont()
  {
    /* will initilize the value if not set, else does nothing */
    GetCurrentFontStyle();

    return CurrentState()->font;
  }

  // This function maintains a list of raw pointers to cycle-collected
  // objects. We need to ensure that no entries persist beyond unlink,
  // since the objects are logically destructed at that point.
  static std::vector<CanvasRenderingContext2D*>& DemotableContexts();
  static void DemoteOldestContextIfNecessary();

  static void AddDemotableContext(CanvasRenderingContext2D* aContext);
  static void RemoveDemotableContext(CanvasRenderingContext2D* aContext);

  // Member vars

  // This is true when the canvas is valid, but of zero size, this requires
  // specific behavior on some operations.
  bool mZero;

  bool mOpaque;

  // This is true when the next time our layer is retrieved we need to
  // recreate it (i.e. our backing surface changed)
  bool mResetLayer;
  // This is needed for drawing in drawAsyncXULElement
  bool mIPC;

  nsTArray<CanvasRenderingContext2DUserData*> mUserDatas;

  // If mCanvasElement is not provided, then a docshell is
  nsCOMPtr<nsIDocShell> mDocShell;

  uint32_t SkiaGLTex() const;

  // This observes our draw calls at the beginning of the canvas
  // lifetime and switches to software or GPU mode depending on
  // what it thinks is best
  CanvasDrawObserver* mDrawObserver;
  void RemoveDrawObserver();
  void DidImageDrawCall() override;

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
    * Returns true if the result of a drawing operation should be
    * drawn with a filter.
    */
  virtual bool NeedToApplyFilter() override
  {
    return EnsureUpdatedFilter().mPrimitives.Length() > 0;
  }

  /**
   * Calls UpdateFilter if the canvas's WriteOnly state has changed between the
   * last call to UpdateFilter and now.
   */
  const gfx::FilterDescription& EnsureUpdatedFilter() {
    bool isWriteOnly = mCanvasElement && mCanvasElement->IsWriteOnly();
    if (CurrentStateForFilter()->filterSourceGraphicTainted != isWriteOnly) {
      UpdateFilter();
      EnsureTarget();
    }
    MOZ_ASSERT(CurrentStateForFilter()->filterSourceGraphicTainted == isWriteOnly);
    return CurrentStateForFilter()->filter;
  }

protected:
  enum class TextDrawOperation : uint8_t {
    FILL,
    STROKE,
    MEASURE
  };

protected:
  inline ContextStateForFilter* CurrentStateForFilter() {
    return static_cast<ContextStateForFilter*>(mStyleStack[mStyleStack.Length() - 1].get());
  }

  inline const ContextStateForFilter* CurrentStateForFilterlter() const {
    return static_cast<ContextStateForFilter*>(mStyleStack[mStyleStack.Length() - 1].get());
  }

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
