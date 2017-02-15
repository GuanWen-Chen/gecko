/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CanvasGradient.h"

#include "mozilla/gfx/Types.h"
#include "mozilla/dom/CanvasRenderingContext2D.h"
#include "nsCSSParser.h"

namespace mozilla {
namespace dom {

using mozilla::gfx::Color;
using mozilla::gfx::GradientStop;
using mozilla::gfx::GradientStops;

void
CanvasGradient::AddColorStop(float aOffset, const nsAString& aColorstr, ErrorResult& aRv)
{
  if (aOffset < 0.0 || aOffset > 1.0) {
    aRv.Throw(NS_ERROR_DOM_INDEX_SIZE_ERR);
    return;
  }

  nsCSSValue value;
  nsCSSParser parser;
  if (!parser.ParseColorString(aColorstr, nullptr, 0, value)) {
    aRv.Throw(NS_ERROR_DOM_SYNTAX_ERR);
    return;
  }

  nscolor color;
  nsCOMPtr<nsIPresShell> presShell =
    mContext ? static_cast<CanvasRenderingContext2D*>(mContext.get())->GetPresShell() : nullptr;
  if (!nsRuleNode::ComputeColor(value, presShell ? presShell->GetPresContext() : nullptr,
                                nullptr, color)) {
    aRv.Throw(NS_ERROR_DOM_SYNTAX_ERR);
    return;
  }

  mStops = nullptr;

  GradientStop newStop;

  newStop.offset = aOffset;
  newStop.color = Color::FromABGR(color);

  mRawStops.AppendElement(newStop);
}

mozilla::gfx::GradientStops *
CanvasGradient::GetGradientStopsForTarget(mozilla::gfx::DrawTarget *aRT)
{
  if (mStops && mStops->GetBackendType() == aRT->GetBackendType()) {
    return mStops;
  }

  mStops =
    gfx::gfxGradientCache::GetOrCreateGradientStops(aRT,
                                                    mRawStops,
                                                    gfx::ExtendMode::CLAMP);

  return mStops;
}

} // namespace dom
} // namespace mozilla