#include "Texture.h"

namespace ospray {

// Texture definitions ////////////////////////////////////////////////////////

Texture::Texture(api::ISPCDevice &device)
    : AddStructShared(device.getDRTDevice(), device)
{
  managedObjectType = OSP_TEXTURE;
}

std::string Texture::toString() const
{
  return "ospray::Texture";
}

OSPTYPEFOR_DEFINITION(Texture *);

} // namespace ospray
