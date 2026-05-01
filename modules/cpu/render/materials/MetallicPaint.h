#include "math/spectrum.h"
#include "render/Material.h"
// ispc shared
#include "MetallicPaintShared.h"

namespace ospray {
namespace pathtracer {

struct MetallicPaint : public AddStructShared<Material, ispc::MetallicPaint>
{
  MetallicPaint(api::ISPCDevice &device);

  virtual std::string toString() const override;

  virtual void commit() override;

 private:
  MaterialParam3f color;
};

} // namespace pathtracer
} // namespace ospray
