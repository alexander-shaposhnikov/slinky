#ifndef SLINKY_INFER_BOUNDS_H
#define SLINKY_INFER_BOUNDS_H

#include "symbol_map.h"

namespace slinky {

stmt infer_bounds(const stmt& s, node_context& ctx);

stmt sliding_window(const stmt& s, node_context& ctx);

}  // namespace slinky

#endif  // SLINKY_INFER_BOUNDS_H