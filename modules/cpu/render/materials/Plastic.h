#include "render/Material.h"
// ispc shared
#include "PlasticShared.h"

namespace ospray {
namespace pathtracer {

struct Plastic : public AddStructShared<Material, ispc::Plastic>
{
  Plastic(api::ISPCDevice &device);

  virtual std::string toString() const override;

  virtual void commit() override;
};

} // namespace pathtracer
} // namespace ospray
