/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CanvasPattern.h"

#include "gfx2DGlue.h"
#include "mozilla/dom/SVGMatrix.h"

namespace mozilla {
namespace dom {

using mozilla::gfx::ToMatrix;

void
CanvasPattern::SetTransform(SVGMatrix& aMatrix)
{
  mTransform = ToMatrix(aMatrix.GetMatrix());
}

} // namespace dom
} // namespace mozilla