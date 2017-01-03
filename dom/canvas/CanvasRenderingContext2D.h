/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef CanvasRenderingContext2D_h
#define CanvasRenderingContext2D_h

#include "mozilla/dom/CanvasRenderingContext2DBinding.h"
#include "RenderingContext2D.h"

namespace mozilla {
namespace dom {

struct CanvasBidiProcessor;

class CanvasDrawObserver
{
public:
  explicit CanvasDrawObserver(RenderingContext2D* aCanvasContext);

  // Only enumerate draw calls that could affect the heuristic
  enum DrawCallType {
    PutImageData,
    GetImageData,
    DrawImage
  };

  // This is the one that we call on relevant draw calls and count
  // GPU vs. CPU preferrable calls...
  void DidDrawCall(DrawCallType aType);

  // When this returns true, the observer is done making the decisions.
  // Right now, we expect to get rid of the observer after the FrameEnd
  // returns true, though the decision could eventually change if the
  // function calls shift.  If we change to monitor the functions called
  // and make decisions to change more than once, we would probably want
  // FrameEnd to reset the timer and counters as it returns true.
  bool FrameEnd();

private:
  // These values will be picked up from preferences:
  int32_t mMinFramesBeforeDecision;
  float mMinSecondsBeforeDecision;
  int32_t mMinCallsBeforeDecision;

  RenderingContext2D* mCanvasContext;
  int32_t mSoftwarePreferredCalls;
  int32_t mGPUPreferredCalls;
  int32_t mFramesRendered;
  TimeStamp mCreationTime;
};

class CanvasRenderingContext2D : public RenderingContext2D
{
public:
  NS_DECL_CYCLE_COLLECTION_SKIPPABLE_SCRIPT_HOLDER_CLASS_INHERITED(CanvasRenderingContext2D,
                                                                   RenderingContext2D)

  explicit CanvasRenderingContext2D(layers::LayersBackend aCompositorBackend)
    : RenderingContext2D(aCompositorBackend){}

  virtual JSObject* WrapObject(JSContext *aCx, JS::Handle<JSObject*> aGivenProto) override;

  //
  // CanvasFilter
  //
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

  //
  // UserInterface
  //
  void DrawFocusIfNeeded(mozilla::dom::Element& aElement, ErrorResult& aRv);
  bool DrawCustomFocusRing(mozilla::dom::Element& aElement);

  //
  // CanvasImageData
  //
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

  //
  // CanvasTextDrawingStyles
  //
  void GetFont(nsAString& aFont)
  {
    aFont = GetFont();
  }

  nsString& GetFont()
  {
    /* will initilize the value if not set, else does nothing */
    GetCurrentFontStyle();

    return CurrentState().font;
  }

  void SetFont(const nsAString& aFont, mozilla::ErrorResult& aError);
  void GetTextAlign(nsAString& aTextAlign);
  void SetTextAlign(const nsAString& aTextAlign);
  void GetTextBaseline(nsAString& aTextBaseline);
  void SetTextBaseline(const nsAString& aTextBaseline);

  void GetMozTextStyle(nsAString& aMozTextStyle)
  {
    GetFont(aMozTextStyle);
  }

  void SetMozTextStyle(const nsAString& aMozTextStyle,
                       mozilla::ErrorResult& aError)
  {
    SetFont(aMozTextStyle, aError);
  }

  //
  // CanvasText
  //
  void FillText(const nsAString& aText, double aX, double aY,
                const Optional<double>& aMaxWidth,
                mozilla::ErrorResult& aError);
  void StrokeText(const nsAString& aText, double aX, double aY,
                  const Optional<double>& aMaxWidth,
                  mozilla::ErrorResult& aError);
  TextMetrics*
    MeasureText(const nsAString& aRawText, mozilla::ErrorResult& aError);

private:
  ~CanvasRenderingContext2D(){}
  NS_DECL_ISUPPORTS_INHERITED

protected:
  friend class CanvasFilterChainObserver;
  friend struct CanvasBidiProcessor;

  nsresult GetImageDataArray(JSContext* aCx, int32_t aX, int32_t aY,
                             uint32_t aWidth, uint32_t aHeight,
                             JSObject** aRetval);
  nsresult PutImageData_explicit(int32_t aX, int32_t aY, uint32_t aW, uint32_t aH,
                                 dom::Uint8ClampedArray* aArray,
                                 bool aHasDirtyRect, int32_t aDirtyX, int32_t aDirtyY,
                                 int32_t aDirtyWidth, int32_t aDirtyHeight);


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

  // Returns whether the font was successfully updated.
  bool SetFontInternal(const nsAString& aFont, mozilla::ErrorResult& aError);

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

};

} // namespace dom
} // namespace mozilla

#endif
