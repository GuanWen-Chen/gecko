#include "CanvasRenderingContext2D.h"

#include "gfxImageSurface.h"
#include "gfxUtils.h"
#include "mozilla/dom/ImageData.h"
#include "mozilla/layers/PersistentBufferProvider.h"
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
