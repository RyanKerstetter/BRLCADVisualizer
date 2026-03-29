#include "interactioncontroller.h"

InteractionController::Result InteractionController::classify(
    Qt::MouseButtons buttons, Qt::KeyboardModifiers mods)
{
  Result r{};

  const bool shift = mods & Qt::ShiftModifier;
  const bool ctrl = mods & Qt::ControlModifier;
  const bool alt = mods & Qt::AltModifier;

  // Default actions
  if (shift && ctrl)
    r.action = Action::Scale;
  else if (ctrl)
    r.action = Action::Rotate;
  else if (shift)
    r.action = Action::Translate;
  else
    r.action = Action::None;

  // Alt-based axis constraints + fallback actions
  if (alt) {
    if (buttons & Qt::LeftButton) {
      // Alt+Left = X
      // Alt+Shift+Left = Y
      if (shift && !ctrl)
        r.axis = AxisConstraint::Y;
      else
        r.axis = AxisConstraint::X;

      // If no action chosen yet, default to translate
      if (r.action == Action::None)
        r.action = Action::Translate;
    } else if (buttons & Qt::RightButton) {
      // Alt+Right = Z
      r.axis = AxisConstraint::Z;

      // If no action chosen yet, default to rotate
      if (r.action == Action::None)
        r.action = Action::Rotate;
    }
  }

  return r;
}