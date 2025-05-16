#include <gst/gst.h>

// #include "deepstream/sources/includes/nvbufsurface.h"
#include "nvbufsurface.h"
#include "nvbufsurftransform.h"

namespace hm {
namespace gst {

void inspect_nvbufsurface_dtype(GstBuffer* buffer);

void print_caps(const GstCaps* caps);
void print_caps_details(const GstCaps* caps);
gint get_batch_size_from_caps(GstCaps* caps);

// Conversion templates for setting values from GValue.
template <typename T>
void set_value(T& member, const GValue* value);

// Specialization for bool.
template <>
inline void set_value<bool>(bool& member, const GValue* value) {
  member = g_value_get_boolean(value);
}

// Specialization for unsigned int.
template <>
inline void set_value<unsigned int>(unsigned int& member, const GValue* value) {
  member = g_value_get_uint(value);
}

template <>
inline void set_value<int>(int& member, const GValue* value) {
  member = g_value_get_int(value);
}

// Specialization for enum types.
template <>
inline void set_value<NvBufSurfaceMemType>(NvBufSurfaceMemType& member, const GValue* value) {
  member = static_cast<NvBufSurfaceMemType>(g_value_get_enum(value));
}

template <>
inline void set_value<NvBufSurfTransform_Inter>(NvBufSurfTransform_Inter& member, const GValue* value) {
  member = static_cast<NvBufSurfTransform_Inter>(g_value_get_enum(value));
}

// Specialization for strings. Note: we free the old string.
template <>
inline void set_value<char*>(char*& member, const GValue* value) {
  if (member)
    g_free(member);
  member = g_value_dup_string(value);
}

// Conversion templates for getting values into a GValue.
template <typename T>
inline void get_value(GValue* value, const T& member);

// Specialization for bool.
template <>
inline void get_value<bool>(GValue* value, const bool& member) {
  g_value_set_boolean(value, member);
}

// Specialization for unsigned int.
template <>
inline void get_value<unsigned int>(GValue* value, const unsigned int& member) {
  g_value_set_uint(value, member);
}

template <>
inline void get_value<int>(GValue* value, const int& member) {
  g_value_set_int(value, member);
}

// Specialization for enum types.
template <>
inline void get_value<NvBufSurfaceMemType>(GValue* value, const NvBufSurfaceMemType& member) {
  g_value_set_enum(value, member);
}

template <>
inline void get_value<NvBufSurfTransform_Inter>(GValue* value, const NvBufSurfTransform_Inter& member) {
  g_value_set_enum(value, member);
}

// Specialization for strings.
template <>
inline void get_value<char*>(GValue* value, char* const& member) {
  g_value_set_string(value, member);
}

// Macros to generate switch-case entries.
#define PROPERTY_SET_CASE(prop, member) \
  case prop:                            \
    hm::gst::set_value(member, value);  \
    break;

#define PROPERTY_GET_CASE(prop, member) \
  case prop:                            \
    hm::gst::get_value(value, member);  \
    break;

} // namespace gst
} // namespace hm
