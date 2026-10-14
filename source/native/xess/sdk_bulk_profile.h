#pragma once
#include <cstddef>

// Exact private XeFG 1.3.1.78 image only (the layout families map the same presenter
// structure); do not infer support from version text or use this as universal discovery.
// SHA256 EC5E0C65E075570C6EDE72618BB666D0BE0C2E10B2EA9762C0FE8CB8E375AB27.
namespace sdk_pacing::bulk_profile {
// The presenter's presentation (copy) queue; chain+0x28 holds the same pointer. Read only
// after the owner's context identity is proven, and used only after NativeQueue's proof.
inline constexpr size_t kPresenterCopyQueueOffset = 0x10;
}
