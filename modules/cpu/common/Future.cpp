#include "Future.h"

namespace ospray {

Future::Future()
{
  managedObjectType = OSP_FUTURE;
}

std::string Future::toString() const
{
  return "ospray::Future";
}

OSPTYPEFOR_DEFINITION(Future *);

} // namespace ospray
