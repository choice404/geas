// A visitor built from lambdas, one per alternative of a variant
#pragma once

namespace geas {

template <typename... Visitors>
struct Overloaded : Visitors... {
    using Visitors::operator()...;
};

}  // namespace geas
