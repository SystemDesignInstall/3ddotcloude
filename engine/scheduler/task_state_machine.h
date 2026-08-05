#pragma once

// Six-state lifecycle transition table (RFC-0003 §5.4, scheduler-design §3).
// The retry edge (failed -> pending) is allowed by the table but gated
// externally: only recoverable failures (ADR-014) with attempts <
// max_attempts may take it. Exactly one terminal state per task is enforced
// as a property invariant here and property-tested.

#include "engine/task/task_types.h"

namespace spatial::engine {

class TaskStateMachine {
 public:
  // True when the transition is part of the ratified state machine:
  //   pending   -> running | cancelled | skipped | failed (strict path)
  //   running   -> succeeded | failed | cancelled
  //   failed    -> pending        (retry, gated externally)
  // Terminal states have no outgoing transitions.
  static bool IsLegal(TaskStatus from, TaskStatus to) noexcept;

  // Returns `to` when legal; throws SchedulerError (kInternal, invariant
  // violation) otherwise.
  static TaskStatus Transition(TaskStatus from, TaskStatus to);
};

}  // namespace spatial::engine
