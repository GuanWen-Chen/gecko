#include "CanvasGradient.h"

#include "mozilla/gfx/2D.h"

namespace mozilla {
namespace dom {

  mozilla::gfx::Pattern& CanvasGeneralPattern::ForStyle(RenderingContext2D* aCtx,
                    Style aStyle,
                    DrawTarget* aRT)
  {
    // This should only be called once or the mPattern destructor will
    // not be executed.
    NS_ASSERTION(!mPattern.GetPattern(), "ForStyle() should only be called once on CanvasGeneralPattern!");

    const ContextState &state = aCtx->CurrentState();

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
      if (aCtx->mCanvasElement) {
        CanvasUtils::DoDrawImageSecurityCheck(aCtx->mCanvasElement,
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