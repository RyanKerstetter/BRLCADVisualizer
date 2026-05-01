#pragma once

#include <memory>
#include <vector>
#include "../../fb/TileOperation.h"

namespace ospray {
struct AlphaCompositeTileOperation : public TileOperation
{
  std::unique_ptr<LiveTileOperation> makeTile(DistributedFrameBuffer *dfb,
      const vec2i &tileBegin,
      size_t tileID,
      size_t ownerID) override;

  std::string toString() const override;
};
} // namespace ospray
