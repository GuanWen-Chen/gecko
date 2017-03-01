/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/PaintRenderingContext2D.h"
#include "mozilla/dom/PaintRenderingContext2DBinding.h"

namespace mozilla {
namespace dom {

JSObject*
PaintRenderingContext2D::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto)
{
  return PaintRenderingContext2DBinding::Wrap(aCx, this, aGivenProto);
}

already_AddRefed<mozilla::gfx::SourceSurface>
PaintRenderingContext2D::GetSnapShot()
{
  EnsureTarget();

  return mTarget->Snapshot();
}

void
PaintRenderingContext2D::SetDimensions(int32_t aWidth, int32_t aHeight)
{
  Reset();
  SetInitialState();

  // Zero sized surfaces can cause problems.
  if (aHeight == 0) {
    aHeight = 1;
  }
  if (aWidth == 0) {
    aWidth = 1;
  }
  mWidth = aWidth;
  mHeight = aHeight;
}

bool
PaintRenderingContext2D::TrySkiaGLTarget(
  RefPtr<gfx::DrawTarget>& aOutDT,
  RefPtr<layers::PersistentBufferProvider>& aOutProvider)
{
  aOutDT = nullptr;
  aOutProvider = nullptr;

  mIsSkiaGL = false;

  return false;
}

bool
PaintRenderingContext2D::TrySharedTarget(
  RefPtr<gfx::DrawTarget>& aOutDT,
  RefPtr<layers::PersistentBufferProvider>& aOutProvider)
{
  aOutDT = nullptr;
  aOutProvider = nullptr;

  return false;
}

bool
PaintRenderingContext2D::ParseColor(const nsAString& aString,
                                    nscolor* aColor)
{
  // Pass the CSS Loader object to the parser, to allow parser error
  // reports to include the outer window ID.
  nsCSSParser parser(nullptr);
  nsCSSValue value;
  if (!parser.ParseColorString(aString, nullptr, 0, value)) {
    return false;
  }

  if (value.IsNumericColorUnit()) {
    // if we already have a color we can just use it directly
    *aColor = value.GetColorValue();
  } else {
    // otherwise resolve it
    nsCOMPtr<nsIPresShell> presShell = nullptr;
    RefPtr<nsStyleContext> parentContext;

    Unused << nsRuleNode::ComputeColor(
      value, presShell ? presShell->GetPresContext() : nullptr, parentContext,
      *aColor);
  }
  return true;
}

NS_IMPL_CYCLE_COLLECTING_ADDREF(PaintRenderingContext2D)
NS_IMPL_CYCLE_COLLECTING_RELEASE(PaintRenderingContext2D)

NS_IMPL_CYCLE_COLLECTION_CLASS(PaintRenderingContext2D)

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN(PaintRenderingContext2D)
  // Make sure we remove ourselves from the list of demotable contexts (raw pointers),
  // since we're logically destructed at this point.
//  PaintRenderingContext2D::RemoveDemotableContext(tmp);
  for (uint32_t i = 0; i < tmp->mStyleStack.Length(); i++) {
    ImplCycleCollectionUnlink(tmp->mStyleStack[i].patternStyles[Style::STROKE]);
    ImplCycleCollectionUnlink(tmp->mStyleStack[i].patternStyles[Style::FILL]);
    ImplCycleCollectionUnlink(tmp->mStyleStack[i].gradientStyles[Style::STROKE]);
    ImplCycleCollectionUnlink(tmp->mStyleStack[i].gradientStyles[Style::FILL]);
  }
  NS_IMPL_CYCLE_COLLECTION_UNLINK_PRESERVED_WRAPPER
NS_IMPL_CYCLE_COLLECTION_UNLINK_END

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN(PaintRenderingContext2D)
  for (uint32_t i = 0; i < tmp->mStyleStack.Length(); i++) {
    ImplCycleCollectionTraverse(cb, tmp->mStyleStack[i].patternStyles[Style::STROKE], "Stroke CanvasPattern");
    ImplCycleCollectionTraverse(cb, tmp->mStyleStack[i].patternStyles[Style::FILL], "Fill CanvasPattern");
    ImplCycleCollectionTraverse(cb, tmp->mStyleStack[i].gradientStyles[Style::STROKE], "Stroke CanvasGradient");
    ImplCycleCollectionTraverse(cb, tmp->mStyleStack[i].gradientStyles[Style::FILL], "Fill CanvasGradient");
    ImplCycleCollectionTraverse(cb, tmp->mStyleStack[i].filterChainObserver, "Filter Chain Observer");
  }
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

NS_IMPL_CYCLE_COLLECTION_TRACE_WRAPPERCACHE(PaintRenderingContext2D)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(PaintRenderingContext2D)
  NS_INTERFACE_MAP_ENTRY(nsISupports)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(BasicRenderingContext2D)
NS_INTERFACE_MAP_END

} // namespace dom
} // namespace mozilla
