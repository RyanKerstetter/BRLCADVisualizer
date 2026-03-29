#pragma once

#include <Qt>

class InteractionController
{
 public:
  enum class Action
  {
    None,
    Translate,
    Rotate,
    Scale
  };

  enum class AxisConstraint
  {
    Free,
    X,
    Y,
    Z
  };

  struct Result
  {
    Action action = Action::None;
    AxisConstraint axis = AxisConstraint::Free;
  };

  static Result classify(Qt::MouseButtons buttons, Qt::KeyboardModifiers mods);
};