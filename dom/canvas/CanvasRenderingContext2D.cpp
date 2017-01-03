#include "CanvasRenderingContext2D.h"

#include "gfxImageSurface.h"
#include "gfxUtils.h"
#include "mozilla/dom/ImageData.h"
#include "mozilla/dom/TextMetrics.h"
#include "mozilla/layers/PersistentBufferProvider.h"
#include "nsBidiPresUtils.h"
#include "nsCCUncollectableMarker.h"
#include "nsComputedDOMStyle.h"
#include "nsCSSParser.h"
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

static already_AddRefed<Declaration>
CreateDeclaration(nsINode* aNode,
  const nsCSSPropertyID aProp1, const nsAString& aValue1, bool* aChanged1,
  const nsCSSPropertyID aProp2, const nsAString& aValue2, bool* aChanged2)
{
  nsIPrincipal* principal = aNode->NodePrincipal();
  nsIDocument* document = aNode->OwnerDoc();

  nsIURI* docURL = document->GetDocumentURI();
  nsIURI* baseURL = document->GetDocBaseURI();

  // Pass the CSS Loader object to the parser, to allow parser error reports
  // to include the outer window ID.
  nsCSSParser parser(document->CSSLoader());

  RefPtr<Declaration> declaration =
    parser.ParseStyleAttribute(EmptyString(), docURL, baseURL, principal);

  if (aProp1 != eCSSProperty_UNKNOWN) {
    parser.ParseProperty(aProp1, aValue1, docURL, baseURL, principal,
                         declaration, aChanged1, false);
  }

  if (aProp2 != eCSSProperty_UNKNOWN) {
    parser.ParseProperty(aProp2, aValue2, docURL, baseURL, principal,
                         declaration, aChanged2, false);
  }

  declaration->SetImmutable();
  return declaration.forget();
}

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
CreateFontDeclaration(const nsAString& aFont,
                      nsINode* aNode,
                      bool* aOutFontChanged)
{
  bool lineHeightChanged;
  return CreateDeclaration(aNode,
    eCSSProperty_font, aFont, aOutFontChanged,
    eCSSProperty_line_height, NS_LITERAL_STRING("normal"), &lineHeightChanged);
}

static already_AddRefed<nsStyleContext>
GetFontParentStyleContext(Element* aElement, nsIPresShell* aPresShell,
                          ErrorResult& aError)
{
  if (aElement && aElement->IsInUncomposedDoc()) {
    // Inherit from the canvas element.
    RefPtr<nsStyleContext> result =
      nsComputedDOMStyle::GetStyleContextForElement(aElement, nullptr,
                                                    aPresShell);
    if (!result) {
      aError.Throw(NS_ERROR_FAILURE);
      return nullptr;
    }
    return result.forget();
  }

  // otherwise inherit from default (10px sans-serif)

  nsStyleSet* styleSet = aPresShell->StyleSet()->GetAsGecko();
  if (!styleSet) {
    // XXXheycam ServoStyleSets do not support resolving style from a list of
    // rules yet.
    NS_ERROR("stylo: cannot resolve style for canvas from a ServoStyleSet yet");
    aError.Throw(NS_ERROR_FAILURE);
    return nullptr;
  }

  bool changed;
  RefPtr<css::Declaration> parentRule =
    CreateFontDeclaration(NS_LITERAL_STRING("10px sans-serif"),
                          aPresShell->GetDocument(), &changed);

  nsTArray<nsCOMPtr<nsIStyleRule>> parentRules;
  parentRules.AppendElement(parentRule);
  RefPtr<nsStyleContext> result =
    styleSet->ResolveStyleForRules(nullptr, parentRules);

  if (!result) {
    aError.Throw(NS_ERROR_FAILURE);
    return nullptr;
  }
  return result.forget();
}

static bool
PropertyIsInheritOrInitial(Declaration* aDeclaration, const nsCSSPropertyID aProperty)
{
  // We know the declaration is not !important, so we can use
  // GetNormalBlock().
  const nsCSSValue* filterVal =
    aDeclaration->GetNormalBlock()->ValueFor(aProperty);
  return (!filterVal || (filterVal->GetUnit() == eCSSUnit_Unset ||
                         filterVal->GetUnit() == eCSSUnit_Inherit ||
                         filterVal->GetUnit() == eCSSUnit_Initial));
}

static already_AddRefed<nsStyleContext>
GetFontStyleContext(Element* aElement, const nsAString& aFont,
                    nsIPresShell* aPresShell,
                    nsAString& aOutUsedFont,
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

  bool fontParsedSuccessfully = false;
  RefPtr<css::Declaration> decl =
    CreateFontDeclaration(aFont, aPresShell->GetDocument(),
                          &fontParsedSuccessfully);

  if (!fontParsedSuccessfully) {
    // We got a syntax error.  The spec says this value must be ignored.
    return nullptr;
  }

  // In addition to unparseable values, the spec says we need to reject
  // 'inherit' and 'initial'. The easiest way to check for this is to look
  // at font-size-adjust, which the font shorthand resets to either 'none' or
  // '-moz-system-font'.
  if (PropertyIsInheritOrInitial(decl, eCSSProperty_font_size_adjust)) {
    return nullptr;
  }

  // have to get a parent style context for inherit-like relative
  // values (2em, bolder, etc.)
  RefPtr<nsStyleContext> parentContext =
    GetFontParentStyleContext(aElement, aPresShell, aError);

  if (aError.Failed()) {
    aError.Throw(NS_ERROR_FAILURE);
    return nullptr;
  }

  MOZ_RELEASE_ASSERT(parentContext,
                     "GFX: GetFontParentStyleContext should have returned an error if it couldn't get a parent context.");

  MOZ_ASSERT(!aPresShell->IsDestroying(),
             "GetFontParentStyleContext should have returned an error if the presshell is being destroyed.");

  nsTArray<nsCOMPtr<nsIStyleRule>> rules;
  rules.AppendElement(decl);
  // add a rule to prevent text zoom from affecting the style
  rules.AppendElement(new nsDisableTextZoomStyleRule);

  RefPtr<nsStyleContext> sc =
    styleSet->ResolveStyleForRules(parentContext, rules);

  // The font getter is required to be reserialized based on what we
  // parsed (including having line-height removed).  (Older drafts of
  // the spec required font sizes be converted to pixels, but that no
  // longer seems to be required.)
  decl->GetPropertyValueByID(eCSSProperty_font, aOutUsedFont);

  return sc.forget();
}

static already_AddRefed<Declaration>
CreateFilterDeclaration(const nsAString& aFilter,
                        nsINode* aNode,
                        bool* aOutFilterChanged)
{
  bool dummy;
  return CreateDeclaration(aNode,
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
  if (PropertyIsInheritOrInitial(decl, eCSSProperty_filter)) {
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

//
// CanvasImageData
//
static already_AddRefed<ImageData>
CreateImageData(JSContext* aCx, RenderingContext2D* aContext,
                uint32_t aW, uint32_t aH, ErrorResult& aError)
{
  if (aW == 0)
      aW = 1;
  if (aH == 0)
      aH = 1;

  CheckedInt<uint32_t> len = CheckedInt<uint32_t>(aW) * aH * 4;
  if (!len.isValid()) {
    aError.Throw(NS_ERROR_DOM_INDEX_SIZE_ERR);
    return nullptr;
  }

  // Create the fast typed array; it's initialized to 0 by default.
  JSObject* darray = Uint8ClampedArray::Create(aCx, aContext, len.value());
  if (!darray) {
    aError.Throw(NS_ERROR_OUT_OF_MEMORY);
    return nullptr;
  }

  RefPtr<mozilla::dom::ImageData> imageData =
    new mozilla::dom::ImageData(aW, aH, *darray);
  return imageData.forget();
}

already_AddRefed<ImageData>
CanvasRenderingContext2D::CreateImageData(JSContext* aCx, double aSw,
                                          double aSh, ErrorResult& aError)
{
  if (!aSw || !aSh) {
    aError.Throw(NS_ERROR_DOM_INDEX_SIZE_ERR);
    return nullptr;
  }

  int32_t wi = JS::ToInt32(aSw);
  int32_t hi = JS::ToInt32(aSh);

  uint32_t w = Abs(wi);
  uint32_t h = Abs(hi);
  return mozilla::dom::CreateImageData(aCx, this, w, h, aError);
}

already_AddRefed<ImageData>
CanvasRenderingContext2D::CreateImageData(JSContext* aCx,
                                          ImageData& aImagedata,
                                          ErrorResult& aError)
{
  return mozilla::dom::CreateImageData(aCx, this, aImagedata.Width(),
                                       aImagedata.Height(), aError);
}

already_AddRefed<ImageData>
CanvasRenderingContext2D::GetImageData(JSContext* aCx, double aSx,
                                       double aSy, double aSw,
                                       double aSh, ErrorResult& aError)
{
  if (mDrawObserver) {
    mDrawObserver->DidDrawCall(CanvasDrawObserver::DrawCallType::GetImageData);
  }

  if (!mCanvasElement && !mDocShell) {
    NS_ERROR("No canvas element and no docshell in GetImageData!!!");
    aError.Throw(NS_ERROR_DOM_SECURITY_ERR);
    return nullptr;
  }

  // Check only if we have a canvas element; if we were created with a docshell,
  // then it's special internal use.
  if (mCanvasElement && mCanvasElement->IsWriteOnly() &&
      !nsContentUtils::IsCallerChrome())
  {
    // XXX ERRMSG we need to report an error to developers here! (bug 329026)
    aError.Throw(NS_ERROR_DOM_SECURITY_ERR);
    return nullptr;
  }

  if (!IsFinite(aSx) || !IsFinite(aSy) ||
      !IsFinite(aSw) || !IsFinite(aSh)) {
    aError.Throw(NS_ERROR_DOM_NOT_SUPPORTED_ERR);
    return nullptr;
  }

  if (!aSw || !aSh) {
    aError.Throw(NS_ERROR_DOM_INDEX_SIZE_ERR);
    return nullptr;
  }

  int32_t x = JS::ToInt32(aSx);
  int32_t y = JS::ToInt32(aSy);
  int32_t wi = JS::ToInt32(aSw);
  int32_t hi = JS::ToInt32(aSh);

  // Handle negative width and height by flipping the rectangle over in the
  // relevant direction.
  uint32_t w, h;
  if (aSw < 0) {
    w = -wi;
    x -= w;
  } else {
    w = wi;
  }
  if (aSh < 0) {
    h = -hi;
    y -= h;
  } else {
    h = hi;
  }

  if (w == 0) {
    w = 1;
  }
  if (h == 0) {
    h = 1;
  }

  JS::Rooted<JSObject*> array(aCx);
  aError = GetImageDataArray(aCx, x, y, w, h, array.address());
  if (aError.Failed()) {
    return nullptr;
  }
  MOZ_ASSERT(array);

  RefPtr<ImageData> imageData = new ImageData(w, h, *array);
  return imageData.forget();
}

nsresult
CanvasRenderingContext2D::GetImageDataArray(JSContext* aCx,
                                            int32_t aX,
                                            int32_t aY,
                                            uint32_t aWidth,
                                            uint32_t aHeight,
                                            JSObject** aRetval)
{
  if (mDrawObserver) {
    mDrawObserver->DidDrawCall(CanvasDrawObserver::DrawCallType::GetImageData);
  }

  MOZ_ASSERT(aWidth && aHeight);

  CheckedInt<uint32_t> len = CheckedInt<uint32_t>(aWidth) * aHeight * 4;
  if (!len.isValid()) {
    return NS_ERROR_DOM_INDEX_SIZE_ERR;
  }

  CheckedInt<int32_t> rightMost = CheckedInt<int32_t>(aX) + aWidth;
  CheckedInt<int32_t> bottomMost = CheckedInt<int32_t>(aY) + aHeight;

  if (!rightMost.isValid() || !bottomMost.isValid()) {
    return NS_ERROR_DOM_SYNTAX_ERR;
  }

  JS::Rooted<JSObject*> darray(aCx, JS_NewUint8ClampedArray(aCx, len.value()));
  if (!darray) {
    return NS_ERROR_OUT_OF_MEMORY;
  }

  if (mZero) {
    *aRetval = darray;
    return NS_OK;
  }

  IntRect srcRect(0, 0, mWidth, mHeight);
  IntRect destRect(aX, aY, aWidth, aHeight);
  IntRect srcReadRect = srcRect.Intersect(destRect);
  RefPtr<DataSourceSurface> readback;
  DataSourceSurface::MappedSurface rawData;
  if (!srcReadRect.IsEmpty()) {
    RefPtr<SourceSurface> snapshot;
    if (!mTarget && mBufferProvider) {
      snapshot = mBufferProvider->BorrowSnapshot();
    } else {
      EnsureTarget();
      if (!IsTargetValid()) {
        return NS_ERROR_FAILURE;
      }
      snapshot = mTarget->Snapshot();
    }

    if (snapshot) {
      readback = snapshot->GetDataSurface();
    }

    if (!mTarget && mBufferProvider) {
      mBufferProvider->ReturnSnapshot(snapshot.forget());
    }

    if (!readback || !readback->Map(DataSourceSurface::READ, &rawData)) {
      return NS_ERROR_OUT_OF_MEMORY;
    }
  }

  IntRect dstWriteRect = srcReadRect;
  dstWriteRect.MoveBy(-aX, -aY);

  JS::AutoCheckCannotGC nogc;
  bool isShared;
  uint8_t* data = JS_GetUint8ClampedArrayData(darray, &isShared, nogc);
  MOZ_ASSERT(!isShared);        // Should not happen, data was created above

  uint8_t* src;
  uint32_t srcStride;
  if (readback) {
    srcStride = rawData.mStride;
    src = rawData.mData + srcReadRect.y * srcStride + srcReadRect.x * 4;
  } else {
    src = data;
    srcStride = aWidth * 4;
  }

  uint8_t* dst = data + dstWriteRect.y * (aWidth * 4) + dstWriteRect.x * 4;

  if (mOpaque) {
    for (int32_t j = 0; j < dstWriteRect.height; ++j) {
      for (int32_t i = 0; i < dstWriteRect.width; ++i) {
        // XXX Is there some useful swizzle MMX we can use here?
#if MOZ_LITTLE_ENDIAN
        uint8_t b = *src++;
        uint8_t g = *src++;
        uint8_t r = *src++;
        src++;
#else
        src++;
        uint8_t r = *src++;
        uint8_t g = *src++;
        uint8_t b = *src++;
#endif
        *dst++ = r;
        *dst++ = g;
        *dst++ = b;
        *dst++ = 255;
      }
      src += srcStride - (dstWriteRect.width * 4);
      dst += (aWidth * 4) - (dstWriteRect.width * 4);
    }
  } else
  for (int32_t j = 0; j < dstWriteRect.height; ++j) {
    for (int32_t i = 0; i < dstWriteRect.width; ++i) {
      // XXX Is there some useful swizzle MMX we can use here?
#if MOZ_LITTLE_ENDIAN
      uint8_t b = *src++;
      uint8_t g = *src++;
      uint8_t r = *src++;
      uint8_t a = *src++;
#else
      uint8_t a = *src++;
      uint8_t r = *src++;
      uint8_t g = *src++;
      uint8_t b = *src++;
#endif
      // Convert to non-premultiplied color
      *dst++ = gfxUtils::sUnpremultiplyTable[a * 256 + r];
      *dst++ = gfxUtils::sUnpremultiplyTable[a * 256 + g];
      *dst++ = gfxUtils::sUnpremultiplyTable[a * 256 + b];
      *dst++ = a;
    }
    src += srcStride - (dstWriteRect.width * 4);
    dst += (aWidth * 4) - (dstWriteRect.width * 4);
  }

  if (readback) {
    readback->Unmap();
  }

  *aRetval = darray;
  return NS_OK;
}

void
CanvasRenderingContext2D::PutImageData(ImageData& aImageData, double aDx,
                                       double aDy, ErrorResult& aError)
{
  RootedTypedArray<Uint8ClampedArray> arr(RootingCx());
  DebugOnly<bool> inited = arr.Init(aImageData.GetDataObject());
  MOZ_ASSERT(inited);

  aError = PutImageData_explicit(JS::ToInt32(aDx), JS::ToInt32(aDy),
                                aImageData.Width(), aImageData.Height(),
                                &arr, false, 0, 0, 0, 0);
}

void
CanvasRenderingContext2D::PutImageData(ImageData& aImageData, double aDx,
                                       double aDy, double aDirtyX,
                                       double aDirtyY, double aDirtyWidth,
                                       double aDirtyHeight,
                                       ErrorResult& aError)
{
  RootedTypedArray<Uint8ClampedArray> arr(RootingCx());
  DebugOnly<bool> inited = arr.Init(aImageData.GetDataObject());
  MOZ_ASSERT(inited);

  aError = PutImageData_explicit(JS::ToInt32(aDx), JS::ToInt32(aDy),
                                aImageData.Width(), aImageData.Height(),
                                &arr, true,
                                JS::ToInt32(aDirtyX),
                                JS::ToInt32(aDirtyY),
                                JS::ToInt32(aDirtyWidth),
                                JS::ToInt32(aDirtyHeight));
}

nsresult
CanvasRenderingContext2D::PutImageData_explicit(int32_t aX, int32_t aY, uint32_t aW, uint32_t aH,
                                                dom::Uint8ClampedArray* aArray,
                                                bool aHasDirtyRect, int32_t aDirtyX, int32_t aDirtyY,
                                                int32_t aDirtyWidth, int32_t aDirtyHeight)
{
  if (mDrawObserver) {
    mDrawObserver->DidDrawCall(CanvasDrawObserver::DrawCallType::PutImageData);
  }

  if (aW == 0 || aH == 0) {
    return NS_ERROR_DOM_INVALID_STATE_ERR;
  }

  IntRect dirtyRect;
  IntRect imageDataRect(0, 0, aW, aH);

  if (aHasDirtyRect) {
    // fix up negative dimensions
    if (aDirtyWidth < 0) {
      NS_ENSURE_TRUE(aDirtyWidth != INT_MIN, NS_ERROR_DOM_INDEX_SIZE_ERR);

      CheckedInt32 checkedDirtyX = CheckedInt32(aDirtyX) + aDirtyWidth;

      if (!checkedDirtyX.isValid())
        return NS_ERROR_DOM_INDEX_SIZE_ERR;

      aDirtyX = checkedDirtyX.value();
      aDirtyWidth = -aDirtyWidth;
    }

    if (aDirtyHeight < 0) {
      NS_ENSURE_TRUE(aDirtyHeight != INT_MIN, NS_ERROR_DOM_INDEX_SIZE_ERR);

      CheckedInt32 checkedDirtyY = CheckedInt32(aDirtyY) + aDirtyHeight;

      if (!checkedDirtyY.isValid())
        return NS_ERROR_DOM_INDEX_SIZE_ERR;

      aDirtyY = checkedDirtyY.value();
      aDirtyHeight = -aDirtyHeight;
    }

    // bound the dirty rect within the imageData rectangle
    dirtyRect = imageDataRect.Intersect(IntRect(aDirtyX, aDirtyY, aDirtyWidth, aDirtyHeight));

    if (dirtyRect.Width() <= 0 || dirtyRect.Height() <= 0)
      return NS_OK;
  } else {
    dirtyRect = imageDataRect;
  }

  dirtyRect.MoveBy(IntPoint(aX, aY));
  dirtyRect = IntRect(0, 0, mWidth, mHeight).Intersect(dirtyRect);

  if (dirtyRect.Width() <= 0 || dirtyRect.Height() <= 0) {
    return NS_OK;
  }

  aArray->ComputeLengthAndData();

  uint32_t dataLen = aArray->Length();

  uint32_t len = aW * aH * 4;
  if (dataLen != len) {
    return NS_ERROR_DOM_INVALID_STATE_ERR;
  }

  uint32_t copyWidth = dirtyRect.Width();
  uint32_t copyHeight = dirtyRect.Height();
  RefPtr<gfxImageSurface> imgsurf = new gfxImageSurface(gfx::IntSize(copyWidth, copyHeight),
                                                          SurfaceFormat::A8R8G8B8_UINT32,
                                                          false);
  if (!imgsurf || imgsurf->CairoStatus()) {
    return NS_ERROR_FAILURE;
  }

  uint32_t copyX = dirtyRect.x - aX;
  uint32_t copyY = dirtyRect.y - aY;
  //uint8_t *src = aArray->Data();
  uint8_t *dst = imgsurf->Data();
  uint8_t* srcLine = aArray->Data() + copyY * (aW * 4) + copyX * 4;
  // For opaque canvases, we must still premultiply the RGB components, but write the alpha as opaque.
  uint8_t alphaMask = mOpaque ? 255 : 0;
#if 0
  printf("PutImageData_explicit: dirty x=%d y=%d w=%d h=%d copy x=%d y=%d w=%d h=%d ext x=%d y=%d w=%d h=%d\n",
       dirtyRect.x, dirtyRect.y, copyWidth, copyHeight,
       copyX, copyY, copyWidth, copyHeight,
       x, y, w, h);
#endif
  for (uint32_t j = 0; j < copyHeight; j++) {
    uint8_t *src = srcLine;
    for (uint32_t i = 0; i < copyWidth; i++) {
      uint8_t r = *src++;
      uint8_t g = *src++;
      uint8_t b = *src++;
      uint8_t a = *src++;

      // Convert to premultiplied color (losslessly if the input came from getImageData)
#if MOZ_LITTLE_ENDIAN
      *dst++ = gfxUtils::sPremultiplyTable[a * 256 + b];
      *dst++ = gfxUtils::sPremultiplyTable[a * 256 + g];
      *dst++ = gfxUtils::sPremultiplyTable[a * 256 + r];
      *dst++ = a | alphaMask;
#else
      *dst++ = a | alphaMask;
      *dst++ = gfxUtils::sPremultiplyTable[a * 256 + r];
      *dst++ = gfxUtils::sPremultiplyTable[a * 256 + g];
      *dst++ = gfxUtils::sPremultiplyTable[a * 256 + b];
#endif
    }
    srcLine += aW * 4;
  }

  // The canvas spec says that the current path, transformation matrix, shadow attributes,
  // global alpha, the clipping region, and global composition operator must not affect the
  // getImageData() and putImageData() methods.
  const gfx::Rect putRect(dirtyRect);
  EnsureTarget(&putRect);

  if (!IsTargetValid()) {
    return NS_ERROR_FAILURE;
  }

  RefPtr<SourceSurface> sourceSurface =
    mTarget->CreateSourceSurfaceFromData(imgsurf->Data(), IntSize(copyWidth, copyHeight), imgsurf->Stride(), SurfaceFormat::B8G8R8A8);

  // In certain scenarios, requesting larger than 8k image fails.  Bug 803568
  // covers the details of how to run into it, but the full detailed
  // investigation hasn't been done to determine the underlying cause.  We
  // will just handle the failure to allocate the surface to avoid a crash.
  if (!sourceSurface) {
    return NS_ERROR_FAILURE;
  }

  mTarget->CopySurface(sourceSurface,
                       IntRect(0, 0,
                               dirtyRect.width, dirtyRect.height),
                       IntPoint(dirtyRect.x, dirtyRect.y));

  Redraw(gfx::Rect(dirtyRect.x, dirtyRect.y, dirtyRect.width, dirtyRect.height));

  return NS_OK;
}

//
// CanvasTextDrawingStyles
//
void
CanvasRenderingContext2D::SetFont(const nsAString& aFont,
                                  ErrorResult& aError)
{
  SetFontInternal(aFont, aError);
}

bool
CanvasRenderingContext2D::SetFontInternal(const nsAString& aFont,
                                          ErrorResult& aError)
{
  /*
    * If font is defined with relative units (e.g. ems) and the parent
    * style context changes in between calls, setting the font to the
    * same value as previous could result in a different computed value,
    * so we cannot have the optimization where we check if the new font
    * string is equal to the old one.
    */

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
  RefPtr<nsStyleContext> sc =
    GetFontStyleContext(mCanvasElement, aFont, presShell, usedFont, aError);
  if (!sc) {
    return false;
  }

  const nsStyleFont* fontStyle = sc->StyleFont();

  nsPresContext *c = presShell->GetPresContext();

  // Purposely ignore the font size that respects the user's minimum
  // font preference (fontStyle->mFont.size) in favor of the computed
  // size (fontStyle->mSize).  See
  // https://bugzilla.mozilla.org/show_bug.cgi?id=698652.
  MOZ_ASSERT(!fontStyle->mAllowZoom,
             "expected text zoom to be disabled on this nsStyleFont");
  nsFont resizedFont(fontStyle->mFont);
  // Create a font group working in units of CSS pixels instead of the usual
  // device pixels, to avoid being affected by page zoom. nsFontMetrics will
  // convert nsFont size in app units to device pixels for the font group, so
  // here we first apply to the size the equivalent of a conversion from device
  // pixels to CSS pixels, to adjust for the difference in expectations from
  // other nsFontMetrics clients.
  resizedFont.size =
    (fontStyle->mSize * c->AppUnitsPerDevPixel()) / c->AppUnitsPerCSSPixel();

  nsFontMetrics::Params params;
  params.language = fontStyle->mLanguage;
  params.explicitLanguage = fontStyle->mExplicitLanguage;
  params.userFontSet = c->GetUserFontSet();
  params.textPerf = c->GetTextPerfMetrics();
  RefPtr<nsFontMetrics> metrics =
    c->DeviceContext()->GetMetricsFor(resizedFont, params);

  gfxFontGroup* newFontGroup = metrics->GetThebesFontGroup();
  CurrentState().fontGroup = newFontGroup;
  NS_ASSERTION(CurrentState().fontGroup, "Could not get font group");
  CurrentState().font = usedFont;
  CurrentState().fontFont = fontStyle->mFont;
  CurrentState().fontFont.size = fontStyle->mSize;
  CurrentState().fontLanguage = fontStyle->mLanguage;
  CurrentState().fontExplicitLanguage = fontStyle->mExplicitLanguage;

  return true;
}

void
CanvasRenderingContext2D::SetTextAlign(const nsAString& aTextAlign)
{
  if (aTextAlign.EqualsLiteral("start"))
    CurrentState().textAlign = TextAlign::START;
  else if (aTextAlign.EqualsLiteral("end"))
    CurrentState().textAlign = TextAlign::END;
  else if (aTextAlign.EqualsLiteral("left"))
    CurrentState().textAlign = TextAlign::LEFT;
  else if (aTextAlign.EqualsLiteral("right"))
    CurrentState().textAlign = TextAlign::RIGHT;
  else if (aTextAlign.EqualsLiteral("center"))
    CurrentState().textAlign = TextAlign::CENTER;
}

void
CanvasRenderingContext2D::GetTextAlign(nsAString& aTextAlign)
{
  switch (CurrentState().textAlign)
  {
  case TextAlign::START:
    aTextAlign.AssignLiteral("start");
    break;
  case TextAlign::END:
    aTextAlign.AssignLiteral("end");
    break;
  case TextAlign::LEFT:
    aTextAlign.AssignLiteral("left");
    break;
  case TextAlign::RIGHT:
    aTextAlign.AssignLiteral("right");
    break;
  case TextAlign::CENTER:
    aTextAlign.AssignLiteral("center");
    break;
  }
}

void
CanvasRenderingContext2D::SetTextBaseline(const nsAString& aTextBaseline)
{
  if (aTextBaseline.EqualsLiteral("top"))
    CurrentState().textBaseline = TextBaseline::TOP;
  else if (aTextBaseline.EqualsLiteral("hanging"))
    CurrentState().textBaseline = TextBaseline::HANGING;
  else if (aTextBaseline.EqualsLiteral("middle"))
    CurrentState().textBaseline = TextBaseline::MIDDLE;
  else if (aTextBaseline.EqualsLiteral("alphabetic"))
    CurrentState().textBaseline = TextBaseline::ALPHABETIC;
  else if (aTextBaseline.EqualsLiteral("ideographic"))
    CurrentState().textBaseline = TextBaseline::IDEOGRAPHIC;
  else if (aTextBaseline.EqualsLiteral("bottom"))
    CurrentState().textBaseline = TextBaseline::BOTTOM;
}

void
CanvasRenderingContext2D::GetTextBaseline(nsAString& aTextBaseline)
{
  switch (CurrentState().textBaseline)
  {
  case TextBaseline::TOP:
    aTextBaseline.AssignLiteral("top");
    break;
  case TextBaseline::HANGING:
    aTextBaseline.AssignLiteral("hanging");
    break;
  case TextBaseline::MIDDLE:
    aTextBaseline.AssignLiteral("middle");
    break;
  case TextBaseline::ALPHABETIC:
    aTextBaseline.AssignLiteral("alphabetic");
    break;
  case TextBaseline::IDEOGRAPHIC:
    aTextBaseline.AssignLiteral("ideographic");
    break;
  case TextBaseline::BOTTOM:
    aTextBaseline.AssignLiteral("bottom");
    break;
  }
}

//
// CanvasText
//
/**
 * Used for nsBidiPresUtils::ProcessText
 */
struct MOZ_STACK_CLASS CanvasBidiProcessor : public nsBidiPresUtils::BidiProcessor
{

  CanvasBidiProcessor()
    : nsBidiPresUtils::BidiProcessor()
  {
    if (Preferences::GetBool(GFX_MISSING_FONTS_NOTIFY_PREF)) {
      mMissingFonts = new gfxMissingFontRecorder();
    }
  }

  ~CanvasBidiProcessor()
  {
    // notify front-end code if we encountered missing glyphs in any script
    if (mMissingFonts) {
      mMissingFonts->Flush();
    }
  }

  virtual void SetText(const char16_t* aText, int32_t aLength, nsBidiDirection aDirection)
  {
    mFontgrp->UpdateUserFonts(); // ensure user font generation is current
    // adjust flags for current direction run
    uint32_t flags = mTextRunFlags;
    if (aDirection == NSBIDI_RTL) {
      flags |= gfxTextRunFactory::TEXT_IS_RTL;
    } else {
      flags &= ~gfxTextRunFactory::TEXT_IS_RTL;
    }
    mTextRun = mFontgrp->MakeTextRun(aText,
                                     aLength,
                                     mDrawTarget,
                                     mAppUnitsPerDevPixel,
                                     flags,
                                     mMissingFonts);
  }

  virtual nscoord GetWidth()
  {
    gfxTextRun::Metrics textRunMetrics = mTextRun->MeasureText(
        mDoMeasureBoundingBox ? gfxFont::TIGHT_INK_EXTENTS
                              : gfxFont::LOOSE_INK_EXTENTS, mDrawTarget);

    // this only measures the height; the total width is gotten from the
    // the return value of ProcessText.
    if (mDoMeasureBoundingBox) {
      textRunMetrics.mBoundingBox.Scale(1.0 / mAppUnitsPerDevPixel);
      mBoundingBox = mBoundingBox.Union(textRunMetrics.mBoundingBox);
    }

    return NSToCoordRound(textRunMetrics.mAdvanceWidth);
  }

  already_AddRefed<gfxPattern> GetGradientFor(Style aStyle)
  {
    RefPtr<gfxPattern> pattern;
    CanvasGradient* gradient = mState->gradientStyles[aStyle];
    CanvasGradient::Type type = gradient->GetType();

    switch (type) {
    case CanvasGradient::Type::RADIAL: {
      CanvasRadialGradient* radial =
        static_cast<CanvasRadialGradient*>(gradient);
      pattern = new gfxPattern(radial->mCenter1.x, radial->mCenter1.y,
                               radial->mRadius1, radial->mCenter2.x,
                               radial->mCenter2.y, radial->mRadius2);
      break;
    }
    case CanvasGradient::Type::LINEAR: {
      CanvasLinearGradient* linear =
        static_cast<CanvasLinearGradient*>(gradient);
      pattern = new gfxPattern(linear->mBegin.x, linear->mBegin.y,
                               linear->mEnd.x, linear->mEnd.y);
      break;
    }
    default:
      MOZ_ASSERT(false, "Should be linear or radial gradient.");
      return nullptr;
    }

    for (auto stop : gradient->mRawStops) {
      pattern->AddColorStop(stop.offset, stop.color);
    }

    return pattern.forget();
  }

  gfx::ExtendMode CvtCanvasRepeatToGfxRepeat(
    CanvasPattern::RepeatMode aRepeatMode)
  {
    switch (aRepeatMode) {
    case CanvasPattern::RepeatMode::REPEAT:
      return gfx::ExtendMode::REPEAT;
    case CanvasPattern::RepeatMode::REPEATX:
      return gfx::ExtendMode::REPEAT_X;
    case CanvasPattern::RepeatMode::REPEATY:
      return gfx::ExtendMode::REPEAT_Y;
    case CanvasPattern::RepeatMode::NOREPEAT:
      return gfx::ExtendMode::CLAMP;
    default:
      return gfx::ExtendMode::CLAMP;
    }
  }

  already_AddRefed<gfxPattern> GetPatternFor(Style aStyle)
  {
    const CanvasPattern* pat = mState->patternStyles[aStyle];
    RefPtr<gfxPattern> pattern = new gfxPattern(pat->mSurface, Matrix());
    pattern->SetExtend(CvtCanvasRepeatToGfxRepeat(pat->mRepeat));
    return pattern.forget();
  }

  virtual void DrawText(nscoord aXOffset, nscoord aWidth)
  {
    gfxPoint point = mPt;
    bool rtl = mTextRun->IsRightToLeft();
    bool verticalRun = mTextRun->IsVertical();
    RefPtr<gfxPattern> pattern;

    gfxFloat& inlineCoord = verticalRun ? point.y : point.x;
    inlineCoord += aXOffset;

    // offset is given in terms of left side of string
    if (rtl) {
      // Bug 581092 - don't use rounded pixel width to advance to
      // right-hand end of run, because this will cause different
      // glyph positioning for LTR vs RTL drawing of the same
      // glyph string on OS X and DWrite where textrun widths may
      // involve fractional pixels.
      gfxTextRun::Metrics textRunMetrics =
        mTextRun->MeasureText(mDoMeasureBoundingBox ?
                                gfxFont::TIGHT_INK_EXTENTS :
                                gfxFont::LOOSE_INK_EXTENTS,
                              mDrawTarget);
      inlineCoord += textRunMetrics.mAdvanceWidth;
      // old code was:
      //   point.x += width * mAppUnitsPerDevPixel;
      // TODO: restore this if/when we move to fractional coords
      // throughout the text layout process
    }

    mCtx->EnsureTarget();

    // Defer the tasks to gfxTextRun which will handle color/svg-in-ot fonts
    // appropriately.
    StrokeOptions strokeOpts;
    DrawOptions drawOpts;
    Style style = (mOp == TextDrawOperation::FILL)
                    ? Style::FILL
                    : Style::STROKE;
    //AdjustedTarget target(mCtx);
    RefPtr<gfxContext> thebes =
      gfxContext::CreatePreservingTransformOrNull(mCtx->mTarget);
    if (!thebes) {
      // If CreatePreservingTransformOrNull returns null, it will also have
      // issued a gfxCriticalNote already, so here we'll just bail out.
      return;
    }
    gfxTextRun::DrawParams params(thebes);

    if (mState->StyleIsColor(style)) { // Color
      nscolor fontColor = mState->colorStyles[style];
      if (style == Style::FILL) {
        params.context->SetColor(Color::FromABGR(fontColor));
      } else {
        params.textStrokeColor = fontColor;
      }
    } else {
      if (mState->gradientStyles[style]) { // Gradient
        pattern = GetGradientFor(style);
      } else if (mState->patternStyles[style]) { // Pattern
        pattern = GetPatternFor(style);
      } else {
        MOZ_ASSERT(false, "Should never reach here.");
        return;
      }
      MOZ_ASSERT(pattern, "No valid pattern.");

      if (style == Style::FILL) {
        params.context->SetPattern(pattern);
      } else {
        params.textStrokePattern = pattern;
      }
    }

    const ContextState& state = *mState;
    drawOpts.mAlpha = state.globalAlpha;
    drawOpts.mCompositionOp = mCtx->UsedOperation();
    params.drawOpts = &drawOpts;

    if (style == Style::STROKE) {
      strokeOpts.mLineWidth = state.lineWidth;
      strokeOpts.mLineJoin = state.lineJoin;
      strokeOpts.mLineCap = state.lineCap;
      strokeOpts.mMiterLimit = state.miterLimit;
      strokeOpts.mDashLength = state.dash.Length();
      strokeOpts.mDashPattern =
        (strokeOpts.mDashLength > 0) ? state.dash.Elements() : 0;
      strokeOpts.mDashOffset = state.dashOffset;

      params.drawMode = DrawMode::GLYPH_STROKE;
      params.strokeOpts = &strokeOpts;
    }

    mTextRun->Draw(gfxTextRun::Range(mTextRun.get()), point, params);
  }

  // current text run
  RefPtr<gfxTextRun> mTextRun;

  // pointer to a screen reference context used to measure text and such
  RefPtr<DrawTarget> mDrawTarget;

  // Pointer to the draw target we should fill our text to
  RenderingContext2D *mCtx;

  // position of the left side of the string, alphabetic baseline
  gfxPoint mPt;

  // current font
  gfxFontGroup* mFontgrp;

  // to record any unsupported characters found in the text,
  // and notify front-end if it is interested
  nsAutoPtr<gfxMissingFontRecorder> mMissingFonts;

  // dev pixel conversion factor
  int32_t mAppUnitsPerDevPixel;

  // operation (fill or stroke)
  TextDrawOperation mOp;

  // context state
  ContextState *mState;

  // union of bounding boxes of all runs, needed for shadows
  gfxRect mBoundingBox;

  // flags to use when creating textrun, based on CSS style
  uint32_t mTextRunFlags;

  // true iff the bounding box should be measured
  bool mDoMeasureBoundingBox;
};

void
CanvasRenderingContext2D::FillText(const nsAString& aText, double aX,
                                   double aY,
                                   const Optional<double>& aMaxWidth,
                                   ErrorResult& aError)
{
  aError = DrawOrMeasureText(aText, aX, aY, aMaxWidth, TextDrawOperation::FILL, nullptr);
}

void
CanvasRenderingContext2D::StrokeText(const nsAString& aText, double aX,
                                     double aY,
                                     const Optional<double>& aMaxWidth,
                                     ErrorResult& aError)
{
  aError = DrawOrMeasureText(aText, aX, aY, aMaxWidth, TextDrawOperation::STROKE, nullptr);
}

TextMetrics*
CanvasRenderingContext2D::MeasureText(const nsAString& aRawText,
                                      ErrorResult& aError)
{
  float width;
  Optional<double> maxWidth;
  aError = DrawOrMeasureText(aRawText, 0, 0, maxWidth, TextDrawOperation::MEASURE, &width);
  if (aError.Failed()) {
    return nullptr;
  }

  return new TextMetrics(width);
}

/*
 * Helper function that replaces the whitespace characters in a string
 * with U+0020 SPACE. The whitespace characters are defined as U+0020 SPACE,
 * U+0009 CHARACTER TABULATION (tab), U+000A LINE FEED (LF), U+000B LINE
 * TABULATION, U+000C FORM FEED (FF), and U+000D CARRIAGE RETURN (CR).
 * @param str The string whose whitespace characters to replace.
 */
static inline void
TextReplaceWhitespaceCharacters(nsAutoString& aStr)
{
  aStr.ReplaceChar("\x09\x0A\x0B\x0C\x0D", char16_t(' '));
}

gfxFontGroup *CanvasRenderingContext2D::GetCurrentFontStyle()
{
  // use lazy initilization for the font group since it's rather expensive
  if (!CurrentState().fontGroup) {
    ErrorResult err;
    NS_NAMED_LITERAL_STRING(kDefaultFontStyle, "10px sans-serif");
    static float kDefaultFontSize = 10.0;
    nsCOMPtr<nsIPresShell> presShell = GetPresShell();
    bool fontUpdated = SetFontInternal(kDefaultFontStyle, err);
    if (err.Failed() || !fontUpdated) {
      err.SuppressException();
      gfxFontStyle style;
      style.size = kDefaultFontSize;
      gfxTextPerfMetrics* tp = nullptr;
      if (presShell && !presShell->IsDestroying()) {
        tp = presShell->GetPresContext()->GetTextPerfMetrics();
      }
      int32_t perDevPixel, perCSSPixel;
      GetAppUnitsValues(&perDevPixel, &perCSSPixel);
      gfxFloat devToCssSize = gfxFloat(perDevPixel) / gfxFloat(perCSSPixel);
      CurrentState().fontGroup =
        gfxPlatform::GetPlatform()->CreateFontGroup(FontFamilyList(eFamily_sans_serif),
                                                    &style, tp,
                                                    nullptr, devToCssSize);
      if (CurrentState().fontGroup) {
        CurrentState().font = kDefaultFontStyle;
      } else {
        NS_ERROR("Default canvas font is invalid");
      }
    }
  }

  return CurrentState().fontGroup;
}

nsresult
CanvasRenderingContext2D::DrawOrMeasureText(const nsAString& aRawText,
                                            float aX,
                                            float aY,
                                            const Optional<double>& aMaxWidth,
                                            TextDrawOperation aOp,
                                            float* aWidth)
{
  nsresult rv;

  if (!mCanvasElement && !mDocShell) {
    NS_WARNING("Canvas element must be non-null or a docshell must be provided");
    return NS_ERROR_FAILURE;
  }

  nsCOMPtr<nsIPresShell> presShell = GetPresShell();
  if (!presShell)
    return NS_ERROR_FAILURE;

  nsIDocument* document = presShell->GetDocument();

  // replace all the whitespace characters with U+0020 SPACE
  nsAutoString textToDraw(aRawText);
  TextReplaceWhitespaceCharacters(textToDraw);

  // According to spec, the API should return an empty array if maxWidth was provided
  // but is less than or equal to zero or equal to NaN.
  if (aMaxWidth.WasPassed() && (aMaxWidth.Value() <= 0 || IsNaN(aMaxWidth.Value()))) {
    textToDraw.Truncate();
  }

  // for now, default to ltr if not in doc
  bool isRTL = false;

  RefPtr<nsStyleContext> canvasStyle;
  if (mCanvasElement && mCanvasElement->IsInUncomposedDoc()) {
    // try to find the closest context
    canvasStyle =
      nsComputedDOMStyle::GetStyleContextForElement(mCanvasElement,
                                                    nullptr,
                                                    presShell);
    if (!canvasStyle) {
      return NS_ERROR_FAILURE;
    }

    isRTL = canvasStyle->StyleVisibility()->mDirection ==
      NS_STYLE_DIRECTION_RTL;
  } else {
    isRTL = GET_BIDI_OPTION_DIRECTION(document->GetBidiOptions()) == IBMBIDI_TEXTDIRECTION_RTL;
  }

  gfxFontGroup* currentFontStyle = GetCurrentFontStyle();
  if (!currentFontStyle) {
    return NS_ERROR_FAILURE;
  }

  MOZ_ASSERT(!presShell->IsDestroying(),
             "GetCurrentFontStyle() should have returned null if the presshell is being destroyed");

  // ensure user font set is up to date
  currentFontStyle->
    SetUserFontSet(presShell->GetPresContext()->GetUserFontSet());

  if (currentFontStyle->GetStyle()->size == 0.0F) {
    if (aWidth) {
      *aWidth = 0;
    }
    return NS_OK;
  }

  if (!IsFinite(aX) || !IsFinite(aY)) {
    return NS_OK;
  }

  const ContextState &state = CurrentState();

  // This is only needed to know if we can know the drawing bounding box easily.
  bool doCalculateBounds = NeedToCalculateBounds();

  CanvasBidiProcessor processor;

  // If we don't have a style context, we can't set up vertical-text flags
  // (for now, at least; perhaps we need new Canvas API to control this).
  processor.mTextRunFlags = canvasStyle ?
    nsLayoutUtils::GetTextRunFlagsForStyle(canvasStyle,
                                           canvasStyle->StyleFont(),
                                           canvasStyle->StyleText(),
                                           0) : 0;

  GetAppUnitsValues(&processor.mAppUnitsPerDevPixel, nullptr);
  processor.mPt = gfxPoint(aX, aY);
  processor.mDrawTarget =
    gfxPlatform::GetPlatform()->ScreenReferenceDrawTarget();

  // If we don't have a target then we don't have a transform. A target won't
  // be needed in the case where we're measuring the text size. This allows
  // to avoid creating a target if it's only being used to measure text sizes.
  if (mTarget) {
    processor.mDrawTarget->SetTransform(mTarget->GetTransform());
  }
  processor.mCtx = this;
  processor.mOp = aOp;
  processor.mBoundingBox = gfxRect(0, 0, 0, 0);
  processor.mDoMeasureBoundingBox = doCalculateBounds || !mIsEntireFrameInvalid;
  processor.mState = &CurrentState();
  processor.mFontgrp = currentFontStyle;

  nscoord totalWidthCoord;

  // calls bidi algo twice since it needs the full text width and the
  // bounding boxes before rendering anything
  nsBidi bidiEngine;
  rv = nsBidiPresUtils::ProcessText(textToDraw.get(),
                                    textToDraw.Length(),
                                    isRTL ? NSBIDI_RTL : NSBIDI_LTR,
                                    presShell->GetPresContext(),
                                    processor,
                                    nsBidiPresUtils::MODE_MEASURE,
                                    nullptr,
                                    0,
                                    &totalWidthCoord,
                                    &bidiEngine);
  if (NS_FAILED(rv)) {
    return rv;
  }

  float totalWidth = float(totalWidthCoord) / processor.mAppUnitsPerDevPixel;
  if (aWidth) {
    *aWidth = totalWidth;
  }

  // if only measuring, don't need to do any more work
  if (aOp==TextDrawOperation::MEASURE) {
    return NS_OK;
  }

  // offset pt.x based on text align
  gfxFloat anchorX;

  if (state.textAlign == TextAlign::CENTER) {
    anchorX = .5;
  } else if (state.textAlign == TextAlign::LEFT ||
            (!isRTL && state.textAlign == TextAlign::START) ||
            (isRTL && state.textAlign == TextAlign::END)) {
    anchorX = 0;
  } else {
    anchorX = 1;
  }

  processor.mPt.x -= anchorX * totalWidth;

  // offset pt.y (or pt.x, for vertical text) based on text baseline
  processor.mFontgrp->UpdateUserFonts(); // ensure user font generation is current
  const gfxFont::Metrics& fontMetrics =
    processor.mFontgrp->GetFirstValidFont()->GetMetrics(gfxFont::eHorizontal);

  gfxFloat baselineAnchor;

  switch (state.textBaseline)
  {
  case TextBaseline::HANGING:
      // fall through; best we can do with the information available
  case TextBaseline::TOP:
    baselineAnchor = fontMetrics.emAscent;
    break;
  case TextBaseline::MIDDLE:
    baselineAnchor = (fontMetrics.emAscent - fontMetrics.emDescent) * .5f;
    break;
  case TextBaseline::IDEOGRAPHIC:
    // fall through; best we can do with the information available
  case TextBaseline::ALPHABETIC:
    baselineAnchor = 0;
    break;
  case TextBaseline::BOTTOM:
    baselineAnchor = -fontMetrics.emDescent;
    break;
  default:
    MOZ_CRASH("GFX: unexpected TextBaseline");
  }

  // We can't query the textRun directly, as it may not have been created yet;
  // so instead we check the flags that will be used to initialize it.
  uint16_t runOrientation =
    (processor.mTextRunFlags & gfxTextRunFactory::TEXT_ORIENT_MASK);
  if (runOrientation != gfxTextRunFactory::TEXT_ORIENT_HORIZONTAL) {
    if (runOrientation == gfxTextRunFactory::TEXT_ORIENT_VERTICAL_MIXED ||
        runOrientation == gfxTextRunFactory::TEXT_ORIENT_VERTICAL_UPRIGHT) {
      // Adjust to account for mTextRun being shaped using center baseline
      // rather than alphabetic.
      baselineAnchor -= (fontMetrics.emAscent - fontMetrics.emDescent) * .5f;
    }
    processor.mPt.x -= baselineAnchor;
  } else {
    processor.mPt.y += baselineAnchor;
  }

  // correct bounding box to get it to be the correct size/position
  processor.mBoundingBox.width = totalWidth;
  processor.mBoundingBox.MoveBy(processor.mPt);

  processor.mPt.x *= processor.mAppUnitsPerDevPixel;
  processor.mPt.y *= processor.mAppUnitsPerDevPixel;

  EnsureTarget();
  Matrix oldTransform = mTarget->GetTransform();
  // if text is over aMaxWidth, then scale the text horizontally such that its
  // width is precisely aMaxWidth
  if (aMaxWidth.WasPassed() && aMaxWidth.Value() > 0 &&
      totalWidth > aMaxWidth.Value()) {
    Matrix newTransform = oldTransform;

    // Translate so that the anchor point is at 0,0, then scale and then
    // translate back.
    newTransform.PreTranslate(aX, 0);
    newTransform.PreScale(aMaxWidth.Value() / totalWidth, 1);
    newTransform.PreTranslate(-aX, 0);
    /* we do this to avoid an ICE in the android compiler */
    Matrix androidCompilerBug = newTransform;
    mTarget->SetTransform(androidCompilerBug);
  }

  // save the previous bounding box
  gfxRect boundingBox = processor.mBoundingBox;

  // don't ever need to measure the bounding box twice
  processor.mDoMeasureBoundingBox = false;

  rv = nsBidiPresUtils::ProcessText(textToDraw.get(),
                                    textToDraw.Length(),
                                    isRTL ? NSBIDI_RTL : NSBIDI_LTR,
                                    presShell->GetPresContext(),
                                    processor,
                                    nsBidiPresUtils::MODE_DRAW,
                                    nullptr,
                                    0,
                                    nullptr,
                                    &bidiEngine);


  mTarget->SetTransform(oldTransform);

  if (aOp == TextDrawOperation::FILL &&
      !doCalculateBounds) {
    RedrawUser(boundingBox);
    return NS_OK;
  }

  Redraw();
  return NS_OK;
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
