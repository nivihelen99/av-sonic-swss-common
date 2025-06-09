#include "common/eventbus.h"

// All method implementations are currently inline in common/eventbus.h.
// This file is provided for completeness and for potential future non-inline definitions.

// For example, if EventBus had a static member, it would be defined here:
// Type EventBus::static_member_name = initial_value;

// Or if a method was declared in the .h but not defined inline:
// void EventBus::someMethod() {
//     // implementation
// }

// As of the current design, no definitions are strictly required here
// because the header common/eventbus.h provides all implementations inline.
// This makes it effectively a header-only library.
//
// If we decide to add features like a dedicated dispatch thread,
// its management logic (e.g., thread creation in constructor, joining in destructor,
// and a separate event queue) would likely involve more substantial code here
// and modifications to the header file.
//
// For now, this file serves as a placeholder and ensures that the header
// is included in a compilation unit.

namespace common {
// If there were any non-inline methods for EventBus or Subscription, they would go here.
// For example, the destructor for Subscription if it needed to call unsubscribe:
// Subscription::~Subscription() {
//     if (auto bus_ptr = bus_.lock()) { // Check if EventBus still exists
//         bus_ptr->unsubscribe(this->topic_name_if_it_was_stored, id_); // Hypothetical
//     }
// }
// However, to implement this, Subscription would need to know its topic or the EventBus
// would need a more complex unsubscribe mechanism not solely reliant on topic + id.
// The current `unsubscribe(std::shared_ptr<Subscription> sub)` in EventBus handles this.
} // namespace common
