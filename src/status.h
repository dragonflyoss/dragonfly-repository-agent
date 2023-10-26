#ifndef STATUS_H
#define STATUS_H

#include "triton/core/tritonserver.h"

struct ErrorException {
  ErrorException(TRITONSERVER_Error* err) : err_(err) {}
  TRITONSERVER_Error* err_;
};

#define RETURN_IF_ERROR(X)              \
 do {                                   \
   TRITONSERVER_Error* rie_err__ = (X); \
   if (rie_err__ != nullptr) {          \
     return rie_err__;                  \
   }                                    \
 } while (false)
#endif // STATUS_H