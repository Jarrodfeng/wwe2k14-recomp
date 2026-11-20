// fixture: invented code that reads kWidgetMax (a same-class reader)
#include "limits.h"

int ClampToMax(int v) {
  if (v > kWidgetMax) {
    return kWidgetMax;
  }
  return v;
}
