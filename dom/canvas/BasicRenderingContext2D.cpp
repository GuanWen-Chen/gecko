/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/BasicRenderingContext2D.h"

namespace mozilla {
namespace dom{

// Cap sigma to avoid overly large temp surfaces.
const mozilla::gfx::Float SIGMA_MAX = 100;

const size_t MAX_STYLE_STACK_SIZE = 1024;

/**
 ** BasicRenderingContext2D impl
 **/

//
// state
//
void
BasicRenderingContext2D::Save()
{
  EnsureTarget();
  mStyleStack[mStyleStack.Length() - 1].transform = mTarget->GetTransform();
  mStyleStack.SetCapacity(mStyleStack.Length() + 1);
  mStyleStack.AppendElement(CurrentState());

  if (mStyleStack.Length() > MAX_STYLE_STACK_SIZE) {
    // This is not fast, but is better than OOMing and shouldn't be hit by
    // reasonable code.
    mStyleStack.RemoveElementAt(0);
  }
}

void
BasicRenderingContext2D::Restore()
{
  if (mStyleStack.Length() - 1 == 0)
    return;

  TransformWillUpdate();

  for (const auto& clipOrTransform : CurrentState().clipsAndTransforms) {
    if (clipOrTransform.IsClip()) {
      mTarget->PopClip();
    }
  }

  mStyleStack.RemoveElementAt(mStyleStack.Length() - 1);

  mTarget->SetTransform(CurrentState().transform);
}

//
// transformations
//

void
BasicRenderingContext2D::Scale(double aX, double aY, ErrorResult& aError)
{
  TransformWillUpdate();
  if (!IsTargetValid()) {
    aError.Throw(NS_ERROR_FAILURE);
    return;
  }

  Matrix newMatrix = mTarget->GetTransform();
  newMatrix.PreScale(aX, aY);

  SetTransformInternal(newMatrix);
}

void
BasicRenderingContext2D::Rotate(double aAngle, ErrorResult& aError)
{
  TransformWillUpdate();
  if (!IsTargetValid()) {
    aError.Throw(NS_ERROR_FAILURE);
    return;
  }

  Matrix newMatrix = Matrix::Rotation(aAngle) * mTarget->GetTransform();

  SetTransformInternal(newMatrix);
}

void
BasicRenderingContext2D::Translate(double aX, double aY, ErrorResult& aError)
{
  TransformWillUpdate();
  if (!IsTargetValid()) {
    aError.Throw(NS_ERROR_FAILURE);
    return;
  }

  Matrix newMatrix = mTarget->GetTransform();
  newMatrix.PreTranslate(aX, aY);

  SetTransformInternal(newMatrix);
}

void
BasicRenderingContext2D::Transform(double aM11, double aM12, double aM21,
                                   double aM22, double aDx, double aDy,
                                   ErrorResult& aError)
{
  TransformWillUpdate();
  if (!IsTargetValid()) {
    aError.Throw(NS_ERROR_FAILURE);
    return;
  }

  Matrix newMatrix(aM11, aM12, aM21, aM22, aDx, aDy);
  newMatrix *= mTarget->GetTransform();

  SetTransformInternal(newMatrix);
}

void
BasicRenderingContext2D::SetTransform(double aM11, double aM12,
                                      double aM21, double aM22,
                                      double aDx, double aDy,
                                      ErrorResult& aError)
{
  TransformWillUpdate();
  if (!IsTargetValid()) {
    aError.Throw(NS_ERROR_FAILURE);
    return;
  }

  SetTransformInternal(Matrix(aM11, aM12, aM21, aM22, aDx, aDy));
}

void
BasicRenderingContext2D::SetTransformInternal(const Matrix& aTransform)
{
  if (!aTransform.IsFinite()) {
    return;
  }

  // Save the transform in the clip stack to be able to replay clips properly.
  auto& clipsAndTransforms = CurrentState().clipsAndTransforms;
  if (clipsAndTransforms.IsEmpty() || clipsAndTransforms.LastElement().IsClip()) {
    clipsAndTransforms.AppendElement(ClipState(aTransform));
  } else {
    // If the last item is a transform we can replace it instead of appending
    // a new item.
    clipsAndTransforms.LastElement().transform = aTransform;
  }
  mTarget->SetTransform(aTransform);
}

void
BasicRenderingContext2D::ResetTransform(ErrorResult& aError)
{
  SetTransform(1.0, 0.0, 0.0, 1.0, 0.0, 0.0, aError);
}

//
// compositing
//

void
BasicRenderingContext2D::SetGlobalCompositeOperation(const nsAString& aOp,
                                                     ErrorResult& aError)
{
  CompositionOp comp_op;

#define CANVAS_OP_TO_GFX_OP(cvsop, op2d) \
  if (aOp.EqualsLiteral(cvsop))   \
    comp_op = CompositionOp::OP_##op2d;

  CANVAS_OP_TO_GFX_OP("copy", SOURCE)
  else CANVAS_OP_TO_GFX_OP("source-atop", ATOP)
  else CANVAS_OP_TO_GFX_OP("source-in", IN)
  else CANVAS_OP_TO_GFX_OP("source-out", OUT)
  else CANVAS_OP_TO_GFX_OP("source-over", OVER)
  else CANVAS_OP_TO_GFX_OP("destination-in", DEST_IN)
  else CANVAS_OP_TO_GFX_OP("destination-out", DEST_OUT)
  else CANVAS_OP_TO_GFX_OP("destination-over", DEST_OVER)
  else CANVAS_OP_TO_GFX_OP("destination-atop", DEST_ATOP)
  else CANVAS_OP_TO_GFX_OP("lighter", ADD)
  else CANVAS_OP_TO_GFX_OP("xor", XOR)
  else CANVAS_OP_TO_GFX_OP("multiply", MULTIPLY)
  else CANVAS_OP_TO_GFX_OP("screen", SCREEN)
  else CANVAS_OP_TO_GFX_OP("overlay", OVERLAY)
  else CANVAS_OP_TO_GFX_OP("darken", DARKEN)
  else CANVAS_OP_TO_GFX_OP("lighten", LIGHTEN)
  else CANVAS_OP_TO_GFX_OP("color-dodge", COLOR_DODGE)
  else CANVAS_OP_TO_GFX_OP("color-burn", COLOR_BURN)
  else CANVAS_OP_TO_GFX_OP("hard-light", HARD_LIGHT)
  else CANVAS_OP_TO_GFX_OP("soft-light", SOFT_LIGHT)
  else CANVAS_OP_TO_GFX_OP("difference", DIFFERENCE)
  else CANVAS_OP_TO_GFX_OP("exclusion", EXCLUSION)
  else CANVAS_OP_TO_GFX_OP("hue", HUE)
  else CANVAS_OP_TO_GFX_OP("saturation", SATURATION)
  else CANVAS_OP_TO_GFX_OP("color", COLOR)
  else CANVAS_OP_TO_GFX_OP("luminosity", LUMINOSITY)
  // XXX ERRMSG we need to report an error to developers here! (bug 329026)
  else return;

#undef CANVAS_OP_TO_GFX_OP
  CurrentState().op = comp_op;
}

void
BasicRenderingContext2D::GetGlobalCompositeOperation(nsAString& aOp,
                                                     ErrorResult& aError)
{
  CompositionOp comp_op = CurrentState().op;

#define CANVAS_OP_TO_GFX_OP(cvsop, op2d) \
  if (comp_op == CompositionOp::OP_##op2d) \
    aOp.AssignLiteral(cvsop);

  CANVAS_OP_TO_GFX_OP("copy", SOURCE)
  else CANVAS_OP_TO_GFX_OP("destination-atop", DEST_ATOP)
  else CANVAS_OP_TO_GFX_OP("destination-in", DEST_IN)
  else CANVAS_OP_TO_GFX_OP("destination-out", DEST_OUT)
  else CANVAS_OP_TO_GFX_OP("destination-over", DEST_OVER)
  else CANVAS_OP_TO_GFX_OP("lighter", ADD)
  else CANVAS_OP_TO_GFX_OP("source-atop", ATOP)
  else CANVAS_OP_TO_GFX_OP("source-in", IN)
  else CANVAS_OP_TO_GFX_OP("source-out", OUT)
  else CANVAS_OP_TO_GFX_OP("source-over", OVER)
  else CANVAS_OP_TO_GFX_OP("xor", XOR)
  else CANVAS_OP_TO_GFX_OP("multiply", MULTIPLY)
  else CANVAS_OP_TO_GFX_OP("screen", SCREEN)
  else CANVAS_OP_TO_GFX_OP("overlay", OVERLAY)
  else CANVAS_OP_TO_GFX_OP("darken", DARKEN)
  else CANVAS_OP_TO_GFX_OP("lighten", LIGHTEN)
  else CANVAS_OP_TO_GFX_OP("color-dodge", COLOR_DODGE)
  else CANVAS_OP_TO_GFX_OP("color-burn", COLOR_BURN)
  else CANVAS_OP_TO_GFX_OP("hard-light", HARD_LIGHT)
  else CANVAS_OP_TO_GFX_OP("soft-light", SOFT_LIGHT)
  else CANVAS_OP_TO_GFX_OP("difference", DIFFERENCE)
  else CANVAS_OP_TO_GFX_OP("exclusion", EXCLUSION)
  else CANVAS_OP_TO_GFX_OP("hue", HUE)
  else CANVAS_OP_TO_GFX_OP("saturation", SATURATION)
  else CANVAS_OP_TO_GFX_OP("color", COLOR)
  else CANVAS_OP_TO_GFX_OP("luminosity", LUMINOSITY)
  else {
    aError.Throw(NS_ERROR_FAILURE);
  }

#undef CANVAS_OP_TO_GFX_OP
}

//
// path bits
//
void
BasicRenderingContext2D::TransformWillUpdate()
{
  EnsureTarget();

  // Store the matrix that would transform the current path to device
  // space.
  if (mPath || mPathBuilder) {
    if (!mPathTransformWillUpdate) {
      // If the transform has already been updated, but a device space builder
      // has not been created yet mPathToDS contains the right transform to
      // transform the current mPath into device space.
      // We should leave it alone.
      mPathToDS = mTarget->GetTransform();
    }
    mPathTransformWillUpdate = true;
  }
}

} // namespace dom
} // namespace mozilla
