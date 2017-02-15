/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_CanvasGradient_h
#define mozilla_dom_CanvasGradient_h

#include "mozilla/Attributes.h"
#include "nsTArray.h"
#include "mozilla/RefPtr.h"
#include "mozilla/dom/CanvasRenderingContext2DBinding.h"
#include "mozilla/dom/BasicRenderingContext2D.h"
#include "mozilla/gfx/2D.h"
#include "nsWrapperCache.h"
#include "gfxGradientCache.h"

using mozilla::gfx::Float;
using mozilla::gfx::Point;

namespace mozilla {
namespace dom {

class BasicRenderingContext2D;

class CanvasGradient : public nsWrapperCache
{
public:
  NS_INLINE_DECL_CYCLE_COLLECTING_NATIVE_REFCOUNTING(CanvasGradient)
  NS_DECL_CYCLE_COLLECTION_SCRIPT_HOLDER_NATIVE_CLASS(CanvasGradient)

  enum class Type : uint8_t {
    LINEAR = 0,
    RADIAL
  };

  Type GetType()
  {
    return mType;
  }

  mozilla::gfx::GradientStops *
  GetGradientStopsForTarget(mozilla::gfx::DrawTarget *aRT);

  // WebIDL
  void AddColorStop(float offset, const nsAString& colorstr, ErrorResult& rv);

  JSObject* WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) override
  {
    return CanvasGradientBinding::Wrap(aCx, this, aGivenProto);
  }

  BasicRenderingContext2D* GetParentObject()
  {
    return mContext;
  }

protected:
  friend struct CanvasBidiProcessor;

  CanvasGradient(BasicRenderingContext2D* aContext, Type aType)
    : mContext(aContext)
    , mType(aType)
  {
  }

  RefPtr<BasicRenderingContext2D> mContext;
  nsTArray<mozilla::gfx::GradientStop> mRawStops;
  RefPtr<mozilla::gfx::GradientStops> mStops;
  Type mType;
  virtual ~CanvasGradient() {}
};

class CanvasRadialGradient : public CanvasGradient
{
public:
  CanvasRadialGradient(BasicRenderingContext2D* aContext,
                       const Point& aBeginOrigin, Float aBeginRadius,
                       const Point& aEndOrigin, Float aEndRadius)
    : CanvasGradient(aContext, Type::RADIAL)
    , mCenter1(aBeginOrigin)
    , mCenter2(aEndOrigin)
    , mRadius1(aBeginRadius)
    , mRadius2(aEndRadius)
  {
  }

  Point mCenter1;
  Point mCenter2;
  Float mRadius1;
  Float mRadius2;
};

class CanvasLinearGradient : public CanvasGradient
{
public:
  CanvasLinearGradient(BasicRenderingContext2D* aContext,
                       const Point& aBegin, const Point& aEnd)
    : CanvasGradient(aContext, Type::LINEAR)
    , mBegin(aBegin)
    , mEnd(aEnd)
  {
  }

protected:
  friend struct CanvasBidiProcessor;
  friend class CanvasGeneralPattern;

  // Beginning of linear gradient.
  Point mBegin;
  // End of linear gradient.
  Point mEnd;
};

} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_CanvasGradient_h
