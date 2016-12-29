#include "CanvasRenderingContext2D.h"

#include "nsCCUncollectableMarker.h"
#include "nsDeviceContext.h"
#include "nsFilterInstance.h"
#include "nsFocusManager.h"
#include "nsFontMetrics.h"
#include "nsIFocusManager.h"
#include "nsSVGEffects.h"

using mozilla::css::Declaration;

namespace mozilla {
namespace dom {

JSObject*
CanvasRenderingContext2D::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto)
{
  return CanvasRenderingContext2DBinding::Wrap(aCx, this, aGivenProto);
}

//
// CanvasFilter
//
class CanvasFilterChainObserver : public nsSVGFilterChainObserver
{
public:
  CanvasFilterChainObserver(nsTArray<nsStyleFilter>& aFilters,
                            Element* aCanvasElement,
                            CanvasRenderingContext2D* aContext)
    : nsSVGFilterChainObserver(aFilters, aCanvasElement)
    , mContext(aContext)
  {
  }

  virtual void DoUpdate() override
  {
    if (!mContext) {
      MOZ_CRASH("GFX: This should never be called without a context");
    }
    // Refresh the cached FilterDescription in mContext->CurrentState().filter.
    // If this filter is not at the top of the state stack, we'll refresh the
    // wrong filter, but that's ok, because we'll refresh the right filter
    // when we pop the state stack in RenderingContext2D::Restore().
    mContext->UpdateFilter();
  }

  void DetachFromContext() { mContext = nullptr; }

private:
  CanvasRenderingContext2D *mContext;
};

class CanvasUserSpaceMetrics : public UserSpaceMetricsWithSize
{
public:
  CanvasUserSpaceMetrics(const gfx::IntSize& aSize, const nsFont& aFont,
                         nsIAtom* aFontLanguage, bool aExplicitLanguage,
                         nsPresContext* aPresContext)
    : mSize(aSize)
    , mFont(aFont)
    , mFontLanguage(aFontLanguage)
    , mExplicitLanguage(aExplicitLanguage)
    , mPresContext(aPresContext)
  {
  }

  virtual float GetEmLength() const override
  {
    return NSAppUnitsToFloatPixels(mFont.size,
                                   nsPresContext::AppUnitsPerCSSPixel());
  }

  virtual float GetExLength() const override
  {
    nsDeviceContext* dc = mPresContext->DeviceContext();
    nsFontMetrics::Params params;
    params.language = mFontLanguage;
    params.explicitLanguage = mExplicitLanguage;
    params.textPerf = mPresContext->GetTextPerfMetrics();
    RefPtr<nsFontMetrics> fontMetrics = dc->GetMetricsFor(mFont, params);
    return NSAppUnitsToFloatPixels(fontMetrics->XHeight(),
                                   nsPresContext::AppUnitsPerCSSPixel());
  }

  virtual gfx::Size GetSize() const override
  { return Size(mSize); }

private:
  gfx::IntSize mSize;
  const nsFont& mFont;
  nsIAtom* mFontLanguage;
  bool mExplicitLanguage;
  nsPresContext* mPresContext;
};

void
CanvasRenderingContext2D::SetFilter(const nsAString& aFilter, ErrorResult& aError)
{
  nsTArray<nsStyleFilter> filterChain;
  if (ParseFilter(aFilter, filterChain, aError)) {
    CurrentState().filterString = aFilter;
    filterChain.SwapElements(CurrentState().filterChain);
    if (mCanvasElement) {
      CurrentState().filterChainObserver =
        new CanvasFilterChainObserver(CurrentState().filterChain,
                                      mCanvasElement, this);
      UpdateFilter();
    }
  }
}

static already_AddRefed<Declaration>
CreateFilterDeclaration(const nsAString& aFilter,
                        nsINode* aNode,
                        bool* aOutFilterChanged)
{
  bool dummy;
  return RenderingContext2D::CreateDeclaration(aNode,
    eCSSProperty_filter, aFilter, aOutFilterChanged,
    eCSSProperty_UNKNOWN, EmptyString(), &dummy);
}

static already_AddRefed<nsStyleContext>
ResolveStyleForFilter(const nsAString& aFilterString,
                      nsIPresShell* aPresShell,
                      nsStyleContext* aParentContext,
                      ErrorResult& aError)
{
  nsStyleSet* styleSet = aPresShell->StyleSet()->GetAsGecko();
  if (!styleSet) {
    // XXXheycam ServoStyleSets do not support resolving style from a list of
    // rules yet.
    NS_ERROR("stylo: cannot resolve style for canvas from a ServoStyleSet yet");
    aError.Throw(NS_ERROR_FAILURE);
    return nullptr;
  }

  nsIDocument* document = aPresShell->GetDocument();
  bool filterChanged = false;
  RefPtr<css::Declaration> decl =
    CreateFilterDeclaration(aFilterString, document, &filterChanged);

  if (!filterChanged) {
    // Refuse to accept the filter, but do not throw an error.
    return nullptr;
  }

  // In addition to unparseable values, the spec says we need to reject
  // 'inherit' and 'initial'.
  if (RenderingContext2D::PropertyIsInheritOrInitial(decl, eCSSProperty_filter)) {
    return nullptr;
  }

  nsTArray<nsCOMPtr<nsIStyleRule>> rules;
  rules.AppendElement(decl);

  RefPtr<nsStyleContext> sc =
    styleSet->ResolveStyleForRules(aParentContext, rules);

  return sc.forget();
}

void
CanvasRenderingContext2D::UpdateFilter()
{
  nsCOMPtr<nsIPresShell> presShell = GetPresShell();
  if (!presShell || presShell->IsDestroying()) {
    // Ensure we set an empty filter and update the state to
    // reflect the current "taint" status of the canvas
    CurrentState().filter = FilterDescription();
    CurrentState().filterSourceGraphicTainted =
      (mCanvasElement && mCanvasElement->IsWriteOnly());
    return;
  }

  // The filter might reference an SVG filter that is declared inside this
  // document. Flush frames so that we'll have an nsSVGFilterFrame to work
  // with.
  presShell->FlushPendingNotifications(Flush_Frames);

  bool sourceGraphicIsTainted =
    (mCanvasElement && mCanvasElement->IsWriteOnly());

  CurrentState().filter =
    nsFilterInstance::GetFilterDescription(mCanvasElement,
      CurrentState().filterChain,
      sourceGraphicIsTainted,
      CanvasUserSpaceMetrics(GetSize(),
                             CurrentState().fontFont,
                             CurrentState().fontLanguage,
                             CurrentState().fontExplicitLanguage,
                             presShell->GetPresContext()),
      gfxRect(0, 0, mWidth, mHeight),
      CurrentState().filterAdditionalImages);
  CurrentState().filterSourceGraphicTainted = sourceGraphicIsTainted;
}

bool
CanvasRenderingContext2D::ParseFilter(const nsAString& aString,
                                      nsTArray<nsStyleFilter>& aFilterChain,
                                      ErrorResult& aError)
{
  if (!mCanvasElement && !mDocShell) {
    NS_WARNING("Canvas element must be non-null or a docshell must be provided");
    aError.Throw(NS_ERROR_FAILURE);
    return false;
  }

  nsCOMPtr<nsIPresShell> presShell = GetPresShell();
  if (!presShell) {
    aError.Throw(NS_ERROR_FAILURE);
    return false;
  }

  nsString usedFont;
  RefPtr<nsStyleContext> parentContext =
    GetFontStyleContext(mCanvasElement, GetFont(),
                        presShell, usedFont, aError);
  if (!parentContext) {
    aError.Throw(NS_ERROR_FAILURE);
    return false;
  }

  RefPtr<nsStyleContext> sc =
    ResolveStyleForFilter(aString, presShell, parentContext, aError);

  if (!sc) {
    return false;
  }

  aFilterChain = sc->StyleEffects()->mFilters;
  return true;
}

//
// CanvasUserInterface
//
void CanvasRenderingContext2D::DrawFocusIfNeeded(mozilla::dom::Element& aElement,
                                                 ErrorResult& aRv)
{
  EnsureUserSpacePath();

  if (!mPath) {
    return;
  }

  if (DrawCustomFocusRing(aElement)) {
    Save();

    // set state to conforming focus state
    ContextState& state = CurrentState();
    state.globalAlpha = 1.0;
    state.shadowBlur = 0;
    state.shadowOffset.x = 0;
    state.shadowOffset.y = 0;
    state.op = mozilla::gfx::CompositionOp::OP_OVER;

    state.lineCap = CapStyle::BUTT;
    state.lineJoin = mozilla::gfx::JoinStyle::MITER_OR_BEVEL;
    state.lineWidth = 1;
    CurrentState().dash.Clear();

    // color and style of the rings is the same as for image maps
    // set the background focus color
    CurrentState().SetColorStyle(Style::STROKE, NS_RGBA(255, 255, 255, 255));
    // draw the focus ring
    Stroke();

    // set dashing for foreground
    nsTArray<mozilla::gfx::Float>& dash = CurrentState().dash;
    for (uint32_t i = 0; i < 2; ++i) {
      if (!dash.AppendElement(1, fallible)) {
        aRv.Throw(NS_ERROR_OUT_OF_MEMORY);
        return;
      }
    }

    // set the foreground focus color
    CurrentState().SetColorStyle(Style::STROKE, NS_RGBA(0,0,0, 255));
    // draw the focus ring
    Stroke();

    Restore();
  }
}

bool CanvasRenderingContext2D::DrawCustomFocusRing(mozilla::dom::Element& aElement)
{
  EnsureUserSpacePath();

  HTMLCanvasElement* canvas = GetCanvas();

  if (!canvas|| !nsContentUtils::ContentIsDescendantOf(&aElement, canvas)) {
    return false;
  }

  nsIFocusManager* fm = nsFocusManager::GetFocusManager();
  if (fm) {
    // check that the element i focused
    nsCOMPtr<nsIDOMElement> focusedElement;
    fm->GetFocusedElement(getter_AddRefs(focusedElement));
    if (SameCOMIdentity(aElement.AsDOMNode(), focusedElement)) {
      if (nsPIDOMWindowOuter *window = aElement.OwnerDoc()->GetWindow()) {
        return window->ShouldShowFocusRing();
      }
    }
  }

  return false;
}

NS_IMPL_ADDREF_INHERITED(CanvasRenderingContext2D, RenderingContext2D)
NS_IMPL_RELEASE_INHERITED(CanvasRenderingContext2D, RenderingContext2D)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION_INHERITED(CanvasRenderingContext2D)
NS_INTERFACE_MAP_END_INHERITING(RenderingContext2D)

NS_IMPL_CYCLE_COLLECTION_CLASS(CanvasRenderingContext2D)

NS_IMPL_CYCLE_COLLECTION_CAN_SKIP_BEGIN(CanvasRenderingContext2D)
 if (nsCCUncollectableMarker::sGeneration && tmp->IsBlack()) {
   dom::Element* canvasElement = tmp->mCanvasElement;
    if (canvasElement) {
      if (canvasElement->IsPurple()) {
        canvasElement->RemovePurple();
      }
      dom::Element::MarkNodeChildren(canvasElement);
    }
    return true;
  }
NS_IMPL_CYCLE_COLLECTION_CAN_SKIP_END

NS_IMPL_CYCLE_COLLECTION_CAN_SKIP_IN_CC_BEGIN(CanvasRenderingContext2D)
  return nsCCUncollectableMarker::sGeneration && tmp->IsBlack();
NS_IMPL_CYCLE_COLLECTION_CAN_SKIP_IN_CC_END

NS_IMPL_CYCLE_COLLECTION_CAN_SKIP_THIS_BEGIN(CanvasRenderingContext2D)
  return nsCCUncollectableMarker::sGeneration && tmp->IsBlack();
NS_IMPL_CYCLE_COLLECTION_CAN_SKIP_THIS_END

NS_IMPL_CYCLE_COLLECTION_TRACE_BEGIN_INHERITED(CanvasRenderingContext2D,
                                               RenderingContext2D)
NS_IMPL_CYCLE_COLLECTION_TRACE_END

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN_INHERITED(CanvasRenderingContext2D,
                                                  RenderingContext2D)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN_INHERITED(CanvasRenderingContext2D,
                                                RenderingContext2D)
  // Make sure we remove ourselves from the list of demotable contexts (raw pointers),
  // since we're logically destructed at this point.
  CanvasRenderingContext2D::RemoveDemotableContext(tmp);
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mCanvasElement)
  for (uint32_t i = 0; i < tmp->mStyleStack.Length(); i++) {
    CanvasFilterChainObserver *filterChainObserver =
      static_cast<CanvasFilterChainObserver*>(tmp->mStyleStack[i].filterChainObserver.get());
    if (filterChainObserver) {
      filterChainObserver->DetachFromContext();
    }
    ImplCycleCollectionUnlink(tmp->mStyleStack[i].filterChainObserver);
  }
NS_IMPL_CYCLE_COLLECTION_UNLINK_END

} // namespace dom
} // namespace mozilla
