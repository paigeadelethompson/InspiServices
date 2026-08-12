// InspiServices - database schema initialisation.
#pragma once

#include "services/db.h"

namespace svc {

  // Creates the full schema (idempotent) on first run.
  void db_schema(db &d);

} // namespace svc