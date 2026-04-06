#include "interactioncontroller.h"

InteractionController::Result InteractionController::classify(
    Qt::MouseButtons buttons, Qt::KeyboardModifiers mods)
{
  Result r{};

  const bool shift = mods & Qt::ShiftModifier;
  const bool ctrl = mods & Qt::ControlModifier;
  const bool alt = mods & Qt::AltModifier;
  const bool left = buttons & Qt::LeftButton;
  const bool right = buttons & Qt::RightButton;

  if (alt) {
    // Strict matching for documented axis-constrained gestures only.
    if (left && !right && !ctrl) {
      if (shift) {
        r.action = Action::Translate;
        r.axis = AxisConstraint::Y;
      } else {
        r.action = Action::Translate;
        r.axis = AxisConstraint::X;
      }
    } else if (right && !left && !shift && !ctrl) {
      r.axis = AxisConstraint::Z;
      r.action = Action::Translate;
    }

    return r;
  }

  // Default non-alt actions.
  if (shift && ctrl)
    r.action = Action::Scale;
  else if (ctrl)
    r.action = Action::Rotate;
  else if (shift)
    r.action = Action::Translate;

  return r;
}
