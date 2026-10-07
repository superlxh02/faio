#ifndef FAIO_DETAIL_EXPERIMENTAL_REFLECTION_HPP
#define FAIO_DETAIL_EXPERIMENTAL_REFLECTION_HPP

#include "faio/detail/experimental/runtime_options.hpp"
#if !FAIO_EXPERIMENTAL_HAS_REFLECTION
#error "faio experimental: reflection capability requires faio::experimental_reflection"
#endif
#include <meta>

namespace faio::experimental {
namespace detail {
consteval runtime_options checked_annotation(runtime_options options) {
  if (const auto error = validate_options(options); error != options_error::none)
    throw options_error_message(error);
  return options;
}
}

template <std::meta::info Entity>
consteval runtime_options runtime_options_of() {
  const auto annotations = std::meta::annotations_of_with_type(Entity, ^^runtime_options);
  if (annotations.size() > 1)
    throw "faio experimental: duplicate runtime_options";
  return detail::checked_annotation(annotations.empty() ? runtime_options{}
      : std::meta::extract<runtime_options>(annotations.front()));
}
} // namespace faio::experimental
#endif
