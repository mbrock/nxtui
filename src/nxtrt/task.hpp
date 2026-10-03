#pragma once

// Public coroutine runtime API. The implementation is organized by
// responsibility under nxtrt/task/; runtime.hpp supplies the definitions
// that connect promises, child records, firms, decks, and wishes.
// Keep this umbrella as the supported include for the complete runtime.

#include "nxtrt/task/promise.hpp"
#include "nxtrt/task/task.hpp"
#include "nxtrt/task/context.hpp"
#include "nxtrt/task/deed.hpp"
#include "nxtrt/task/firm.hpp"
#include "nxtrt/task/compose.hpp"
#include "nxtrt/task/scope.hpp"
#include "nxtrt/task/concurrent.hpp"
#include "nxtrt/task/root.hpp"
#include "nxtrt/task/polling.hpp"
#include "nxtrt/task/runtime.hpp"
