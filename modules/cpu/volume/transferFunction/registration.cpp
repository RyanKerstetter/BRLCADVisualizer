#include "LinearTransferFunction.h"

namespace ospray {

void registerAllTransferFunctions()
{
  TransferFunction::registerType<LinearTransferFunction>("piecewiseLinear");
}

} // namespace ospray
